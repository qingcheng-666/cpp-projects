/* ============================================================================
 * [P3] 范围查询性能基准
 * ----------------------------------------------------------------------------
 * 目的：用数据回答「跳表的范围查询到底强在哪」。
 *
 * 对比三种做法（同样取区间内 k 条）：
 *   1. 跳表 scan()            —— 一次 O(log n) 定位 + 顺序扫 k 条
 *   2. std::map lower_bound   —— 红黑树，同样 O(log n) 定位
 *   3. 全量遍历 + 过滤        —— 模拟「哈希表只能用这种方式做范围查询」
 *                                （哈希表无序，想按 key 范围取数必须全扫）
 *
 * 编译运行：
 *   g++ tests/p3_bench.cpp -o bin/p3_bench --std=c++17 -pthread -O2
 *   ./bin/p3_bench
 * ============================================================================
 */
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>
#include "../skiplist.h"

using Clock = std::chrono::high_resolution_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main() {
    const int N = 1000000;        // 100 万条数据
    const int K = 100;            // 每次取 100 条
    const int REPS = 1000;        // 重复 1000 次

    std::cout.clear();
    std::cout << "数据规模 N = " << N << "，每次取 K = " << K
              << " 条，重复 " << REPS << " 次\n" << std::endl;

    // ---------------- 建表 ----------------
    SkipList<int, std::string> sl(20);
    std::map<int, std::string> mp;
    std::unordered_map<int, std::string> um;

    std::cout << "建表中..." << std::endl;
    std::cout.setstate(std::ios::failbit);   // 屏蔽建表期间跳表的逐条插入打印
    for (int i = 0; i < N; i++) {
        sl.insert_element(i, "v");
        mp[i] = "v";
        um[i] = "v";
    }
    std::cout.clear();
    std::cout << "建表完成：跳表 size = " << sl.size() << "\n" << std::endl;

    std::mt19937 rng(42);
    std::vector<int> starts(REPS);
    for (int i = 0; i < REPS; i++) starts[i] = rng() % (N - K);

    // ---------------- 1. 跳表 scan ----------------
    volatile size_t sink = 0;
    auto t0 = Clock::now();
    for (int r = 0; r < REPS; r++) {
        auto res = sl.scan(starts[r], starts[r] + K);
        sink += res.size();
    }
    auto t1 = Clock::now();
    double t_skip = ms(t0, t1);

    // ---------------- 2. std::map lower_bound ----------------
    t0 = Clock::now();
    for (int r = 0; r < REPS; r++) {
        size_t c = 0;
        for (auto it = mp.lower_bound(starts[r]); it != mp.end() && it->first < starts[r] + K; ++it) c++;
        sink += c;
    }
    t1 = Clock::now();
    double t_map = ms(t0, t1);

    // ---------------- 3. 哈希表全量遍历 + 过滤 ----------------
    // 哈希表无序，想按 key 范围取数只能全扫一遍
    t0 = Clock::now();
    for (int r = 0; r < REPS; r++) {
        size_t c = 0;
        for (auto& kv : um) {
            if (kv.first >= starts[r] && kv.first < starts[r] + K) c++;
        }
        sink += c;
    }
    t1 = Clock::now();
    double t_hash = ms(t0, t1);

    // ---------------- 输出 ----------------
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=============================================================" << std::endl;
    std::cout << " 方式                        总耗时(ms)   单次(us)   相对倍数" << std::endl;
    std::cout << "-------------------------------------------------------------" << std::endl;
    std::cout << " 1. 跳表 scan()              " << std::setw(9) << t_skip
              << std::setw(11) << (t_skip * 1000 / REPS)
              << std::setw(11) << "1.00x" << std::endl;
    std::cout << " 2. std::map lower_bound     " << std::setw(9) << t_map
              << std::setw(11) << (t_map * 1000 / REPS)
              << std::setw(11) << (t_map / t_skip) << "x" << std::endl;
    std::cout << " 3. 哈希表全量扫描           " << std::setw(9) << t_hash
              << std::setw(11) << (t_hash * 1000 / REPS)
              << std::setw(11) << (t_hash / t_skip) << "x" << std::endl;
    std::cout << "=============================================================" << std::endl;
    std::cout << "\n结论：" << std::endl;
    std::cout << "  - 跳表 scan 与 std::map 同属 O(log n) 定位，量级相当" << std::endl;
    std::cout << "  - 哈希表无序，范围查询只能全量扫描，慢 "
              << std::fixed << std::setprecision(0) << (t_hash / t_skip) << " 倍左右" << std::endl;
    std::cout << "  - 这就是 Redis ZSET 选跳表、LevelDB MemTable 选跳表的根本原因" << std::endl;
    std::cout << "\n(校验 sink = " << sink << ")" << std::endl;
    return 0;
}
