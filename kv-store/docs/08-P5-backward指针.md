# P5：backward 指针 —— 让反向遍历变成 O(log n + n)

> 代码：`fixed/skiplist.h`（搜索 `[P5]`）
> 测试：`fixed/tests/p5_reverse_test.cpp`（50 项）
> 一键运行：`cd fixed && make p5`

---

## 一、P5 解决的是 P3 留下的一个"诚实局限"

P3 加范围查询时，我实现了一个 `scan_reverse`，但在文档里**主动标注了它的局限**：

> 原版跳表只有 `forward` 指针，没有 `backward`，所以反向遍历做不到 O(log n + k)。
> 这里的实现是「先正向收集再截取」，复杂度 O(range)，是个诚实的简化版。

**P5 就是把这个"诚实的简化版"变成真正的实现。**

### 问题有多严重

P3 的 `scan_reverse(start_key, n)` 实现是：

```cpp
// P3：从头开始正向扫到 start_key，全部收集，再截取最后 n 条
for (Node<K,V>* p = _header->forward[0]; p && p->get_key() <= start_key; p = p->forward[0])
    out.emplace_back(...);
if (out.size() > n) out.erase(out.begin(), out.end() - n);
std::reverse(out.begin(), out.end());
```

**如果 `start_key` 在 100 万条数据的末尾**，要取最后 100 条：

- P3 的做法：**正向扫 100 万次**，再截取
- 正确做法：一次 `O(log n)` 定位 + 走 100 步

**差了 4 个数量级。**

---

## 二、解法：给 level 0 加双向链表

### 为什么只加在 level 0

```cpp
Node<K, V> **forward;      // 原有：每层一个后继
Node<K, V>* backward;      // [P5] 新增：level 0 上的前驱
```

**反向遍历只需要「按 key 降序走一遍」，而 level 0 含有全部节点。**

高层是索引，反向走它们没有意义（而且要维护每一层的 backward，成本高、收益零）。

**这和 Redis 的做法一致** —— Redis 的 `zskiplistNode` 也是只有一个 `backward` 指针：

```c
typedef struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;      // ← 只有这一个
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned long span;
    } level[];
} zskiplistNode;
```

**代价**：每个节点多 8 字节。100 万节点约多 8MB。

### 新的 `scan_reverse`

```cpp
std::vector<std::pair<K, V>> scan_reverse(const K& start_key, size_t n) const {
    std::shared_lock<std::shared_mutex> lock(_rwlock);
    std::vector<std::pair<K, V>> out;
    out.reserve(n);

    // ① O(log n) 定位到「最后一个 key <= start_key 的节点」
    Node<K, V>* p = upper_bound(start_key);

    // ② 沿 backward 往回走 n 步（结果天然是降序，不需要再 reverse）
    while (p != nullptr && out.size() < n) {
        out.emplace_back(p->get_key(), p->get_value());
        p = p->backward;
    }
    return out;
}
```

**顺带新增了 `upper_bound`**（最后一个 `<= target` 的节点），和 P3 的 `lower_bound`（第一个 `>= target`）相对。

---

## 三、最容易出错的地方：删除时的 backward 重连

### 插入时（简单）

```cpp
// 新节点的前驱 = update[0]（level 0 上的前驱）
inserted_node->backward = (update[0] == _header) ? nullptr : update[0];
// 新节点的后继的 backward 要指向新节点
if (inserted_node->forward[0] != nullptr) {
    inserted_node->forward[0]->backward = inserted_node;
}
```

**注意 `update[0] == _header` 的情况**：头节点不存业务数据，它的 `backward` 恒为 `nullptr`。所以第一个数据节点的 `backward` 必须是 `nullptr`（正好是它的初始值），**不能指向 header**。

### 删除时（容易错）

```cpp
// [P5] 必须在 delete current 之前做 —— 删完就读不到 forward[0] 了
if (current->forward[0] != nullptr) {
    current->forward[0]->backward = current->backward;
}
```

**这一行是关键**：让后继的 `backward` **跳过** current，直接指向 current 的前驱。

```
删除前：  A <──> B <──> C
          A.forward = B,  B.backward = A
          B.forward = C,  C.backward = B

删除 B：  ① C.backward = B.backward = A
          ② A.forward  = B.forward  = C   （原有的 forward 摘链循环处理）
删除后：  A <──> C
```

**如果忘了第 ① 步**，`C.backward` 还指向已 `delete` 的 B → **悬空指针** → 反向遍历时读到野内存 → 崩溃或数据错乱。

**如果顺序反了**（先 `delete current` 再改 `forward[0]->backward`），那就是**在已释放的内存上读指针** —— use-after-free。

---

## 四、怎么验证 backward 没被破坏

### 核心不变式：backward 链与 forward 链必须互为逆

```cpp
// 正向: h -> n1 -> n2 -> ... -> nk -> null
// 反向: nk -> ... -> n2 -> n1 -> null
static bool backward_consistent(const SL& sl) {
    auto fwd = forward_keys(sl);      // 正向收集
    auto bwd = backward_keys(sl);     // 反向收集
    std::reverse(bwd.begin(), bwd.end());
    return fwd == bwd;
}
```

**这个不变式在每次插入/删除后都必须成立。** 测试里每个场景都断言它。

### 测试覆盖（50 项）

| 组 | 内容 |
|---|---|
| A | backward 链表完整性、第一个节点 backward 为 nullptr |
| B | 升序/降序/随机/往中间插入后仍一致 |
| C | **删除后**：删中间、删最大、删最小 |
| D | **极端场景**：删光、只剩一个、500 次随机插删、连续删相邻一段 |
| E | `scan_reverse` 语义与边界（start 不存在/超界/小于所有/空表/n 超总数） |
| F | 反向迭代器（`rbegin`/`rend`、`operator->`、后置 ++） |
| G | **与 `std::map` 对拍**：3000 次随机操作后全量反向比对 + 50 次随机 `scan_reverse` 比对 |
| H | **复杂度验证** |

**D 组的「连续删相邻一段」是最狠的** —— 它连续删除 `5..14`，考验 backward 反复重连：

```
删之前: 0 1 2 3 4 [5 6 7 8 9 10 11 12 13 14] 15 16 17 18 19
删之后: 0 1 2 3 4 15 16 17 18 19
反向应为: 19 18 17 16 15 4 3 2 1 0
```

**G 组的对拍最有说服力** —— 用 STL 当"标准答案"，随机 3000 次操作后逐条比对。

---

## 五、复杂度验证（这是 P5 的核心价值）

```bash
cd ~/Skiplist-CPP/fixed && make p5
```

在 **100 万条**数据的表上，各取 100 条：

| 取数位置 | 耗时 |
|---|---|
| 表尾（key ≈ 999999） | **0.066 ms** |
| 表头（key ≈ 200） | **0.060 ms** |

**两者几乎相同。**

这就是 O(log n + n) 的证据：**耗时不再依赖 `start_key` 在表中的位置**。

对比 P3 的旧实现：从表尾取数要先正向扫 100 万次，而表头只需扫 200 次 —— **差 5000 倍**。

> 测试里用 `ratio < 20.0` 作为判据（而不是严格相等），
> 是为了避免虚拟机调度抖动导致误判。只要不是**数量级差异**就算通过。

---

## 六、面试话术

> "P3 我做范围查询时，反向遍历是 O(range) 的 —— 因为原版跳表只有 forward 指针。我在文档里主动标注了这个局限。
>
> P5 把它修掉了：**给 level 0 加一个 backward 指针**，反向遍历就变成 O(log n + n) —— 一次 `upper_bound` 定位到起点，再沿 backward 走 n 步。
>
> **只加在 level 0 是有意的**：反向遍历只需要降序走一遍，而 level 0 含全部节点。高层是索引，反向走没意义。这也和 Redis 的 `zskiplistNode` 一致 —— 它也只有一个 backward。
>
> **最容易出错的是删除**：必须在 `delete current` **之前**把后继的 backward 重连到 current 的前驱，否则后继会指向已释放的内存。顺序反了就是 use-after-free。
>
> **验证上我定义了一个不变式**：backward 链和 forward 链必须互为逆。每次插入/删除后都断言它。测试里用 `std::map` 对拍 —— 3000 次随机插删后全量反向比对，再加 50 次随机 `scan_reverse` 比对。
>
> **复杂度我做了实测**：100 万数据下，从表尾取 100 条用 0.066ms，从表头取 100 条用 0.060ms —— **几乎相同，证明不再依赖 start_key 的位置**。P3 的旧实现在表尾要扫 100 万次。"

---

## 七、新增的 API

| API | 语义 | 复杂度 |
|---|---|---|
| `upper_bound(k)` | 最后一个 `key <= k` 的节点 | O(log n) |
| `scan_reverse(k, n)` | <= k 的最大 n 条，降序 | **O(log n + n)**（原 O(range)） |
| `rbegin()` / `rend()` | 反向迭代器 | O(log n) 起 |

```cpp
SkipList<int, std::string> sl(18);
// ... 插入数据 ...

// 反向迭代
for (auto it = sl.rbegin(); it != sl.rend(); ++it)
    std::cout << it->first << " " << it->second << "\n";

// 取最大的 10 条
auto top10 = sl.scan_reverse(INT32_MAX, 10);
```

---

## 八、诚实说明：还有什么没做

**1. 反向迭代器不加锁**

和 P3 的正向迭代器一样，`rbegin`/`rend` 持有裸指针，并发删除时会悬空。**要用安全版本请用 `scan_reverse()`**（内部加 `shared_lock` 返回快照）。

**2. 只有 level 0 有 backward**

所以**不能**反向利用高层索引做"反向跳跃"。Redis 也是这样，够用了。

**3. `upper_bound` 是 O(log n)，不是 O(1)**

它走的是标准跳表查找路径。对于"取最后 n 条"这种需求，其实可以维护一个 tail 指针做到 O(1) 定位 —— 但那是过度优化，没做。

---

## 九、下一步

P5 完成后，`02-魔改路线图.md` 里的 P1–P6 主线就闭环了。剩下：

| | 内容 | 说明 |
|---|---|---|
| **P7** | SSTable + Compaction | 迷你 LevelDB，加分项 |
| **P8** | gtest / 火焰图 | 工程化 |

**这两个是可选项，不是必需品。** 当前项目（1300+ 行有效代码、351 项测试）已经足够撑起简历上的一个项目。
