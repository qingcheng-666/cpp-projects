/* ============================================================================
 * [P4] 崩溃恢复演示 —— 校验端
 * ----------------------------------------------------------------------------
 * 从 WAL 恢复后，检查「所有被 ACK 过的 key」是否都在。
 *
 * 用法：
 *   crash_recover <data_dir> <last_acked_key>
 *
 * 判定标准：
 *   - 所有 key ∈ [0, last_acked_key] 都必须能读到，且值正确
 *   - 这就是「已确认的写入不能丢」（durability）的验证
 *
 * 编译：g++ tests/crash_recover.cpp -o bin/crash_recover --std=c++17 -pthread
 * ============================================================================
 */
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../kvstore.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法: crash_recover <data_dir> <last_acked_key>" << std::endl;
        return 2;
    }
    std::string dir = argv[1];
    int last_acked = std::atoi(argv[2]);

    std::cout.setstate(std::ios::failbit);   // 屏蔽跳表打印

    KVStore<int, std::string> db(dir);
    if (!db.ok()) {
        std::cerr << "FATAL: 无法打开 " << dir << std::endl;
        return 1;
    }

    size_t recovered = db.recover(false);

    // 逐条校验 [0, last_acked] 全部可读且值正确
    int missing = 0;
    int wrong = 0;
    for (int k = 0; k <= last_acked; k++) {
        std::string v;
        if (!db.get(k, v)) {
            if (missing < 5) std::cerr << "  丢失 key=" << k << std::endl;
            missing++;
        } else if (v != "value_" + std::to_string(k)) {
            if (wrong < 5) std::cerr << "  值错误 key=" << k << " got=" << v << std::endl;
            wrong++;
        }
    }

    std::cerr << "RECOVER_RESULT recovered_records=" << recovered
              << " size=" << db.size()
              << " checked=" << (last_acked + 1)
              << " missing=" << missing
              << " wrong=" << wrong << std::endl;

    if (missing == 0 && wrong == 0) {
        std::cerr << "✅ 所有已确认的写入全部恢复（durability 成立）" << std::endl;
        return 0;
    }
    std::cerr << "❌ 有数据丢失" << std::endl;
    return 1;
}
