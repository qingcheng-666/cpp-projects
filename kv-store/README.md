# KV Store Engine · 基于跳表的键值存储引擎

从零实现的轻量级 KV 存储引擎：**跳表索引 + WAL 崩溃恢复 + RESP 协议网络层**，
支持任意字符串 key，`redis-cli` 可直接连接。

> 项目起点是 [代码随想录 Skiplist-CPP](https://github.com/youngyangyang04/Skiplist-CPP)（268 行教学代码）。
> 本仓库在理解其实现的基础上做了系统性改造：修复 7 处缺陷、补齐范围查询、实现 WAL 崩溃恢复、
> 增加网络层，有效代码 268 → 1384 行，配套 351 项测试全部通过。

---

## 特性

| 能力 | 说明 |
|---|---|
| **跳表索引** | 泛型 `SkipList<K, V>`，随机层高几何分布（期望层高 2），查询期望 O(log n) |
| **范围查询** | `scan` / `lower_bound` / `count_range`，一次 O(log n) 定位后顺序扫描 |
| **反向遍历** | `backward` 指针（同 Redis `zskiplistNode` 设计），O(log n + n) |
| **WAL 崩溃恢复** | CRC32 校验 + 先写日志后改内存，`kill -9` 实测零丢失 |
| **快照压缩** | 全量快照 + 清空 WAL，`.tmp` + 原子 `rename` 保证不会读到半个文件 |
| **并发控制** | `shared_mutex` 读写分离 + store 级锁保证日志与内存顺序一致 |
| **网络层** | epoll + RESP 协议子集，`redis-cli` 直连，支持 pipeline |
| **泛型 Key** | `KeyCodec<K>` 策略，支持 `int` 与 `std::string` |

---

## 快速开始

```bash
make server          # 编译服务端
./bin/kv_server 6380 /tmp/kvdata
```

另开终端：

```bash
redis-cli -p 6380 ping                     # PONG
redis-cli -p 6380 set user:1001:name alice # OK
redis-cli -p 6380 get user:1001:name       # alice
redis-cli -p 6380 scan user: "user:$(printf '\xff')"   # 前缀范围查询
redis-cli -p 6380 snapshot                 # 快照压缩 WAL
redis-cli -p 6380 info                     # 服务状态
```

### 崩溃恢复演示

```bash
make crash      # 3 轮 kill -9，验证已确认写入零丢失
make compare    # 对照：未加 WAL 的原版崩溃后数据全部丢失
```

---

## 测试

```bash
make test      # P1 缺陷修复对照（原版 vs 修复版）
make p2        # Key Codec          62 项
make p3        # 范围查询            33 项
make p4        # WAL 崩溃恢复        38 项
make p5        # 反向遍历            50 项
make resp      # RESP 协议          100 项
make p6        # 端到端（redis-cli） 36 项
make all-test  # 全部
```

**351 项测试全部通过**，编译 `-Wall` 零告警。

---

## 架构

```
客户端 redis-cli
      │  RESP over TCP
      ▼
┌─────────────────────────────────────┐
│  kv_server (epoll + RESP 协议)      │
│    SET/GET/DEL/SCAN/SNAPSHOT/INFO   │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│  KVStore                            │
│   ① 写 WAL（CRC32 校验）             │  ← 先落盘
│   ② 改跳表内存                       │  ← 再改内存
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│  SkipList                           │
│   forward[] 多层索引 + backward 指针 │
└─────────────────────────────────────┘
```

**核心保证**：先写日志落盘 → 再改内存 → 才向调用方确认。

---

## 文件说明

```
├── skiplist.h       跳表核心（泛型、范围查询、反向遍历）
├── kvstore.h        CRC32 + WAL + 快照 + KVStore
├── resp.h           RESP 协议编解码与增量式解析器
├── kv_server.cpp    epoll 服务器
├── codec.h          Key 序列化策略
├── makefile
├── tests/           全部测试
├── docs/            设计与改造文档
├── learn/           跳表入门 + 源码导读 + 可视化程序
└── original/        改造前的原版（用于对照）
```

---

## 实测数据

| 指标 | 数据 |
|---|---|
| 范围查询（100 万条取 100 条） | **25.8 µs**（哈希表全量扫描 108 ms，**快 4000 倍以上**） |
| 反向遍历（表尾 vs 表头） | 0.066 ms vs 0.060 ms（同量级，O(log n + n)） |
| `kill -9` 恢复 | 3 轮 missing=0 wrong=0 |
| 并发写正确性 | 2000/2000 无丢失，TSan 0 data race |

---

## 已知边界

- **未调用 `fsync`**：用的是 `fstream::flush()`，能抗进程被 `kill`（页缓存仍在），
  但**抗不了机器断电**。真正持久化需要 POSIX `fsync`
- **单线程事件循环**：未利用多核，可改为多 Reactor
- **WAL 未自动触发快照**：`_wal_threshold` 已定义但未实现自动压缩

---

## 许可

原项目为 GPL-3.0，本项目遵循同样许可。
原版代码版权归 [程序员Carl](https://github.com/youngyangyang04) 所有。
