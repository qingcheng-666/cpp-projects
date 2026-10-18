/* ============================================================================
 * [P4] 崩溃恢复演示 —— 写入端
 * ----------------------------------------------------------------------------
 * 持续写入，每成功一条就把 key 打到 stderr（无缓冲），
 * 这样被 kill -9 之后，我们能知道「最后一条确认成功的写入」是哪条。
 *
 * 配合 crash_demo.sh 使用：
 *   1. 启动本程序，它持续写入
 *   2. 脚本随机时刻 kill -9（模拟断电/进程被杀）
 *   3. 启动 crash_recover 验证：所有「确认过的写入」必须一条不少
 *
 * 编译：g++ tests/crash_writer.cpp -o bin/crash_writer --std=c++17 -pthread
 * ============================================================================
 */
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

#include "../kvstore.h"

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "/tmp/p4crash";
    int count = (argc > 2) ? std::atoi(argv[2]) : 100000;

    // 屏蔽跳表的逐条插入打印（它会污染 stdout）
    std::cout.setstate(std::ios::failbit);

    KVStore<int, std::string> db(dir);
    if (!db.ok()) {
        std::cerr << "FATAL: 无法打开 " << dir << std::endl;
        return 1;
    }

    // 恢复已有数据（支持反复崩溃-重启）
    db.recover(false);

    std::cerr << "WRITER_START dir=" << dir << " count=" << count
              << " size=" << db.size() << std::endl;

    for (int i = 0; i < count; i++) {
        int key = i;
        std::string val = "value_" + std::to_string(i);

        if (!db.put(key, val)) {
            std::cerr << "WRITE_FAIL " << key << std::endl;
            return 1;
        }

        // 关键：写成功后立刻用无缓冲的 stderr 报告。
        // 用 write() 系统调用而不是 std::cerr，确保即使被 kill 也不丢这条记录。
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "ACK %d\n", key);
        ssize_t w = write(2, buf, static_cast<size_t>(n));
        (void)w;
    }

    std::cerr << "WRITER_DONE total=" << db.size() << std::endl;
    return 0;
}
