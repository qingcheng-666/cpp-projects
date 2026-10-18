/* ============================================================================
 * [P6 前置] KVStore 写路径并发安全测试
 * ----------------------------------------------------------------------------
 * 背景：
 *   P4 的 KVStore::put() 做两件事：
 *       ① _wal.append(...)        —— 写日志
 *       ② _list.upsert_element()  —— 改内存
 *   跳表内部有 shared_mutex 保护，但 WAL 是【裸 ofstream】，
 *   多个线程同时 append 会竞争 _out 和 _bytes。
 *
 * 这个测试就是来验证这个怀疑的。
 *
 * 编译运行：
 *   g++ tests/p4_concurrency_test.cpp -o bin/p4_conc --std=c++17 -pthread -fsanitize=thread
 *   setarch $(uname -m) -R ./bin/p4_conc
 * ============================================================================
 */
#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../kvstore.h"

int main() {
    std::cout.setstate(std::ios::failbit);

    const std::string DIR = "/tmp/p4conc";
    if (system(("rm -rf '" + DIR + "' && mkdir -p '" + DIR + "'").c_str()) != 0) {}

    const int THREADS = 4;
    const int PER_THREAD = 500;

    KVStore<int, std::string> db(DIR);
    if (!db.ok()) {
        std::cerr << "无法打开 KVStore" << std::endl;
        return 1;
    }

    std::atomic<int> ok_count{0};
    std::vector<std::thread> ts;

    for (int t = 0; t < THREADS; t++) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; i++) {
                int key = t * PER_THREAD + i;      // 每个线程写自己的 key 区间
                if (db.put(key, "v" + std::to_string(key))) ok_count++;
            }
        });
    }
    for (auto& th : ts) th.join();

    int expected = THREADS * PER_THREAD;
    std::cerr << "put 成功: " << ok_count.load() << " / " << expected << std::endl;
    std::cerr << "内存 size: " << db.size() << std::endl;

    // 重新打开并重放，看 WAL 有没有被写坏
    {
        KVStore<int, std::string> db2(DIR);
        size_t n = db2.recover();
        std::cerr << "重放记录数: " << n << std::endl;
        std::cerr << "恢复后 size: " << db2.size() << std::endl;

        int missing = 0;
        for (int k = 0; k < expected; k++) {
            std::string v;
            if (!db2.get(k, v)) missing++;
        }
        std::cerr << "丢失 key 数: " << missing << std::endl;
        std::cerr << (missing == 0 ? "✅ 无丢失" : "❌ 有丢失") << std::endl;
    }

    if (system(("rm -rf '" + DIR + "'").c_str()) != 0) {}
    return 0;
}
