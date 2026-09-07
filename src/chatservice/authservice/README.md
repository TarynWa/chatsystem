# AuthService — 认证服务设计文档

> `chatservice` 是分布式 IM 的**业务服务层仓库**（承载 AuthService / MsgService / SocialService），
> 依赖底座 `logsystem / NetWork(nwl) / threadpool / mprpc` 四个子模块。本文档先落地其中的**认证服务（AuthService）**。
>
> 关联文档：
> - `../../plan.md` — 分布式 IM 整体架构方案（本服务的需求来源，见 §4.1）
> - `../../db/README.md` — 数据库设计方案（`im_auth` 库结构依据，见 §2）
> - `../../db/init.sql` / `../../db/init_db.py` — 数据库初始化脚本（已交付）
> - `../../README.md` — 仓库整体说明
> - `../mprpc` `../NetWork` `../logsystem` `../threadpool` — 底座模块

---

## 1. 定位与目标

**认证服务**是分布式 IM 里第一个业务微服务，负责「账号体系 + 登录会话 + 安全控制」，被网关及其它服务以 RPC 方式调用。

```
功能清单（对齐 plan.md §4.1）
┌──────────────────────────────────────────────────────────────┐
│  1. 用户管理   注册 / 登录 / 注销 / 资料修改(昵称头像密码等)  │
│  2. 会话管理   access_token + refresh_token / 多端≤5台 / 踢人 │
│                / 7天无操作自动过期                            │
│  3. 安全控制   bcrypt(cost=10) / 防暴力破解(5次锁30min)        │
│                / 登录与操作审计(audit_logs)                   │
└──────────────────────────────────────────────────────────────┘
```

**非目标（本期不实现）**：主界面聊天/社交业务与长连接网关、WebSocket、推送、跨服务 RPC/MQ 编排。登录/注册阶段客户端仍走 mprpc **短 RPC**（阶段一折衷：底座尚无网关/长连接/库内 RpcChannel，见 §2 缺口与 §12）。

### 1.1 客户端接入形态（先登录/注册，后 RPC+MQ）

整体访问链路（对齐 plan.md §2 的落地形态）：

```
客户端 (App / CLI / Web)
   │  ① 通过网络连接服务器 —— 先进「登录 / 注册」页
   ▼
接入 / 网关层                 ← 阶段一(本期): 登录注册入口直接走 mprpc 短 RPC(复用 rpc_cli 通路)
   │                           ← 阶段二(后续): 主界面 WebSocket 长连接(接入层工作, 本期无)
   ▼
AuthService (认证)
   │  登录/注册成功 → 下发 session(access_token + refresh_token)
   ▼
主界面业务                      ← 阶段二(后续): 聊天/社交等经 RPC(同步) + MQ(异步) 调用 Msg/Social
                                     都以本阶段下发的 token 作为鉴权凭证
```

**本期只做到「客户端 → 登录/注册 → 拿到会话凭证」这一门槛**（§8 业务梳理）；进入主页面之后的 RPC/MQ 编排属于后续，但结构上依赖本期产出的凭证体系。

---

## 2. 现状基线（能复用 vs 还缺什么）

写代码前先对齐底座真实能力，避免把文档写成空中楼阁。

| 底座模块 | 现状 | 认证服务如何用 |
|---|---|---|
| `mprpc` (Provider) | ✅ RPC 服务端 + ZooKeeper 注册已跑通（`rpc_ser/rpc_cli` 实测），网络层在自研 nwl 上 | 承载 `AuthServiceRpc` 全部接口 |
| `logsystem` | ✅ `wangt::AsyncLogging` / `WT_LOG_*` | 服务运行日志、审计旁路日志 |
| `threadpool` | ✅ 纯头文件 Fixed/Cached/**WorkStealing** 等 | bcrypt(cost=10) 是 ~50-100ms 的 CPU 活，投线程池跑，别堵 nwl 事件线程 |
| `NetWork(nwl)` | ✅ 服务端 TcpServer（mprpc 内部已用） | 无需直接使用 |
| `mprpc` (Client) | 🚧 **库内无 RpcChannel/服务发现**；demo 客户端内联在 `test/rpc_cli.cpp` | 后续补库内 RpcChannel；见 §12/§13 |

**底座缺口（做认证服务会踩到，需同步规划）**
1. RPC 协议头 `RpcHeader` 只有 `service_name / method_name / args_size`，**没有承载 token / request_id 的字段**（`src/mprpc/src/rpcheader.proto`）；服务端 `service->CallMethod(method, nullptr, …)` 传的是 `nullptr` controller（`src/mprpc/src/rpcprovider.cpp:153`），RPC 实现拿不到“调用方是谁”。
   → **暂态折衷**：不动 mprpc，在业务 proto 里包一层 `RequestHeader{token, request_id}`，token 随 body 走；**阶段 M4** 再把它下放进 RpcHeader（§13）。
2. 当前 provider 是**短连接**模型（回完包即 `conn->shutdown()`，见 `SendRpcResponse`）。对认证服务这不是问题——会话状态放 Redis/MySQL，不依赖 TCP 连接；但要写进代码约束里，别用连接存状态。
3. 底座服务发现目前是 **ZooKeeper**；plan 里写的是 Consul（远期目标）。认证服务的结构不感知这点，mprpc 内部换注册中心即可，本文档不展开。

---

## 3. 总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         调用方 (Client / Gateway / 其它服务)      │
│              通过 mprpc Client 按 zk 上注册的地址调用              │
└──────────────────────────────┬──────────────────────────────────┘
                               │ RPC (protobuf, 短连接, 暂态)
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                     AuthService  (auth_server)                   │
│                                                                 │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │  AuthServiceImpl  (RPC 实现层: 参数校验/协议编解码/回包) │   │
│   └──────────────┬──────────────────────┬──────────────────┘   │
│                  │ 调用                  │ 调用                  │
│         ┌────────▼────────┐     ┌───────▼─────────┐            │
│         │  service 领域层   │     │   registry(走   │            │
│         │ UserService       │     │   mprpc Run()   │            │
│         │ SessionService    │     │   自动注册 zk)  │            │
│         └──┬─────────┬─────┘     └─────────────────┘            │
│   ┌───────▼──┐  ┌───▼────────┐                                 │
│   │ security │  │ dao / cache │                                 │
│   │ Hasher   │  │  MySql 池   │                                 │
│   │ Jwt      │  │  Redis 池   │                                 │
│   │ Guard    │  │  DAO/缓存   │                                 │
│   └───┬──────┘  └─┬────┬─────┘                                 │
└───────┼───────────┼────┼────────────────────────────────────────┘
        │           │    │
        ▼           ▼    ▼
 ┌────────────┐ ┌──────┐ ┌────┐   ┌─────────────┐
 │ MySQL      │ │ Redis│ │ zk │   │ 日志         │
 │ im_auth    │ │ 会话 │ │    │   │ AsyncLogging │
 │ 4张表      │ │ 计数 │ │    │   │ + audit_logs │
 └────────────┘ └──────┘ └────┘   └─────────────┘
```

依赖方向（单向，禁止反向）：
`rpc(AuthServiceImpl)` → `service` → `dao / cache` / `security`；`model` 是各层间的纯结构体契约（不引 protobuf）。

---

## 4. 功能设计：RPC 接口

### 4.1 接口总览

| # | 方法 | 说明 | 归属 |
|---|---|---|---|
| 1 | `Register` | 注册（用户名唯一；可选邮箱/手机） | 用户管理 |
| 2 | `Login` | 登录（账号+密码+设备信息）→ 下发双 token + 用户信息 | 用户/会话 |
| 3 | `Logout` | 登出（按 token 或 device_id 注销，端上状态清除） | 会话管理 |
| 4 | `RefreshToken` | refresh_token 换新双 token（防重放，轮换制） | 会话管理 |
| 5 | `GetUserInfo` | 查询用户公开资料（按 user_id / username） | 用户管理 |
| 6 | `UpdateUserInfo` | 改昵称/头像/性别/生日/邮箱/手机（邮箱/手机需二次校验，见 §15） | 用户管理 |
| 7 | `ChangePassword` | 改密：验旧密 → bcrypt 新密 → 吊销该用户全部会话 | 用户管理 |
| 8 | `ListDevices` | 查看当前账号已登录设备 | 会话管理 |
| 9 | `KickDevice` | 强制某设备下线（本人/管理员） | 会话管理 |
| 10 | `VerifyToken` | 供网关/其它服务校验 access_token → 返回 uid/did（高频，查本地缓存+Redis） | 会话管理 |

### 4.2 proto 草案（暂态：token 放 body）

```proto
syntax = "proto3";
package chatservice;
option cc_generic_services = true;   // 与现有 user.proto 风格一致，走反射派发

// 通用回包结果
message Result { int32 code = 1; string msg = 2; }   // code 见 common/errcode.h

// —— 所有需要鉴权的请求都携带的头部(暂态折衷, 见 §2 缺口1) ——
message RequestMeta { bytes token = 1; string request_id = 2; }

message RegisterRequest  { string username=1; string password=2; string nickname=3;
                           string email=4; string phone=5; }
message RegisterResponse { Result result=1; uint64 user_id=2; }

message DeviceInfo { string device_id=1; string device_name=2;
                     int32  device_type=3; string os_version=4; string app_version=5; }

message LoginRequest { string account=1;   // username/email/phone 三选一
                       string password=2; DeviceInfo device=3;
                       string ip=4; string user_agent=5; }
message TokenPair { bytes access_token=1; uint32 access_expires_in=2;
                    bytes refresh_token=3; uint32 refresh_expires_in=4; }
message LoginResponse { Result result=1; TokenPair tokens=2;
                        UserBrief user=3; bool reach_device_limit=4; }

message LogoutRequest { RequestMeta meta=1; optional string device_id=2; } // 缺省=登当前
message RefreshTokenRequest { bytes refresh_token=1; DeviceInfo device=2; }
message RefreshTokenResponse { Result result=1; TokenPair tokens=2; }

message UserBrief { uint64 user_id=1; string username=2; string nickname=3;
                    string avatar=4; int32 status=5; }
message GetUserInfoRequest { RequestMeta meta=1; uint64 user_id=2; }
message GetUserInfoResponse { Result result=1; UserBrief user=2; }

message UpdateUserInfoRequest { RequestMeta meta=1; optional string nickname=2;
    optional string avatar=3; optional int32 gender=4; optional string birthday=5; }
message UpdateUserInfoResponse { Result result=1; }

message ChangePasswordRequest { RequestMeta meta=1; string old_password=2;
                                string new_password=3; }
message ChangePasswordResponse { Result result=1; }

message DeviceView { DeviceInfo device=1; int32 online_status=2; int64 last_active_at=3; }
message ListDevicesRequest { RequestMeta meta=1; }
message ListDevicesResponse { Result result=1; repeated DeviceView devices=2; }
message KickDeviceRequest { RequestMeta meta=1; string device_id=2; }
message KickDeviceResponse { Result result=1; }

message VerifyTokenRequest { bytes token=1; }
message VerifyTokenResponse { Result result=1; uint64 user_id=2;
                              string device_id=3; bool valid=4; }

service AuthServiceRpc {
  rpc Register(RegisterRequest) returns (RegisterResponse);
  rpc Login(LoginRequest) returns (LoginResponse);
  rpc Logout(LogoutRequest) returns (LogoutResponse);
  rpc RefreshToken(RefreshTokenRequest) returns (RefreshTokenResponse);
  rpc GetUserInfo(GetUserInfoRequest) returns (GetUserInfoResponse);
  rpc UpdateUserInfo(UpdateUserInfoRequest) returns (UpdateUserInfoResponse);
  rpc ChangePassword(ChangePasswordRequest) returns (ChangePasswordResponse);
  rpc ListDevices(ListDevicesRequest) returns (ListDevicesResponse);
  rpc KickDevice(KickDeviceRequest) returns (KickDeviceResponse);
  rpc VerifyToken(VerifyTokenRequest) returns (VerifyTokenResponse);
}
```

> `common.proto` 与各消息体单独成文件；`auth.proto` 只管服务。字段号按表递增、预留扩展位。

### 4.3 接口裁剪（本期登录/注册 → 后续主页面）

| 接口 | 本期 | 理由 |
|---|---|---|
| `Register` | ✅ 必做 | 注册页入口 |
| `Login` | ✅ 必做 | 登录页入口 |
| `RefreshToken` | ✅ 最小版 | access 过期续期，支撑“进入主界面后”不被频繁踢下线 |
| `Logout` | ✅ 最小版 | 关闭当前会话，保证登录/注册闭环可退 |
| `VerifyToken` | 🔵 预研版 | 供 auth_cli 自检 / 后续网关逐请求校验，本期先按 §8.3 简单实现 |
| `GetUserInfo` | 🔵 最小版 | 登录后展示“我”的资料（可并入 Login 返回，本期可不单开） |
| `UpdateUserInfo` / `ChangePassword` / `ListDevices` / `KickDevice` | ⛔ 后置 | 主界面“设置/账号/多端”页的事，登录/注册不需要 |

> 划分原则：**先让「注册→登录→进主界面」这条用户主路径闭环**，其余接口定义保留、实现后置。

---

## 5. 关键技术设计

### 5.1 密码存储：bcrypt

- 注册时 `bcrypt(password, salt, cost=10)`，**只存最终哈希串**，不存明文/不存盐（盐嵌在哈希串里）。
- 校验用 `bcrypt_check(明文, 存储哈希)`，内部做**常数时间比较**防时序攻击。
- 实现：内嵌单文件 bcrypt 参考实现 + 自研 `security/PasswordHasher` 薄封装（选型见 §15）；哈希计算投 **WorkStealing 线程池**，避免阻塞 RPC 事件线程。

### 5.2 认证与会话模型

双 token + Redis 会话 + 本地读缓存，各端独立会话。

| token | 形态 | 有效期 | 存哪 | 作用 |
|---|---|---|---|---|
| `access_token` | JWT HS256，claims: `uid/did/typ=access/iat/exp` | 2h | 无状态可验签；Redis 存会话存在性 | 鉴权 |
| `refresh_token` | 32B 随机(OpenSSL `RAND_bytes`)，带随机性，**不可伪造、可吊销** | 7d，滑动续期 | MySQL `sessions`(权威) + Redis | 换新双 token |

**为什么 access 用 JWT、refresh 用随机串**：access 高频校验（网关/服务间），本地验签即可（毫秒级、无 IO）；refresh 低频、但一旦泄露危害大，用随机串 + 服务端吊销，杜绝“签名无法吊销”问题。

**会话生命周期（状态机）**
```
 登录 ──► ACTIVE ──access 过期──► (本地校验失败) ──带 refresh 调 RefreshToken──► ACTIVE
  ▲         │                                                              │
  └─────────┴── 登出 / 改密 / 踢人 / 7天无操作滑动过期 ──► REVOKED ──► 清理
```
- **7 天无操作过期**：每次合法访问对 Redis `sess:{uid}:{did}` 做 TTL 重置（滑动）；刷新时同步 `sessions.expires_at = now+7d`。
- **撤销即时生效**：登出/踢人/改密 = 删 Redis 会话 + 置 MySQL `sessions.status=0`（过期行由清理任务定期删，见 db/README §7.2 清理2）。
- **多端 ≤ 5**：`user_devices` 为权威（按 user_id 统计有效设备）；Redis `sess:user:{uid}`（set 存 dev_id）维护“在线”枚举用于快速判断。超限策略**可配**，默认“挤掉最久未活跃设备”。
- **本地读缓存**：`VerifyToken` 高频（网关逐请求调用），JWT 验签后在本地小 LRU（TTL 5min）缓存 `token → uid/did`，规避每请求查 Redis；撤销时通过 `cache.invalidate` 类信号失效（当前无 MQ，先退化为“最多 5min 生效”，接入 MQ 后改订阅，见 roadmap）。

### 5.3 防暴力破解（BruteForceGuard）

```
Redis key: login:fail:{account}   (INCR + EXPIRE)
规则: 连续失败 5 次 → 锁定 30 分钟(用 TTL 表达)
命中锁定后即使密码正确也拒绝(1104), 锁定到期自动解除
登录成功 → DEL 该 key
```
分布式下天然用 Redis 计数；多实例共享同一计数。登录成功/失败都写审计（§5.4）。

### 5.4 审计与操作日志

- 敏感操作（login/logout/register/change_password/kick）双写：`wangt` 日志（开发排查）+ `audit_logs` 表（安全溯源，含 ip/ua/action/detail JSON，结构见 db/README §2.4）。
- `audit_logs` 写入放**异步线程**（不阻塞主请求）；表按月分区/定期归档策略见 db/README §7，落地时补 DDL。

### 5.5 数据访问层（MySQL）

- `dao/` 全部基于**预处理语句**（`mysql_stmt_*`）防注入，输入一律参数绑定。
- `DbPool`：自研连接池封装 `mysqlclient` C API——初始化 `min/max` 连接、借还、空闲保活（`mysql_ping`）、取连接超时。**连接只属于单次请求，绝不在请求间共享**（响应后即还池）。
- 每层返回 `model/` 纯结构体；`Result` 里带回错码。

### 5.6 Redis 访问层与 key 约定

`cache/` 封装 `hiredis` 连接池（min/max、重连），上层只管语义。key 约定：

| key | 类型 | TTL | 说明 |
|---|---|---|---|
| `sess:{uid}:{did}` | string(JWT access) | 滑动，≤7d | 会话“活着”的证据，撤销=删 |
| `sess:user:{uid}` | set(dev_id) | 不设 | 在线设备枚举/踢人遍历 |
| `login:fail:{account}` | counter | 命中锁 30min | 防爆破 |
| `auth:user:{uid}` | string(user brief json) | 5min~1d | 用户资料读缓存（改资料时删/更） |

### 5.7 错误码规范（`common/errcode.h`）

| 段 | 含义 | 取值 |
|---|---|---|
| 0 | 成功 | `OK = 0` |
| 1xxx | 公共 | 1001 参数非法 / 1002 服务繁忙 / 1003 操作频繁 |
| 11xx | 用户 | 1101 用户名已存在 / 1102 账号不存在 / 1103 密码错误 / 1104 账号锁定 / 1105 用户已删除 / 1106 用户名或密码格式非法 |
| 12xx | 会话 | 1201 token 无效或已吊销 / 1202 access 过期 / 1203 refresh 无效或已使用 / 1204 设备数超上限 / 1205 会话不存在 / 1206 非本人操作 |
| 13xx | 权限 | 1301 无权限(非本人/非管理员) |

---

## 6. 数据模型（im_auth）

服务只访问自己的域库 `im_auth`（库表由 `../../db/init.sql` 初始化，已可执行）。职责映射：

| 表 | 用途 | 主要操作方 | 对应 db/README |
|---|---|---|---|
| `users` | 账号/资料/状态/软删除 | UserDao | §2.1 |
| `user_devices` | 多端设备、推送 token、在线状态 | DeviceDao + 会话逻辑 | §2.2 |
| `sessions` | refresh_token/会话过期/吊销记录（权威兜底） | SessionDao | §2.3 |
| `audit_logs` | 登录与敏感操作审计 | AuditDao | §2.4 |

一致性口径：**MySQL 是权威**，Redis 是加速层；写路径先落库再写缓存，读路径缓存 miss 回源库。允许短暂不一致（本地缓存 5min / Redis 兜底），最终一致由清理任务收敛。

---

## 7. 目录结构

> **现状（S0 已落地）**：`proto/`、`common/`、`authservice/` 已按本节实现（独立构建产出 `bin/auth_server`、`bin/auth_cli`，配置 `conf/auth.conf`、`conf/auth_cli.conf`）。目录树中标注「占位/后置」的项与 `msgservice/`、`socialservice/` 仍为目标结构，随里程碑逐目录新增。本文档现位于 `authservice/` 下（与代码同目录）。

```
src/chatservice/
├── CMakeLists.txt                 # 独立构建: chatauth_proto → chatauth_core → bin/*(S0)
├── README.md                      # 本文件(现位于 authservice/)
├── conf/
│   ├── auth.conf                  # 认证服务运行配置(见 §11)
│   └── auth_cli.conf              # 测试客户端配置(直连 rpcserver, 免 zk)
├── proto/                         # 业务 proto 根(跨服务共享)
│   ├── auth.proto                 # AuthServiceRpc(Register/Login/VerifyToken/Logout, S0)
│   ├── common.proto               # 占位(后续拆分)
│   ├── msg.proto                  # 占位(后续 MsgService)
│   └── social.proto               # 占位(后续 SocialService)
├── common/                        # 跨服务通用(不依赖业务)
│   ├── errcode.h                  # 错误码(0/1xxx/11xx/12xx)
│   └── validate.h                 # 字段级校验(username/password/email/phone/device_id)
└── authservice/
    ├── main.cpp                   # 入口: MprpcApplication::Init(-i conf) → 连接池 → NotifyService → Run()
    │                              #   → NotifyService → Run()
    ├── authservice_impl.h/.cc     # RPC 实现层(继承 AuthServiceRpc): 校验参数/查 model/回包
    ├── service/
    │   ├── user_service.h/.cc     # 注册/资料/改密(不含 RPC 细节, 不碰 SQL)
    │   └── session_service.h/.cc  # 登录/登出/刷新/多端限额/踢人/verify
    ├── model/                     # 纯 C++ 结构体(DAO/cache/service/rpc 层间契约)
    │   ├── user.h                 #   UserInfo / UserBrief
    │   └── session.h              #   DeviceInfo / SessionInfo / TokenPair
    ├── dao/                       # MySQL 数据访问(mysqlclient C API)
    │   ├── db_pool.h/.cc          #   连接池
    │   ├── user_dao.h/.cc
    │   ├── device_dao.h/.cc
    │   ├── session_dao.h/.cc
    │   └── audit_dao.h/.cc
    ├── cache/                     # Redis 访问(hiredis)
    │   ├── redis_pool.h/.cc       #   连接池
    │   ├── session_cache.h/.cc    #   会话存取/touch/撤销 + 在线设备 set
    │   ├── user_cache.h/.cc
    │   └── fail_counter.h/.cc     #   防爆破计数
    ├── security/                  # 安全原语(自研薄封装)
    │   ├── password_hasher.h/.cc  #   bcrypt(cost=10)
    │   ├── jwt.h/.cc              #   HS256 签发/校验(OpenSSL EVP)
    │   └── token_util.h/.cc       #   refresh_token 随机生成
    └── test/
        ├── auth_cli.cpp           # 认证服务测试客户端(仿 test/rpc_cli.cpp, 直接裸socket/未来RpcChannel)
        └── auth_cli.conf
```

> `msgservice/`、`socialservice/` 后续与本目录并列新增（各自独立 RPC 服务），共享上面的 `proto/` 与 `common/`，正因此把公共部分放在 `chatservice` 顶层而不是 `authservice/` 内。

### 各层职责红线

| 层 | 可以 | 不可以 |
|---|---|---|
| `authservice_impl` | 协议编解码、参数校验、调 service | 直接写 SQL / 碰 Redis |
| `service` | 业务流程编排、调 dao/cache/security | 依赖 protobuf 消息体、感知 RPC |
| `dao` / `cache` | 单表/单 key 增删改查 | 跨表业务、事务编排以外的逻辑 |
| `security` | 密码/token 原语 | 访问 DB/Redis |

---

## 8. 业务梳理：登录 / 注册（本期聚焦）

> 范围 = 客户端从「连接服务」到「拿到会话凭证、可进主界面」；本节把注册、登录的**字段约束、异常分支、数据落点、时序**一次讲清，作为 S0 实现的直接依据。

### 8.0 接入形态与数据归属

```
客户端 ──(网络短 RPC, 仿 test/rpc_cli)──▶ AuthServiceRpc (AuthServiceImpl)
     │ Register / Login / Logout / VerifyToken(自检)
     ▼  AuthServiceImpl ─▶ service 层 ─▶ dao(MySQL) / cache(Redis) / security
数据归属:  写入 im_auth.users / user_devices / sessions / audit_logs
          写入 Redis:  sess:{uid}:{did} / sess:user:{uid} / login:fail:{account}
产物:      TokenPair(access+refresh) + UserBrief —— 进主界面凭证, 后续业务 RPC/MQ 的鉴权依据
```

### 8.1 注册 Register

**字段约束**（违规统一 `1106`/`1001`，先本地判、后 DB 兜底）：

| 字段 | 规则 | 违规码 |
|---|---|---|
| username | 3~32 位，`^[A-Za-z][A-Za-z0-9_]*$` | 1106 |
| password | 8~32 位，须同时含字母与数字（强度可 conf 放宽） | 1106 |
| nickname | 1~32 位，缺省 = username | 1001 |
| email / phone | 本期**可空**；填了才做格式校验；**不做唯一校验/验证码**（库内已有唯一索引，留空） | 1001 |
| device | 首个登录设备信息（随 Register 一并登记，见登录 §8.2） | 1001 |

**处理分支**（顺序即代码判断顺序）：

| # | 条件 | 动作 | 结果 |
|---|---|---|---|
| P1 | 必填缺失 / 超长 | 直接返回 | 1001 |
| P2 | username / password 不合规 | 直接返回 | 1106 |
| P3 | `UserDao.getByUsername` 命中 | 直接返回 | 1101 |
| P4 | username 唯一键冲突（并发，DB err 1062） | 兜底转 1101 | 1101 |
| P5 | `PasswordHasher.hash(bcrypt cost10)`（线程池） | 投 WorkStealing，勿堵事件线程 | - |
| P6 | 正常 | 写库 + 审计 | 成功 |

**时序与落点**

```
Client ──Register──▶ AuthServiceImpl
   P1/P2 校验 ──► err 1001/1106
   UserDao.getByUsername ──► 命中? err 1101
   hash(bcrypt cost10)              [WorkStealing 线程池]
   UserDao.insert ────────────────────────────► im_auth.users  (password 存哈希串, 非明文)
   AuditDao.insert(register, ip/ua) ─────────► audit_logs     (异步)
   ──注册成功默认「自动登录」──► 走 §8.2 Login 的会话签发(同一 device)
   ◀── Result{0} + user_id + TokenPair + UserBrief ── Client
```

> **自动登录决策**（默认）：注册成功即按本次 device 走一遍登录会话签发、直接下发 token——真实 App 都是注册完直接进主界面，省一次往返；也让 S0 只验证一条“拿 token”路径。备选：注册只回 `user_id`，客户端再走登录（两段独立可测，多一步）。

### 8.2 登录 Login

**字段约束**：

| 字段 | 规则 | 违规码 |
|---|---|---|
| account | username / email / phone 三选一。**识别规则**：含 `@` → 按 email；11 位纯数字 → 按 phone；否则按 username | 1102 |
| password | 非空，长度≤32 | 1106 |
| device | `device_id` 必填；同名 device_id 重复登录 = **覆盖**旧会话（幂等，不重复计数） | 1001 |
| ip / user_agent | 仅审计，缺省允许 | - |

**处理分支**：

| # | 条件 | 动作 | 结果 |
|---|---|---|---|
| P1 | 参数缺失/越界 | 直接返回 | 1001/1106 |
| P2 | `BruteForceGuard.check(account)` 命中锁 | 直接返回 | 1104 |
| P3 | 账号不存在（防枚举：可统一走“密码错误”口径，见注 1） | 直接返回 | 1102 |
| P4 | 用户软删除（deleted_at 非空） | 直接返回 | 1105 |
| P5 | `PasswordHasher.verify` 失败 | 失败计数 +1；达阈值(默认5) → 置 Redis 锁(TTL 30min)；审计 | 1103 →(锁定后)1104 |
| P6 | 密码正确 | 清空 fail 计数 | - |
| P7 | 有效会话数 ≥ 上限(默认 5)：`kick_oldest` 挤最久未活跃 / `reject_new` 拒绝（conf 切，默认 kick_oldest） | 挤：先吊销目标设备会话再继续 | (reject 时)1204 |
| P8 | 会话签发 / 落库异常 | 回滚已写、记日志 | 1002 |

**会话签发细节**（P8 内部顺序）：`users.last_login_*` 更新 → `user_devices` upsert(device, online) → `sessions.insert(refresh, expires)` → Redis `SET sess:{uid}:{did}`(TTL) + `SADD sess:user:{uid}` → 回 TokenPair。

**时序与落点**

```
Client ──Login(account,password,device,ip)──▶ AuthServiceImpl
  P1 参数校验 ──► err 1001/1106
  P2 BruteForceGuard.check ──► 锁? err 1104
  P3/P4 UserDao.getByAccount / 软删除 ──► err 1102/1105
  P5 PasswordHasher.verify ──► 败: fail++/审计/err 1103 (达阈值置锁 → 1104)
  P6 成功 → 清 fail 计数
  P7 device 幂等 / 多端≤5 检查(kick_oldest|reject_new)
  签发 access(JWT,2h)+refresh(随机32B,7d)
    MySQL: sessions.insert; user_devices.upsert(online)
    Redis: SET sess:{uid}:{did}=access; SADD sess:user:{uid} did; (TTL 滑动, ≤7d)
  users.last_login_at/ip/device 更新; AuditDao.insert(login, detail=JSON)   [异步]
  (后续) 好友上线/未读数通知 —— 阶段二 RPC/MQ 的事, 本期不做
  ◀── Result{0} + TokenPair + UserBrief ── Client  →  客户端持 token 进入主界面
```

> 注 1（防枚举）：账号不存在与密码错误默认**分开返回 1102 / 1103**（便于排障）；若对外暴露不想泄露账号存在性，可把 1102 并入“密码错误”口径，conf 开关。注 2：失败计数与锁定基于 Redis，天然多实例共享；账号不存在时**不计**失败次数（只防“对已知账号爆破”，也防批量探测——取舍见 §15）。

### 8.3 VerifyToken（登录/注册自检 + 后续网关预研）

```
VerifyToken(token)
  1 jwt.verify(HS256, exp, typ=access)   失败 → err 1201/1202(本地, 无 IO)
  2 本地 LRU(token→{uid,did})            命中 → 直接 valid
  3 Redis EXISTS sess:{uid}:{did} 且值==token  不命中(已删/过期) → err 1201
  4 写本地 LRU(TTL 5min) → 返回 valid + uid/did
```
用途：`auth_cli` 拿登录返回的 token 自检链路；后续网关/主界面服务逐请求鉴权复用它。

### 8.4 登录/注册之外的会话接口（后置，进入主界面后再说）

`RefreshToken` / `Logout`(S0 做最小版) 之外的 `KickDevice / ListDevices / ChangePassword / UpdateUserInfo` 属主界面“设置/账号/多端”业务，本期不梳理——状态机与接口见 §5.2 / §4，落地排在 S0 之后。

---

## 9. 依赖与构建

- C++17 / pthread；外部基础库走系统包：`protobuf`、`libzookeeper_mt`(mprpc 带)、`mysqlclient`、`hiredis`、`openssl`。
  本机均已验证存在（`pkg-config --exists mysqlclient/hiredis/openssl`）。
- 底座 .so 与 include：从仓库顶层聚合构建时直接复用 alias `lib::muduo_log / lib::nwl / lib::threadpool / lib::mprpc`（同 `test/CMakeLists.txt` 的用法）。

```cmake
# src/chatservice/CMakeLists.txt (示意, target/路径以落地为准)
cmake_minimum_required(VERSION 3.14)
project(chatservice CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)

# proto 编译: common/auth → 生成 auth.pb.cc/.h (cc_generic_services=true)
# 参考现有工程: protoc --cpp_out=<gen> proto/common.proto proto/auth.proto

add_library(chatauth_common proto/*.pb.cc common/*.cc)          # 共享公共件
add_executable(auth_server authservice/main.cpp ...)
target_link_libraries(auth_server PRIVATE
    lib::muduo_log lib::nwl lib::threadpool lib::mprpc
    chatauth_common PkgConfig::MYSQL PkgConfig::HIREDIS PkgConfig::OPENSSL)
```

- 归属两种形态（见 §15 决策）：
  1. **纳入顶层聚合**：在 `../../CMakeLists.txt` 末尾加 `add_subdirectory(src/chatservice)`，可执行文件随顶层输出到 `bin/`；
  2. **独立仓库**：`chatservice` 自持 CMake，`add_subdirectory(../NetWork ../NetWork …)` 或用 `find_library` 指到 `lib/`。

---

## 10. 配置项（conf/auth.conf.example）

沿用 mprpc 的 `-i <conf>`（`MprpcConfig` 按 `key=value` 解析，可加自定义 key）：

```ini
# ---- RPC / 服务注册 (mprpc, 必需) ----
rpcserverip=127.0.0.1
rpcserverport=8001                 # 认证服务端口, 避免与演示 8000 冲突
zookeeperip=127.0.0.1
zookeeperport=2181

# ---- MySQL (im_auth) ----
mysqlip=127.0.0.1
mysqlport=3306
mysqluser=root
mysqlpassword=
mysqldb=im_auth
mysqlpool_min=4
mysqlpool_max=32

# ---- Redis (会话/防爆破/缓存) ----
redisip=127.0.0.1
redisport=6379
redisdb=0
redispool_min=2
redispool_max=16

# ---- 安全 (密钥务必从环境变量/部署密管读取, 勿入库) ----
jwt_secret=change-me-please
access_expire_seconds=7200          # access 2h
refresh_expire_days=7
brute_force_max_fail=5
brute_force_lock_minutes=30
max_devices_per_user=5
device_limit_policy=kick_oldest     # kick_oldest | reject_new
```

---

## 11. 验证与运行

前置：`zookeeper`(systemd) / `mysql`(已建 `im_auth`) / `redis-server` 均需可用。
DB 需用当前 `db/init.sql` 初始化：`users.email/phone` 已改为可空(NULL=未绑定)，避免唯一索引下多用户空串冲突。

```bash
# 在 src/chatservice/ 下执行(S0 独立构建; 不挂顶层 chat 聚合)
cmake -S . -B ../../build/chatservice && cmake --build ../../build/chatservice -j$(nproc)
mysql -uroot < ../../db/init.sql            # 仅首次或 schema 变更后(DROP 重建, 清空数据)
./bin/auth_server -i conf/auth.conf         # 应见 zookeeper_init success! + 4 个 znode create success + start service
./bin/auth_cli -i conf/auth_cli.conf        # 直连 rpcserver, 免 zk
```
验收口令：
- zk 上出现 `/AuthServiceRpc` 与 `/AuthServiceRpc/Login` 等**临时节点**（`zkCli.sh ls /AuthServiceRpc` 可见 4 个方法）；
- 注册成功默认自动登录下发双 token，`im_auth.users` 落 1 行（S0 为 `盐$FNV1a` 哈希, 非明文; M2 换 bcrypt `$2b$10$…`）；
- 登录成功 `sessions` 落 1 行(status=1)、Redis 出现 `sess:{uid}:{did}`、`tok:{access}`、`tok:{refresh}`；
- 登出后原 token 立即失效；连续 5 次错密码 → 1104 锁定（M3 收紧）；手动删 Redis 会话后原 access_token 调 `VerifyToken` 返回 1201。

---

## 12. mprpc / 底座需配合的改造点（里程碑 M4）

| 改造 | 位置 | 目的 |
|---|---|---|
| `RpcHeader` 增加 `token / request_id` 字段 | `src/mprpc/src/rpcheader.proto` → 重编 | 框架级承载鉴权元数据 |
| 服务端把 token 注入 controller | `src/mprpc/src/rpcprovider.cpp:153`（把 `nullptr` 换成携带 token 的 controller 对象，再塞进 `service->CallMethod`） | RPC 实现统一取“调用方是谁”，业务 proto 去掉 RequestMeta 冗余 |
| mprpc 库内实现 **RpcChannel + 服务发现**（客户端） | mprpc 新增 client 端（现仅 test/rpc_cli.cpp 内联 demo） | AuthService 才能被网关/Msg/Social 以正式 RPC 调用；认证服务的 verify 接口正是第一个受益方 |

暂态（不改造底座）用 `RequestMeta{token,request_id}` 先行；字段、行号均为现值，动前以实际代码为准。

---


## 13. 里程碑与验收

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **S0 登录/注册闭环（当前聚焦）** | chatservice 骨架 + `auth.proto` 编译 + `Register`/`Login` 落地（注册默认自动登录下发 token，会话落 MySQL+Redis，审计入 audit_logs）+ `auth_cli` 端到端打通 | `auth_cli` 里 注册→自动登录→拿 token→凭 token 校验 通过；DB/Redis 落点正确（§11 验收口令） |
| **M0 骨架** | chatservice 目录树、proto 编译、`AuthServiceImpl` 空实现注册 zk、`auth_cli` 打通（仿 rpc_ser/rpc_cli） | 两端跑通任意方法 |
| **M1 用户基础** | DbPool + UserDao + Register / Login(仅校验) / GetUserInfo；密码 bcrypt(cost10)；审计入 audit_logs | DB 落库正确、密码非明文 |
| **M2 会话** | JWT 签发/校验 + Redis 会话 + Login 出双 token + Logout / RefreshToken / ListDevices / KickDevice / VerifyToken + 多端≤5 | 登录→鉴权→登出全链路；踢人即时生效 |
| **M3 安全加固** | BruteForceGuard 防爆破；access 本地 LRU 缓存；改密全端踢；输入/长度/格式校验；审计齐全 | 爆破锁 30min；吊销即时生效 |
| **M4 底座演进** | §12 三项 mprpc 改造（RpcHeader token、controller 注入、库内 RpcChannel） | 认证服务成为可被正式调用的服务，RequestMeta 移除 |
| **M5 集成与压测** | Redis/MQ 缓存失效链路；登录 QPS 压测；补充单元测试 | 登录 QPS 达到 plan §7.1 目标量级的本机水平 |

---

## 14. 待确认决策点（写文档时的默认假设）

1. **`chatservice` 归属**：默认当作**新子模块仓库**（对齐 `src/*` 四个底座、自持 CMake），**由顶层 `chat` 聚合构建**；它当前是普通空目录，纳入聚合前需先定归属并补 `.gitmodules`/CMake。
2. **密码/签名自研度**：默认**自研薄封装**——bcrypt 内嵌实现 + JWT(OpenSSL HMAC)。备选：引入第三方(jwt-cpp/libbcrypt) 或本期先用简单哈希占位、后续替换。
3. **注册邮箱/手机唯一性**：本期仅 username 唯一；email/phone 若启用唯一，需先做「发送验证码」闭环，默认暂缓（db 设计里已有唯一索引，先留空）。
4. **多端超限策略**：默认 `kick_oldest`（挤掉最久未活跃设备）；可在 conf 切 `reject_new`。
5. **JWT secret / 部署密钥**：代码里只读配置，不上库。

> 认可以上默认就按 §13 从 **M0** 开始落地；有异议的决策点单独过一遍即可。
