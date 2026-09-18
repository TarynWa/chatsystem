# 腾讯 WXG 测试与质量管理岗 — 项目复习（chat）

> 生成时间：2026-09-11。所有结论以**本地实测 + 源码走查**为准，标注了 file:line 便于被追问时现场翻代码。
> 本文档为 git 工作区未跟踪文件，面试后可删。

---

## 0. 两分钟自述（背下来）

> 「我这个项目是一套**自研的 C++17 Linux 服务端基础设施 + 分布式 IM 业务栈**，目标是用自研库替掉对第三方 muduo 的依赖。
>
> 分两层：
> - **底座四个子模块**（git submodule，各自独立仓库）：日志 `libmuduo_log.so`、epoll 网络库 `libnwl.so`、四类线程池 `libthreadpool.so`、RPC 框架 `libmprpc.so`。网络库是纯 Linux syscall（epoll/timerfd/eventfd/accept4）实现的 one-loop-per-thread Reactor，API 与 muduo 同构，原 muduo 代码换 include 即可迁移。
> - **业务层 `chatservice`**：三个 RPC 微服务 —— AuthService（注册/登录/会话/防爆破）、MsgService（单聊收发/离线收件箱/历史分页）、SocialService（好友/申请/黑名单），MySQL + Redis + ZooKeeper，protobuf 序列化。
>
> 代码量约 **7000 行**（底座 ~2700 + 业务 ~3600）。
>
> 质量侧我的实践是：**每个服务一套端到端黑盒回归用例**，带断言和退出码，我面试前刚跑完 —— **auth 9 个、msg 13 个、social 25 个，合计 47 个用例全绿**；另外用 SQL 抽查落点验证副作用。同时我在走查里定位到了若干真实缺陷，比如服务换机器启动即 core（库内硬编码绝对路径 + assert）、定时器在时钟回拨后永久停摆、分帧缺少粘包处理等。」

**如果只让你说一句**：「自研 muduo 全家桶 + 三微服务 IM，47 个 E2E 用例全绿，走查出 10+ 个可复现缺陷。」

---

## 1. 项目全景（必须能白板画出来）

### 1.1 架构图

```
┌──────────── 业务层 src/chatservice（3 个 RPC 服务）────────────┐
│  auth_server:8001        msg_server:8002      social_server:8003│
│  AuthServiceImpl         MsgServiceImpl       SocialServiceImpl │
│   ↓  service 领域层（业务流程编排，不碰 SQL/protobuf）           │
│   ↓  dao(MySQL) / cache(Redis) / security(哈希·token)           │
└───────────────────────────┬────────────────────────────────────┘
                            │ 依赖
        ┌───────────────────┴───────────────────┐
        │  libmprpc.so  RpcProvider + ZkClient  │  RPC 框架
        └───────────────────┬───────────────────┘
                            │
        ┌───────────────────┴───────────────────┐
        │  libnwl.so   (one-loop-per-thread)    │  网络库
        └───────────────────┬───────────────────┘
                            │
        ┌───────────────────┴───────────────────┐
        │  libmuduo_log.so   WT_LOG_* / LogFile │  日志
        └───────────────────────────────────────┘
        libthreadpool.so（独立，未被业务实际接入）

外部依赖：MySQL 8.4(三库) · Redis 7 · ZooKeeper 3.9 · protobuf · OpenSSL · hiredis · mysqlclient
```

**构建**：顶层是 CMake 聚合外壳，顺序敏感 `logsystem → NetWork → threadpool → mprpc`。
⚠️ `src/chatservice` **没有**被顶层 `add_subdirectory` 纳入，是独立构建（`cmake -S src/chatservice -B build/chatservice`），产物在 `src/chatservice/bin/`。这是面试官可能问的「为什么两个构建入口」。

### 1.2 一次登录的完整链路（最能体现你懂全栈）

```
Client → mprpc RpcChannel(短连接)
  组帧: [4B header_size][RpcHeader{service_name,method_name,args_size}][args(LoginRequest)]
     ↓ TCP
nwl::TcpServer（4 个 IO 线程）→ onMessage → retrieveAllAsString
     ↓
RpcProvider::onMessage 解析分帧 → 反射查 m_serviceMap → LoginRequest.ParseFromString
     ↓
service->CallMethod(method, nullptr, req, resp, done)   ← controller 传 nullptr
     ↓
AuthServiceImpl::Login
  1) 参数校验（validate::deviceId 等）→ 1001/1106
  2) cache::failLocked(account)   查 Redis login:lock → 1104
  3) classifyAccount: 含@→email / 11位数字→phone / 否则 username
  4) user_dao.findUserBy → 1102 不存在 / 1105 已软删
  5) verifyPassword（FNV-1a + 盐）→ 失败 recordFail(INCR) → 1103/1104
  6) 成功 clearFail → issueOnConn:
       upsertDevice → 多端≤5 检查(kick_oldest) → deactivateByDevice
       → genAccessToken/genRefreshToken → insertSession(MySQL)
       → updateLoginInfo → writeSession(Redis) → 回 TokenPair
     ↓
SendRpcResponse: conn->send(序列化响应) → conn->shutdown()   ← 短连接
```

**关键设计点（要能讲出"为什么"）**：
- **RPC 是短连接**（回完包 `conn->shutdown()`），会话状态不依赖 TCP，放 Redis/MySQL。
- **access_token 是随机串不是 JWT**（S0 占位），靠 Redis `tok:{token}` 反查，因此**可吊销**（比无状态 JWT 强），但也因此**无法离线验签**、**没有 exp**。
- **`Result.code` 走业务错误通道，不走 RpcController** —— 因为服务端 `CallMethod` 的 controller 传的是 `nullptr`（`src/mprpc/src/rpcprovider.cpp:153`）。所以客户端要**同时判 controller.Failed() 和 Result.code()**，两套通道。

### 1.3 三个服务各自的业务要点

| 服务 | RPC | 数据 | 亮点设计 |
|---|---|---|---|
| **Auth** | Register / Login / VerifyToken / Logout（仅 4 个） | `im_auth`: users / user_devices / sessions / audit_logs | 双 token、防爆破（5 次锁 30min）、滑动续期、多端≤5 kick_oldest、审计表 |
| **Msg** | SendMsg / PullNewMsg / GetHistory | `im_message`: messages_YYYYMM(按月) / offline_messages | **雪花 msg_id**（41bit毫秒+10bit worker+12bit seq）、**单事务双写**（历史+离线收件箱）、`FOR UPDATE SKIP LOCKED` 原子认领防并发双发、msg_id 游标分页 |
| **Social** | AddFriend / HandleFriend / ListFriends / DeleteFriend / AddBlacklist / ListBlacklist / RemoveBlacklist | `im_social`: friends / friend_requests / blacklist | 申请 3 天过期、**双护栏 UPDATE ... WHERE status=0 AND to_user=? AND expire_at>NOW()** 做 CAS 防并发、黑名单双向校验 1507/1508、软删 status=3 可复活 |

**Msg 的实时触达模型**（容易被问）：不是 push，是**客户端主动拉取**。`SendMsg` 时服务端只落库（权威历史 + 接收方离线收件箱 status=0），接收方调 `PullNewMsg` 认领（0→1）才算投递。

---

## 2. 测试岗核心：我的测试资产盘点（★本岗位最重要一节）

### 2.1 实测结果（2026-09-11 实跑，环境：ZK 2181 / MySQL 3306 / Redis 6379 全 active）

前置：清表 `TRUNCATE messages_202609, offline_messages, friend_requests, friends, blacklist`，启动三个 server，zk 上出现 `/AuthServiceRpc`、`/MsgServiceRpc`、`/SocialServiceRpc`。

| 用例集 | 结果 | 退出码 |
|---|---|---|
| `auth_cli -i conf/auth_cli.conf` | **pass=9 fail=0** | 0 |
| `msg_cli -i conf/msg_cli.conf` | **pass=13 fail=0** | 0 |
| `social_cli -i conf/social_cli.conf` | **pass=25 fail=0** | 0 |
| **合计** | **47 个用例，0 失败** | 全 0 |

覆盖的**状态机分支**（这些就是我讲的"测试设计"）：
- auth：注册+自动登录拿 token → VerifyToken → Logout → **登出后 token 失效** → 设备2 登录 → email 不存在 1102 → 错密码 1103 → 重复注册 1101 → 空设备 1001
- msg：A→B / B→A 收发 → pull(B) 得 1 条 → **再拉为空（status=1 认领）** → 双向历史 2 条倒序 → **翻页 before=最新→剩1条 / before=最早→空** → 自聊 1401 → 空文本 1001 → 非法 type 1001
- social：申请→**重复申请 1502**→自加 1506→**越权处理 1505**→接受→双向互见→**已是好友 1501**→重复处理 1503→拉黑→**被拉黑方申请 1507 / 我方申请已拉黑 1508**→移除→删好友→**删除后重加+重接受（status=3 复活）**→自清理

### 2.2 副作用落点抽查（不只看返回值，还要看数据真的对了）

```
im_message.messages_202609:     2 行（雪花 id 356668002931838976 / ...3024113664，递增）
im_message.offline_messages:    2 行 status=1 delivered_at 非空（已被 pull 认领）
im_social.friends:              2 行 status=3（social_cli 结束时自清理软删，两侧一致）
im_social.friend_requests:      2 行；blacklist: 0 行（已清理）
im_auth.users:                  password 形如 `bbc357a9$29b4d0b4458`（盐$哈希，非明文）✓
im_auth.sessions:               4 行；audit_logs: register 4 / login 8 / logout 2
Redis: sock sess:{uid}:{did}、tok:{access}、tok:{refresh}、login:fail:{account}(TTL 1679s)
```

### 2.3 我必须主动承认的测试短板（诚实是加分项）

1. **零单元测试**：全仓库无 gtest/Catch2、无 `enable_testing()`/`add_test`/CTest、无 CI。47 个用例是**端到端黑盒 CLI**，靠手写 `check(name, ok)` lambda 打 `[PASS]/[FAIL]` 并累加，`return fail==0?0:1`。**CI 感知不到失败**（没有测试目标去消费退出码）。
2. **零压测**：`src/NetWork/plan.md` M4 定的五项对照指标**全部无数据**。README 承诺的 `echo_bench_client`、`tools/chat_stress.py` **在代码中不存在**。
3. **零静态/动态分析**：plan 要求 ASan + Helgrind 双工具干净，**CMake 里连 ASan 选项都没有**，从未运行。
4. **零覆盖率**：plan M5 要求 `gcda 覆盖率 nwl ≥85%`，无工具链。
5. **不可并行、不可重复**：用例用**固定 uid**（1000001/2/3）、共享真实库；msg_cli **不自清理**，重跑前必须手动 TRUNCATE；social_cli 自清理但中断后仍需手动清。
6. **断言偏弱**：auth_cli 的「错密码」用例断言 `code==1103 || code==1104`，实际只错 1 次永远是 1103，**1104 锁定分支根本没被验证**。
7. **无 mock 点**：`DbPool::instance()` / `RedisPool::instance()` 是进程单例，dao 收裸 `MYSQL*` 无接口抽象，service 层直接取单例 → **service/dao 无法脱离真实 MySQL 做单测**。唯一天然注入点是 `service::Policy`（`session_service.h`，由 main 从 conf 装载，可把 maxFail/maxDevices 调小来覆盖边界）。

**➜ 我的改进计划（面试官问"你会怎么补"就答这个）**：
- **第一优先**：给 `common/validate.h`（纯函数）、`errcode.h::ErrText`（码→文案全覆盖）、`security/password_util`、`cache/fail_counter` 补**表驱动单测**——它们无 IO 依赖，投入产出比最高。
- **第二**：`add_test` + `enable_testing()` 把现有 3 个 cli 的退出码接进 CTest，让 CI 能红；用例改随机 uid + 自清理或 fixture teardown。
- **第三**：抽 `IRepository`/`ICache` 接口 + 构造注入，才能对 service 层做 mock 单测。
- **第四**：补 `Test_HalfFrame`（半包/粘包/超大长度/非法长度）—— 因为分帧本身就有缺陷（见 §3）。
- **第五**：参数化测试配置（conf 可被环境变量覆盖），才能并行、才能在 CI 隔离端口。

---

## 3. 我走查出的缺陷清单（★你的"测试价值"证据，也是追问防线）

按"能不能现场复现"排序。前面几条我**亲自实测复现过**。

### 🔴 已实测复现

**3.1 服务换机器/换目录启动即 core（退出码 134）**
`src/mprpc/src/rpcprovider.cpp:8` 在**命名空间作用域**构造 `wangt::LogFile logfile("/home/wangt/chat/src/logsystem/test/rpc_provide", ...)`，路径硬编码；`AppendFile` 构造里 `assert(fp_ != nullptr)`（`src/logsystem/logsys/src/AppendFile.cpp:13`）。
实测：删掉该目录后 `./bin/auth_server -i conf/auth.conf` →
```
auth_server: AppendFile.cpp:13: AppendFile::AppendFile(const std::string&):
             Assertion `fp_!=nullptr' failed.   → Aborted (134)
```
**危害**：换机器/容器/CI 直接起不来，且是**静态初始化期崩溃**，main 都进不去。NDEBUG 下 assert 消失 → `setbuffer(nullptr, ...)` 段错误，更糟。
**讲法**：「这是典型的**可移植性缺陷 + 用断言代替错误处理**。库代码不该硬编码绝对路径，也不该在构造失败时 assert——应该抛异常或返回错误码。而且这段代码的作者自己在 `:7` 写了注释说'原硬编码路径会导致断言'，结果 `:8` 还是硬编码。」

**3.2 数据库落点验证发现的密码存储问题**
实测 `im_auth.users.password = bbc357a9$29b4d0b4458` → 是 **8 位 hex 盐 + '$' + 16 位 hex**，即 **FNV-1a 64 非加密哈希 + 4 字节盐**（`security/password_util.cc:32-39,59-62`），**不是 README 声称的 bcrypt cost=10**。
**危害**：FNV 极快、无迭代无成本因子，库泄露后可 GPU 高速爆破；4 字节盐空间小。
**讲法**：诚实说这是 S0 占位（代码里自述"M2 换 bcrypt"），但**要能讲清 bcrypt 好在哪**：自带盐、可调 cost 因子、故意慢。

**3.3 Redis `sess:user:{uid}` 永久不过期**
实测 `TTL sess:user:1 = -1`、`TTL sess:user:2 = -1`。
代码里 `SADD` 之后**没有任何 TTL 设置**，而且**全仓库没有任何代码去读这个 key**（半成品）。长期运行会无限堆积陈旧 device_id。

### 🔴 源码确认（讲解时带 file:line，被追问可现场翻）

| # | 缺陷 | 位置 | 后果 |
|---|---|---|---|
| 3.4 | **AsyncLogging 丢日志**：缓冲满触发滚动时，**触发滚动的那条日志被丢**（只入队不 append）；单条 ≥4000B 必丢 | `AsyncLogging.cpp:47-59` | 与注释描述的 muduo 双缓冲设计不符。`logmsg/cli/` 下 11 个日志文件**全是 0 字节**即为实证 |
| 3.5 | **`AsyncLogging::flush()` 与后台线程并发写同一个非线程安全 LogFile**（`output_` 以 `threadSafe=false` 构造），且**持锁做磁盘 I/O** | `AsyncLogging.cpp:13,111-123` | 数据竞争，日志错乱/覆盖 |
| 3.6 | **FixedThreadPool::Stop() 必然挂死**：`stop()` 从不置 `isrunning=false`，而 `SyncQueue::Take` 在 stop 时立即返回不阻塞 → worker 纯空转死循环 → `join()` 永不返回 | `FiexdThreadPool.hpp:36-44` | 100% CPU + 析构挂死 |
| 3.7 | **CachedThreadPool 正常析构必然 `std::terminate`**：`StopThreadGroup()` 空实现不 join，析构时 `shared_ptr<thread>` 销毁 joinable 线程 | `CacheThreadPool.hpp:67-69,78-80` | 进程 abort |
| 3.8 | **SyncQueue1/3 的 Stop 是活锁**：先排空队列、**后**置停止位 → 排空期间生产者还能继续 Put → 永久阻塞 | `SyncQueue1.hpp:89-97`、`SyncQueue3.hpp:79-87` | 析构挂死 |
| 3.9 | **ScheduledThreadPool 不是周期任务**：`execute(interval,f)` 只延时执行一次，**不重新入队** | `ScheduledThreadPool.hpp:56-63` | 与类名/文档语义不符 |
| 3.10 | **定时器 clock 源不一致 + 无符号下溢**：`Timestamp::Now()` 用 `gettimeofday`(REALTIME)，而 `timerfd_create(CLOCK_MONOTONIC)`；`microsFromNow` 里 `when < now` 时 uint64 相减得 ~1.8e19（不是钳到 100），直接把 timerfd arm 到"永恒未来" | `TimerQueue.cpp:16-20,32` | **NTP 回拨/`date -s` 后所有定时器永久停摆** |
| 3.11 | **周期定时器在自身回调里 cancel 无效**：收尾重排时不复查 `canceledWhilePending_` → 定时器"复活" | `TimerQueue.cpp:122-144` | 与文件头注释"回调执行期间 cancel 同样安全"矛盾 |
| 3.12 | **`handleError` 诊断时调 `getpeername`，失败即 `abort()` 整个进程**：错误路径用 `detail::fatal` | `TcpConnection.cpp:195-199` → `SocketsOps.cpp:69-76` | 一次普通客户端 RST 就能打挂服务 |
| 3.13 | **EPOLLERR/RST 只打日志不关连接** → LT 模式空转 100% CPU + 日志风暴 | `Channel.cpp:38-49`、`TcpConnection.cpp:195-199` | 连接不回收 |
| 3.14 | **`TcpServer::~TcpServer` 在 threadNum=0（默认配置）下自死锁**：持 `connMutex_` 遍历时 `runInLoop` 内联执行 → `removeConnection` 再次加同一非递归锁 + 迭代器失效 | `TcpServer.cpp:28-33` vs `:86` | 双重缺陷；**所有已提交代码都调了 setThreadNum(4) 所以从未触发** —— 典型的"没测到" |
| 3.15 | **粘包/半包完全没处理**：`retrieveAllAsString()` 一次取走全部字节，盲目读前 4 字节 | `src/mprpc/src/rpcprovider.cpp:80-83` | 粘包丢请求；`substr(4+header_size, ...)` 在 header_size 非法时抛 `out_of_range` → **未捕获 → 进程 terminate（远程 DoS）** |
| 3.16 | **`header_size` 无 `htonl/ntohl`**：按本机字节序解析 | 同上 `:82` | 跨架构（大端客户端）错帧 |
| 3.17 | **每个 RPC 泄漏两个 protobuf Message**：`New()` 出的 request/response 从不 delete | `rpcprovider.cpp:134,141,164-177` | 压测会放大 |
| 3.18 | **客户端 `rpc_client.h` 固定 8KB 单次 recv**：PullNewMsg 上限 100 条 × content 5000B → 响应可达数百 KB → **静默截断** | `base/rpc_client.h:96-97` | 真实 bug 被伪装成 `parse response error`，**测试信号失真** |
| 3.19 | **好友申请可并发重复插入（TOCTOU）**：5 步前置检查与 INSERT **不在同一事务、无锁**，`friend_requests` **无唯一键兜底** | `social_service.cc:21-35`、`db/init.sql:311-328` | 高并发下同向重复 pending |
| 3.20 | **DeleteFriend 非原子**：两次 `softDeleteFriend` 用同一连接但**未开事务** → 可能只删单侧 | `social_service.cc:80-87` | A 列表有 B、B 列表没 A |
| 3.21 | **发送路径缺事务回滚保证 / claim 是 at-most-once**：`claimOffline` 先置 `status=1` 再回读，**若回读前进程崩溃，消息永久丢失**（无 ack/重投） | `offline_dao.cc:70-96` | 丢消息 |
| 3.22 | **无幂等键**：客户端超时重试 → 新雪花 id → 同一内容落两条 | `message_service.cc:35` | 重复消息 |
| 3.23 | **跨月读不到历史**：`currentMonthTable()` 只返回当月表，`init.sql` 只建了 `messages_202609` → 2026-10 起**所有发送直接 1002 失败** | `message_dao.cc:25-33` | 高危运维缺陷，无自动建表任务 |
| 3.24 | **防爆破可被大小写绕过**：计数 key `login:fail:{account}` 用**原始输入串不归一化**，而 MySQL username 是 `utf8mb4_unicode_ci` 大小写不敏感 → `Alice`/`alice` 是不同 key，交替使用即绕过 5 次锁定 | `session_service.cc:97,114` + `fail_counter.cc:8-13` | 防爆破实质性绕过 |
| 3.25 | **登出可静默失效**：`deleteSession` 返回值被忽略，Redis 故障时 DB 已 status=0 但 `tok:` 还在 → VerifyToken 仍 valid，**而接口返回 OK** | `session_service.cc:181` | 安全语义错误 |
| 3.26 | **连接池持锁做网络 I/O**：`acquire()` 持 `mu_` 调 `mysql_real_connect` / `mysql_ping`；redis 持锁 `PING`/`redisConnect` | `db_pool.cc:60-89`、`redis_pool.cc:67-99` | 高并发下池成串行瓶颈；且**未设任何连接/读写超时**，锁可被无限期占住 |
| 3.27 | **`timeoutMs` 配置形同虚设**：声明了但 `init` 从不保存，`acquire` 硬编码 5s | `db_pool.h:19-22` vs `db_pool.cc:36-61` | 配置与实际行为脱节 |
| 3.28 | **`Txn::commit()` 吞掉失败**：不检查 `txCommit` 返回值就置 `active=false`，析构不再 rollback | `sqlutil.h:50-55` | 连接带未提交事务还池 → 后续请求脏读/持锁 |
| 3.29 | **鉴权缺失（S0 已知）**：请求体自带 `from_user`/`user_id`，服务端**信任调用方 uid**，无 token 校验 | `msg.proto:5`、`msgservice_impl.cc` | 任何能连上端口者可伪造任意 uid |
| 3.30 | **黑名单不拦截消息**：msg 与 social 是两个进程两个库，发送链路无 `isBlocked` 校验 | `message_service.cc:20-55` | A 拉黑 B 后 B 仍能发消息 |
| 3.31 | **无事务（auth）**：`issueOnConn` 的 5-6 条独立自动提交语句，中途失败无法回滚；`sqlutil.h` 提供了 `Txn` RAII 但 **authservice 零处引用** | `session_service.cc:43-76` | "孤儿会话"占用多端名额 |
| 3.32 | **文档-代码不一致（本身就是质量问题）**：根 README 末尾大量"未包含 chatsystem"，但 `src/chatservice/` 已落地三个服务；可用性指标 99.99%(§1.1) / 99.98%(§7.1) / 99.999%(§9.2) 三个口径并存；服务发现文档写 Consul 实际用 ZooKeeper | README.md / plan.md | 可直接作为"文档一致性质量管理"切入 |

---

## 4. 面试官大概率追问的技术点（准备好答案）

### 4.1 网络库（WXG 最爱问）

**Q：为什么 one-loop-per-thread，而不是一个 loop 处理所有连接？**
A：单 loop 里业务回调会阻塞所有连接；多线程共享一个 epoll 需要加锁且唤醒路径复杂（thundering herd）。one-loop-per-thread 让每个连接的所有事件都在**同一线程**内串行处理，**连接级别天然无锁**；跨线程操作只能经 `runInLoop/queueInLoop` 投递，用 eventfd 唤醒。

**Q：跨线程投递为什么必须唤醒？**
A：`queueInLoop` 把任务塞进 `pendingFunctors_`，但目标线程此时可能正阻塞在 `epoll_wait`（超时 10s）。必须写 eventfd 让 epoll 立即返回。
**加分点**：`EventLoop.cpp:97` 的判断是 `if (!isInLoopThread() || callingPendingFunctors_)` —— 第二个条件很关键：**本线程**调用但此刻正在执行 `doPendingFunctors`，本轮 poll 已经过了，新任务的唤醒不会被"顺带"处理，必须自己写 eventfd，否则**丢唤醒**。

**Q：`doPendingFunctors` 为什么锁内 swap 再锁外执行？**
A：防止回调里再 `queueInLoop` 导致**自死锁**（同一把锁重入）。swap 出本地副本后立刻解锁，回调执行期间生产者的 `queueInLoop` 不会阻塞。
**诚实补充**：`callingPendingFunctors_` 的置位/清位没有 RAII 兜底，回调抛异常会让标志永久为 true（功能不坏但语义脏），异常继续逃出 `loop()` → `std::terminate`。

**Q：生命周期怎么保证不 UAF？**（必问，这是 Reactor 的核心）
A：**三重保险**：
1. `TcpConnection : enable_shared_from_this`，`getSelf()` 在 `connectEstablished`/`send`/`shutdown` 里把 shared_ptr 提升到调用栈；
2. `Channel::tie(weak_ptr)` —— `handleEvent` 入口 `lock()` 得到**局部强引用**，覆盖整个事件处理，保证 in-flight 事件期间宿主不被析构；`tie_` 只存 weak 所以不会循环引用；
3. `closeCallback → TcpServer::removeConnection` 按值捕获 shared_ptr 延续到 `connectDestroyed` 返回。
**诚实补充**：`Channel::~Channel()` 不会自己从 Poller 注销，若有路径让 `connectDestroyed` 没跑完，Poller 的 `channels_` 会留悬垂 `Channel*`；fd 号被复用后 `EPOLL_CTL_ADD` 返回 EEXIST，而 `update()` **只打日志不降级为 MOD** → 新连接静默收不到事件。

**Q：Buffer 为什么要 prepend 区？readFd 为什么要 64KB 栈上备用块？**
A：8 字节 prepend 区为了往回填长度前缀（**但实际全代码库无调用点，是死代码**，分帧走的是 read 路径）。readFd 用 `readv` + 栈上 64KB extrabuf：当 Buffer 可写区不够时，第二次 iovec 直接读进栈缓冲再 append，**一次系统调用读尽可能多**，避免"读满→read 一次→扩容→再 read"。
**诚实补充**：Buffer **完全没有收缩逻辑**，`retrieveAll` 只复位索引，`vector` capacity 永不归还 —— 峰值 64MB 的连接会永久占 64MB。这直接冲击 plan 的"万连接内存 ≤ muduo 110%"指标。

**Q：EMFILE 怎么处理？**
A：`Acceptor` 在构造时预开一个 `/dev/null` 的 fd 作"席位"（`idleFd_`）。fd 耗尽时 `accept` 返回 EMFILE，此时 **close 掉 idleFd_ → accept 拿到新连接 → 立刻 close → 再 open("/dev/null") 补回席位**。这样避免"监听队列一直可读但 accept 一直失败"导致的 LT 空转风暴。
**诚实补充**：`::open` 的返回值没检查，失败则 `idleFd_ = -1`，之后会 `close(-1)`，兜底能力失效。

### 4.2 RPC / 协议

**Q：你的 RPC 协议长什么样？**
A：`TCP 载荷 = [4B header_size][RpcHeader(protobuf: service_name/method_name/args_size)][args(protobuf)]`。服务端反射查 `m_serviceMap` 找 `Service*` 和 `MethodDescriptor*`，`New()` 出 request 反序列化，`NewCallback` 生成 done，`service->CallMethod(...)`。
**必须主动承认的坑**：① `header_size` 无 `htonl/ntohl`；② **没有分帧循环**，粘包只解析第一个请求、半包会读到垃圾长度值；③ 非法 header_size 触发 `substr` 的 `out_of_range` 且**无人捕获** → 进程 terminate，是个远程 DoS。

**Q：服务端 controller 传 nullptr 有什么问题？**
A：业务实现拿不到"调用方是谁"，也无法通过 `SetFailed` 回传传输层错误。这是 mprpc 的已知缺口（`rpcprovider.cpp:153`），文档里列为 M4 改造项：给 `RpcHeader` 加 token/request_id 字段 + 服务端注入携带 token 的 controller。当前折衷是在业务 proto 里包一层 `RequestMeta{token, request_id}`。

### 4.3 并发与数据一致性

**Q：`FOR UPDATE SKIP LOCKED` 为什么能防并发双发？**
A：多个短连接并发 `PullNewMsg` 时，事务 A 的 `SELECT ... FOR UPDATE` 锁住候选行，事务 B 执行到同一行时**不阻塞而是跳过（SKIP LOCKED）**，继续拿后面的行。这样两个 pull 拿到**不相交**的行集，各自 `UPDATE status=1` 后 COMMIT。避免了"读到同一批 → 都标记已拉 → 同一消息发两次"。
**诚实补充**：这是 **at-most-once** 语义 —— 先 UPDATE 置 status=1 再回读内容，**若回读前进程崩溃，消息永久丢失**（没有 ack/重投机制）。真正要可靠得做 at-least-once + 客户端去重。

**Q：好友申请怎么防并发重复？**
A：接受路径靠 `UPDATE ... WHERE id=? AND status=0 AND to_user=? AND expire_at>NOW()` 的**影响行数 ==0 即判定并发抢先**并回滚——这是把判断下推到 SQL 做 CAS，是正确的。
**但申请路径有洞**：5 步前置检查（isFriend / hasPendingRequest / isBlocked）与 INSERT **不在同一事务、无锁**，且 `friend_requests` **没有唯一键兜底**，两个并发 AddFriend 可同时通过检查 → 插两条 pending。**这是我把"读到代码→推出场景→给出修法"完整讲出来的例子**：修法是给 `(from_user, to_user, status)` 加唯一键，或把检查+插入放进同一事务加 `SELECT ... FOR UPDATE`。
（顺带：拒绝路径 `rejectRequest` 不检查 affected rows，并发下可能"提示拒绝成功但实际已被接受"——吞掉并发失败。）

### 4.4 雪花算法

**Q：讲讲你的雪花 ID 布局，时钟回拨怎么办？**
A：`41bit 毫秒(epoch 2024-01-01) | 10bit worker | 12bit seq`，`std::mutex` 短临界。同毫秒 seq 耗尽（4096）则 sleep 等下一毫秒。时钟回拨用 `while (ts < lastMs_) sleep_for(1ms)` **自旋等追平**。
**诚实补充**：① worker id 是**手工传的**，默认 1（`main.cpp` 里 `init(1)`），**无自动分配**，多实例部署会同毫秒撞 ID（文档写"从 Consul 获取"但没实现）；② 回拨时**持锁 sleep**，阻塞所有取号线程，无告警无备用序列；③ 未处理系统时钟早于 epoch 时的 uint64 下溢。
**为什么用雪花而不是 DB 自增**：应用层出号，不依赖 DB 往返，且**msg_id 单调递增可直接当分页游标**（`before_id`），无需额外翻主键。

---

## 5. 危险区：不能吹的地方 & 怎么答

| 如果面试官问… | 千万别说… | 应该说 |
|---|---|---|
| 「QPS 多少？压测过吗？」 | 别说有数据 | 「**没压测**。plan 里定了五项对照 muduo 的指标（QPS≥95%、p50≤120%、p99≤150%、内存≤110%、体积≤70%），但 `echo_bench_client` 和压测脚本都还没落地，所以**一个数都没有**。这是我下一步优先级最高的。」 |
| 「有单元测试吗？」 | 别说"有 47 个测试"就完 | 「有 **47 个端到端黑盒用例**全绿，但**单元测试是零**，没有 gtest 也没有 CTest 接住退出码 —— CI 感知不到失败。我知道这是个明显短板。」 |
| 「生产级可用吗？」 | 别吹 | 「**不是**。S0 骨架：密码是 FNV 占位不是 bcrypt、token 是随机串不是 JWT、鉴权信任调用方 uid、RPC 明文无 TLS、分帧没处理粘包。**这些都在 README 里如实标了遗留项。**」 |
| 「业务功能都实现了吗？」 | 别把 plan 当已实现 | 「auth 的 proto 定义了 10 个接口但**只实现了 4 个**（Register/Login/VerifyToken/Logout），RefreshToken/改密/设备管理都是后置。群聊、撤回、已读回执、消息推送**都没做**，表建了但代码没接。」 |
| 「threadpool 用在哪？」 | 别硬套 | 「**实际上没被业务接入** —— 库编译出来了但三个服务都没用它。README 里设计是拿它跑 bcrypt（50-100ms 的 CPU 活）避免堵 nwl 事件线程，但没落地，哈希现在就是在 RPC 事件线程同步跑的。」 |
| 「为什么两个构建入口？」 | — | 「`src/chatservice` 是新加的子模块，**还没被顶层 `add_subdirectory` 纳入**，目前独立构建。README 也还没同步更新，这是文档滞后于代码。」 |

**核心策略**：**主动承认短板，并把短板转成"我知道怎么补"**。测试岗最怕的不是项目有缺陷，而是**你不知道有缺陷**。上面 §3 那 30 多条缺陷就是你的弹药 —— 说明你有系统的质量视角。

---

## 6. 高频行为题准备

**Q：你为什么想来测试岗？/ 你理解的测试是什么？**
结合项目答：我在写这个项目时最深的体会是「**能跑」和「正确」是两回事**。比如 47 个用例全绿，但我仍然走查出了 30 多个缺陷 —— 因为用例只覆盖了**正向主路径和显式错误码**，没覆盖粘包、时钟回拨、并发竞态、RST 这些**边界与异常路径**。测试的价值就是把这些"你以为不会发生"的场景变成可复现的用例。质量管理更进一步：让这些用例**自动化、进 CI、有门禁**，而不是靠人记得跑。

**Q：如果让你现在接手这个项目的质量，你第一周做什么？**
1. **止血**：把 `AppendFile` 的 assert 改错误处理、`getpeername` 的 abort 改返回错误 —— 这两个是**能直接打挂进程**的。
2. **建基线**：3 个 cli 接进 CTest + `enable_testing()`，让现有 47 个用例有 CI 门禁。
3. **补最值钱的单测**：`validate`/`errctext`/`password_util`/`fail_counter` 这些纯函数，表驱动，成本最低覆盖最快。
4. **立边界用例清单**：粘包/半包、RST 断开、定时器时钟回拨、并发注册/并发申请、超长字段、SQL 注入 payload。
5. **对齐指标**：把 plan 的 M4 五项指标写成可自动跑的基准脚本（这需要一个 `echo_bench_client`）。

**Q：你怎么定位一个 bug？举个例子。**
用 3.1 讲：现象是「换个目录服务起不来，退出码 134」。134 = 128+6 = SIGABRT，说明是 assert 或 abort 而非普通异常。看栈顶定位到 `AppendFile` 构造函数，进而发现是 `rpcprovider.cpp:8` 的**静态初始化期**构造 `LogFile` + 硬编码路径。**根因不是"路径写错了"，而是两个设计问题**：库代码硬编码绝对路径，以及用 assert 处理"可预期的运行环境问题"。修法：路径走配置 + 构造失败抛异常/返回错误码。

---

## 7. 面试前一小时 checklist

- [ ] 能白板画出 §1.1 的四层架构图 + §1.2 的登录链路
- [ ] 能说出「47 个 E2E 用例全绿」以及各服务的覆盖分支
- [ ] 能讲 3 个**亲自复现**的缺陷（3.1 core、3.2 密码哈希、3.3 Redis 无 TTL）
- [ ] 准备好 §5 危险区的诚实答法 —— **别吹压测、别吹单测**
- [ ] `FOR UPDATE SKIP LOCKED` / one-loop-per-thread / `tie` 三重生命周期 / 雪花布局 四个技术点能讲透
- [ ] 记住"为什么用雪花当游标"、"为什么 access 用 JWT 而 refresh 用随机串"（README §5.2 的设计论证 —— 实际没实现 JWT，但**设计理由要能讲**）

---

## 附：需要你自己复核的点

以下来自静态走查，我**没有逐一动态复现**，被追问时建议先翻代码确认：
- §3.6–3.11 线程池与定时器的挂死/活锁路径（结论有源码支撑，但未写复现程序）
- §3.24 防爆破大小写绕过（依赖 MySQL collation，理论上成立，未构造攻击用例）
- §3.14 `~TcpServer` 死锁（需 threadNum=0，所有现有代码都设了 4，故未触发）
