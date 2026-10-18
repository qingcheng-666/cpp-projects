/* ============================================================================
 * 为什么「重放走 insert_element 会得到 first」—— 直接跑给你看
 * ----------------------------------------------------------------------------
 * 这个程序对比 insert_element 和 upsert_element 在「同一 key 写三次」时的行为，
 * 并模拟 WAL 重放的场景。
 *
 * 编译运行：
 *   g++ tests/why_first.cpp -o bin/why_first --std=c++17 -pthread
 *   ./bin/why_first
 * ============================================================================
 */
#include <iostream>
#include <string>
#include <vector>
#include "../skiplist.h"

int main() {
    std::cout << "================================================================" << std::endl;
    std::cout << " 实验 1：insert_element 遇到重复 key 会怎样？" << std::endl;
    std::cout << "================================================================" << std::endl;

    {
        SkipList<int, std::string> sl(6);
        std::cout << "\n依次插入 key=7 三次：" << std::endl;

        int r1 = sl.insert_element(7, "first");
        std::cout << "  insert(7,\"first\")  -> 返回 " << r1 << std::endl;
        int r2 = sl.insert_element(7, "second");
        std::cout << "  insert(7,\"second\") -> 返回 " << r2 << std::endl;
        int r3 = sl.insert_element(7, "third");
        std::cout << "  insert(7,\"third\")  -> 返回 " << r3 << std::endl;

        std::cout << "\n最终 value 是什么？" << std::endl;
        std::cout << "  遍历跳表实际内容: ";
        for (auto it = sl.begin(); it != sl.end(); ++it) {
            std::cout << it->first << "=" << it->second << "  ";
        }
        std::cout << "\n  size = " << sl.size() << std::endl;

        std::cout << "\n>>> 结论：值是 first！后两次插入被【丢弃】了，返回 1 表示\"已存在\"。" << std::endl;
    }

    std::cout << "\n\n================================================================" << std::endl;
    std::cout << " 实验 2：upsert_element 遇到重复 key 会怎样？" << std::endl;
    std::cout << "================================================================" << std::endl;

    {
        SkipList<int, std::string> sl(6);
        std::cout << "\n依次 upsert key=7 三次：" << std::endl;

        int r1 = sl.upsert_element(7, "first");
        std::cout << "  upsert(7,\"first\")  -> 返回 " << r1 << " (0=新插入)" << std::endl;
        int r2 = sl.upsert_element(7, "second");
        std::cout << "  upsert(7,\"second\") -> 返回 " << r2 << " (1=更新)" << std::endl;
        int r3 = sl.upsert_element(7, "third");
        std::cout << "  upsert(7,\"third\")  -> 返回 " << r3 << " (1=更新)" << std::endl;

        std::cout << "\n最终 value 是什么？" << std::endl;
        std::cout << "  遍历跳表实际内容: ";
        for (auto it = sl.begin(); it != sl.end(); ++it) {
            std::cout << it->first << "=" << it->second << "  ";
        }
        std::cout << "\n  size = " << sl.size() << std::endl;

        std::cout << "\n>>> 结论：值是 third！最后一次写入生效。" << std::endl;
    }

    std::cout << "\n\n================================================================" << std::endl;
    std::cout << " 实验 3：模拟 WAL 重放" << std::endl;
    std::cout << "================================================================" << std::endl;

    // WAL 里存的是「操作序列」，重放就是把这些操作再执行一遍
    std::vector<std::pair<int, std::string>> wal = {
        {7, "first"},
        {7, "second"},
        {7, "third"},
    };

    std::cout << "\nWAL 内容（3 条 PUT 记录）：" << std::endl;
    for (size_t i = 0; i < wal.size(); i++) {
        std::cout << "  [" << i + 1 << "] PUT key=" << wal[i].first
                  << " value=" << wal[i].second << std::endl;
    }

    std::cout << "\n--- 用 insert_element 重放 ---" << std::endl;
    {
        SkipList<int, std::string> sl(6);
        for (auto& rec : wal) sl.insert_element(rec.first, rec.second);
        std::string got;
        for (auto it = sl.begin(); it != sl.end(); ++it) got = it->second;
        std::cout << "  重放后 value = \"" << got << "\"  ❌ 错的！应该是 third" << std::endl;
    }

    std::cout << "\n--- 用 upsert_element 重放 ---" << std::endl;
    {
        SkipList<int, std::string> sl(6);
        for (auto& rec : wal) sl.upsert_element(rec.first, rec.second);
        std::string got;
        for (auto it = sl.begin(); it != sl.end(); ++it) got = it->second;
        std::cout << "  重放后 value = \"" << got << "\"  ✅ 正确" << std::endl;
    }

    std::cout << "\n================================================================" << std::endl;
    std::cout << " 根因：insert_element 里这三行" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << R"(
    current = current->forward[0];              // 找到 >= key 的第一个节点

    if (current != NULL && current->get_key() == key) {
        std::cout << "key: " << key << ", exists" << std::endl;
        return 1;                               // ← 直接返回！不碰旧 value
    }
)" << std::endl;
    std::cout << "  「key 已存在」= 立刻 return，既没有 set_value()，也没有新建节点。" << std::endl;
    std::cout << "  所以第二次、第三次的 value 根本没机会写进去。" << std::endl;
    std::cout << "  这就是缺陷 6：SkipList 没有 update 语义，set_value() 是死代码。" << std::endl;
    std::cout << std::endl;

    return 0;
}
