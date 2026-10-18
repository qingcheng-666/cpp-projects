/* ============================================================================
 * 跳表可视化教学程序（零基础友好）
 * ----------------------------------------------------------------------------
 * 这不是原项目代码，而是一个「数据结构和算法与原版完全一致、
 * 但把内部结构暴露出来」的教学版本，用来把跳表长什么样、查找怎么走，
 * 直接画在屏幕上。
 *
 * 原版对应关系（对照着看）：
 *   本文件                     原项目 skiplist.h
 *   ----------------------     --------------------------------
 *   struct Node                template<typename K,typename V> class Node
 *   forward (std::vector)      forward (裸数组 Node** + memset)
 *   random_level()             get_random_level()
 *   insert()                   insert_element()
 *   search()                   search_element()
 *   erase()                    delete_element()
 *
 * 编译运行：
 *   g++ skiplist_visual.cpp -o visual --std=c++11
 *   ./visual
 * ============================================================================
 */
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Node：每个节点带一个「层高」，以及一个指针数组 forward
//       forward[i] 表示「本节点在第 i 层的下一个节点」
//
// 原版写法： Node<K,V>** forward = new Node*[level+1]; memset(...)
// 这里用 std::vector 替代裸数组：语义完全一样，但不用手动管理内存，更好懂
// ---------------------------------------------------------------------------
struct Node {
    int key;
    std::vector<Node*> forward;  // 长度 = level + 1

    Node(int k, int level) : key(k), forward(level + 1, nullptr) {}
};

class SkipList {
public:
    explicit SkipList(int max_level)
        : max_level_(max_level), cur_level_(0), count_(0) {
        // 头节点：不存业务数据，层高固定为 max_level
        header_ = new Node(-1, max_level);
    }

    ~SkipList() {
        // 迭代析构（原版是递归的，100 万节点会爆栈，见 docs/01）
        Node* p = header_->forward[0];
        while (p != nullptr) {
            Node* next = p->forward[0];
            delete p;
            p = next;
        }
        delete header_;
    }

    int size() const { return count_; }

    // -----------------------------------------------------------------------
    // 随机层高：k 从 1 开始，每次有 1/2 概率再升一层，封顶 max_level
    //   P(level = 1) = 1/2,  P(level = 2) = 1/4,  P(level >= k) = 2^-(k-1)
    //   期望层高 = 1 + 1/2 + 1/4 + ... = 2
    // -----------------------------------------------------------------------
    int random_level() {
        int k = 1;
        while (rand() % 2) k++;
        return k < max_level_ ? k : max_level_;
    }

    // -----------------------------------------------------------------------
    // 插入
    // -----------------------------------------------------------------------
    void insert(int key) {
        Node* cur = header_;
        std::vector<Node*> update(max_level_ + 1, nullptr);  // 每层的前驱

        // 第 1 步：从最高层往下找，记录每层「最后一个 key < 目标」的节点
        for (int i = cur_level_; i >= 0; i--) {
            while (cur->forward[i] != nullptr && cur->forward[i]->key < key)
                cur = cur->forward[i];
            update[i] = cur;
        }
        cur = cur->forward[0];

        // 第 2 步：key 已存在 → 原版直接返回 1，不覆盖
        if (cur != nullptr && cur->key == key) {
            std::cout << "    (key " << key << " 已存在，原版会直接返回、不覆盖)\n";
            return;
        }

        // 第 3 步：掷骰子决定新节点层高
        int lv = random_level();
        std::cout << "    新节点 " << key << " 随机层高 = " << lv;

        // 第 4 步：若比当前最高层还高，高出部分的「前驱」就是 header
        if (lv > cur_level_) {
            for (int i = cur_level_ + 1; i <= lv; i++) update[i] = header_;
            std::cout << "  → 抬高全表最高层: " << cur_level_ << " → " << lv;
            cur_level_ = lv;
        }
        std::cout << "\n";

        // 第 5 步：逐层插入（等价于每个层做一次链表头插）
        Node* node = new Node(key, lv);
        for (int i = 0; i <= lv; i++) {
            node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = node;
        }
        count_++;
    }

    // -----------------------------------------------------------------------
    // 查找：从最高层出发，能往右就往右，不能就往下一层
    //       这就是「特快列车 → 普通列车 → 逐站停」的过程
    // -----------------------------------------------------------------------
    bool search(int key, bool verbose) {
        Node* cur = header_;
        if (verbose) std::cout << "    查找 " << key << "：\n";

        for (int i = cur_level_; i >= 0; i--) {
            while (cur->forward[i] != nullptr && cur->forward[i]->key < key) {
                if (verbose)
                    std::cout << "      L" << i << ": " << cur->key << " → "
                              << cur->forward[i]->key << "  ("
                              << cur->forward[i]->key << " < " << key << "，继续右移)\n";
                cur = cur->forward[i];
            }
            if (verbose) {
                int nxt = cur->forward[i] ? cur->forward[i]->key : -1;
                if (nxt == -1)
                    std::cout << "      L" << i << ": 在 " << cur->key
                              << " 处到头了，下一层\n";
                else
                    std::cout << "      L" << i << ": 停在 " << cur->key << "（右边是 "
                              << nxt << " ≥ " << key << "，下一层）\n";
            }
        }

        cur = cur->forward[0];
        if (cur != nullptr && cur->key == key) {
            if (verbose) std::cout << "    ✅ 找到 " << key << "\n";
            return true;
        }
        if (verbose) std::cout << "    ❌ 没有 " << key << "\n";
        return false;
    }

    // -----------------------------------------------------------------------
    // 删除：先找到每层前驱，再逐层摘链，最后收缩最高层
    // -----------------------------------------------------------------------
    bool erase(int key) {
        Node* cur = header_;
        std::vector<Node*> update(max_level_ + 1, nullptr);

        for (int i = cur_level_; i >= 0; i--) {
            while (cur->forward[i] != nullptr && cur->forward[i]->key < key)
                cur = cur->forward[i];
            update[i] = cur;
        }
        cur = cur->forward[0];

        if (cur == nullptr || cur->key != key) {
            std::cout << "    (key " << key << " 不存在，删不掉)\n";
            return false;
        }

        for (int i = 0; i <= cur_level_; i++) {
            if (update[i]->forward[i] != cur) break;  // 该层没有这个节点
            update[i]->forward[i] = cur->forward[i];
        }

        // 高层空了就降下来
        int before = cur_level_;
        while (cur_level_ > 0 && header_->forward[cur_level_] == nullptr) cur_level_--;
        if (before != cur_level_)
            std::cout << "    最高层收缩: " << before << " → " << cur_level_ << "\n";

        delete cur;
        count_--;
        return true;
    }

    // -----------------------------------------------------------------------
    // 把跳表画成 ASCII 图
    //   列位置按 level 0 的顺序排列，所以上下对齐，能直接看出「跳跃」
    // -----------------------------------------------------------------------
    void render() const {
        std::vector<int> keys;
        for (Node* p = header_->forward[0]; p != nullptr; p = p->forward[0])
            keys.push_back(p->key);

        const int W = 9;  // 每列宽度（留够空间，避免箭头压到数字）
        auto col_of = [&](int key) -> int {
            for (size_t i = 0; i < keys.size(); i++)
                if (keys[i] == key) return static_cast<int>(i) + 1;
            return -1;
        };
        const int ncol = static_cast<int>(keys.size()) + 1;  // +1 是 header

        std::cout << "\n";
        for (int lv = max_level_; lv >= 0; lv--) {
            std::vector<char> buf(ncol * W, ' ');

            // 本层有哪些节点（含 header），以及它们在第几列
            std::vector<std::pair<int, std::string>> placed;
            placed.push_back({0, "H"});
            for (Node* p = header_->forward[lv]; p != nullptr; p = p->forward[lv])
                placed.push_back({col_of(p->key), std::to_string(p->key)});

            // 放标签
            for (size_t i = 0; i < placed.size(); i++) {
                int start = placed[i].first * W;
                const std::string& s = placed[i].second;
                for (size_t k = 0; k < s.size(); k++)
                    if (start + static_cast<int>(k) < static_cast<int>(buf.size()))
                        buf[start + k] = s[k];
            }
            // 画箭头：只画在「上一个标签结束」和「下一个标签开始」之间的空隙里，
            // 箭头 '>' 占下一列的前一格，绝不覆盖标签本身
            for (size_t i = 0; i + 1 < placed.size(); i++) {
                int from = placed[i].first * W + static_cast<int>(placed[i].second.size()) + 1;
                int to = placed[i + 1].first * W - 2;  // 留一格给 '>'
                if (to < from) continue;
                for (int c = from; c <= to && c < static_cast<int>(buf.size()); c++)
                    buf[c] = '-';
                if (to + 1 < static_cast<int>(buf.size()))
                    buf[to + 1] = '>';
            }

            std::string line(buf.begin(), buf.end());
            while (!line.empty() && line.back() == ' ') line.pop_back();

            bool active = (lv <= cur_level_);
            std::cout << "  L" << lv << (active ? " │ " : " ┆ ") << line;
            if (!active) std::cout << "   (空，还没用到)";
            std::cout << "\n";
        }
        std::cout << "       ^\n";
        std::cout << "       H = 头节点(不存数据)   L0 = 最底层(存全部数据)\n";
        std::cout << "       元素个数 = " << count_ << "，当前最高层 = " << cur_level_ << "\n";
    }

private:
    Node* header_;
    int max_level_;
    int cur_level_;
    int count_;
};

// ---------------------------------------------------------------------------
int main() {
    srand(2024);  // 固定种子，保证每次运行画出来一样，方便对照

    const int MAX_LEVEL = 4;
    SkipList sl(MAX_LEVEL);

    std::cout << "=============================================================\n";
    std::cout << " 跳表可视化  (max_level = " << MAX_LEVEL << ")\n";
    std::cout << "=============================================================\n";

    std::cout << "\n【第 1 步】空跳表长这样：\n";
    sl.render();

    int data[] = {3, 6, 7, 9, 12, 19, 17, 26, 21, 25};
    std::cout << "\n【第 2 步】依次插入 ";
    for (int i = 0; i < 10; i++) std::cout << data[i] << " ";
    std::cout << "\n";
    for (int i = 0; i < 10; i++) {
        std::cout << "  插入 " << data[i] << ":\n";
        sl.insert(data[i]);
    }

    std::cout << "\n【第 3 步】插入完毕，看看整体结构：\n";
    sl.render();

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << " 关键观察 1：上面 L0 和 L1 是不是一模一样？\n";
    std::cout << "   是的！因为原版 get_random_level() 的 k 从 1 开始，\n";
    std::cout << "   所以每个节点至少有 2 层 → L1 必然包含全部节点。\n";
    std::cout << "   也就是说 L1 完全没有「跳过」任何节点，白白浪费一个指针。\n";
    std::cout << "   经典跳表是 k 从 0 开始，只有一半节点在 L1 以上。\n";
    std::cout << "   （这是我在原项目里发现的一个真实小缺陷，面试可以讲）\n";
    std::cout << "-------------------------------------------------------------\n";

    std::cout << "\n【第 4 步】查找一个存在的 key：19\n";
    sl.search(19, true);

    std::cout << "\n【第 5 步】查找一个不存在的 key：20\n";
    sl.search(20, true);

    std::cout << "\n【第 6 步】删除 9\n";
    sl.erase(9);
    sl.render();

    std::cout << "\n【第 7 步】再删一个高层的节点：3\n";
    sl.erase(3);
    sl.render();

    std::cout << "\n=============================================================\n";
    std::cout << " 核心思想一句话：\n";
    std::cout << "   跳表 = 有序链表 + 多级索引。\n";
    std::cout << "   用「掷硬币」决定每个节点出现在几层，\n";
    std::cout << "   高层的节点稀疏、跳得远，低层的节点密集、走得细。\n";
    std::cout << "   查找时从最高层一路向下，每层尽量右移，\n";
    std::cout << "   就像「高铁 → 普快 → 公交」，期望 O(log n)。\n";
    std::cout << "=============================================================\n";
    return 0;
}
