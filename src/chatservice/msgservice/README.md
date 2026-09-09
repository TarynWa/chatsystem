# MsgService — 消息服务（S0 单聊）设计文档

> `chatservice` 是分布式 IM 的**业务服务层仓库**（承载 AuthService / MsgService / SocialService），
> 依赖底座 `logsystem / NetWork(nwl) / threadpool / mprpc` 四个子模块。本文档对应其中的**消息服务 S0（单聊最小闭环）**：
> **单聊收发 + 聊天历史 + 拉新消息**。
>
> 关联文档：`../README.md`(auth 同款文档) / `db/init.sql`(`im_message` 库依据) / `common/errcode.h` / `proto/msg.proto`

---

## 1. 定位与裁剪

**消息服务**负责「单聊消息的写入、历史读取、接收方离线收件箱」。S0 落地最小闭环：

```
RPC 清单(S0)
┌──────────────────────────────────────────────────────────────┐
│  SendMsg     发单聊消息(双写: 会话历史 + 接收方离线收件箱)      │
│  PullNewMsg  接收方主动拉取新消息(认领 offline 收件箱)          │
│  GetHistory  双向会话历史(msg_id 倒序游标分页)                 │
└──────────────────────────────────────────────────────────────┘
```

**非目标（本期不做）**：群聊/群组消息、消息撤回、已读回执、未读红点、消息状态变更同步、
在线状态/推送（无在线连接表）、客户端长连接网关。

### 1.1 实时触达模型：客户端拉取（不是 push）

底座现状是 mprpc **短连接 RPC**（一发一收即断），S0 没有在线连接表、无法判断接收方是否在线，
因此不做服务端推送，采用**客户端拉取模型**：

```
send(A→B) 时服务端只落库(同一事务双写)：
   ① messages_YYYYMM   —— A、B 会话的权威历史(未来 App 打开会话用 GetHistory 拉)
   ② offline_messages  —— 给 B 的"收件箱"行(status=0, is_group=0)
B 何时"收到"？ → 由 B 主动调 PullNewMsg 从收件箱认领(status 0→1), 才算已投递。
```

这等价于「发送即落离线表，接收方主动取」；真正的 push / 在线通道 / 网关在 M4 之后（见 §7）。

### 1.2 鉴权口径（S0）

请求体直接携带调用方 uid（`SendMsg.from_user` / `PullNewMsg.user_id` / `GetHistory.user_id`）。
服务端**不调 AuthService.VerifyToken** —— 因为 mprpc `RpcHeader` 目前只有 service/method/args_size、
**没有 token 槽位**；把 token 下沉到 RpcHeader 并跨服校验属于里程碑 M4。S0 因此**信任调用方 uid**。

## 2. 接口总览（proto: msg.proto, 依赖 common.proto 的 `chatservice.Result`）

| RPC | 请求 | 响应 | 语义 |
|---|---|---|---|
| `SendMsg` | `from_user,to_user,msg_type,content,media_urls` | `Result,msg_id,created_at` | 雪花出 msg_id → 双写历史+收件箱 |
| `PullNewMsg` | `user_id,limit` | `Result,repeated MsgRecord` | 认领 ≤limit 条未拉取消息 |
| `GetHistory` | `user_id,peer_id,before_id,limit` | `Result,repeated MsgRecord,has_more` | 双向历史倒序分页 |

- `MsgRecord{msg_id,from_user,to_user,msg_type,content,media_urls,created_at}`；msg_type `0文本/1图片/2语音/3视频/4文件/5位置/6表情`。
- 分页游标 = **msg_id**（雪花单调递增，天然可作游标，无需翻 id 主键）。`before_id=0` 取最新一页；`>0` 取 `msg_id < before_id` 的更早。
- limit：Send 不受限（本地长度校验）；Pull `<=0→50`、封顶 `100`；GetHistory `<=0→20`、封顶 `50`。多取 1 行判定 `has_more`。

## 3. 层职责红线与目录结构

```
msgservice/
├── main.cpp                # 组装: Init → DbPool(im_message) → Snowflake(1) → NotifyService → Run
├── msgservice_impl.{h,cc}  # RPC 层: 参数校验(0/相等/长度/type区间) + 调 service + 回填响应
├── model/message.h         # 纯结构体(MessageRow/SentMessage), 不引 protobuf/SQL
├── dao/
│   ├── message_dao.{h,cc}  # messages_YYYYMM 单表读写(currentMonthTable/insertMessage/queryCreatedAt/fetchHistory)
│   └── offline_dao.{h,cc}  # offline_messages 单表读写(insertOffline/claimOffline 原子认领)
└── service/message_service.{h,cc}  # 流程编排(雪花+事务双写/拉取/历史), 不碰 SQL 不依赖 protobuf
```

```
impl(RPC: 校验+编排, 不写SQL/不碰Redis) → service(编排, 无proto无SQL) → dao(单表) / cache(无)
model = 纯结构体 跨层传递; 连接池 DbConn 单请求借用、用完即还(不跨请求持连接)。
```

线程安全：mprpc 的 4 个 nwl IO 线程**同步内联**执行服务方法 → 方法无共享可变状态（雪花/连接池自线程安全）、不做长阻塞。

## 4. 核心流程时序

### 4.1 SendMsg：双写（单事务）+ 回显 created_at

```
SendMsg(A→B, type, content)
  └─ Snowflake.nextId() → msg_id                     # 应用层出号, 不靠 DB 生成器
  └─ 单连接 Txn：
       insertMessage(messages_YYYYMM, msg_id,A,B,…)  # 权威历史
       insertOffline(offline_messages, msg_id, B,…)  # B 收件箱 status=0
       queryCreatedAt 回读 created_at
     └─ commit(任一步失败自动回滚 → 1002)
  └─ 回 msg_id + created_at
```

### 4.2 PullNewMsg：原子认领（防并发双发）

```
PullNewMsg(B, limit)
  └─ claimOffline(同连接事务):
       SELECT id … WHERE user_id=B AND status=0 AND is_group=0
             ORDER BY id ASC LIMIT ?  FOR UPDATE SKIP LOCKED   # 锁住候选行
       UPDATE … SET status=1, delivered_at=NOW() WHERE id IN(…)
       SELECT 全列 WHERE id IN(…)  → 回读
       COMMIT(空集也空手 COMMIT; 中途异常 Txn RAII 回滚)
```
`FOR UPDATE SKIP LOCKED`（MySQL 8.4）保证：**多个短连接并发 pull 不会把同一条发给同一用户两次**。

### 4.3 GetHistory：双向会话 + 游标分页

```
SELECT … FROM messages_YYYYMM
 WHERE ( (from=me AND to=peer) OR (from=peer AND to=me) )   # 方向组整体括起来!
   [AND msg_id < before_id]                                  # 游标(>0 时), 严格小于
 ORDER BY msg_id DESC LIMIT limit+1                          # 多取 1 判 has_more
```
> ⚠️ 方向 OR 组**必须整体加括号**。SQL 中 `AND` 优先级高于 `OR`，若写成
> `((from=me AND to=peer) OR (from=peer AND to=me) AND msg_id<X)`，
> 前一个方向会**绕过 msg_id 边界**（`from=me` 方向的旧消息也返回来）——已踩坑并修复（见 msg_cli 用例「history 翻页」）。

## 5. 关键实现要点

- **雪花 ID**：`base/snowflake.{h,cc}`，单机 worker=1；位布局 `41bit 毫秒(epoch 2024-01-01) | 10bit worker | 12bit seq`；
  mutex 短临界；时钟回拨自旋等追平。多进程部署时每实例配不同 worker（远期）。
- **表名按月**：`messages_YYYYMM`（`localtime_r` 拼当月）；未建表 insert 会报错 → 归 1002。S0 只落当月表。
- **SQL 注入**：全走 `esc/q/opt` 转义 + `std::to_string` 拼数字，无拼接字段白名单外的直接内插。
- **校验（impl 层本地判，200B 内提前拒）**：`from||to==0`→1001；`from==to`→**1401**(不能给自己发)；
  type 越界→1001；`type==0 且 content 空` / content>5000B / media>8192B→1001。
- **落库口径**：`content/media_urls` 空串存 NULL（`opt`）。
- **错误兜底**：DB 异常/连接超时统一 1002。

## 6. DAO → 表映射（im_message）

| DAO | 表 | 说明 |
|---|---|---|
| `message_dao::insertMessage / queryCreatedAt / fetchHistory` | `messages_YYYYMM` | 单聊会话历史（msg_id 雪花主键、双向可见） |
| `offline_dao::insertOffline / claimOffline` | `offline_messages` | B 收件箱；claim = 认领并置 `status=1`、`delivered_at=NOW()` |

## 7. 错误码（`common/errcode.h`）

| code | 名 | 触发 |
|---|---|---|
| 0 | OK | |
| 1001 | ERR_PARAM | uid 为 0 / 类型越界 / 文本空或超长 |
| 1002 | ERR_BUSY | DB 异常 / 连接超时 / 当月表未建 |
| 1401 | ERR_MSG_SELF | `from == to`（不能给自己发） |

## 8. 配置项（conf/msg.conf / msg_cli.conf）

server：`rpcserverip/port`(8002) `zookeeperip/port`(2181) `mysqlip/user/password/db=im_message` `mysqlpool_min/max`。
S0 不接 Redis。cli 只读 `rpcserverip/port`（**须指向 msg_server 8002**，直连免 zk）。

## 9. 构建与验收

```bash
cmake -S . -B build && cmake --build build -j$(nproc) --target msg_server msg_cli
./bin/msg_server -i conf/msg.conf          # 应见 zk 3 节点: /MsgServiceRpc/{SendMsg,PullNewMsg,GetHistory}
./bin/msg_cli    -i conf/msg_cli.conf       # 期望 pass=13 fail=0
```

E2E 覆盖（msg_cli 13 用例）：A→B / B→A 收发、pull(B)、pull(A) 各得 1 条且再拉为空、
双向历史 2 条倒序、**翻页 before=最新→剩 1 条 / before=最早→空**、自聊 1401、空文本与非法类型 1001。

**MySQL 落点抽查**（跑一次后）：

```sql
-- 历史表: 每发一条即一行(雪花 msg_id); 收件箱每发一条即给接收方一行
SELECT msg_id,from_user,to_user,content FROM messages_202609 ORDER BY msg_id;
-- pull 认领后 status=1、delivered_at 非空; 再 pull 为空
SELECT user_id,msg_id,status,delivered_at FROM offline_messages ORDER BY id;
```

**重复运行**：msg_cli 用固定测试 uid(1000001/1000002)，重跑前先清库，否则历史/收件箱行数不满足断言：
`mysql -uroot im_message -e "TRUNCATE messages_202609; TRUNCATE offline_messages;"`

## 10. 遗留与 M4 方向

- 鉴权：token 下沉 RpcHeader + 跨服校验（现信任调用方 uid）；未校验收/发方是否真实存在（测试 uid 非 im_auth 用户）。
- 群聊消息/群组、撤回、已读回执、未读计数：表结构已在 `db/init.sql`（group_messages/message_read_status/unread_counts），S0 未接。
- 历史表跨月：`currentMonthTable()` 只落当月，需月度建表任务或动态建表。
- 压测(M4 里程碑) / 与 SocialService 的「好友才可发消息」联动校验（S0 未做好友校验）。
