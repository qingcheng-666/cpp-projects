# 卿诚 · 项目集

C++ 后端方向的两个项目：**分布式聊天服务器** 与 **基于跳表的 KV 存储引擎**。

| 项目 | 方向 | 技术栈 |
|---|---|---|
| [chat-server](./chat-server/) | 网络层 + 分布式微服务 | C++ / Boost.Asio / epoll / gRPC / protobuf / MySQL / Redis |
| [kv-store](./kv-store/) | 存储引擎 | C++17 / 跳表 / WAL / CRC32 / RESP 协议 |

---

## chat-server

分布式即时通讯服务，微服务架构拆分为网关、聊天、状态、验证、资源五类服务。
覆盖**服务发现与负载均衡**、**跨服消息路由**、**gRPC 连接池**、**Redis 分布式锁**、
**多 Reactor 长连接承载**等核心机制。

→ [查看详情](./chat-server/README.md)

## kv-store

从零实现的轻量级 KV 存储引擎。跳表索引 + WAL 崩溃恢复 + RESP 协议网络层，
`redis-cli` 可直接连接。351 项测试全部通过。

关键数据：范围查询比哈希表全量扫描**快 4000 倍以上**；`kill -9` 实测**已确认写入零丢失**。

→ [查看详情](./kv-store/README.md)
