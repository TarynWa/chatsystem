# CLAUDE.md

本文件为 Claude Code 在此仓库工作时提供的项目简介。详细文档见根目录 `README.md`。

## 项目是什么

`chat` 是**自研「muduo 全家桶」基础设施聚合工程**：用四个 git 子模块从零实现 C++17/Linux 服务器基础库，逐步替代对第三方 muduo 的依赖（git 历史中 `third_party/muduo-pic/` 的删除即此过程的落点），为后续分布式聊天 / RPC 应用提供统一底座。

- 纯 Linux syscall（epoll/timerfd/eventfd/accept4），API 与 muduo 同构，便于原 muduo 代码**换 include 即迁移**。
- 本仓库是 CMake 聚合外壳：**不含业务代码/可执行文件**，当前只交付四个 `.so`。

## 目录结构

```
chat/
├── CMakeLists.txt     # 顶层聚合，顺序敏感：logsystem → NetWork → threadpool → mprpc
├── .gitmodules        # 四个子模块指向 GitHub
├── lib/               # 全部 .so 统一输出目录
└── src/               # 子模块（各自含独立 .git）
```

## 子模块速查

依赖链：**logsystem → nwl → mprpc**；threadpool 独立；mprpc 另需 protobuf + zookeeper_mt。

| 子模块 | 产物 | 命名空间 | 职责 |
|---|---|---|---|
| `src/logsystem` | `libmuduo_log.so` | `wangt` | 日志：`WT_LOG_*` 宏、`LogFile` 滚动、`AsyncLogging` 异步落盘、`Timestamp` |
| `src/NetWork` | `libnwl.so` | `nwl` | muduo 式 one-loop-per-thread Reactor（epoll LT），TimerQueue/timerfd、TcpServer、Buffer、EventLoopThreadPool |
| `src/threadpool` | `libthreadpool.so` | 全局 | 纯头文件线程池：Fixed/Cached/Scheduled/WorkStealing + `SyncQueue1~3` |
| `src/mprpc` | `libmprpc.so` | 全局 | RPC Provider：protobuf 反射 + ZooKeeper 注册，**网络层已移植到自研 nwl** |

### 关键约定与注意点

- `nwl::InetAddress` 构造参数为 **`(port, ip)`**（与 muduo 的 `(ip, port)` 相反，迁移时需交换）。
- `nwl::Timestamp = wangt::Timestamp`，nwl 复用 logsystem 的日志与时间戳。
- 线程池类名与文档/文件名有漂移：真实类为 `ScheduledThreadPool`（文档写作 `ScheduleThreadPool`，少个 `d`）；文件名 `FiexdThreadPool.hpp`/`CacheThreadPool.hpp` 与类名 `FixedThreadPool`/`CachedThreadPool` 不一致。
- mprpc 首 4 字节 `header_size` 按**本机字节序**解析（无 htonl/ntohl）。

## 构建

```bash
cmake -S . -B build && cmake --build build -j$(nproc)   # 依赖 CMake≥3.14、C++17、pthread
ls lib/   # libmuduo_log.so libnwl.so libthreadpool.so libmprpc.so
```

mprpc 需系统安装 `protobuf`、`libzookeeper_mt`；各子模块也可独立构建。

## 运行 RPC 测试（本机已跑通）

> 本机环境（2026-09-06 实测）：ZooKeeper 服务端由 **systemd 的 `zookeeper` 服务**提供（apt 装 `zookeeperd` 3.9.5），监听 127.0.0.1:2181，数据目录在 apt 默认 `/tmp/zookeeper`（仅适合开发）。`rpc_ser`/`rpc_cli` 卡住时先查 `systemctl is-active zookeeper` 与 `bin/wangt.zookeeper.log`。

```bash
# 顺序必须：先服务端后客户端（/UserServiceRpc/Login 是 zk 临时节点，server 退出即消失）
cd bin && ./rpc_ser -i test.conf    # 应见 zookeeper_init success! + znode create success... + start service
cd bin && ./rpc_cli -i cli.conf     # 应见 rpc login response success:1 / message:login success!
```

- 客户端/服务端都依赖 zk：任何一方卡住通常不是网络库问题，而是 zk 未起或配置未加载。
- 行为约定：provider 对"服务/方法不存在、头/参数解析失败"分支会 `conn->shutdown()`（不回包即断连）；client 对 `recv` 设了 10s 超时、用真实 `SimpleController` 承接错误（原 `nullptr` controller 会在失败分支解引用崩溃）。验证脚本在 `test/`（rpc_ser/rpc_cli 用例，属根仓库；`src/` 下四个模块是 submodule）。

## 当前状态与口径

- ✅ 已实现：nwl 网络库核心、wangt 日志、四类线程池、mprpc Provider（含 ZooKeeper 注册）；**test/ 下 RPC 服务端+客户端链路已本机跑通**（rpc_ser 注册 `/UserServiceRpc/Login`，rpc_cli 登录成功）。
- 🚧 代码中**尚无**：子模块内 examples/ 与单元测试、ET 模式、select/poll 降级后端；`AsyncLogging` 库内未实际接入（仅 test/ 用例启用）；**mprpc 库内**仍无客户端 RpcChannel/服务发现（demo 客户端类 `MprpcChannel` 内联在 test/rpc_cli.cpp）；顶层 `chatsystem` 应用。
- 阶段对应 `src/NetWork/plan.md` 的 **M1–M3 已完成，M4（压测）M5（集成）未落地**。
- 子模块 README/plan 描述的部分内容属规划，**以代码为准**，改动前先核对。
- 顶层忽略文件名 `.gitinore` 拼写不规范；`build/`、`lib/` 曾被提交入库（提交 `71be047`）。

## 文档指引

| 文档 | 内容 |
|---|---|
| `README.md` | 整体架构与模块详解（含走查注意点清单） |
| `src/NetWork/README.md` | nwl 使用手册（示例中 examples/ET 为规划） |
| `src/NetWork/task.md` | 底层多方案选型与验收指标 |
| `src/NetWork/plan.md` | 方案一落地计划、易错点 Checklist、M1–M5 里程碑 |
