# P2：Key Codec —— 让 key 支持任意字符串

> 代码：`fixed/codec.h`（新增）
> 改动：`fixed/skiplist.h`、`fixed/kvstore.h`、`fixed/kv_server.cpp`
> 测试：`fixed/tests/p2_codec_test.cpp`（62 项）、`fixed/tests/p2_redis_test.sh`（25 项）
> 一键运行：`cd fixed && make p2` / `make p2e2e`

---

## 一、为什么要做 P2

P6 做完后暴露了一个很别扭的问题：

```bash
$ redis-cli -p 6380 set username alice
ERR key must be an integer (this build uses int keys)
```

**你的 KV 服务只能存整数 key。**

这不像个真正的 KV 服务 —— 实际场景里 key 几乎都是字符串（`user:1001:name`、`session:abc`）。

### 根因：key 类型被写死了 3 处

| 位置 | 硬编码 |
|---|---|
| `skiplist.h` | `load_file()` 里 `insert_element(stoi(*key), *value)` |
| `kvstore.h` | `encode_key()` 用 `std::to_string`，`recover()` 用 `std::stoi` |
| `kv_server.cpp` | `parse_key()` 强制要求整数 |

原版 README 的「待优化」也承认了这一点：

> 跳表的key用int型，如果使用其他类型需要自定义比较函数，当然把这块抽象出来更好

**P2 就是把这块抽象出来。**

---

## 二、关键洞察：跳表本身不用改

这是 P2 最值得说的一点。

**跳表本来就是 `template<typename K, typename V>`，key 的比较用的是 `operator<`：**

```cpp
while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
```

- `K = int` → 走**数值比较**（`10 > 9`）
- `K = std::string` → 走**字典序比较**（`"10" < "9"`）

**两者天然都对，一行都不用改。**

真正卡住泛型化的只有**「序列化」**这一步 —— 把 key 变成字节存进文件/日志，以及从字节还原回来。原版在这里写死了 `stoi`/`to_string`。

所以 P2 的本质是：**把「序列化」从跳表里抽出来，做成可替换的策略。**

---

## 三、Codec 设计

```cpp
template<typename K>
struct KeyCodec;

// int：十进制字符串
template<>
struct KeyCodec<int> {
    static std::string encode(int k)          { return std::to_string(k); }
    static int         decode(const std::string& s);   // 严格校验
};

// string：原样
template<>
struct KeyCodec<std::string> {
    static std::string encode(const std::string& k) { return k; }
    static std::string decode(const std::string& s) { return s; }
};
```

**用起来就是两个函数：**

```cpp
std::string bytes = KeyCodec<K>::encode(key);   // 存盘/写日志时
K key            = KeyCodec<K>::decode(bytes);  // 加载/重放时
```

**主模板故意不定义** —— 想用自定义 key 类型（比如复合 key）必须显式特化，否则**编译期就报错**。这比运行期悄悄出错好。

### 一个容易搞混的点

**Codec 只管「字节表示」，不参与排序。**

```cpp
KeyCodec<int>::encode(10) == "10"
KeyCodec<int>::encode(9)  == "9"
// "10" < "9"  （字典序）
// 但跳表里 10 > 9  （因为比较的是 int 本身）
```

所以存成 `"10"` **不会**影响跳表内部的顺序。这是对的设计：**存储格式和排序语义解耦**。

反过来，如果你把 key 声明成 `std::string` 但存的是数字，那跳表就按字典序排 —— 这是使用者的选择，不是 bug。

---

## 四、三个实际改动

### 改动 1：`load_file()` 去掉 `stoi`

```cpp
// 原版（key 只能是 int）
insert_element(stoi(*key), *value);

// P2
insert_element(KeyCodec<K>::decode(key), value);
```

顺带做了两件事：
- **`std::string* key = new std::string()` 改成栈上对象** —— 原版手动 `new`/`delete` 完全没必要，还容易在异常路径泄漏
- **单行解码失败只跳过这一行**，不让整个加载失败（用 `try/catch`）

### 改动 2：分隔符切分从 `find` 改成 `rfind`（**修了一个真 bug**）

原版：

```cpp
*key   = str.substr(0, str.find(kDelimiter));       // 第一个冒号
*value = str.substr(str.find(kDelimiter)+1, ...);
```

**含冒号的 key 会被截断：**

```
"user:1001:alice"
   ↑ find 在这里切
key = "user"          ❌ 错
value = "1001:alice"  ❌ 错
```

改成 `rfind`（最后一个冒号）：

```
"user:1001:alice"
          ↑ rfind 在这里切
key = "user:1001"   ✅
value = "alice"     ✅
```

**为什么用最后一个而不是第一个？**
value 是用户完全无法控制的内容，key 的格式通常更有规律。用最后一个冒号能让「带命名空间的 key」正常工作。

> ⚠️ 但这只是原版**文本格式**的补救 —— 它仍然限制 value 里不能有冒号。
> 真正要支持任意字节必须用**长度前缀**，见改动 3。

### 改动 3：快照格式从「文本行」改成「长度前缀」（**这是必需的**）

原版快照格式是 `key\tvalue\n` 文本行：

```cpp
out << it->first << '\t' << it->second << '\n';
```

**string key 会把这个格式彻底破坏：**

| key | 后果 |
|---|---|
| `"a\tb"` | 被切成 `key="a"`, `value="b\t<真实value>"` ❌ |
| `"a\nb"` | 被拆成**两行**，完全错位 ❌ |

P2 改成**长度前缀二进制格式**，和 WAL 用同一套：

```
+--------+--------+--------+---------+---------+
| op(1B) | klen   | vlen   |  key    |  value  |
|        |  4B    |  4B    | klen B  | vlen B  |
+--------+--------+--------+---------+---------+
```

**长度前缀对任意字节都安全**，而且因为格式和 WAL 一致，读取逻辑能复用（`WALRecord::read_one`），避免两份实现走偏。

**代价**：快照不再是人类可读的。这是个真实的取舍 —— 要可读性还是要正确性。**选了正确性**。

---

## 五、测试结果

### 单元测试（62 项全过）

```bash
cd ~/Skiplist-CPP/fixed && make p2
```

| 组 | 内容 |
|---|---|
| A | `KeyCodec<int>` 编解码、往返一致性、边界值 |
| B | `KeyCodec<std::string>` 原样保留（含冒号/空格/中文/控制字符/emoji） |
| C | 非法输入抛异常（`""`、`"abc"`、`"12a"`、`"1.5"`、`"-"`） |
| D | 跳表用 string key：插入/查找/删除/字典序 |
| E | **含冒号的 key**（原版会截断的地方） |
| F | 特殊字符 key（空格、中文、emoji） |
| G | `KVStore<string,string>` 的 put/get/del/scan/覆盖 |
| H | **持久化往返**：快照 + WAL 恢复后 string key 完好 |
| I | **含 `\t` 和 `\n` 的 key**（旧快照格式必然坏掉的地方） |
| J | **int key 向后兼容**（数值序，不是字典序） |

**I 组是关键**：它专门验证 `key\twith\ttab` 和 `key\nwith\nnewline` 这类 key 能正确持久化 —— 这正是旧格式会失败的地方。

### 端到端（25 项全过）

```bash
make p2e2e
```

```
--- 核心：现在 key 可以是任意字符串了 ---
  ✅ SET username alice
  ✅ GET username
  ✅ SET 中文键 中文值
  ✅ GET 中文键
  ✅ SET 'key with space' v
--- 含冒号的 key（原版 find 会截断，现改为 rfind）---
  ✅ SET user:1001:name bob
  ✅ GET user:1001:name
  ✅ SET user:1001:age 30
  ✅ GET user:1001:age
--- 覆盖语义 ---
  ✅ 同一 key 写两次，取最后一次
--- 字典序范围查询 ---
  ✅ COUNT user: 前缀（3 个带冒号的）
  ✅ COUNT user 前缀（含 username 共 4 个）
--- 持久化 ---
  ✅ 重启后 GET username / user:1001:name / 中文键 / WAL 中的 key
--- 旧限制已移除 ---
  ✅ 非整数 key 不再报 'must be an integer'
```

---

## 六、一个真实的坑：前缀查询的区间怎么写

做 P2 时我**连续写错了两次测试期望**，因为搞混了 ASCII 顺序。

想取所有以 `user` 开头的 key，区间**不能**随便写 `["user", "user;")`。

**实测字典序**（用程序打出来的，不是靠记忆）：

```
':' = 58    ';' = 59    'n' = 110

user:1001:age
user:1001:name
user:1002:name
user;              ← 上界在这里
username           ← 被漏掉了！
```

**`'n'`(110) > `';'`(59)，所以 `username` 排在 `user;` 之后**，用 `user;` 当上界会漏掉它。

### 正确写法

把最后一个字节 +1，得到「刚好大于所有 `prefix*` 的字符串」：

```cpp
std::string prefix_end(const std::string& prefix) {
    return prefix + '\xff';      // 比任何 ASCII 都大
}
```

然后 `scan("user", prefix_end("user"))` 就能命中全部 4 个。

### 这组测试本身就是教学材料

我把它保留在测试里，作为**对照**：

```bash
expect "3" "COUNT [user, user;) 只命中带冒号的 3 个（漏掉 username）" count user "user;"
```

**这条断言故意用错的上界**，用来演示"上界选错会漏数据"。

> **面试价值**：这说明你理解**字典序的字节语义**，而不只是会调 API。
> 真实系统里这种 bug 很常见 —— 数据明明写进去了，范围查询就是查不到。

---

## 七、面试话术

> "原版把 key 类型写死成 int，`SET username alice` 直接报错。我做了 Codec 抽象，让 key 支持任意字符串。
>
> **关键洞察是跳表本身不用改** —— 它本来就是 `template<typename K, typename V>`，比较用 `operator<`，int 走数值比较、string 走字典序比较，天然都对。真正卡住的只有**序列化**这一步。
>
> 所以我抽了个 `KeyCodec<K>` 策略：int 走十进制字符串，string 原样。主模板故意不定义，自定义类型必须显式特化，这样**编译期就能拦住**而不是运行期出错。
>
> **过程中修了一个真 bug**：原版用 `find`（第一个冒号）切分 key:value，导致含冒号的 key 被截断 —— `user:1001:alice` 会变成 `key="user"`。改成 `rfind` 后正确。
>
> **还有个更根本的问题**：原版快照是 `key\tvalue\n` 文本行格式，string key 里只要有 `\t` 或 `\n` 就会破坏格式、恢复时数据错位。我改成了**长度前缀二进制格式**，和 WAL 复用同一套读写逻辑。代价是快照不再人类可读 —— 我选了正确性。
>
> **测试上我专门验证了这些边界**：含冒号的 key、含 `\t`/`\n` 的 key、中文、emoji、空 key，62 项单元测试 + 25 项 redis-cli 端到端全过。
>
> 另外我做前缀范围查询时踩了个坑：`['user', 'user;')` 会漏掉 `username`，因为 `'n'`(110) > `';'`(59)。正确的写法是把前缀最后一个字节 +1。**我把这个错误写法保留在测试里当对照。**"

---

## 八、当前项目规模

| 文件 | 总行数 | 有效代码 |
|---|---|---|
| `skiplist.h` | 819 | ~420 |
| `kvstore.h` | 585 | ~310 |
| `resp.h` | 313 | 165 |
| `kv_server.cpp` | 592 | ~390 |
| `codec.h` | 128 | ~75 |
| **合计** | **2437** | **~1360** |

### 全部测试

| 阶段 | 测试数 | 状态 |
|---|---|---|
| P1 缺陷修复 | 7 项 | ✅ |
| **P2 Codec** | **62 项** | ✅ |
| P3 范围查询 | 33 项 | ✅ |
| P4 WAL | 38 项 | ✅ |
| RESP 协议 | 81 项 | ✅ |
| P6 集成 | 36 项 | ✅ |
| **P2 端到端** | **25 项** | ✅ |
| TSan 并发 | 0 data race | ✅ |
| **合计** | **282 项** | **全部通过** |

---

## 九、现在可以这样用了

```bash
cd ~/Skiplist-CPP/fixed
make server && ./bin/kv_server 6380 /tmp/kvdata
```

```bash
redis-cli -p 6380 set user:1001:name alice
redis-cli -p 6380 get user:1001:name          # alice
redis-cli -p 6380 set 中文键 中文值
redis-cli -p 6380 get 中文键                   # 中文值

# 前缀查询（注意上界写法）
redis-cli -p 6380 scan user: "user:$(printf '\xff')"

redis-cli -p 6380 snapshot                    # 快照压缩 WAL
redis-cli -p 6380 info
```

---

## 十、下一步

| | 内容 | 价值 |
|---|---|---|
| **P5** | backward 指针 | `scan_reverse` 变成真 O(log n) |
| **P7** | SSTable + Compaction | 迷你 LevelDB |
| **P6+** | 多 Reactor / 线程池 | 利用多核（VM 上看不出效果） |
| **补** | 总览 README | 5 份文档缺一个入口 |

**建议先补总览 README** —— 现在 `docs/` 有 6 份文档，但没有入口。新读者（或面试官）不知道从哪看起。

**然后 P5** —— 它是最小改动、能讲清楚的一个点，而且补上了 P3 留下的"诚实局限"。
