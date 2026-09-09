# SocialService — 社交服务（S0 好友+黑名单）设计文档

> `chatservice` 是分布式 IM 的**业务服务层仓库**（承载 AuthService / MsgService / SocialService），
> 依赖底座 `logsystem / NetWork(nwl) / threadpool / mprpc` 四个子模块。本文档对应其中的**社交服务 S0（最小闭环）**：
> **好友（申请/处理/列表/删除）+ 黑名单（加/列表/移除）**。
>
> 关联文档：`../README.md`(auth 同款文档) / `db/init.sql`(`im_social` 库依据) / `common/errcode.h` / `proto/social.proto`

---

## 1. 定位与裁剪

**社交服务**负责「好友关系与黑名单」的关系型数据与校验。S0 落地最小闭环：

```
RPC 清单(S0)                       好友: friends(关系) + friend_requests(申请流水)
┌──────────────────────────────────────────────────────────────┐
│  AddFriend        发好友申请 from→to(3 天过期)                 │
│  HandleFriend     处理申请: action 1接受 / 2拒绝               │
│  ListFriends      我的好友列表(status=1, 双向已确认)           │
│  DeleteFriend     删除好友(两侧软删 status=3)                 │
│  AddBlacklist     拉黑(幂等 upsert)                          │
│  ListBlacklist    我的黑名单列表                               │
│  RemoveBlacklist  移除黑名单(幂等 delete)                     │
└──────────────────────────────────────────────────────────────┘
```

**非目标（本期不做）**：群组/群聊、好友备注(remark 恒空)、关系标签/分组、
申请消息模板、朋友圈等其它社交、`friends.status` 0待/2拒 状态机与 TCC 场景（表结构在，见 §7）。

### 1.1 好友写口径（S0 关键约定）

- **pending 只落 `friend_requests`**（status=0），**不往 `friends` 写 status=0 行**。
- `friends` 关系行只在 **HandleFriend=接受** 的那一个事务里**双向各建一行**（A→B 与 B→A），status=1。
- **重复申请**靠 friend_requests 无唯一键 → 应用层查「未过期 pending」拦截（1502）。
- **删除** = 两侧 `status=3` 软删（保留历史关系，可再接受恢复）。
- 接受后若两行已存在（如 status=3 重加），用 `ON DUPLICATE KEY UPDATE status=1, confirm_at=NOW()` **复活**。

> `friends.status` 的 0待/2拒 以及「申请被拒绝后落 status=2」的完整状态机、TCC 补偿，
> 属 `db/README` 场景 4 的后续演进；S0 只保留 1(确认)/3(删除) 两个落库值。

### 1.2 鉴权口径（S0）

请求体直接携带操作者 uid（`from_user` / `handler_id` / `user_id`）。服务端**不调 AuthService.VerifyToken**
（mprpc `RpcHeader` 无 token 槽位，M4 下放），**信任调用方 uid**；且**不校验对方 users 是否存在**
（im_social 域库连不到 im_auth 的 users 表，由未来网关保证真实身份）。

## 2. 接口总览（proto: social.proto, 依赖 common.proto 的 `chatservice.Result`）

| RPC | 请求 | 响应 | 语义 |
|---|---|---|---|
| `AddFriend` | `from_user,to_user,apply_msg` | `Result,request_id,expire_at` | 校验前置后落一条 3 天有效申请 |
| `HandleFriend` | `handler_id,request_id,action(1/2)` | `Result` | 接受→双向建关系; 拒绝→置 2 |
| `ListFriends` | `user_id` | `Result,repeated FriendItem` | `FriendItem{friend_id,remark,apply_msg,created_at(=confirm_at)}` |
| `DeleteFriend` | `user_id,friend_id` | `Result` | 两侧软删, 幂等 |
| `AddBlacklist` | `user_id,blocked_user_id,reason` | `Result` | 幂等 upsert(刷新 reason) |
| `ListBlacklist` | `user_id` | `Result,repeated BlackItem` | `BlackItem{blocked_user_id,reason,created_at}` |
| `RemoveBlacklist` | `user_id,blocked_user_id` | `Result` | 幂等 delete |

## 3. 层职责红线与目录结构

```
socialservice/
├── main.cpp                # 组装: Init → DbPool(im_social) → NotifyService → Run(无需雪花)
├── socialservice_impl.{h,cc}  # RPC 层: 校验(0/相等/长度/action) + 调 service + 回填
├── model/social.h          # 纯结构体(FriendRow/RequestRow/BlackRow)
├── dao/
│   ├── friend_dao.{h,cc}   # friends 表(isFriend/upsertFriend/listFriends/softDeleteFriend)
│   ├── request_dao.{h,cc}  # friend_requests 表(pending 查重/插单/load/接受/拒绝护栏/过期标记)
│   └── black_dao.{h,cc}    # blacklist 表(addBlack/removeBlack/listBlack/isBlocked)
└── service/social_service.{h,cc}  # 流程编排(加好友前置序/接受事务/删除/列表), 无proto无SQL
```

```
impl(RPC: 校验+编排, 不写SQL) → service(编排, 无proto无SQL) → dao(单表)
model = 纯结构体 跨层传递; DbConn 单请求借用; 多写用 Txn RAII 保证原子。
```

线程安全：mprpc 4 个 nwl IO 线程同步内联执行 → 方法无共享可变状态、不长阻塞。
并发正确性由 **SQL 护栏**(UPDATE … WHERE status=0 AND to_user=? AND expire_at>NOW()) + 事务保证。

## 4. 核心流程时序

### 4.1 AddFriend：前置校验序（任一命中即短路）

```
AddFriend(from→to, apply_msg)
  1. from==to                          → 1506 不能对自己
  2. isFriend(from,to)                 → 1501 已是好友
  3. 存在未过期 pending(from,to)        → 1502 重复申请
  4. isBlocked(to, from)               → 1507 你在对方黑名单
  5. isBlocked(from, to)               → 1508 你已拉黑对方
  6. INSERT friend_requests(status=0, expire_at=NOW()+INTERVAL 3 DAY)  # 显式给 expire_at
     回填 request_id + expire_at
```

### 4.2 HandleFriend=接受：同一事务内「置接受 + 双向建关系」

```
HandleFriend(handler=B, request_id, action=1)
  ├ load 不存在                          → 1503
  ├ req.to_user != handler               → 1505 非处理人
  ├ req.status != 0                      → 1503 已处理/已标记
  ├ requestExpired                       → markExpired(status=3) → 1504
  └ accept 路径(单连接 Txn):
       acceptRequest: UPDATE status=1,handled_at=NOW(),handler_id=B
                      WHERE id=? AND status=0 AND to_user=B AND expire_at>NOW()
       —— 影响行数==0 → 1503(并发抢先/恰过期), 回滚
       upsertFriend(A,B,apply_msg)  # A 视角行(旧 status=3 则复活为 1)
       upsertFriend(B,A,apply_msg)  # B 视角行
       COMMIT                        # 缺任一行 = 单向好友, 故两 upsert 必须同事务
```

拒绝(action=2) 路径：单语句 `rejectRequest`(status=2)，护栏并发下谁先到谁生效，后到者也返回 0。

### 4.3 DeleteFriend / 黑名单

- DeleteFriend：`softDeleteFriend(me,fid)` + `softDeleteFriend(fid,me)`，仅当 status=1→3，幂等返回 0。
- AddBlacklist：`INSERT … ON DUPLICATE KEY UPDATE reason=VALUES(reason)` 幂等；黑名单本身不自动解除好友。
- RemoveBlacklist：`DELETE` 幂等（拉黑/移除双向均可解除）。

## 5. 关键实现要点

- **`friend_requests.expire_at` NOT NULL 且无默认值** → INSERT 必须显式给 `NOW()+INTERVAL 3 DAY`，漏写 MySQL 报 1364（已踩坑规避）。
- **处理并发护栏**：接受/拒绝的 UPDATE 都带 `status=0 AND to_user=? AND expire_at>NOW()`；
  接受还套事务，`affected==0` 即判定并发抢先/过期并回滚（不产生单向好友）。
- **过期判定**：先 `requestExpired`(status=0 且 expire_at<=NOW()) → 打标 status=3 → 返回 1504；
  状态机里 status 1/2/3 均按 1503 处理（已处理/已过期不留 pending）。
- **SQL 注入**：全走 `esc/q` + `std::to_string`；无表名/字段拼接（黑名单表名单数 `blacklist`）。
- **校验（impl 层）**：目标/操作者为 0→1001；`from==to` / `uid==friend_id` / `uid==blocked`→**1506**；
  `apply_msg`/`reason`>255B→1001；`action∉{1,2}`→1001。

## 6. DAO → 表映射（im_social）

| DAO | 表 | 说明 |
|---|---|---|
| `friend_dao` | `friends` | 单向模板行；isFriend 两方向任一 status=1 防单向脏数据 |
| `request_dao` | `friend_requests` | 申请流水(0待/1接受/2拒绝/3过期)；3 天自动失效 |
| `black_dao` | `blacklist` | user_id→blocked_user_id 唯一 |

## 7. 错误码（`common/errcode.h`）

| code | 名 | 触发 |
|---|---|---|
| 0 | OK | |
| 1001 | ERR_PARAM | 目标为 0 / 超长 / action 非法 |
| 1002 | ERR_BUSY | DB 异常 / 连接超时 |
| 1501 | ERR_ALREADY_FRIEND | 已是好友再申请 |
| 1502 | ERR_REQUEST_PENDING | 已有未过期 pending 申请 |
| 1503 | ERR_REQUEST_INVALID | 申请不存在 / 已处理(接受/拒绝/过期后) / 并发抢先 |
| 1504 | ERR_REQUEST_EXPIRED | 申请已过期(先打标 status=3) |
| 1505 | ERR_REQUEST_FORBIDDEN | 非接收人处理他人申请 |
| 1506 | ERR_SELF_OP | 对自己加好友 / 拉黑 / 删好友 |
| 1507 | ERR_BLOCKED_BY_PEER | 你在对方黑名单（对方拉黑了你） |
| 1508 | ERR_BLOCKED_PEER | 你已拉黑对方 |

## 8. 配置项（conf/social.conf / social_cli.conf）

server：`rpcserverip/port`(8003) `zookeeperip/port`(2181) `mysqlip/user/password/db=im_social` `mysqlpool_min/max`。
S0 不接 Redis；friend_requests.id 自增，无需雪花。cli 只读 `rpcserverip/port`（**须指向 social_server 8003**）。

## 9. 构建与验收

```bash
cmake -S . -B build && cmake --build build -j$(nproc) --target social_server social_cli
./bin/social_server -i conf/social.conf   # 应见 zk 7 方法节点: /SocialServiceRpc/{AddFriend,HandleFriend,ListFriends,DeleteFriend,AddBlacklist,ListBlacklist,RemoveBlacklist}
./bin/social_cli    -i conf/social_cli.conf  # 期望 pass=25 fail=0
```

E2E 覆盖（social_cli 25 用例）：A→B 申请成功→重复 1502→自加 1506→C 越权 1505→B 接受→
**A/B 列表双向互见(带申请语)**→已是好友 1501→重复处理 1503→不存在 1503；
黑名单：加 C→幂等重加→列表含 C→**C 申请 A 被拉黑 1507→A 申请已拉黑的 C 1508**→自拉黑 1506→移除→空；
删好友 A→B 两侧空→**重加+重接受(从 status=3 复活)**→恢复→结束清理删好友。

**MySQL 落点抽查**（接受后 / 删除后）：

```sql
-- 接受后: friends 两行 status=1 + confirm_at 非空; 对应申请 status=1/handler_id 记录处理人
SELECT user_id,friend_id,status,confirm_at FROM friends ORDER BY user_id;
SELECT id,from_user,to_user,status,handler_id,expire_at FROM friend_requests;
-- 删除后: 两行 status=3(软删, 行保留可复活); 黑名单增删可见
SELECT user_id,friend_id,status FROM friends ORDER BY user_id;
SELECT user_id,blocked_user_id,reason FROM blacklist;
```

**重复运行**：social_cli 用固定测试 uid(1000001/1000002/1000003)，用例**结束时已自清理**
（删好友 + 移除黑名单），可直接重跑；若中途中断需重置：
`mysql -uroot im_social -e "TRUNCATE friend_requests; TRUNCATE friends; TRUNCATE blacklist;"`

## 10. 遗留与 M4 方向

- 鉴权：token 下沉 RpcHeader + 跨服校验（现信任 uid，不校验用户存在）。
- 好友备注 remark / 关系分组标签、申请被拒的 status=2 状态机与 TCC 补偿（`db/README` 场景 4）。
- 「只有好友才能发消息」跨 MsgService 联动（S0 未接，需网关或 msg 侧调用 social）。
- 黑名单的拉黑是否自动移除好友关系、以及消息侧对黑名单的拦截（S0 未做）。
