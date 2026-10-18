/* ============================================================================
 * [P4] WAL + 崩溃恢复 测试
 * ----------------------------------------------------------------------------
 * 覆盖：
 *   A. CRC32 正确性（标准测试向量）
 *   B. WAL 记录编解码往返
 *   C. 基本恢复：写 -> 重新打开 -> 数据还在
 *   D. 覆盖语义：同一 key 写多次，恢复后是最后一次的值
 *   E. 删除恢复
 *   F. 撕裂写（torn write）：截断文件中间 -> 恢复时能识别并截断
 *   G. CRC 损坏：翻转一个字节 -> 恢复时能检出并丢弃
 *   H. 快照 + WAL 组合恢复
 *   I. 快照后 WAL 被清空
 *
 * 编译运行：
 *   g++ tests/p4_wal_test.cpp -o bin/p4_test --std=c++17 -pthread
 *   ./bin/p4_test
 * ============================================================================
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../kvstore.h"

static int g_pass = 0, g_fail = 0;

// 注意：测试输出全部走 std::cerr。
// 因为 main 开头对 std::cout 设了 failbit 来屏蔽跳表的逐条打印，
// 如果测试输出也用 cout 就会被一起吞掉（这正是第一版的问题）。
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; std::cerr << "  ❌ FAIL: " << msg << std::endl; }     \
    } while (0)

static void section(const char* name) {
    std::cerr << "\n===== " << name << " =====" << std::endl;
}

static void rmtree(const std::string& dir) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (system(cmd.c_str()) != 0) { /* ignore */ }
}
static void mkdirp(const std::string& dir) {
    std::string cmd = "mkdir -p '" + dir + "'";
    if (system(cmd.c_str()) != 0) { /* ignore */ }
}
static size_t filesize(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return static_cast<size_t>(st.st_size);
}

int main() {
    std::cout.setstate(std::ios::failbit);   // 屏蔽跳表内部打印

    const std::string BASE = "/tmp/p4test";

    // ------------------------------------------------------------------ A
    section("A. CRC32 正确性");
    {
        // 标准测试向量：CRC-32/ISO-HDLC("123456789") = 0xCBF43926
        uint32_t c = CRC32::compute(std::string("123456789"));
        CHECK(c == 0xCBF43926u, "CRC32(\"123456789\") 应为 0xCBF43926");

        CHECK(CRC32::compute(std::string("")) == 0x00000000u, "CRC32(\"\") 应为 0");
        // 同样的输入必须得到同样的输出（确定性）
        CHECK(CRC32::compute(std::string("hello")) == CRC32::compute(std::string("hello")),
              "CRC32 应确定性");
        // 差一个字节，结果必须不同
        CHECK(CRC32::compute(std::string("hello")) != CRC32::compute(std::string("hellp")),
              "CRC32 对单字节差异应敏感");
    }

    // ------------------------------------------------------------------ B
    section("B. WAL 记录编解码往返");
    {
        std::string rec = WALRecord::encode(WALRecord::kOpPut, "42", "hello");
        // 4(crc) + 1(op) + 4(klen) + 4(vlen) + 2(key) + 5(value) = 20
        CHECK(rec.size() == 20, "记录长度应为 20 字节");
        CHECK(rec[4] == 'P', "op 应为 'P'");

        uint32_t klen, vlen;
        std::memcpy(&klen, rec.data() + 5, 4);
        std::memcpy(&vlen, rec.data() + 9, 4);
        CHECK(klen == 2 && vlen == 5, "klen/vlen 应为 2/5");
        CHECK(rec.substr(13, 2) == "42", "key 应为 \"42\"");
        CHECK(rec.substr(15, 5) == "hello", "value 应为 \"hello\"");
    }

    // ------------------------------------------------------------------ C
    section("C. 基本恢复：写 -> 重开 -> 数据还在");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            CHECK(db.ok(), "KVStore 应能打开");
            for (int i = 0; i < 100; i++) db.put(i, "v" + std::to_string(i));
            CHECK(db.size() == 100, "写入后应有 100 条");
            // 析构 = 进程退出，模拟"正常关闭但没做快照"
        }
        {
            KVStore<int, std::string> db(BASE);
            size_t n = db.recover();
            CHECK(n == 100, "应从 WAL 恢复 100 条");
            CHECK(db.size() == 100, "恢复后 size 应为 100");
            std::string v;
            CHECK(db.get(42, v) && v == "v42", "key=42 应恢复为 v42");
            CHECK(db.get(99, v) && v == "v99", "key=99 应恢复为 v99");
            CHECK(!db.get(100, v), "key=100 不应存在");
        }
    }

    // ------------------------------------------------------------------ D
    section("D. 覆盖语义：同一 key 写多次，取最后一次");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            db.put(7, "first");
            db.put(7, "second");
            db.put(7, "third");
            db.put(8, "other");
        }
        {
            KVStore<int, std::string> db(BASE);
            db.recover();
            std::string v;
            // 这是 P4 必须加 upsert_element 的原因：
            // 如果重放走原版 insert_element，这里会得到 "first"
            CHECK(db.get(7, v) && v == "third", "重放后 key=7 应为最后一次的 third");
            CHECK(db.size() == 2, "重复写入不应增加元素数");
        }
    }

    // ------------------------------------------------------------------ E
    section("E. 删除恢复");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            for (int i = 0; i < 50; i++) db.put(i, "v");
            db.del(10);
            db.del(20);
            db.del(30);
        }
        {
            KVStore<int, std::string> db(BASE);
            db.recover();
            std::string v;
            CHECK(db.size() == 47, "删除 3 条后应剩 47 条");
            CHECK(!db.get(10, v), "key=10 应已被删除");
            CHECK(!db.get(20, v), "key=20 应已被删除");
            CHECK(db.get(11, v), "key=11 应还在");
        }
    }

    // ------------------------------------------------------------------ F
    section("F. 撕裂写：截断文件中间 -> 恢复时识别并截断");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            for (int i = 0; i < 100; i++) db.put(i, "value" + std::to_string(i));
        }
        std::string wal = BASE + "/wal.log";
        size_t full = filesize(wal);
        CHECK(full > 0, "WAL 文件应非空");

        // 模拟崩溃：砍掉最后 17 个字节（必然落在某条记录中间）
        {
            std::ifstream in(wal, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            in.close();
            buf.resize(buf.size() - 17);
            std::ofstream out(wal, std::ios::binary | std::ios::trunc);
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }

        {
            KVStore<int, std::string> db(BASE);
            size_t n = db.recover();
            // 应恢复出「完整的那些记录」，最后 1~2 条因不完整被丢弃
            CHECK(n >= 97 && n <= 99, "应恢复 97~99 条（最后一条不完整）");
            CHECK(db.size() == static_cast<int>(n), "size 应与恢复条数一致");
            // 恢复后 WAL 应被截断到合法长度
            size_t after = filesize(wal);
            CHECK(after <= full - 17 + 64, "恢复后 WAL 应被截断到合法长度");
        }
    }

    // ------------------------------------------------------------------ G
    section("G. CRC 损坏：翻转字节 -> 检出并丢弃");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            for (int i = 0; i < 50; i++) db.put(i, "v" + std::to_string(i));
        }
        std::string wal = BASE + "/wal.log";

        // 翻转文件中部的某个字节（破坏某条记录的 CRC）
        {
            std::ifstream in(wal, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            in.close();
            size_t pos = buf.size() / 2;
            buf[pos] = static_cast<char>(buf[pos] ^ 0xFF);   // 翻转
            std::ofstream out(wal, std::ios::binary | std::ios::trunc);
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }

        {
            KVStore<int, std::string> db(BASE);
            size_t n = db.recover();
            // 损坏点之后的记录应全部被丢弃
            CHECK(n < 50, "CRC 损坏后恢复条数应少于 50");
            CHECK(n > 0, "损坏点之前的记录应仍能恢复");
            std::cerr << "  (损坏点之前恢复了 " << n << " 条)" << std::endl;
        }
    }

    // ------------------------------------------------------------------ H
    section("H. 快照 + WAL 组合恢复");
    {
        rmtree(BASE); mkdirp(BASE);
        {
            KVStore<int, std::string> db(BASE);
            for (int i = 0; i < 100; i++) db.put(i, "old" + std::to_string(i));
            CHECK(db.snapshot(), "快照应成功");
            // 快照之后再写 50 条（这些只在 WAL 里）
            for (int i = 100; i < 150; i++) db.put(i, "new" + std::to_string(i));
            // 再改几个快照里已有的 key
            db.put(0, "updated0");
            db.put(1, "updated1");
        }
        {
            KVStore<int, std::string> db(BASE);
            size_t n = db.recover();
            CHECK(n == 152, "应恢复 100(快照) + 52(WAL) = 152 条记录");
            CHECK(db.size() == 150, "最终应有 150 个不同 key");
            std::string v;
            CHECK(db.get(0, v) && v == "updated0", "快照中的 key 应被 WAL 覆盖为 updated0");
            CHECK(db.get(50, v) && v == "old50", "快照中的 key=50 应保持 old50");
            CHECK(db.get(120, v) && v == "new120", "WAL 中的 key=120 应恢复");
        }
    }

    // ------------------------------------------------------------------ I
    section("I. 快照后 WAL 被清空");
    {
        rmtree(BASE); mkdirp(BASE);
        KVStore<int, std::string> db(BASE);
        for (int i = 0; i < 1000; i++) db.put(i, "v");
        size_t before = filesize(BASE + "/wal.log");
        CHECK(before > 0, "快照前 WAL 应非空");

        CHECK(db.snapshot(), "快照应成功");
        size_t after = filesize(BASE + "/wal.log");
        CHECK(after == 0, "快照后 WAL 应被清空");
        CHECK(filesize(BASE + "/snapshot") > 0, "快照文件应非空");
        std::cerr << "  (WAL 从 " << before << " 字节压缩到 " << after
                  << " 字节，快照 " << filesize(BASE + "/snapshot") << " 字节)" << std::endl;
    }

    rmtree(BASE);

    std::cerr << "\n==================================================" << std::endl;
    std::cerr << " 通过: " << g_pass << "   失败: " << g_fail << std::endl;
    std::cerr << (g_fail == 0 ? " ✅ 全部通过" : " ❌ 有失败项") << std::endl;
    std::cerr << "==================================================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
