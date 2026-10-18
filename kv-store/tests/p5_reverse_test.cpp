/* ============================================================================
 * [P5] backward 指针 + 反向遍历测试
 * ----------------------------------------------------------------------------
 * 覆盖：
 *   A. backward 链表完整性（正向/反向互为逆）
 *   B. 插入后 backward 正确
 *   C. 删除后 backward 正确（最容易出错的地方）
 *   D. 删头/删尾/删中间/删光
 *   E. scan_reverse 语义与边界
 *   F. 反向迭代器
 *   G. 与 std::map 对拍（随机操作后反向遍历一致）
 *   H. 复杂度验证：scan_reverse 不再依赖数据规模
 *
 * 编译运行：
 *   g++ tests/p5_reverse_test.cpp -o bin/p5_test --std=c++17 -pthread
 *   ./bin/p5_test
 * ============================================================================
 */
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../skiplist.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; std::cerr << "  ❌ FAIL: " << msg << std::endl; }     \
    } while (0)

static void section(const char* name) {
    std::cerr << "\n===== " << name << " =====" << std::endl;
}

using SL = SkipList<int, std::string>;

// 把整个跳表按正向收集成 vector
static std::vector<int> forward_keys(const SL& sl) {
    std::vector<int> v;
    for (auto it = sl.begin(); it != sl.end(); ++it) v.push_back(it->first);
    return v;
}

// 把整个跳表按反向收集成 vector
static std::vector<int> backward_keys(const SL& sl) {
    std::vector<int> v;
    for (auto it = sl.rbegin(); it != sl.rend(); ++it) v.push_back(it->first);
    return v;
}

/* ----------------------------------------------------------------------------
 * 核心不变式：backward 链必须和 forward 链互为逆
 *   正向: h -> n1 -> n2 -> ... -> nk -> null
 *   反向: nk -> ... -> n2 -> n1 -> null   （n1 的 backward 是 null）
 * 任何一次插入/删除破坏了这个关系，反向遍历就会走错。
 * -------------------------------------------------------------------------- */
static bool backward_consistent(const SL& sl) {
    auto fwd = forward_keys(sl);
    auto bwd = backward_keys(sl);
    std::reverse(bwd.begin(), bwd.end());
    return fwd == bwd;
}

int main() {
    std::cout.setstate(std::ios::failbit);

    // ------------------------------------------------------------------ A
    section("A. backward 链表完整性");
    {
        SL sl(6);
        for (int k : {5, 1, 9, 3, 7}) sl.insert_element(k, "v");

        CHECK(backward_consistent(sl), "插入后 backward 应与 forward 互为逆");

        auto fwd = forward_keys(sl);
        auto bwd = backward_keys(sl);
        CHECK(fwd == std::vector<int>({1, 3, 5, 7, 9}), "正向应为升序 1,3,5,7,9");
        CHECK(bwd == std::vector<int>({9, 7, 5, 3, 1}), "反向应为降序 9,7,5,3,1");

        // 第一个数据节点的 backward 必须是 nullptr
        CHECK(sl.begin().raw()->backward == nullptr, "最小节点的 backward 应为 nullptr");
    }

    // ------------------------------------------------------------------ B
    section("B. 各种顺序插入后仍一致");
    {
        // 升序插入
        {
            SL sl(6);
            for (int i = 1; i <= 20; i++) sl.insert_element(i, "v");
            CHECK(backward_consistent(sl), "升序插入后一致");
        }
        // 降序插入
        {
            SL sl(6);
            for (int i = 20; i >= 1; i--) sl.insert_element(i, "v");
            CHECK(backward_consistent(sl), "降序插入后一致");
        }
        // 随机插入
        {
            SL sl(6);
            std::mt19937 rng(7);
            for (int i = 0; i < 200; i++) sl.insert_element((int)(rng() % 1000), "v");
            CHECK(backward_consistent(sl), "随机插入后一致");
        }
        // 往中间插
        {
            SL sl(6);
            sl.insert_element(1, "v");
            sl.insert_element(100, "v");
            for (int i = 50; i < 60; i++) sl.insert_element(i, "v");
            CHECK(backward_consistent(sl), "往中间插入后一致");
        }
    }

    // ------------------------------------------------------------------ C
    section("C. 删除后 backward 正确（最易出错）");
    {
        SL sl(6);
        for (int i = 1; i <= 10; i++) sl.insert_element(i, "v");

        sl.delete_element(5);
        CHECK(backward_consistent(sl), "删除中间元素后一致");
        auto bwd = backward_keys(sl);
        CHECK(bwd == std::vector<int>({10, 9, 8, 7, 6, 4, 3, 2, 1}),
              "删 5 后反向应为 10..6,4..1");

        sl.delete_element(10);
        CHECK(backward_consistent(sl), "删除最大元素后一致");
        CHECK(backward_keys(sl).front() == 9, "删 10 后最大应为 9");

        sl.delete_element(1);
        CHECK(backward_consistent(sl), "删除最小元素后一致");
        CHECK(backward_keys(sl).back() == 2, "删 1 后最小应为 2");
    }

    // ------------------------------------------------------------------ D
    section("D. 极端删除场景");
    {
        // 删光
        {
            SL sl(6);
            for (int i = 1; i <= 10; i++) sl.insert_element(i, "v");
            for (int i = 1; i <= 10; i++) sl.delete_element(i);
            CHECK(sl.size() == 0, "删光后 size=0");
            CHECK(backward_consistent(sl), "删光后一致");
            CHECK(sl.rbegin() == sl.rend(), "空表 rbegin == rend");
        }
        // 只剩一个
        {
            SL sl(6);
            sl.insert_element(42, "v");
            CHECK(backward_consistent(sl), "单元素一致");
            CHECK(sl.rbegin().key() == 42, "rbegin 是唯一元素");
            sl.delete_element(42);
            CHECK(backward_consistent(sl), "删掉唯一元素后一致");
        }
        // 交替插删
        {
            SL sl(6);
            std::mt19937 rng(99);
            std::map<int, std::string> ref;
            for (int i = 0; i < 500; i++) {
                int k = (int)(rng() % 100);
                if (rng() % 2) {
                    sl.upsert_element(k, "v");
                    ref[k] = "v";
                } else {
                    if (ref.erase(k)) sl.delete_element(k);
                }
            }
            CHECK(backward_consistent(sl), "500 次随机插删后一致");
            CHECK((size_t)sl.size() == ref.size(), "size 与 std::map 一致");
        }
        // 连续删相邻节点（考验 backward 重连）
        {
            SL sl(6);
            for (int i = 0; i < 20; i++) sl.insert_element(i, "v");
            for (int i = 5; i < 15; i++) sl.delete_element(i);
            CHECK(backward_consistent(sl), "连续删除一段后一致");
            auto bwd = backward_keys(sl);
            CHECK(bwd == std::vector<int>({19, 18, 17, 16, 15, 4, 3, 2, 1, 0}),
                  "连续删除后反向序列正确");
        }
    }

    // ------------------------------------------------------------------ E
    section("E. scan_reverse 语义与边界");
    {
        SL sl(6);
        for (int i = 1; i <= 20; i++) sl.insert_element(i, "v" + std::to_string(i));

        auto r = sl.scan_reverse(15, 3);
        CHECK(r.size() == 3, "scan_reverse(15,3) 应返回 3 条");
        CHECK(r[0].first == 15 && r[1].first == 14 && r[2].first == 13, "应为 15,14,13");

        // start_key 不存在于表中 -> 取比它小的最大值
        auto r2 = sl.scan_reverse(15, 2);
        CHECK(r2[0].first == 15, "start 存在时从它开始");

        auto r3 = sl.scan_reverse(100, 3);
        CHECK(r3.size() == 3 && r3[0].first == 20, "start 超上界应取最大值 20");

        auto r4 = sl.scan_reverse(0, 5);
        CHECK(r4.empty(), "start 小于所有 key 应为空");

        auto r5 = sl.scan_reverse(1, 5);
        CHECK(r5.size() == 1 && r5[0].first == 1, "start=1 只能取到 1 条");

        // n 大于总数
        auto r6 = sl.scan_reverse(20, 999);
        CHECK(r6.size() == 20, "n 超过总数应返回全部 20 条");
        CHECK(r6.front().first == 20 && r6.back().first == 1, "全量反向应为 20..1");

        // start_key 落在空隙里（比如 15.5 不存在，用 15 测等价性）
        auto r7 = sl.scan_reverse(14, 2);
        CHECK(r7[0].first == 14 && r7[1].first == 13, "从 14 往回取");

        // 空表
        SL empty(4);
        CHECK(empty.scan_reverse(100, 5).empty(), "空表 scan_reverse 应为空");
    }

    // ------------------------------------------------------------------ F
    section("F. 反向迭代器");
    {
        SL sl(6);
        for (int i = 1; i <= 5; i++) sl.insert_element(i * 10, "v");

        std::vector<int> got;
        for (auto it = sl.rbegin(); it != sl.rend(); ++it) got.push_back(it->first);
        CHECK(got == std::vector<int>({50, 40, 30, 20, 10}), "rbegin 遍历应为降序");

        // operator->
        auto it = sl.rbegin();
        CHECK(it->first == 50, "rbegin()->first 应为 50");
        CHECK(it->second == "v", "rbegin()->second");

        // 后置 ++
        auto it2 = sl.rbegin();
        auto old = it2++;
        CHECK(old.key() == 50 && it2.key() == 40, "后置 ++ 语义正确");

        // 空表
        SL empty(4);
        CHECK(empty.rbegin() == empty.rend(), "空表 rbegin == rend");
    }

    // ------------------------------------------------------------------ G
    section("G. 与 std::map 对拍");
    {
        SL sl(10);
        std::map<int, std::string> ref;
        std::mt19937 rng(2024);

        for (int i = 0; i < 3000; i++) {
            int k = (int)(rng() % 800);
            if (rng() % 3 != 0) {
                sl.upsert_element(k, "v" + std::to_string(k));
                ref[k] = "v" + std::to_string(k);
            } else {
                if (ref.erase(k)) sl.delete_element(k);
            }
        }

        CHECK(backward_consistent(sl), "随机 3000 次操作后 backward 一致");
        CHECK((size_t)sl.size() == ref.size(), "size 与 map 一致");

        // 全量反向比对
        std::vector<int> got, want;
        for (auto it = sl.rbegin(); it != sl.rend(); ++it) got.push_back(it->first);
        for (auto it = ref.rbegin(); it != ref.rend(); ++it) want.push_back(it->first);
        CHECK(got == want, "全量反向遍历与 std::map 一致");

        // 随机 scan_reverse 对拍 50 次
        bool ok = true;
        for (int t = 0; t < 50; t++) {
            int start = (int)(rng() % 900);
            size_t n = rng() % 20;
            auto r = sl.scan_reverse(start, n);

            std::vector<int> exp;
            for (auto it = ref.upper_bound(start); it != ref.begin() && exp.size() < n; ) {
                --it;
                exp.push_back(it->first);
            }
            std::vector<int> gotk;
            for (auto& kv : r) gotk.push_back(kv.first);
            if (gotk != exp) { ok = false; break; }
        }
        CHECK(ok, "50 次随机 scan_reverse 与 std::map 一致");
    }

    // ------------------------------------------------------------------ H
    section("H. 复杂度验证：反向查询不再依赖数据规模");
    {
        // 建 100 万数据的表
        SL sl(20);
        const int N = 1000000;
        for (int i = 0; i < N; i++) sl.insert_element(i, "v");
        CHECK(sl.size() == N, "建成 100 万条");
        CHECK(backward_consistent(sl), "100 万条后 backward 一致");

        // 从末尾往回取 100 条。
        // P3 的旧实现（正向扫到 start_key）在这里要扫 100 万次；
        // P5 用 upper_bound 一次 O(log n) 定位 + 100 步 backward。
        auto t0 = std::chrono::high_resolution_clock::now();
        auto r = sl.scan_reverse(N - 1, 100);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        CHECK(r.size() == 100, "取到 100 条");
        CHECK(r[0].first == N - 1, "第一条是最大值");
        CHECK(r[99].first == N - 100, "第 100 条正确");

        // 取表头附近 100 条，耗时应该和取表尾附近差不多
        t0 = std::chrono::high_resolution_clock::now();
        auto r2 = sl.scan_reverse(200, 100);
        t1 = std::chrono::high_resolution_clock::now();
        double ms2 = std::chrono::duration<double, std::milli>(t1 - t0).count();

        CHECK(r2.size() == 100 && r2[0].first == 200, "表头附近取数正确");

        std::cerr << "  表尾取 100 条: " << ms << " ms" << std::endl;
        std::cerr << "  表头取 100 条: " << ms2 << " ms" << std::endl;
        std::cerr << "  （两者应同量级 —— 证明不再依赖 start_key 的位置）" << std::endl;

        // 只要不是数量级差异就算通过（避免 VM 抖动导致误判）
        double ratio = (ms2 + 0.01) / (ms + 0.01);
        CHECK(ratio < 20.0, "表头/表尾取数耗时应在同一量级（O(log n + n) 而非 O(range)）");
    }

    std::cerr << "\n==================================================" << std::endl;
    std::cerr << " 通过: " << g_pass << "   失败: " << g_fail << std::endl;
    std::cerr << (g_fail == 0 ? " ✅ 全部通过" : " ❌ 有失败项") << std::endl;
    std::cerr << "==================================================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
