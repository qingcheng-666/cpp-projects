/* ============================================================================
 * [P3] 迭代器 + 范围查询 测试
 * ----------------------------------------------------------------------------
 * 覆盖：
 *   A. 迭代器遍历（顺序、完整性）
 *   B. lower_bound / seek
 *   C. scan 左闭右开语义、边界（空区间、越界、单点）
 *   D. scan_n / count_range / scan_reverse
 *   E. 并发安全：scan 与写并发（TSan 应无告警）
 *   F. 与 std::map 对拍：随机操作后逐个比对
 *
 * 编译运行：
 *   g++ tests/p3_scan_test.cpp -o bin/p3_test --std=c++17 -pthread
 *   ./bin/p3_test
 * ============================================================================
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include "../skiplist.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; std::cout << "  ❌ FAIL: " << msg << std::endl; }     \
    } while (0)

static void section(const char* name) {
    std::cout << "\n===== " << name << " =====" << std::endl;
}

int main() {
    std::cout.setstate(std::ios::failbit);   // 屏蔽跳表内部打印

    // ------------------------------------------------------------------ A
    section("A. 迭代器遍历");
    {
        SkipList<int, std::string> sl(8);
        for (int i = 1; i <= 10; i++) sl.insert_element(i * 10, "v" + std::to_string(i));

        std::vector<int> got;
        for (auto it = sl.begin(); it != sl.end(); ++it) got.push_back(it->first);
        CHECK(got.size() == 10, "迭代器应遍历 10 个元素");
        CHECK(std::is_sorted(got.begin(), got.end()), "迭代器应按 key 升序");
        CHECK(got.front() == 10 && got.back() == 100, "首尾应为 10 和 100");

        // 基于范围的 for（依赖 begin/end）
        int cnt = 0;
        for (auto it = sl.begin(); it != sl.end(); ++it) cnt++;
        CHECK(cnt == 10, "范围遍历计数应为 10");

        // operator-> 写法
        auto it = sl.begin();
        CHECK(it->first == 10 && it->second == "v1", "operator-> 应可用");

        // 空表遍历
        SkipList<int, std::string> empty(4);
        CHECK(empty.begin() == empty.end(), "空表 begin == end");
    }

    // ------------------------------------------------------------------ B
    section("B. lower_bound / seek");
    {
        SkipList<int, std::string> sl(8);
        for (int k : {10, 20, 30, 40, 50}) sl.insert_element(k, "v");

        auto* p = sl.lower_bound(25);
        CHECK(p != nullptr && p->get_key() == 30, "lower_bound(25) 应为 30");
        p = sl.lower_bound(30);
        CHECK(p != nullptr && p->get_key() == 30, "lower_bound(30) 应为 30（>= 语义）");
        p = sl.lower_bound(5);
        CHECK(p != nullptr && p->get_key() == 10, "lower_bound(5) 应为 10");
        p = sl.lower_bound(100);
        CHECK(p == nullptr, "lower_bound(100) 应为 nullptr");

        int k; std::string v;
        CHECK(sl.seek(35, k, v) && k == 40, "seek(35) 应命中 40");
        CHECK(!sl.seek(999, k, v), "seek(999) 应返回 false");
    }

    // ------------------------------------------------------------------ C
    section("C. scan 左闭右开语义与边界");
    {
        SkipList<int, std::string> sl(8);
        for (int i = 1; i <= 10; i++) sl.insert_element(i, "v" + std::to_string(i));

        auto r = sl.scan(3, 7);                       // [3,7) => 3,4,5,6
        CHECK(r.size() == 4, "scan(3,7) 应有 4 条");
        CHECK(r.front().first == 3 && r.back().first == 6, "scan(3,7) 应为 3..6（不含 7）");

        CHECK(sl.scan(7, 3).empty(), "scan 逆序区间应为空");
        CHECK(sl.scan(100, 200).empty(), "scan 完全越界应为空");
        CHECK(sl.scan(-5, 1).empty(), "scan 左越界右=1 应为空（1 不含）");

        auto one = sl.scan(5, 6);                     // 单点区间
        CHECK(one.size() == 1 && one[0].first == 5, "scan(5,6) 应只有 5");

        auto all = sl.scan(-100, 1000);
        CHECK(all.size() == 10, "scan 全区间应有 10 条");

        // 端点等于已存在的 key
        auto edge = sl.scan(1, 10);
        CHECK(edge.size() == 9, "scan(1,10) 应为 1..9 共 9 条");
    }

    // ------------------------------------------------------------------ D
    section("D. scan_n / count_range / scan_reverse");
    {
        SkipList<int, std::string> sl(8);
        for (int i = 1; i <= 20; i++) sl.insert_element(i, "v");

        auto n5 = sl.scan_n(5, 5);
        CHECK(n5.size() == 5 && n5[0].first == 5 && n5[4].first == 9, "scan_n(5,5) 应为 5..9");

        auto n99 = sl.scan_n(15, 99);
        CHECK(n99.size() == 6, "scan_n(15,99) 只能取到 15..20 共 6 条");

        CHECK(sl.count_range(1, 11) == 10, "count_range(1,11) 应为 10");
        CHECK(sl.count_range(5, 6) == 1, "count_range(5,6) 应为 1");
        CHECK(sl.count_range(50, 60) == 0, "count_range 空区间应为 0");

        auto rev = sl.scan_reverse(15, 3);            // <=15 的最大 3 条，降序
        CHECK(rev.size() == 3, "scan_reverse 应返回 3 条");
        CHECK(rev[0].first == 15 && rev[1].first == 14 && rev[2].first == 13,
              "scan_reverse 应为 15,14,13 降序");

        auto revAll = sl.scan_reverse(100, 5);
        CHECK(revAll.size() == 5 && revAll[0].first == 20, "scan_reverse 上限应取最大 key");
    }

    // ------------------------------------------------------------------ E
    section("E. 并发：scan 与写并发（TSan 应无告警）");
    {
        SkipList<int, std::string> sl(16);
        for (int i = 0; i < 1000; i++) sl.insert_element(i, "v");

        std::atomic<bool> stop{false};
        std::atomic<long> scans{0};

        std::thread writer([&] {
            int k = 1000;
            while (!stop) sl.insert_element(k++, "w");
        });
        std::vector<std::thread> readers;
        for (int i = 0; i < 3; i++) {
            readers.emplace_back([&] {
                while (!stop) {
                    auto r = sl.scan(100, 200);      // 内部 shared_lock，快照
                    (void)r;
                    scans++;
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
        stop = true;
        writer.join();
        for (auto& t : readers) t.join();

        std::cout << "  并发 scan 完成 " << scans.load() << " 次，无崩溃" << std::endl;
        CHECK(scans.load() > 0, "并发 scan 应实际执行过");
    }

    // ------------------------------------------------------------------ F
    section("F. 与 std::map 对拍（随机操作）");
    {
        SkipList<int, std::string> sl(16);
        std::map<int, std::string> ref;
        std::mt19937 rng(12345);

        // 随机插入（key 0..499，重复插入不覆盖 —— 与原版语义一致）
        for (int i = 0; i < 2000; i++) {
            int k = rng() % 500;
            if (ref.find(k) == ref.end()) {
                sl.insert_element(k, "v" + std::to_string(k));
                ref[k] = "v" + std::to_string(k);
            }
        }
        // 随机删除
        for (int i = 0; i < 300; i++) {
            int k = rng() % 500;
            if (ref.erase(k)) sl.delete_element(k);
        }

        CHECK((size_t)sl.size() == ref.size(), "size 应与 std::map 一致");

        // 逐条比对全量
        bool same = true;
        auto it = sl.begin();
        for (auto& kv : ref) {
            if (it == sl.end() || it->first != kv.first || it->second != kv.second) {
                same = false; break;
            }
            ++it;
        }
        if (it != sl.end()) same = false;
        CHECK(same, "全量遍历应与 std::map 完全一致");

        // 随机区间对拍 50 次
        bool range_ok = true;
        for (int t = 0; t < 50; t++) {
            int a = rng() % 600, b = rng() % 600;
            if (a > b) std::swap(a, b);
            auto got = sl.scan(a, b);
            std::vector<std::pair<int, std::string>> exp;
            for (auto it2 = ref.lower_bound(a); it2 != ref.end() && it2->first < b; ++it2)
                exp.emplace_back(it2->first, it2->second);
            if (got != exp) { range_ok = false; break; }
        }
        CHECK(range_ok, "50 次随机区间 scan 应与 std::map 一致");

        // count_range 对拍
        bool cnt_ok = true;
        for (int t = 0; t < 50; t++) {
            int a = rng() % 600, b = rng() % 600;
            if (a > b) std::swap(a, b);
            size_t exp = 0;
            for (auto it2 = ref.lower_bound(a); it2 != ref.end() && it2->first < b; ++it2) exp++;
            if (sl.count_range(a, b) != exp) { cnt_ok = false; break; }
        }
        CHECK(cnt_ok, "50 次随机 count_range 应与 std::map 一致");
    }

    std::cout.clear();
    std::cout << "\n==================================================" << std::endl;
    std::cout << " 通过: " << g_pass << "   失败: " << g_fail << std::endl;
    std::cout << (g_fail == 0 ? " ✅ 全部通过" : " ❌ 有失败项") << std::endl;
    std::cout << "==================================================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
