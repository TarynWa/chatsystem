# chat — 自研「muduo 全家桶」基础设施聚合工程

> 一个从零实现的 C++17 Linux 服务器基础设施栈，用四个可独立演进、也可组合使用的子库，逐步**替代对第三方 muduo 的依赖**，并为后续的分布式聊天 / RPC 应用提供统一的底层底座。

本仓库本身是一层 **CMake 聚合外壳**：通过 git submodule 引入四个子模块并统一产出 `.so`，不含业务代码。当前阶段交付的是**可链接的基础库**，尚未包含运行在它们之上的应用（设计文档中规划的应用层 `chatsystem` 见下文 Roadmap）。

---

## 1. 背景与目标

经典「集群聊天服务器 / 自研 RPC」教程项目通常直接依赖第三方网络库 muduo。本工程选择**自己造一套懂行的 muduo**：

- 底层纯 Linux syscall（`epoll / timerfd / eventfd / accept4`）实现，`NetWork` 子库对外零网络依赖；
- 各库 **API 与 muduo 同构**，使原本基于 muduo 的代码（如 `chatserver`、muduo 版 mprpc）可以**换 include、换类型名即迁移**；
- git 历史可见 `third_party/muduo-pic/`（muduo 头文件拷贝）已在提交 `09ba6c4` 中整体删除，正是被这四个子库取代的痕迹。

设计依据：`src/NetWork/task.md`（多方案选型）与 `src/NetWork/plan.md`（方案一落地计划）。

---

## 2. 仓库结构

```
chat/
├── CMakeLists.txt               # 顶层聚合：依次 add_subdirectory 四个子模块，统一输出 .so
├── .gitmodules                  # 四个 git submodule 指向 GitHub
├── .gitinore                    # 注意：文件名拼写为 .gitinore（非标准 .gitignore）
├── lib/                         # 全部动态库统一输出目录（libmuduo_log/nwl/threadpool/mprpc .so）
└── src/                         # 四个子模块
    ├── logsystem/    → libmuduo_log.so   日志库（命名空间 wangt）
    ├── NetWork/      → libnwl.so         网络库（命名空间 nwl，别名 NetWorkLibrary）
    ├── threadpool/   → libthreadpool.so  线程池库（纯头文件 + 显式实例化）
    └── mprpc/        → libmprpc.so       RPC 框架（protobuf + ZooKeeper）
```

顶层构建顺序敏感：`logsystem` 必须先于 `NetWork` 定义（nwl 依赖其日志符号），`mprpc` 最后（依赖 nwl）。

---

## 3. 模块总览与依赖关系

| 子模块 | 动态库 | 命名空间 | 核心职责 | 外部依赖 |
|---|---|---|---|---|
| `logsystem` | `libmuduo_log.so` | `wangt` | 日志（流式宏 + 日志文件滚动 + 异步落盘） | 无 |
| `NetWork` (nwl) | `libnwl.so` | `nwl` | epoll 网络库：Reactor、TcpServer、TimerQueue | `logsystem`（日志 / `Timestamp`） |
| `threadpool` | `libthreadpool.so` | 全局 | 多种线程池（定长 / 缓存 / 定时 / 工作窃取） | 无（仅 pthread） |
| `mprpc` | `libmprpc.so` | 全局 | RPC Provider：protobuf 方法暴露 + ZooKeeper 注册 | `nwl`(传递日志) · `protobuf` · `zookeeper_mt` |

```
┌─────────────────────────── 上层应用（规划中 chatsystem / RPC 业务） ───────────────────────────┐
│      基于 nwl::TcpServer 承载长连接、提交任务到 threadpool、由 mprpc 暴露内部 RPC 服务            │
└───────────────────────────────────────────┬───────────────────────────────────────────────────┘
                 ┌──────────────────────────┼───────────────────────────┐
                 ▼                          ▼                           ▼
        ┌─────────────────┐       ┌─────────────────────┐     ┌──────────────────────┐
        │ libmprpc.so     │       │ libthreadpool.so    │     │ libnwl.so            │
        │ RpcProvider     │       │ 业务线程池/定时调度   │     │ One-Loop-Per-Thread  │
        │  + ZkClient     │       └─────────────────────┘     │ Reactor (epoll LT)   │
        └────────┬────────┘                                    └──────────┬───────────┘
                 └───────────────────────────────┬──────────────────────────┘
                                                 ▼
                                     ┌─────────────────────┐
                                     │ libmuduo_log.so     │  wangt::Logger / WT_LOG_* / Timestamp
                                     └─────────────────────┘
```

> `mprpc` 依赖 `nwl` 是**自研替代的落点**：经典 mprpc 教程版跑在 muduo 上，本工程的 `rpcprovider.cpp` 已整体改用 `nwl::TcpServer / nwl::EventLoop / nwl::Buffer`（仅保留了日志库 `muduo_log` 这一与 muduo 同名的自研库）。

---

## 4. 模块详解

### 4.1 `logsystem` — 日志库 → `libmuduo_log.so`

命名空间 **`wangt`**，是 muduo 日志的精简复刻（约 750 行）。

- **对外接口**：流式宏 `WT_LOG_TRACE / DEBUG / INFO / WARN / ERROR / FATAL`（`Logger.hpp`），用法 `WT_LOG_INFO << "conn up: " << conn->name();`。宏内先比对 `Logger::GetLogLevel()` 再构造临时 `wangt::Logger(level, __FILE__, __func__, __LINE__).stream()`，避免低级别日志无谓开销。
- **同步路径（默认）**：`Logger` 是进程级静态门面，持全局 `output_ / flush_` 钩子；默认输出到 **stdout**。每条语句经 `LogMessage` 拼装（时间戳 + 级别 + 文件/函数/行号 + 正文），析构时输出并 `flush`；`FATAL` 额外写 stderr 并 `exit`。
- **滚动文件**：`LogFile(basename, rollSize, …)` 按字节 / 按天滚动，文件名 `<basename>.<YYYYmmddHHMMSS.us>.<host>.<pid>.log`；`AppendFile` 用 64 KiB stdio 缓冲兜底短写。`Timestamp` 提供微秒时间戳与格式化。
- **异步落盘**：`AsyncLogging` 采用前/后端线程模型（写线程 + 定时唤醒落盘），与 muduo 思路一致但实现为 `std::string` 单缓冲简化版。**目前没有任何模块接入它**，mprpc 走的是 `LogFile` + `setOutput/setFlush` 的同步文件路径。

### 4.2 `NetWork` (nwl) — 网络库 → `libnwl.so`

命名空间 **`nwl`**，约 2100 行，把 muduo 的 `one-loop-per-thread` Reactor 核心在纯 Linux syscall 上重写了一遍。

**线程模型**

- 一个 **main loop**：只负责 `Acceptor` 监听与 accept，新连接按 **round-robin** 分发给 `EventLoopThreadPool` 中的某个 IO 线程（`setThreadNum(n)`；为 0 时全部连接落在主 loop）；
- N 个 **sub-reactor**：`EventLoopThread` 内 `thread_local` 独占一个 `EventLoop`，阻塞在 `loop()`；
- **跨线程铁律**：任何线程要操作别的 loop 上的对象，只能经 `runInLoop()/queueInLoop()` 投递。每个 loop 持 `eventfd` 作唤醒通道，`queueInLoop` 在「非本线程或正在执行 pending」时必写唤醒；`doPendingFunctors` 采用锁内 `swap` 出队、锁外执行，避免回调内再投递造成死锁。

**已实现的关键机制（与 muduo 对齐）**

| 机制 | 实现要点 |
|---|---|
| IO 多路复用 | `Poller` 抽象 + `EpollPoller`，默认 **LT**；`data.ptr` 直接挂 `Channel*` O(1) 还原 |
| 定时器 | `TimerQueue` 基于 `timerfd_create` + 有序 `set`，支持一次性 / 周期 / `cancel`，定时任务增删跨线程安全 |
| 事件通道 | `Channel` 封装 fd 事件 + 读/写/关闭/错误回调 |
| 生命周期安全 | `TcpConnection : enable_shared_from_this`；`Channel::tie(weak_ptr)` 防 in-flight 事件悬垂 |
| 智能缓冲 | `Buffer`：`[8B prepend][可读][可写]` 双端索引，`readFd()` 用 `readv` + 栈上 64KB 备用块一次读尽 |
| 半关闭 | `shutdown()` 状态机 `Connected → Disconnecting`，输出缓冲排空后才 `SHUT_WR` |
| 背压回调 | `writeComplete` / 高水位 `highWaterMark`（默认 64MB） |
| EMFILE 兜底 | `Acceptor` 预开 `/dev/null`，fd 耗尽时「借位 accept 再关闭」避免监听队列风暴空转 |
| 常量 | 忽略 `SIGPIPE`、`SO_REUSEADDR`、`TCP_NODELAY`、`accept4(SOCK_NONBLOCK|CLOEXEC)` 等 |

**对外 API 一览**（`include/nwl/`，回调签名与 muduo 同构）

```cpp
// 类型别名
using TcpConnPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnPtr&)>;
using MessageCallback     = std::function<void(const TcpConnPtr&, Buffer*, Timestamp)>;
using WriteCompleteCallback, HighWaterMarkCallback, CloseCallback ...
using nwl::Timestamp = wangt::Timestamp;      // 直接复用 logsystem 的 Timestamp

// 典型服务端骨架（mprpc 正是这样用）
EventLoop loop;
TcpServer server(&loop, InetAddress(port, ip), "name");
server.setThreadNum(4);
server.setConnectionCallback(...);
server.setMessageCallback(...);   // onMessage(conn, buffer, t)：自行处理 4B 长度前缀分帧
server.start();
loop.loop();
```

> 注意 `nwl::InetAddress` 构造参数顺序是 **`(port, ip)`**，与 muduo 的 `(ip, port)` 相反（迁移时需交换）。

### 4.3 `threadpool` — 线程池库 → `libthreadpool.so`

纯头文件库（约 800 行，全局命名空间），四个池 + 四版同步队列。`CMakeLists` 只编译 `src/threadpool.cpp`——它通过**显式模板实例化 + 引用各池公开方法**把内联实现发射进 `.so`，使动态链接生效。

| 头文件（注意拼写） | 实际类名 | 语义 | 底层队列 |
|---|---|---|---|
| `SyncQueue.hpp` | `SyncQueue<T>` | 有界 FIFO 阻塞队列 | 互斥 + 两个条件变量，默认 200 |
| `SyncQueue1.hpp` | `SyncQueue1<T>` | 支持超时的队列（`Put/Take` 返回 0/1/2 状态），`push_front` → LIFO | `wait_for` + `atomic_bool` |
| `SyncQueue2.hpp` | `SyncQueue2<T>` | 分桶队列，支持整桶 `Take(index, list&)` **批量窃取** | vector<list> + 单锁 |
| `SyncQueue3.hpp` | `SyncQueue3` | 按时延排序的**优先级队列**（非模板），`Take` 内按任务秒数定时等待 | `priority_queue` min-heap |
| `FiexdThreadPool.hpp` | `FixedThreadPool` | 固定 N 线程，`AddTask` 入队 | `SyncQueue` |
| `CacheThreadPool.hpp` | `CachedThreadPool` | 核心线程 + 空闲超时(10s)回收 + 按需扩容到 `hw*2+1`，`execute/submit` 返回 `future` | `SyncQueue1` |
| `ScheduledThreadPool.hpp` | `ScheduledThreadPool` | 固定 worker，`execute(interval, …)/submit(interval, …)`，首参为**秒级延时**（注意：文档写作 `ScheduleThreadPool`，类实为带 `d` 的 `ScheduledThreadPool`） | `SyncQueue3` |
| `WorkStealingPool.hpp` | `WorkStealingPool` | N 个 worker 各守一个桶，本桶取空后随机窃取他桶 | `SyncQueue2` |

### 4.4 `mprpc` — RPC 框架 → `libmprpc.so`

经典 muduo 版 mprpc 教程项目的 **provider 侧**实现，网络层已移植到自研 `nwl`。

- **外部依赖**：`protobuf`（序列化）+ `zookeeper_mt`（服务注册，需定义 `THREADED`）；编译期用 `MprpcApplication::Init(argc, argv)` 解析 `-i 配置文件`。
- **配置格式**：`key=value` / `#` 注释，读取 `rpcserverip / rpcserverport / zookeeperip / zookeeperport`。
- **对外接口**：`RpcProvider::NotifyService(proto::Service*)` 反射注册方法；`Run()` 从配置启动 `nwl::TcpServer`（4 个 IO 线程）并把每个 `服务/方法` 注册到 ZooKeeper（`/服务名` 持久节点 + `/服务名/方法名` **临时**节点，data 为 `ip:port`）。
- **调用流程 / 线上协议**：

```
TCP 载荷 = [4B header_size][RpcHeader(protobuf)][args(protobuf)]
RpcHeader{ bytes service_name=1; bytes method_name=2; uint32 args_size=3; }
收到请求 → 解析 RpcHeader → 反射查 ServiceInfo → 反序列化 request → service->CallMethod()
完成后 SendRpcResponse(conn, response) → conn->send() → conn->shutdown()   （短连接，即发即断）
```

- **当前范围**：`libmprpc.so` 库内只有 **Provider** 侧；库内尚无客户端 `RpcChannel`。但 `test/rpc_cli.cpp` 内联了一个最小 `MprpcChannel` demo 客户端（连 zk 查 `/UserServiceRpc/Login` → 直连 provider），并已与 `test/rpc_ser.cpp` 完成**真实登录往返验证**（见 §5 运行验证）。`ZkClient::GetData()` 即由该 demo 客户端调用。

---

## 5. 构建

依赖：Linux + CMake ≥ 3.14 + C++17 编译器 + pthread；构建 `mprpc` 另需系统安装 `protobuf`、`libzookeeper_mt` 及其头文件。

```bash
cmake -S . -B build
cmake --build build -j$(nproc)

# 四个动态库统一产出到顶层 lib/
ls lib/
# libmuduo_log.so  libnwl.so  libthreadpool.so  libmprpc.so
```

链接方式（以 nwl 为例，其余同理）：

```cmake
target_link_libraries(your_app PRIVATE
    nwl            # 或 mprpc / threadpool / muduo_log
    protobuf zookeeper_mt   # 仅用到 mprpc 时需要
)
```

各子模块均支持**独立构建**（`mprpc` 在独立构建时会自动把 `logsystem/NetWork` 作为子目录引入）。

### 运行验证（本机已实测通过）

`test/` 提供 RPC 服务端 + 客户端用例（根仓库 CMake 已 `add_subdirectory(test)`，产物进 `bin/`）。前置条件：**先启动 ZooKeeper 服务端**（本机为 apt `zookeeperd` 3.9.5 + systemd `zookeeper` 服务，监听 127.0.0.1:2181）：

```bash
systemctl is-active zookeeper   # 需为 active；未起则 systemctl start zookeeper

# 顺序必须：先服务端后客户端（/UserServiceRpc/Login 是 zk 临时节点，server 退出即消失）
cd bin
./rpc_ser -i test.conf   # 应见 zookeeper_init success! / znode create success / start service at 8000
./rpc_cli -i cli.conf    # 应见 ip:port=127.0.0.1:8000 / rpc login response success:1 / login success!
```

排障要点：任何一方卡住优先检查 zk（`ps`/`bin/wangt.zookeeper.log` 若持续 `errno=111 Connection refused` = zk 未起）；provider 对"服务/方法不存在、解析失败"分支会 `conn->shutdown()`（不回包即断连），client 对 `recv` 设 10s 超时并以 `SimpleController` 承接错误，不会无限阻塞。

---

## 6. 当前状态与 Roadmap

按 `src/NetWork/plan.md` 的里程碑，当前代码覆盖 **M1–M3（基础层 + EventLoop + TCP 会话层）**；M4（压测调优）、M5（集成 `chatsystem`）尚未落地。汇总：

- ✅ 已实现：nwl 网络库核心、`wangt` 日志同步/滚动/异步类、四类线程池、mprpc Provider（含 ZooKeeper 注册）；`test/` 下 RPC 服务端+客户端链路已本机跑通（登录成功，见 §5）
- 🚧 尚未包含 / 与子模块 README 宣称有出入
  - 子模块内 `examples/` 与单元测试 `tests/`（各子模块 README/plan 中的 echo 示例、单测目录在代码中不存在；顶层 `test/` 现含 RPC demo，非单测）
  - ET（edge-triggered）模式、`select/poll` 降级后端——`Poller::newDefaultPoller` 目前无条件返回 `EpollPoller`
  - `AsyncLogging` 异步日志库内未实际接入（仅 `test/` 用例启用），默认日志输出 stdout / LogFile
  - **mprpc 库内**仍无客户端 `RpcChannel` / 服务发现（demo 客户端 `MprpcChannel` 内联在 `test/rpc_cli.cpp`）
  - `chatsystem`（顶层应用）尚未加入，本仓库仍是「基础库 + demo」状态

### 走查注意点（整理自代码，便于后续修复）

- `AsyncLogging::append`：消息塞不下触发缓冲滚动时，会**丢失触发滚动的那条日志**（`AsyncLogging.cpp:47-57`），与其注释描述的 muduo 双缓冲设计不一致。
- `CachedThreadPool::StopThreadGroup()` 为空实现，`Stop()` 实际不会回收/join 线程。
- `ScheduledThreadPool` 是「延时执行」而非真正的周期任务（`execute` 后不重新入队）。
- 命名漂移：文件名/类名不一致（`FiexdThreadPool.hpp`、`CacheThreadPool.hpp` → `FixedThreadPool`/`CachedThreadPool`）；文档里的 `ScheduleThreadPool` 与真实类名 `ScheduledThreadPool` 差一个 `d`；日志库多处拼写笔误（`wittenBytes_`、`kRollPerSecods_`）。
- mprpc 对首 4 字节 `header_size` 按**本机字节序**解析（无 `htonl/ntohl`），若客户端按大端封包会出现不兼容。
- 顶层忽略文件名 `.gitinore` 拼写不规范，且 `build/`、`lib/` 曾被提交入库（见提交 `71be047 编译产物`）。

---

## 7. 相关文档

| 文档 | 内容 |
|---|---|
| `src/NetWork/README.md` | nwl 网络库使用手册（示例 API、架构图；注意其中 examples/ET 描述为规划） |
| `src/NetWork/task.md` | 底层多方案选型分析（方案一~四）与验收指标 |
| `src/NetWork/plan.md` | 方案一落地计划：模块设计、易错点 Checklist、M1–M5 里程碑 |
| 子模块源码 | `logsystem`、`threadpool`、`mprpc` 各自的 CMake 与注释可作 API 参考 |

---

*chat* — 造一套自己懂行的 muduo，为分布式聊天与 RPC 提供纯自研、可组合、可替换的 Linux 底座。


1.初始化数据库
python3 db/init_db.py