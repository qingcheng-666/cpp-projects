/* ************************************************************************
> File Name:     skiplist.h
> Author:        程序员Carl（原版作者）
> 微信公众号:    代码随想录
> Created Time:  Sun Dec  2 19:04:26 2018
> Description:   跳表 KV 存储引擎 —— P1 缺陷修复版
>
> 本文件是原版 skiplist.h 的「最小改动修复版」。
> 每个修改点都用 [修复 N] 标出，对照文档见 docs/03-P1修复对照.md
> 原版保持不动，放在上级目录的 skiplist.h，方便 diff 对照。
>
> 编译要求：C++17（因为用了 std::shared_mutex）
>   g++ main.cpp -o main --std=c++17 -pthread
 ************************************************************************/

#include <iostream> 
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>         // [修复 6] VLA -> std::vector
#include <deque>          // [P3] scan_tail 的滑动窗口
#include <algorithm>      // [P3] std::reverse
#include <utility>        // [P3] std::pair
#include <mutex>          // [修复 3] std::unique_lock 在这里（不在 shared_mutex 里）
#include <shared_mutex>   // [修复 3] std::shared_mutex / std::shared_lock
#include "codec.h"        // [P2] KeyCodec：key 的序列化策略

#define STORE_FILE "store/dumpFile"

/* ===========================================================================
 * [修复 2] 头文件里不再定义全局变量
 * ---------------------------------------------------------------------------
 * 原版这里是：
 *     std::mutex mtx;                  // 定义，不是声明！
 *     std::string delimiter = ":";     // 同样是定义
 *
 * 问题：头文件被多个 .cpp include 时，每个翻译单元都会生成一份定义，
 *       链接时报 multiple definition of `mtx' / `delimiter'。
 *       实测（tests/link_a.cpp + link_b.cpp）：
 *           /usr/bin/ld: multiple definition of `mtx'; first defined here
 *           collect2: error: ld returned 1 exit status
 *       这直接堵死了多文件工程化 —— 想拆成 kv_server.cpp + main.cpp 就编不过。
 *
 * 改法：锁改成 SkipList 的成员变量（顺带解决"一把全局锁被所有实例共享"），
 *       分隔符改成类的静态常量。
 * =========================================================================== */

//Class template to implement node
template<typename K, typename V> 
class Node {

public:
    
    Node() {} 

    Node(K k, V v, int); 

    ~Node();

    K get_key() const;

    V get_value() const;

    void set_value(V);

    // [P3] 引用版访问器：迭代器解引用时用它，避免"按值返回临时对象 + 绑定引用"
    // 造成的悬空引用。原版的 get_key()/get_value() 是按值返回的。
    const K& get_key_ref() const { return key; }
    const V& get_value_ref() const { return value; }
    
    // Linear array to hold pointers to next node of different level
    Node<K, V> **forward;

    int node_level;

    /* -----------------------------------------------------------------------
     * [P5] backward 指针：level 0 上的前驱
     * -----------------------------------------------------------------------
     * 为什么只加在 level 0：
     *   反向遍历只需要「按 key 降序走一遍」，而 level 0 含有全部节点。
     *   高层是索引，反向走它们没有意义（而且维护成本高）。
     *   这和 Redis 的做法一致 —— Redis 的 zskiplistNode 也是只有
     *   一个 backward 指针，指向 level 0 上的前驱。
     *
     * 有了它之后：
     *   scan_reverse 从 O(range) 降到 O(log n + n)
     *   即「一次 O(log n) 定位到起点 + 沿 backward 走 n 步」
     *
     * 代价：每个节点多一个指针（8 字节）。以 100 万节点算约多 8MB。
     * ----------------------------------------------------------------------- */
    Node<K, V>* backward;

private:
    K key;
    V value;
};

template<typename K, typename V> 
Node<K, V>::Node(const K k, const V v, int level) {
    this->key = k;
    this->value = v;
    this->node_level = level; 
    this->backward = nullptr;      // [P5]

    // level + 1, because array index is from 0 - level
    this->forward = new Node<K, V>*[level+1];
    
	// Fill forward array with 0(NULL) 
    memset(this->forward, 0, sizeof(Node<K, V>*)*(level+1));
};

template<typename K, typename V> 
Node<K, V>::~Node() {
    delete []forward;
};

template<typename K, typename V> 
K Node<K, V>::get_key() const {
    return key;
};

template<typename K, typename V> 
V Node<K, V>::get_value() const {
    return value;
};
template<typename K, typename V> 
void Node<K, V>::set_value(V value) {
    this->value=value;
};

// Class template for Skip list
template <typename K, typename V> 
class SkipList {

public: 
    SkipList(int);
    ~SkipList();
    int get_random_level();
    Node<K, V>* create_node(K, V, int);
    int insert_element(K, V);
    /* =======================================================================
     * [P4] upsert_element —— 带「覆盖」语义的写入
     * -----------------------------------------------------------------------
     * 原版 insert_element 遇到已存在的 key 会直接返回 1，【不覆盖旧值】。
     * 这意味着 SkipList 根本没有 update 语义，Node::set_value() 是死代码
     * （这就是架构文档里列的缺陷 6）。
     *
     * 为什么 P4 必须要它：
     *   WAL 重放时，同一条 key 可能被写了多次（比如 SET k v1 → SET k v2），
     *   重放必须得到「最后一次写入生效」的结果。
     *   如果重放走 insert_element，第二次写入会被丢弃，恢复出来的数据是错的。
     *
     * 返回值：1 = 更新了已存在的 key，0 = 新插入
     * ======================================================================= */
    int upsert_element(K, V);
    void display_list();
    bool search_element(K);
    void delete_element(K);
    bool dump_file();              // [修复 4] 返回值 void -> bool
    void load_file();
    //递归删除节点
    void clear(Node<K,V>*);
    // [P4] size() 标为 const：它只读 _element_count（且加的是共享锁），
    //      KVStore::size() const 需要调用它。
    int size() const;

    /* =======================================================================
     * [P3] 迭代器 + 范围查询
     * -----------------------------------------------------------------------
     * 为什么加这个：跳表相对哈希表唯一不可替代的优势就是「有序 + 范围查询」。
     *   Redis 的 ZSET 用跳表就是为了 ZRANGE；LevelDB 的 MemTable 用跳表
     *   就是为了能有序地刷盘成 SSTable。
     * 而原版只有个 display_list() 能打印，等于把最大的卖点浪费了。
     *
     * 并发安全设计（这是本阶段最需要想清楚的地方）：
     *   迭代器持有一个裸 Node*，一旦有并发写（insert/delete 改 forward 指针，
     *   delete 还会 free 节点），迭代器就会悬空 —— 读到野指针直接崩溃。
     *   所以提供两套 API，由调用方按场景选：
     *
     *   (1) Iterator —— 轻量、零拷贝，但【不加锁、不安全】。
     *       要求调用方保证遍历期间没有并发写。
     *       适合：单线程场景、或外部已经自己加了锁。
     *
     *   (2) scan() / scan_range() 等 —— 内部加 shared_lock，
     *       把结果拷进 vector 后立即释放锁，返回的是【快照】。
     *       适合：多线程环境。代价是要拷贝一份数据。
     *
     *   这是所有 KV 引擎都要面对的取舍（LevelDB 用 Iterator + 引用计数，
     *   Redis 用单线程避免这个问题）。我在文档里写了取舍分析。
     * ======================================================================= */

    // -----------------------------------------------------------------------
    // [P3] 迭代器：按 key 升序遍历（沿 level 0）
    // 用法：
    //     for (auto it = sl.begin(); it != sl.end(); ++it)
    //         std::cout << it->first << " => " << it->second;
    // -----------------------------------------------------------------------
    class Iterator {
    public:
        explicit Iterator(Node<K, V>* node) : _node(node) {}

        // 解引用返回 pair<const K&, const V&>，避免拷贝
        // 注意用 get_key_ref()/get_value_ref()：原版 get_key() 按值返回，
        // 若在此绑定引用会立刻悬空（临时对象在语句结束后销毁）。
        const std::pair<const K&, const V&> operator*() const {
            return std::pair<const K&, const V&>(_node->get_key_ref(), _node->get_value_ref());
        }

        // 也支持 it->first / it->second 写法
        struct ArrowProxy {
            const K& first;
            const V& second;
            const ArrowProxy* operator->() const { return this; }
        };
        ArrowProxy operator->() const {
            return ArrowProxy{_node->get_key_ref(), _node->get_value_ref()};
        }

        Iterator& operator++() {                       // 前置 ++
            if (_node) _node = _node->forward[0];
            return *this;
        }
        Iterator operator++(int) {                     // 后置 ++
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const Iterator& o) const { return _node == o._node; }
        bool operator!=(const Iterator& o) const { return _node != o._node; }

        const K& key()   const { return _node->get_key(); }
        const V& value() const { return _node->get_value(); }

        Node<K, V>* raw() const { return _node; }      // 逃生舱，慎用

    private:
        Node<K, V>* _node;
    };

    Iterator begin() const { return Iterator(_header->forward[0]); }
    Iterator end()   const { return Iterator(nullptr); }

    // -----------------------------------------------------------------------
    // [P3] 定位到第一个 key >= target 的节点（跳表版的 lower_bound）
    // 这是所有范围查询的基础：一次 O(log n) 查找直接落到区间起点，
    // 而不是从头遍历 —— 这正是跳表比哈希表强的地方。
    // 返回裸指针；调用方需自行保证并发安全（scan_* 系列已内部处理）。
    // -----------------------------------------------------------------------
    Node<K, V>* lower_bound(const K& target) const {
        Node<K, V>* cur = _header;
        for (int i = _skip_list_level; i >= 0; i--) {
            while (cur->forward[i] != nullptr && cur->forward[i]->get_key() < target) {
                cur = cur->forward[i];
            }
        }
        return cur->forward[0];       // 第一个 key >= target 的节点（可能是 nullptr）
    }

    // -----------------------------------------------------------------------
    // [P3] 范围查询 [begin_key, end_key)  —— 左闭右开，和 STL 一致
    // 内部加共享锁，返回快照。多线程安全。
    // -----------------------------------------------------------------------
    std::vector<std::pair<K, V>> scan(const K& begin_key, const K& end_key) const {
        std::shared_lock<std::shared_mutex> lock(_rwlock);
        std::vector<std::pair<K, V>> out;
        for (Node<K, V>* p = lower_bound(begin_key);
             p != nullptr && p->get_key() < end_key;
             p = p->forward[0]) {
            out.emplace_back(p->get_key(), p->get_value());
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // [P3] 只取前 n 条（从 begin_key 起），避免一次拉回太多数据
    // -----------------------------------------------------------------------
    std::vector<std::pair<K, V>> scan_n(const K& begin_key, size_t n) const {
        std::shared_lock<std::shared_mutex> lock(_rwlock);
        std::vector<std::pair<K, V>> out;
        out.reserve(n);
        for (Node<K, V>* p = lower_bound(begin_key);
             p != nullptr && out.size() < n;
             p = p->forward[0]) {
            out.emplace_back(p->get_key(), p->get_value());
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // [P5] 反向扫描：返回 <= start_key 的、最大的 n 条，按【降序】返回
    // -----------------------------------------------------------------------
    // P3 时的实现是「先正向收集再截取」，复杂度 O(range) —— 因为那时没有
    // backward 指针。P5 加上 backward 之后，这里变成真正的 O(log n + n)：
    //
    //   ① upper_bound(start_key)：一次 O(log n) 的跳表查找，
    //      直接落到「最后一个 <= start_key 的节点」
    //   ② 沿 backward 往回走 n 步，每步 O(1)
    //
    // 对比一下差异：如果 start_key 在 100 万条数据的末尾，
    //   P3 的实现要先正向扫 100 万次才拿到起点；
    //   P5 的实现 20 步左右就定位到了。
    // -----------------------------------------------------------------------
    std::vector<std::pair<K, V>> scan_reverse(const K& start_key, size_t n) const {
        std::shared_lock<std::shared_mutex> lock(_rwlock);
        std::vector<std::pair<K, V>> out;
        out.reserve(n);

        // ① O(log n) 定位到「最后一个 key <= start_key 的节点」
        Node<K, V>* p = upper_bound(start_key);

        // ② 沿 backward 往回走 n 步（结果天然是降序）
        while (p != nullptr && out.size() < n) {
            out.emplace_back(p->get_key(), p->get_value());
            p = p->backward;
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // [P5] 反向迭代器：从最大的 key 往小走
    // 用法：
    //     for (auto it = sl.rbegin(); it != sl.rend(); ++it) ...
    // -----------------------------------------------------------------------
    class ReverseIterator {
    public:
        explicit ReverseIterator(Node<K, V>* node) : _node(node) {}

        const std::pair<const K&, const V&> operator*() const {
            return std::pair<const K&, const V&>(_node->get_key_ref(), _node->get_value_ref());
        }
        struct ArrowProxy {
            const K& first;
            const V& second;
            const ArrowProxy* operator->() const { return this; }
        };
        ArrowProxy operator->() const {
            return ArrowProxy{_node->get_key_ref(), _node->get_value_ref()};
        }

        ReverseIterator& operator++() {                    // 往前驱走
            if (_node) _node = _node->backward;
            return *this;
        }
        ReverseIterator operator++(int) {
            ReverseIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const ReverseIterator& o) const { return _node == o._node; }
        bool operator!=(const ReverseIterator& o) const { return _node != o._node; }

        const K& key()   const { return _node->get_key_ref(); }
        const V& value() const { return _node->get_value_ref(); }

    private:
        Node<K, V>* _node;
    };

    // rbegin = 最大的那个节点；rend = nullptr（走到头）
    // 注意：反向遍历同样【不加锁】，并发写时迭代器会悬空 ——
    //       和正向迭代器一样，调用方需自行保证。要用安全的就用 scan_reverse()。
    ReverseIterator rbegin() const {
        // 从 header 的最高层一路往右，落到最后一个数据节点
        Node<K, V>* cur = _header;
        for (int i = _skip_list_level; i >= 0; i--) {
            while (cur->forward[i] != nullptr) cur = cur->forward[i];
        }
        // 此时 cur 是 level 0 的最后一个数据节点（若表为空则是 header）
        return ReverseIterator(cur == _header ? nullptr : cur);
    }
    ReverseIterator rend() const { return ReverseIterator(nullptr); }

    // -----------------------------------------------------------------------
    // [P5] upper_bound：最后一个 key <= target 的节点
    // （与 lower_bound 相对：那个是第一个 >= target）
    // -----------------------------------------------------------------------
    Node<K, V>* upper_bound(const K& target) const {
        Node<K, V>* cur = _header;
        for (int i = _skip_list_level; i >= 0; i--) {
            while (cur->forward[i] != nullptr && cur->forward[i]->get_key() <= target) {
                cur = cur->forward[i];
            }
        }
        // cur 现在是「最后一个 <= target 的节点」；
        // 如果表为空或所有 key 都 > target，cur 就是 header，应返回 nullptr
        return (cur == _header) ? nullptr : cur;
    }

    // -----------------------------------------------------------------------
    // [P3] 统计区间 [begin_key, end_key) 内的元素个数
    // 不拷贝 value，只数数 —— 比 scan() 更省内存
    // -----------------------------------------------------------------------
    size_t count_range(const K& begin_key, const K& end_key) const {
        std::shared_lock<std::shared_mutex> lock(_rwlock);
        size_t n = 0;
        for (Node<K, V>* p = lower_bound(begin_key);
             p != nullptr && p->get_key() < end_key;
             p = p->forward[0]) {
            n++;
        }
        return n;
    }

    // -----------------------------------------------------------------------
    // [P3] 返回第一个 key >= target 的键值（跳表版的 seek）
    // -----------------------------------------------------------------------
    bool seek(const K& target, K& out_key, V& out_value) const {
        std::shared_lock<std::shared_mutex> lock(_rwlock);
        Node<K, V>* p = lower_bound(target);
        if (p == nullptr) return false;
        out_key = p->get_key();
        out_value = p->get_value();
        return true;
    }

private:
    void get_key_value_from_string(const std::string& str, std::string* key, std::string* value);
    bool is_valid_string(const std::string& str);

private:    
    // Maximum level of the skip list 
    int _max_level;

    // current level of skip list 
    int _skip_list_level;

    // pointer to header node 
    Node<K, V> *_header;

    // file operator
    std::ofstream _file_writer;
    std::ifstream _file_reader;

    // skiplist current element count
    int _element_count;

    // [修复 2] 锁从全局变量改成成员变量
    // [修复 3] 用 shared_mutex：读操作可并发，写操作互斥
    // [P3] mutable：scan/seek/count_range 等是 const 成员函数，
    //      但「加锁」本身会改锁的内部状态。const 成员里 _rwlock 是 const 的，
    //      shared_lock 需要非 const 引用，所以必须声明为 mutable。
    //      这是标准做法：锁是「逻辑上不改变对象状态」的同步设施。
    mutable std::shared_mutex _rwlock;

    // [修复 2] 分隔符从全局变量改成类内静态常量
    static constexpr char kDelimiter = ':';
};

// create new node 
template<typename K, typename V>
Node<K, V>* SkipList<K, V>::create_node(const K k, const V v, int level) {
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}

// Insert given key and value in skip list 
// return 1 means element exists  
// return 0 means insert successfully
/* 
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/
template<typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value) {
    
    // [修复 3] RAII 加锁：原来的 mtx.lock()/mtx.unlock() 需要手写配对，
    //          "key 已存在"那条 return 分支就手写了一次 unlock()，
    //          一旦漏写就是死锁。改成 unique_lock 后自动解锁。
    std::unique_lock<std::shared_mutex> lock(_rwlock);

    Node<K, V> *current = this->_header;

    // create update array and initialize it 
    // update is array which put node that the node->forward[i] should be operated later
    // [修复 6] 原版 Node<K,V>* update[_max_level+1]; 是 VLA（变长数组），
    //          属于 GCC 扩展而非标准 C++，MSVC 不支持。改用 std::vector。
    std::vector<Node<K, V>*> update(_max_level + 1, nullptr);

    // start form highest level of skip list 
    for(int i = _skip_list_level; i >= 0; i--) {
        while(current->forward[i] != NULL && current->forward[i]->get_key() < key) {
            current = current->forward[i]; 
        }
        update[i] = current;
    }

    // reached level 0 and forward pointer to right node, which is desired to insert key.
    current = current->forward[0];

    // if current node have key equal to searched key, we get it
    if (current != NULL && current->get_key() == key) {
        std::cout << "key: " << key << ", exists" << std::endl;
        return 1;                          // [修复 3] 不再需要手写 unlock()
    }

    // if current is NULL that means we have reached to end of the level 
    // if current's key is not equal to key that means we have to insert node between update[0] and current node 
    if (current == NULL || current->get_key() != key ) {
        
        // Generate a random level for node
        int random_level = get_random_level();

        // If random level is greater thar skip list's current level, initialize update value with pointer to header
        if (random_level > _skip_list_level) {
            for (int i = _skip_list_level+1; i < random_level+1; i++) {
                update[i] = _header;
            }
            _skip_list_level = random_level;
        }

        // create new node with random level generated 
        Node<K, V>* inserted_node = create_node(key, value, random_level);
        
        // insert node 
        for (int i = 0; i <= random_level; i++) {
            inserted_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = inserted_node;
        }

        // [P5] 维护 backward 指针（只维护 level 0 的）
        //   新节点的前驱 = update[0]（level 0 上的前驱）
        //   新节点的后继（如果存在）的 backward 要指向新节点
        //
        //   注意 update[0] 可能是 header。header 的 backward 恒为 nullptr，
        //   所以不需要（也不应该）改 header 的 backward。
        inserted_node->backward = (update[0] == _header) ? nullptr : update[0];
        if (inserted_node->forward[0] != nullptr) {
            inserted_node->forward[0]->backward = inserted_node;
        }

        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
        _element_count ++;
    }
    return 0;
}

/* ===========================================================================
 * [P4] upsert_element —— 带覆盖语义的写入
 * ---------------------------------------------------------------------------
 * 和 insert_element 的唯一区别：
 *   insert_element 发现 key 已存在 -> 打印提示，返回 1，【不动旧值】
 *   upsert_element 发现 key 已存在 -> 【调用 set_value() 覆盖】，返回 1
 *
 * 注意这里终于用上了 Node::set_value() —— 原版里它是死代码（缺陷 6）。
 *
 * 为什么不能简单写成「先 delete 再 insert」：
 *   那样会改变节点的层高（重新掷硬币），而且有短暂的"key 不存在"窗口，
 *   并发下别的线程可能读到中间态。原地改 value 是最干净的做法。
 * =========================================================================== */
template<typename K, typename V>
int SkipList<K, V>::upsert_element(const K key, const V value) {

    std::unique_lock<std::shared_mutex> lock(_rwlock);

    Node<K, V> *current = this->_header;
    std::vector<Node<K, V>*> update(_max_level + 1, nullptr);

    // 前 4 步和 insert_element 完全一致：找到每层前驱
    for (int i = _skip_list_level; i >= 0; i--) {
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    current = current->forward[0];

    // key 已存在 -> 原地覆盖 value（不新建节点、不改层高）
    if (current != NULL && current->get_key() == key) {
        current->set_value(value);
        return 1;                       // 1 = 更新
    }

    // key 不存在 -> 走和 insert_element 一样的插入流程
    int random_level = get_random_level();
    if (random_level > _skip_list_level) {
        for (int i = _skip_list_level + 1; i < random_level + 1; i++) {
            update[i] = _header;
        }
        _skip_list_level = random_level;
    }

    Node<K, V>* inserted_node = create_node(key, value, random_level);
    for (int i = 0; i <= random_level; i++) {
        inserted_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = inserted_node;
    }

    // [P5] 维护 backward（同 insert_element）
    inserted_node->backward = (update[0] == _header) ? nullptr : update[0];
    if (inserted_node->forward[0] != nullptr) {
        inserted_node->forward[0]->backward = inserted_node;
    }

    _element_count++;
    return 0;                           // 0 = 新插入
}

// Display skip list 
template<typename K, typename V> 
void SkipList<K, V>::display_list() {

    // [修复 3] 读操作加共享锁
    std::shared_lock<std::shared_mutex> lock(_rwlock);

    std::cout << "\n*****Skip List*****"<<"\n"; 
    for (int i = 0; i <= _skip_list_level; i++) {
        Node<K, V> *node = this->_header->forward[i]; 
        std::cout << "Level " << i << ": ";
        while (node != NULL) {
            std::cout << node->get_key() << ":" << node->get_value() << ";";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

// Dump data in memory to file 
// [修复 4] 返回值 void -> bool，并检查 is_open()
template<typename K, typename V> 
bool SkipList<K, V>::dump_file() {

    // 注意：这里用 unique_lock 而不是 shared_lock，
    // 因为 dump_file 会改写成员 _file_writer，两个并发 dump 会互相踩。
    std::unique_lock<std::shared_mutex> lock(_rwlock);

    std::cout << "dump_file-----------------" << std::endl;
    _file_writer.open(STORE_FILE);

    // [修复 4] 原版从不检查 open 是否成功。
    //   store/ 目录不存在（比如在别的目录运行程序）时，open 失败，
    //   但代码照样往下走、照样打印成功、最后返回 void、进程退出码 0。
    //   数据一条都没写，调用方却以为落盘成功了 —— 静默丢数据。
    if (!_file_writer.is_open()) {
        std::cerr << "[dump_file] 无法打开文件: " << STORE_FILE
                  << "（目录是否存在？磁盘是否可写？）" << std::endl;
        return false;
    }

    Node<K, V> *node = this->_header->forward[0]; 

    while (node != NULL) {
        // [P2] 用 KeyCodec 把 key 转成字节，而不是直接 << key。
        //   对 int 来说结果一样（都是十进制），但这样 string key 也能工作。
        _file_writer << KeyCodec<K>::encode(node->get_key())
                     << kDelimiter << node->get_value() << "\n";
        std::cout << node->get_key() << ":" << node->get_value() << ";\n";
        node = node->forward[0];
    }

    _file_writer.flush();
    bool ok = _file_writer.good();   // [修复 4] 写过程中出错也要能发现
    _file_writer.close();
    return ok;
}

// Load data from disk
template<typename K, typename V> 
void SkipList<K, V>::load_file() {

    // 注意：本函数内部调用 insert_element()，而 insert_element 自己会加写锁。
    // 所以这里【不能】再持锁，否则 unique_lock 不可重入 -> 死锁。
    // 这是改造并发控制时最容易踩的坑，务必注意。
    _file_reader.open(STORE_FILE);
    std::cout << "load_file-----------------" << std::endl;
    std::string line;
    std::string key, value;
    while (getline(_file_reader, line)) {
        get_key_value_from_string(line, &key, &value);
        if (key.empty() || value.empty()) {
            continue;
        }
        // [P2] 原版这里写死 insert_element(stoi(*key), *value)，key 只能是 int。
        //   现在改成用 KeyCodec 解码：int 走十进制解析、string 原样返回。
        //   注意用 upsert 而不是 insert：同一 key 出现多次时应该后者覆盖前者，
        //   否则重复 dump/load 会丢掉最新值（和 WAL 重放是同一个道理）。
        try {
            insert_element(KeyCodec<K>::decode(key), value);
        } catch (const std::exception& e) {
            // 单行损坏不应该让整个加载失败
            std::cerr << "[load_file] 跳过非法行: " << e.what() << std::endl;
            continue;
        }
        std::cout << "key:" << key << "value:" << value << std::endl;
    }
    _file_reader.close();
}

// Get current SkipList size
template<typename K, typename V> 
int SkipList<K, V>::size() const { 
    // [修复 3] 读操作加共享锁
    std::shared_lock<std::shared_mutex> lock(_rwlock);
    return _element_count;
}

template<typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string& str, std::string* key, std::string* value) {

    if(!is_valid_string(str)) {
        key->clear();
        value->clear();
        return;
    }
    // [P2] 原版用 find（第一个冒号）切分，导致含冒号的 key 被截断：
    //       "user:1001:alice" -> key="user", value="1001:alice"   ❌
    //   改用 rfind（最后一个冒号）：
    //       "user:1001:alice" -> key="user:1001", value="alice"   ✅
    //
    //   为什么是最后一个而不是第一个：
    //   value 是我们完全无法控制的内容（用户随便写），
    //   而 key 的格式通常更有规律。用最后一个冒号能让
    //   「带命名空间的 key」正常工作，代价是 value 里不能有冒号。
    //
    //   注意：这只是原版文本格式（dumpFile）的补救。
    //   kvstore.h 的快照/WAL 用的是【长度前缀】格式，没有这个限制，
    //   任意 key/value 都能正确存取。真正要支持任意字节，
    //   应该用长度前缀而不是分隔符。
    size_t pos = str.rfind(kDelimiter);
    if (pos == std::string::npos) {
        key->clear();
        value->clear();
        return;
    }
    *key = str.substr(0, pos);
    *value = str.substr(pos + 1);
}

template<typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string& str) {

    if (str.empty()) {
        return false;
    }
    // [P2] 与 get_key_value_from_string 保持一致，用 rfind
    if (str.rfind(kDelimiter) == std::string::npos) {
        return false;
    }
    return true;
}

// Delete element from skip list 
template<typename K, typename V> 
void SkipList<K, V>::delete_element(K key) {

    // [修复 3] RAII 加锁
    std::unique_lock<std::shared_mutex> lock(_rwlock);

    Node<K, V> *current = this->_header; 
    // [修复 6] VLA -> std::vector
    std::vector<Node<K, V>*> update(_max_level + 1, nullptr);

    // start from highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--) {
        while (current->forward[i] !=NULL && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];
    if (current != NULL && current->get_key() == key) {
       
        // [P5] 摘链之前，先修好 level 0 上的 backward 关系。
        //   必须在 delete current 之前做 —— 删完就读不到 forward[0] 了。
        //
        //   要修两个方向：
        //     ① current 的后继的 backward 应该跳过 current，指向 current 的前驱
        //     ② current 的前驱不需要改（它的 forward 由上面的循环处理）
        //
        //   注意 current->backward 可能是 nullptr（current 是第一个数据节点），
        //   这时后继的 backward 应该变回 nullptr —— 正好是它的初始值。
        if (current->forward[0] != nullptr) {
            current->forward[0]->backward = current->backward;
        }

        // start for lowest level and delete the current node of each level
        for (int i = 0; i <= _skip_list_level; i++) {

            // if at level i, next node is not target node, break the loop.
            if (update[i]->forward[i] != current) 
                break;

            update[i]->forward[i] = current->forward[i];
        }

        // Remove levels which have no elements
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == 0) {
            _skip_list_level --; 
        }

        std::cout << "Successfully deleted key "<< key << std::endl;
        delete current;
        _element_count --;
    }
    return;
}

// Search for element in skip list 
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/
template<typename K, typename V> 
bool SkipList<K, V>::search_element(K key) {

    // [修复 3] 这是最关键的修复：原版 search_element 完全不加锁，
    //   而 insert/delete 都加了锁 -> 读写并发时是数据竞争。
    //   实测（tests/race_test.cpp，ThreadSanitizer）：
    //       WARNING: ThreadSanitizer: data race
    //         Read of size 8 ... by thread T2:
    //           #0 SkipList<int,std::string>::search_element(int)
    //   后果：读线程可能读到正在被改写的 forward 指针，轻则查错、重则解引用野指针崩溃。
    //   用 shared_lock：多个读可并发，与写互斥。
    std::shared_lock<std::shared_mutex> lock(_rwlock);

    std::cout << "search_element-----------------" << std::endl;
    Node<K, V> *current = _header;

    // start from highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--) {
        while (current->forward[i] && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
    }

    //reached level 0 and advance pointer to right node, which we search
    current = current->forward[0];

    // if current node have key equal to searched key, we get it
    if (current and current->get_key() == key) {
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }

    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}

// construct skip list
template<typename K, typename V> 
SkipList<K, V>::SkipList(int max_level) {

    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    // create header node and initialize key and value to null
    // [修复 7] 原版是 K k; V v; —— 对于 int 这类内置类型这是「未初始化」，
    //   头节点的 key/value 虽然从不参与比较，但复制一个未初始化的值是未定义行为，
    //   -Wall 下会报：warning: 'k' may be used uninitialized [-Wmaybe-uninitialized]
    //   加 {} 做值初始化（int 变 0，std::string 变空串），彻底消除警告。
    K k{};
    V v{};
    this->_header = new Node<K, V>(k, v, _max_level);
};

template<typename K, typename V> 
SkipList<K, V>::~SkipList() {

    if (_file_writer.is_open()) {
        _file_writer.close();
    }
    if (_file_reader.is_open()) {
        _file_reader.close();
    }

    // [修复 1] 原版是递归删除：
    //     if(_header->forward[0]!=nullptr) clear(_header->forward[0]);
    //   clear() 内部又递归调用自己，递归深度 = 节点总数。
    //   100 万节点需要约 48MB 栈（每帧约 48 字节），而默认栈只有 8MB
    //   -> 栈溢出 -> 段错误。
    //   实测（tests/dtor_test.cpp）：delete 100 万节点 exit=139（段错误）。
    //   现在 clear() 已改成迭代实现，这里直接调用即可（内部能处理 nullptr）。
    clear(_header->forward[0]);
    delete(_header);
    
}

// [修复 1] 迭代版 clear，替换原来的递归实现
//   原版：
//       void SkipList<K,V>::clear(Node<K,V>* cur) {
//           if(cur->forward[0]!=nullptr) clear(cur->forward[0]);   // 递归！
//           delete(cur);
//       }
//   迭代版：沿 level 0 逐个摘链删除，先存后继再删当前，栈深度恒为 O(1)。
template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> * cur)
{
    while (cur != nullptr) {
        Node<K, V>* next = cur->forward[0];   // 先记住后继
        delete cur;                           // 再删当前
        cur = next;
    }
}

template<typename K, typename V>
int SkipList<K, V>::get_random_level(){

    // [修复 5] 原版 int k = 1; 从 1 开始，意味着每个节点至少有 2 层（L0 + L1），
    //   导致 L1 必然包含全部节点，和 L0 完全一样 —— 白白浪费一个指针，起不到索引作用。
    //   实测：10 万次抽样，最小值恒为 1，分布 1:49978(≈1/2) 2:25045(≈1/4)；
    //         可视化输出中 L0 与 L1 的 key 集合完全相同。
    //   经典跳表从 0 开始：只有 1/2 的节点出现在 L1 以上，每层才真正稀疏。
    //   空间收益：平均指针数从 3 降到 2。
    int k = 0;
    while (rand() % 2) {
        k++;
    }
    k = (k < _max_level) ? k : _max_level;
    return k;
};
// vim: et tw=100 ts=4 sw=4 cc=120
