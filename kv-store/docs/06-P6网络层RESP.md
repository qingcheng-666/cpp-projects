# P6：网络层 —— RESP 协议 KV 服务器

> 代码：`fixed/resp.h`（313 行）、`fixed/kv_server.cpp`（588 行）
> 测试：`fixed/tests/resp_test.cpp`（81 项）、`fixed/tests/p6_redis_test.sh`（35 项）
> 压测：`fixed/tests/p6_bench.sh`
> 一键运行：`cd fixed && make p6` / `make p6bench` / `make run`

---

## 一、为什么 P6 是"两个项目串成一个故事"的关键

```
聊天室项目（恋恋风辰）              本项目（Skiplist-CPP）
├── epoll / Reactor                 ├── 跳表数据结构
├── 线程池                          ├── shared_mutex 并发控制
├── 协议设计                        ├── WAL 持久化 + 崩溃恢复
├── 业务逻辑                        ├── 范围查询
└── 网络层 ✅                       └── 存储层 ✅
              ↘                ↙
            分布式 KV 存储服务
```

**你的聊天室项目积累了网络层经验，这个项目积累了存储层经验。P6 把两者接起来。**

而且这是**能现场演示**的东西 —— `redis-cli` 直连你的服务器，输入 `SET`/`GET`/`SCAN`，屏幕上有反馈。

---

## 二、为什么选 RESP 协议

RESP = REdis Serialization Protocol。

| 理由 | 说明 |
|---|---|
| **有现成客户端** | `redis-cli` 可以直接连，演示效果拉满 |
| **协议简单** | 文本 + 长度前缀，容易实现正确 |
| **可验证** | 能用 `redis-benchmark` 压测，用官方工具验证兼容性 |
| **面试加分** | 可以说"我实现了 Redis 协议的一个子集" |

**自己发明协议是最差的选择** —— 没有现成客户端，没法验证，演示时只能自己写个客户端，说服力大打折扣。

### 用到的 5 种 RESP 类型

```
Simple String   +OK\r\n
Error           -ERR unknown command\r\n
Integer         :1000\r\n
Bulk String     $6\r\nfoobar\r\n          ($-1\r\n 表示 nil)
Array           *2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
```

---

## 三、核心难点：TCP 是字节流，没有消息边界

**这是整个 P6 最容易出 bug 的地方，也是面试高频考点。**

一次 `read()` 可能拿到：

| 情况 | 例子 |
|---|---|
| **半条命令** | `"*2\r\n$3\r\nGE"` |
| **一条半命令** | `"*2\r\n...\r\n*1\r\n$4\r\nPI"` |
| **正好 N 条** | `"*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n"` |

所以必须实现一个**增量式状态机解析器**：把收到的字节喂进去，能解析几条就返回几条，剩下的留在缓冲区等下次。

### 实现

```cpp
class Parser {
    void feed(const char* data, size_t len);   // 收到数据就喂
    bool next(Command& out);                    // 能解析一条就取一条
private:
    std::string _buf;      // 未解析完的字节留在这里
};
```

**关键设计**：解析成功才消费缓冲区；不完整就原样保留（下次重试）。

### 必须区分「不完整」和「协议错误」

```cpp
bool parse_one(size_t& pos, Command& out) {
    ...
    if (p + need > _buf.size()) return false;   // 不完整 → 等数据
    if (buf[p] != '$') return set_error(...);   // 协议错误 → 关连接
}
```

- **不完整** → 继续等数据（正常情况）
- **协议错误** → 关闭连接（否则恶意数据会撑爆内存）

**混淆这两者会导致**：要么把正常的分片当成错误断开连接，要么被恶意客户端用畸形数据把内存吃光。

### 实测验证

`resp_test.cpp` 里最关键的一组测试是**逐字节喂入**：

```cpp
for (size_t i = 0; i < full.size(); i++) {
    p.feed(full.data() + i, 1);          // 一次只喂 1 个字节
    if (p.next(c)) {
        CHECK(i == full.size() - 1, "应恰好在收到最后一个字节时解析成功");
        break;
    }
    CHECK(!p.has_error(), "中途不应报协议错误");
}
```

**81 项测试全过**，覆盖：完整命令、逐字节分片、粘包（一次多条）、半条补齐、空参数、二进制安全（value 含 `\r\n` 和 `\0`）、协议错误、内联命令。

---

## 四、写路径的坑：部分写（partial write）

`write()` **不保证一次写完所有数据** —— 内核发送缓冲区满了就只写一部分。

```cpp
void flush_output(conn) {
    while (conn->has_pending_output()) {
        ssize_t n = ::write(fd, buf + sent, remain);
        if (n > 0) { sent += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            mod_epoll(fd, EPOLLIN | EPOLLOUT);   // 注册可写事件，等下次
            return;
        }
    }
    // 全写完，取消 EPOLLOUT 关注
}
```

**漏了这段会导致**：客户端收到的响应被截断，或者连接卡死。

---

## 五、epoll 服务器结构

```
main thread:
    epoll_wait()
      ├── listen_fd 可读 → accept() 循环（一次尽量多 accept）
      └── 连接 fd 可读  → read() 循环直到 EAGAIN
                        → parser.feed() → drain() 取命令
                        → execute() → 写回响应
```

**几个细节**：
- `TCP_NODELAY`：KV 都是小包往返，禁用 Nagle 降低延迟
- `SIGPIPE` 忽略：向已关闭连接写数据时不要被信号杀死
- 非阻塞 + 循环读到 `EAGAIN`：水平触发下减少 `epoll_wait` 次数

---

## 六、支持的命令

| 命令 | 返回 | 来源 |
|---|---|---|
| `PING` | `+PONG` | — |
| `SET key value` | `+OK` | P4 的 `put`（含 WAL） |
| `GET key` | `$len...` / `$-1` | P3 的 `seek` |
| `DEL key...` | `:n` | P4 的 `del` |
| `EXISTS key` | `:1` / `:0` | — |
| `KEYS` | 数组 | P3 的 `scan` |
| **`SCAN begin end`** | 数组 | **P3 的范围查询** |
| **`COUNT begin end`** | `:n` | **P3 的 `count_range`** |
| `DBSIZE` | `:n` | — |
| **`SNAPSHOT`** | `+OK` | **P4 的快照压缩** |
| `INFO` | 数组 | 服务状态 |
| `ECHO msg` | bulk | 客户端兼容 |
| `SELECT 0` | `+OK` | 客户端兼容 |
| `CONFIG` | 空数组 | redis-benchmark 兼容 |
| `COMMAND` | 空数组 | redis-cli 兼容 |
| `QUIT` | `+OK` | — |

**注意 `SCAN`/`COUNT`/`SNAPSHOT`** —— 这三个命令把 P3 和 P4 的能力**暴露到了网络上**，这是"把前面的工作串起来"的体现。

---

## 七、调试过程中踩的 4 个真实坑

这些都是**只有真跑才会发现**的问题，每个都值得记住：

### 坑 1：命令名大小写不敏感

第一次用 `redis-cli` 测试，**所有命令都返回 `unknown command`**：

```
$ redis-cli -p 6380 ping
ERR unknown command 'ping'
```

原因：我的分发代码比较大写 `"PING"`，但 **redis-cli 实际发的是小写 `"ping"`**。Redis 命令名是**大小写不敏感**的。

修法：统一转大写再分发。

### 坑 2：`redis-cli --pipe` 发空行

管道模式报 `errors: 1`。抓包看 `redis-cli --pipe` 实际发的字节：

```
b'set 901 a\r\nset 902 b\r\nset 903 c\r\nget 902\r\n\r\n*2\r\n$4\r\nECHO\r\n$20\r\n...'
                                          ^^^^
                                          空行！
```

**它在发完命令后、发 `ECHO` 前会发一个裸 `\r\n`**。我的内联解析器把这个空行当成协议错误了。

修法：空行跳过，不作为错误。**但要注意**：`parse_inline` 返回空 args 时，上层 `execute()` 会访问 `args[0]` → **越界**。所以在 `next()` 里直接跳过空命令，绝不把空 args 交给上层。

### 坑 3：`ECHO` 是必需的

`redis-cli --pipe` 结束时会发 `ECHO <20字节随机magic>` 来确认与服务端同步。不实现它，管道模式永远报错。

### 坑 4：`CONFIG` 是必需的

`redis-benchmark` 启动时会发 `CONFIG GET save` 探测配置。不实现会打印警告（虽然不影响压测）。

**教训：实现"兼容某协议"时，除了核心命令，还要实现客户端工具用的探测命令。**

---

## 八、并发安全：P6 暴露了 P4 的隐藏 bug

**这是 P6 最有价值的发现。**

P4 的 `KVStore::put()` 做两件事：
1. `_wal.append(...)` —— 写日志
2. `_list.upsert_element(...)` —— 改内存

跳表内部有 `shared_mutex` 保护，**但 WAL 是裸 `ofstream`**。多线程同时 `append` 会竞争。

### TSan 实测确认

```
WARNING: ThreadSanitizer: data race
  Write of size 8 ... by thread T2:
    #2 std::basic_streambuf::xsputn(...)     ← ofstream::write 内部
  Previous write by thread T1:
    #2 std::basic_streambuf::xsputn(...)
```

**后果**：日志内容交错错乱 → CRC 全废 → 恢复直接失败。

### 两层修复

**第 1 层：WAL 内部加锁**

```cpp
bool append(char op, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(_mtx);   // 整个「编码+写+flush」原子化
    ...
}
```

**第 2 层（更重要）：保证 WAL 顺序与内存生效顺序一致**

光给 WAL 加锁还不够。如果不加 store 级锁，可能出现：

```
T1 写 WAL  →  T2 写 WAL  →  T2 改内存  →  T1 改内存
```

结果：**内存里是 T1 的值，但 WAL 顺序说 T2 最后写** → 重放结果错误。

所以「写日志 + 改内存」必须是**一个原子步骤**：

```cpp
bool put(int key, const V& value) {
    std::lock_guard<std::mutex> lk(_write_mtx);   // ← store 级锁
    if (!_wal.append(...)) return false;
    _list.upsert_element(key, value);
    return true;
}
```

### 验证

```
=== TSan 运行 ===
data race 数量: 0
put 成功: 2000 / 2000
重放记录数: 2000
恢复后 size: 2000
丢失 key 数: 0
✅ 无丢失
```

**锁顺序**：`_write_mtx` → `_mtx`，全局一致，无死锁风险。

> **面试价值极高**：这说明"加了锁"和"加对了锁"是两件事。P4 看起来有并发保护（跳表有锁），实际上写路径有竞争 —— 只有真正做多线程压测才会暴露。

---

## 九、实测结果

### 功能验证（35 项全过）

```
--- 基本命令 ---
  ✅ PING / SET / GET / EXISTS / DBSIZE / DEL
  ✅ GET 100 (UTF-8 中文)
  ✅ GET 999 (不存在→nil)
--- 命令名大小写不敏感 ---
  ✅ SET / GET / GeT 混合大小写
--- 二进制安全 ---
  ✅ value 含空格
--- P3 范围查询能力 ---
  ✅ SCAN 20 40 (左闭右开)
  ✅ COUNT 20 50
--- P4 持久化能力 ---
  ✅ SNAPSHOT 触发快照
  ✅ 快照后 WAL 已清空
--- 管道（粘包处理）---
  ✅ 管道 4 条命令，errors: 0
--- 错误处理 ---
  ✅ 非整数 key 被拒绝并给出清晰错误
  ✅ 参数个数错误被拒绝 / 未知命令被拒绝
--- 重启后数据恢复 ---
  ✅ WAL 中的写入已恢复
  ✅ 快照中的中文值已恢复
  ✅ 重启后 size 从 11 增长到 12
```

### 压测（redis-benchmark）

```
200 并发连接，25 万条命令，无崩溃
内存占用 3.9 MB
压测后数据完整恢复（200 条）
```

---

## 十、关于吞吐：一个诚实的对照实验

初测吞吐只有 **~800 QPS**，看起来很低。我做了对照实验，**启动原生 `redis-server` 用同样的命令测**：

| | 我的 kv_server | 原生 Redis 6.0.16 |
|---|---|---|
| PING 吞吐（50 并发） | 809 QPS | 832 QPS |
| 单客户端 PING 往返 | 2520 µs | 2962 µs |

**结论：我的服务器和原生 Redis 性能基本一致，甚至单客户端延迟略低。**

所以 ~800 QPS **不是我的代码瓶颈，而是这台 VM 的 loopback 网络栈上限** —— localhost 往返 2.5ms 本身就异常慢（正常应该是几十微秒），这是虚拟化网络栈的限制。

**这个对照实验本身就是很好的面试素材**：

> "我测出来 800 QPS，觉得太低，就去启动原生 Redis 做对照 —— 结果它也只有 832 QPS，单客户端往返延迟我的还略低。所以瓶颈在虚拟机网络栈，不在我的实现。**做性能优化前先确认瓶颈在哪，不要盲目优化代码。**"

**这比"我的服务有 XX 万 QPS"更可信** —— 因为你有对照、有分析、知道边界在哪。

### 真正的优化点（如果要在物理机上提速）

1. **多线程 accept + 多 Reactor**：目前是单线程事件循环，没利用多核（`线程数: 1`）
2. **组提交（group commit）**：目前每条 `put` 都 flush WAL，可以合并
3. **线程池处理命令**：把 `execute()` 放到工作线程池
4. **`SO_REUSEPORT` + 多进程**：Redis 6 的做法

---

## 十一、面试话术

> "我给 KV 存储引擎加了网络层，**实现了 RESP 协议的一个子集**，所以 `redis-cli` 可以直接连我的服务。
>
> 核心难点是 **TCP 是字节流、没有消息边界**。一次 `read` 可能拿到半条命令、一条半、或正好 N 条。我实现了一个增量式解析器，把收到的字节喂进去，能解析几条取几条，剩下的留在缓冲区。
>
> 关键是**区分「数据不完整」和「协议错误」** —— 前者继续等数据，后者关闭连接，否则恶意数据会把内存吃光。我用**逐字节喂入**的方式测试，验证在收到最后一个字节时才解析成功，中途不报错。
>
> 写路径上处理了**部分写**：`write()` 不保证一次写完，内核缓冲满了要注册 `EPOLLOUT` 等下次，否则响应会被截断。
>
> **调试时踩了几个真实的坑**：`redis-cli` 发的命令名是小写的，我没做大小写转换导致所有命令都 unknown；`redis-cli --pipe` 会在命令后发一个空行，我把空行当协议错误了；还发现它结束时会发 `ECHO <magic>` 做同步校验，不实现就永远报错。
>
> **最有价值的发现是并发 bug**：P4 的 `put()` 是「写 WAL + 改内存」两步，跳表有锁但 WAL 是裸 `ofstream`。我用 TSan 跑出了数据竞争。修复分两层 —— 一是给 WAL 加锁，二是**加 store 级锁保证 WAL 顺序与内存生效顺序一致**，否则会出现「内存是 T1 的值、WAL 说 T2 最后写」的重放错误。
>
> **性能上我做了对照实验**：初测 800 QPS 觉得低，就启动原生 Redis 用同样命令测，结果它也只有 832 QPS，单客户端延迟我的还略低。所以瓶颈在虚拟机网络栈。**这让我知道优化前要先确认瓶颈在哪。**"

---

## 十二、当前项目规模

| 文件 | 总行数 | 有效代码 |
|---|---|---|
| `skiplist.h` | 784 | 405 |
| `kvstore.h` | 522 | 283 |
| `resp.h` | 313 | 165 |
| `kv_server.cpp` | 588 | 396 |
| **合计** | **2207** | **1249** |

从原版的 **268 行有效代码**长到了 **1249 行** —— 接近 5 倍，而且每一行都知道为什么存在。

### 全部测试

| 阶段 | 测试数 | 状态 |
|---|---|---|
| P1 缺陷修复 | 7 项 | ✅ |
| P3 范围查询 | 33 项 | ✅ |
| P4 WAL | 38 项 | ✅ |
| RESP 协议 | 81 项 | ✅ |
| P6 集成 | 35 项 | ✅ |
| **合计** | **194 项** | **全部通过** |

---

## 十三、下一步

| | 内容 | 价值 |
|---|---|---|
| **P2** | Codec 抽象 | 让 key 支持 string（目前硬编码 int） |
| **P5** | backward 指针 | `scan_reverse` 变成真 O(log n) |
| **P7** | SSTable + Compaction | 迷你 LevelDB |
| **P6+** | 多 Reactor / 线程池 | 利用多核，真正提速 |

**P2 现在优先级变高了** —— 因为网络服务里 key 只能是整数，用起来很别扭。加上 Codec 之后 `SET username alice` 就能用了。
