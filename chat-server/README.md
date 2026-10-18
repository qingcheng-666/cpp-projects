# 分布式聊天服务器

基于 C++ 的分布式即时通讯服务，采用**微服务架构**拆分网关、聊天、状态、验证、资源五类服务，
以 gRPC 完成跨进程调用、Redis 承担共享状态与互斥、MySQL 承载持久化。

> 项目基于 [恋恋风辰](https://github.com/secondtonone1) 的分布式聊天服务器教程实现。
> 本仓库为完整源码整理，包含服务发现、跨服消息路由、分布式锁等核心机制。

---

## 架构

```
客户端
  │  HTTP / WebSocket
  ▼
┌──────────────┐   gRPC    ┌──────────────┐
│  GateServer  │──────────▶│ VerifyServer │  验证码
│   （网关）    │           └──────────────┘
└──────┬───────┘
       │ gRPC（取聊天服务器地址 + token）
       ▼
┌──────────────┐
│ StatusServer │  服务发现：分配 ChatServer、签发 token
└──────┬───────┘
       │ 返回 ChatServer 地址
       ▼
┌──────────────┐   gRPC    ┌──────────────┐
│  ChatServer  │◀─────────▶│  其他 Chat   │  跨服聊天
│  （长连接）   │           │   Server     │
└──────┬───────┘           └──────────────┘
       │
       ├──▶ MySQL（用户、好友、消息持久化）
       └──▶ Redis（Token、在线状态、分布式锁、连接计数）
```

---

## 服务说明

| 服务 | 职责 | 关键实现 |
|---|---|---|
| **GateServer** | HTTP 入口、静态资源、请求转发 | `HttpConnection`、`LogicSystem` |
| **ChatServer** | 长连接、消息收发、跨服转发 | `CSession`、`ChatGrpcClient`、`UserMgr` |
| **StatusServer** | 服务发现、负载均衡、token 签发 | `StatusServiceImpl::GetChatServer` |
| **VerifyServer** | 验证码生成与校验 | gRPC 服务 |
| **ResourceServer** | 文件上传下载 | `FileSystem`、`FileWorker` |

---

## 核心机制

### 多 Reactor 网络模型
`AsioIOServicePool` 管理 N 个 `io_context` 与工作线程，新连接按 round-robin 分发，
将单线程事件循环的吞吐瓶颈拆解到多核。

### 应用层协议分帧
自定义 4 字节头部（`msgid` 2B + `body_len` 2B），两阶段读取：
先读满头部解析包体长度、再按长度读满整包，以 `async_read` 保证整包语义。

### 服务发现与负载均衡
`StatusServiceImpl::GetChatServer` 从配置中心加载服务列表，
按连接数挑选最轻载节点，下发连接地址与一次性 token。

### 跨服消息路由
以 Redis 的 `usession_` 前缀维护 `uid → ChatServer` 映射，
发信方查表定位目标实例后经 gRPC 转发；重复登录由 `NotifyKickUser` 踢除旧连接。

### gRPC 连接池
`ChatConPool` 为每个对端实例维护 Stub 连接池，
以条件变量阻塞取用、归还即复用，避免高频跨服调用反复建连。

### 分布式锁
基于 `Redis SET NX EX` 实现，value 携带 UUID 标识持有者；
释放时以 Lua 脚本原子校验并删除，规避「锁过期后误删他人锁」的经典缺陷。

---

## 构建

### 依赖

```bash
sudo apt install -y \
  cmake g++ \
  libboost-all-dev \
  libjsoncpp-dev \
  libhiredis-dev \
  libmysqlcppconn-dev \
  protobuf-compiler libprotobuf-dev \
  libgrpc++-dev protobuf-compiler-grpc \
  redis-server
```

### 编译

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

> ⚠️ `message.pb.h` / `message.grpc.pb.h` 等 protobuf 生成文件未纳入版本控制，
> 需先由 `message.proto` 生成，或从原教程仓库获取。

### 运行

各服务需独立配置（`config.ini`），启动顺序建议：

```
MySQL / Redis → StatusServer → VerifyServer → ChatServer → GateServer
```

---

## 目录结构

```
server/
├── GateServer/        网关服务（HTTP 入口）
├── ChatServer/        聊天服务（长连接、跨服转发）
├── ChatServer2/       第二个聊天服务实例（演示跨服）
├── StatusServer/      状态服务（服务发现、token）
├── ResourceServer/    资源服务（文件上传下载）
└── VarifyServer/      验证服务（验证码）
```

**每个服务内部的通用模块**：

| 文件 | 作用 |
|---|---|
| `CServer` / `CSession` | 连接管理与会话 |
| `AsioIOServicePool` | 多 io_context 线程池 |
| `LogicSystem` | 消息分发与业务逻辑 |
| `DistLock` | Redis 分布式锁 |
| `RedisMgr` | Redis 连接池 |
| `MysqlDao` / `MysqlMgr` | 数据库访问 |
| `ConfigMgr` | 配置加载 |
| `Singleton` | 单例模板 |
| `const.h` | 常量与 Redis key 前缀 |

---

## 已知局限

- **逻辑处理单线程**：`LogicSystem` 用单线程消费消息队列，吞吐受限，可改为多线程 + 按会话哈希分片
- **`msg_len` 未校验上限**：代码中只校验了 `msg_id`，建议两者都校验
- **心跳扫描 O(n)**：遍历全部会话判断超时，可改用时间轮或最小堆
- **分布式锁无看门狗**：业务执行超过锁超时会破坏互斥，需续期机制或 fencing token

---

## 说明

本项目为学习性质实现，用于理解分布式 IM 的架构设计。
原教程版权归其作者所有。
