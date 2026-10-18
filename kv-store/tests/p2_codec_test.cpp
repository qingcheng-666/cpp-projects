/* ============================================================================
 * [P2] Key Codec 测试：int key 与 string key
 * ----------------------------------------------------------------------------
 * 覆盖：
 *   A. KeyCodec<int> 编解码
 *   B. KeyCodec<std::string> 编解码（原样）
 *   C. 非法输入的错误处理
 *   D. 跳表用 string key：插入/查找/删除/排序
 *   E. 含冒号的 key（原版会被截断的地方）
 *   F. 含空格/中文/特殊字符的 key
 *   G. KVStore<string,string> 的 put/get/del/scan
 *   H. 持久化往返：快照 + WAL 恢复后 string key 完好
 *   I. 含 \t 和 \n 的 key（旧快照格式会坏掉的地方）
 *   J. int key 的 KVStore 仍然工作（向后兼容）
 *
 * 编译运行：
 *   g++ tests/p2_codec_test.cpp -o bin/p2_test --std=c++17 -pthread
 *   ./bin/p2_test
 * ============================================================================
 */
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "../codec.h"
#include "../kvstore.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; std::cerr << "  ❌ FAIL: " << msg << std::endl; }     \
    } while (0)

// 用模板函数而不是宏：既能比较 std::string 也能比较整数，
// 而且不需要 static_cast（那会把字符串比较搞坏）。
// 整数比较时统一提升到 long long，避免 size_t vs int 的有符号性警告。
template<typename A, typename B>
static void check_eq_impl(const A& a, const B& b, const std::string& msg, std::true_type) {
    long long x = static_cast<long long>(a), y = static_cast<long long>(b);
    if (x == y) { g_pass++; }
    else {
        g_fail++;
        std::cerr << "  ❌ FAIL: " << msg
                  << "  (got [" << x << "] want [" << y << "])" << std::endl;
    }
}
template<typename A, typename B>
static void check_eq_impl(const A& a, const B& b, const std::string& msg, std::false_type) {
    if (a == b) { g_pass++; }
    else {
        g_fail++;
        std::cerr << "  ❌ FAIL: " << msg
                  << "  (got [" << a << "] want [" << b << "])" << std::endl;
    }
}
template<typename A, typename B>
static void check_eq(const A& a, const B& b, const std::string& msg) {
    // 两边都是算术类型才走数值比较，否则走 operator==
    using both_arith = std::integral_constant<bool,
        std::is_arithmetic<A>::value && std::is_arithmetic<B>::value>;
    check_eq_impl(a, b, msg, both_arith{});
}
#define CHECK_EQ(a, b, msg) check_eq((a), (b), (msg))

/* ----------------------------------------------------------------------------
 * 前缀扫描的正确写法
 * ----------------------------------------------------------------------------
 * 想取所有以 prefix 开头的 key，区间不是 [prefix, prefix+';') 这种拍脑袋的写法，
 * 而是 [prefix, prefix 的下一个字典序字符串)。
 *
 * 为什么不能随便加个字符：ASCII 里 ':'(0x3A) < ';'(0x3B) < 'n'(0x6E)，
 * 所以 "user:1001" 和 "username" 会被 "user;" 分到不同区间 ——
 * 用 ["user", "user;") 只能拿到带冒号的那些。
 *
 * 惯用法：把最后一个字节 +1，得到"刚好大于所有 prefix* 的字符串"。
 * 这里简化成 append '\xff'（比任何 ASCII 都大），对本项目足够。
 * -------------------------------------------------------------------------- */
static std::string prefix_end(const std::string& prefix) {
    return prefix + '\xff';
}

static void section(const char* name) {
    std::cerr << "\n===== " << name << " =====" << std::endl;
}

static void rmtree(const std::string& d) {
    if (system(("rm -rf '" + d + "'").c_str()) != 0) {}
}

int main() {
    std::cout.setstate(std::ios::failbit);   // 屏蔽跳表内部打印

    // ------------------------------------------------------------------ A
    section("A. KeyCodec<int>");
    {
        CHECK_EQ(KeyCodec<int>::encode(42), std::string("42"), "encode(42)");
        CHECK_EQ(KeyCodec<int>::encode(0), std::string("0"), "encode(0)");
        CHECK_EQ(KeyCodec<int>::encode(-7), std::string("-7"), "encode(-7)");
        CHECK_EQ(KeyCodec<int>::decode("42"), 42, "decode(\"42\")");
        CHECK_EQ(KeyCodec<int>::decode("-7"), -7, "decode(\"-7\")");

        // 往返一致性
        bool ok = true;
        for (int v : {0, 1, -1, 100, -100, 2147483647, -2147483647}) {
            if (KeyCodec<int>::decode(KeyCodec<int>::encode(v)) != v) ok = false;
        }
        CHECK(ok, "int 编解码往返一致");
    }

    // ------------------------------------------------------------------ B
    section("B. KeyCodec<std::string>");
    {
        CHECK_EQ(KeyCodec<std::string>::encode("hello"), std::string("hello"), "encode 原样");
        CHECK_EQ(KeyCodec<std::string>::decode("hello"), std::string("hello"), "decode 原样");

        // 含冒号、空格、中文、控制字符都应原样保留
        std::vector<std::string> tricky = {
            "user:1001", "a b c", "中文键", "tab\there", "nl\nhere",
            "", ":", "::", "\x01\x02", "emoji🚀"
        };
        bool ok = true;
        for (auto& s : tricky) {
            if (KeyCodec<std::string>::decode(KeyCodec<std::string>::encode(s)) != s) {
                ok = false;
                std::cerr << "      往返失败: [" << s << "]" << std::endl;
            }
        }
        CHECK(ok, "string 编解码往返一致（含特殊字符）");
    }

    // ------------------------------------------------------------------ C
    section("C. 非法输入的错误处理");
    {
        auto throws = [](const std::string& s) {
            try { KeyCodec<int>::decode(s); return false; }
            catch (const std::exception&) { return true; }
        };
        CHECK(throws(""), "空字符串应抛异常");
        CHECK(throws("abc"), "非数字应抛异常");
        CHECK(throws("12a"), "尾部有字母应抛异常");
        CHECK(throws("1.5"), "小数应抛异常");
        CHECK(throws("-"), "只有负号应抛异常");
        CHECK(!throws("42"), "合法数字不应抛异常");
        CHECK(!throws("-42"), "合法负数不应抛异常");
    }

    // ------------------------------------------------------------------ D
    section("D. 跳表用 string key");
    {
        SkipList<std::string, std::string> sl(8);
        sl.insert_element("banana", "2");
        sl.insert_element("apple", "1");
        sl.insert_element("cherry", "3");

        CHECK_EQ(sl.size(), 3, "size");
        // 按字典序排序
        std::vector<std::string> keys;
        for (auto it = sl.begin(); it != sl.end(); ++it) keys.push_back(it->first);
        CHECK(keys.size() == 3 && keys[0] == "apple" && keys[1] == "banana" && keys[2] == "cherry",
              "应按字典序遍历");

        std::string v;
        std::string found_key;
        CHECK(sl.seek("banana", found_key, v) && found_key == "banana", "seek 命中 banana");
        CHECK(!sl.seek("durian", found_key, v), "seek 未命中 durian 应返回 false");
        CHECK(sl.search_element("apple"), "search 命中 apple");
        CHECK(!sl.search_element("durian"), "search 未命中 durian");

        sl.delete_element("banana");
        CHECK_EQ(sl.size(), 2, "删除后 size");
        CHECK(!sl.search_element("banana"), "banana 已删除");
    }

    // ------------------------------------------------------------------ E
    section("E. 含冒号的 key（原版会截断）");
    {
        // 原版 get_key_value_from_string 用 find（第一个冒号），
        // 会把 "user:1001" 切成 key="user", value="1001"。
        // 修成 rfind 后应该正确。
        SkipList<std::string, std::string> sl(8);
        sl.insert_element("user:1001", "alice");
        sl.insert_element("user:1002", "bob");
        sl.insert_element("user:1001:name", "ALICE");

        CHECK_EQ(sl.size(), 3, "含冒号 key 的 size");
        CHECK(sl.search_element("user:1001"), "能查到 user:1001");
        CHECK(sl.search_element("user:1001:name"), "能查到 user:1001:name");

        // 范围查询：所有 user: 开头的（用正确的 prefix_end 写法）
        auto rows = sl.scan(std::string("user:"), prefix_end("user:"));
        CHECK_EQ(rows.size(), 3, "scan user: 前缀应有 3 条");
    }

    // ------------------------------------------------------------------ F
    section("F. 特殊字符 key");
    {
        SkipList<std::string, std::string> sl(8);
        sl.insert_element("key with space", "v1");
        sl.insert_element("中文键", "v2");
        sl.insert_element("emoji🚀", "v3");

        CHECK_EQ(sl.size(), 3, "特殊字符 key 的 size");
        CHECK(sl.search_element("key with space"), "空格 key");
        CHECK(sl.search_element("中文键"), "中文 key");
        CHECK(sl.search_element("emoji🚀"), "emoji key");
    }

    // ------------------------------------------------------------------ G
    section("G. KVStore<string,string>");
    {
        const std::string DIR = "/tmp/p2str";
        rmtree(DIR);
        if (system(("mkdir -p '" + DIR + "'").c_str()) != 0) {}

        KVStore<std::string, std::string> db(DIR);
        CHECK(db.ok(), "打开 KVStore");

        CHECK(db.put("username", "alice"), "put username");
        CHECK(db.put("user:1001:name", "bob"), "put 含冒号 key");
        CHECK(db.put("中文键", "中文值"), "put 中文");

        std::string v;
        CHECK(db.get("username", v) && v == "alice", "get username");
        CHECK(db.get("user:1001:name", v) && v == "bob", "get 含冒号 key");
        CHECK(db.get("中文键", v) && v == "中文值", "get 中文");
        CHECK(!db.get("nonexist", v), "get 不存在的 key");

        // 覆盖语义
        db.put("username", "alice2");
        CHECK(db.get("username", v) && v == "alice2", "put 覆盖旧值");

        CHECK_EQ(db.size(), 3, "size");

        // 范围查询（字典序）
        // 注意：这里用 "user:" 前缀，只会命中 "user:1001:name" 一条。
        // "username" 不在这个区间里 —— 因为 ':'(0x3A) < 'n'(0x6E)，
        // 字典序上 "user:1001:name" < "username"。
        // 想同时命中两者应该用 ["user", prefix_end("user"))。
        auto rows = db.scan(std::string("user:"), prefix_end("user:"));
        CHECK_EQ(rows.size(), 1, "scan user: 前缀应有 1 条（只含带冒号的）");

        auto rows2 = db.scan(std::string("user"), prefix_end("user"));
        CHECK_EQ(rows2.size(), 2, "scan user 前缀应有 2 条（含 username 和 user:1001:name）");

        db.del("username");
        CHECK(!db.get("username", v), "del 生效");
        CHECK_EQ(db.size(), 2, "del 后 size");

        rmtree(DIR);
    }

    // ------------------------------------------------------------------ H
    section("H. string key 持久化往返（快照 + WAL）");
    {
        const std::string DIR = "/tmp/p2persist";
        rmtree(DIR);
        if (system(("mkdir -p '" + DIR + "'").c_str()) != 0) {}

        {
            KVStore<std::string, std::string> db(DIR);
            db.put("k1", "v1");
            db.put("k2", "v2");
            db.put("user:1001:name", "alice");
            db.snapshot();                     // k1,k2,user:1001:name 进快照
            db.put("k3", "v3");                // 只进 WAL
            db.put("user:1002:name", "bob");   // 只进 WAL
            db.del("k1");                      // 删除也进 WAL
        }
        {
            KVStore<std::string, std::string> db(DIR);
            size_t n = db.recover();
            CHECK(n > 0, "恢复应读到记录");

            std::string v;
            CHECK(!db.get("k1", v), "k1 应已被删除");
            CHECK(db.get("k2", v) && v == "v2", "快照中的 k2 应恢复");
            CHECK(db.get("user:1001:name", v) && v == "alice", "快照中含冒号 key 应恢复");
            CHECK(db.get("k3", v) && v == "v3", "WAL 中的 k3 应恢复");
            CHECK(db.get("user:1002:name", v) && v == "bob", "WAL 中含冒号 key 应恢复");
            CHECK_EQ(db.size(), 4, "最终应有 4 个 key");
        }
        rmtree(DIR);
    }

    // ------------------------------------------------------------------ I
    section("I. 含 \\t 和 \\n 的 key（旧快照格式会坏）");
    {
        const std::string DIR = "/tmp/p2tricky";
        rmtree(DIR);
        if (system(("mkdir -p '" + DIR + "'").c_str()) != 0) {}

        const std::string k_tab = "key\twith\ttab";
        const std::string k_nl  = "key\nwith\nnewline";

        {
            KVStore<std::string, std::string> db(DIR);
            db.put(k_tab, "tabvalue");
            db.put(k_nl,  "nlvalue");
            db.snapshot();
        }
        {
            KVStore<std::string, std::string> db(DIR);
            db.recover();
            std::string v;
            // 旧格式（key\tvalue\n 文本行）在这里必然失败：
            // 含 \t 的 key 会被切成两半，含 \n 的 key 会被拆成两行。
            // 长度前缀格式应该完全正确。
            CHECK(db.get(k_tab, v) && v == "tabvalue", "含 \\t 的 key 应正确恢复");
            CHECK(db.get(k_nl,  v) && v == "nlvalue",  "含 \\n 的 key 应正确恢复");
            CHECK_EQ(db.size(), 2, "应有 2 个 key");
        }
        rmtree(DIR);
    }

    // ------------------------------------------------------------------ J
    section("J. int key 向后兼容");
    {
        const std::string DIR = "/tmp/p2int";
        rmtree(DIR);
        if (system(("mkdir -p '" + DIR + "'").c_str()) != 0) {}

        {
            KVStore<int, std::string> db(DIR);
            for (int i = 0; i < 50; i++) db.put(i, "v" + std::to_string(i));
            db.snapshot();
            db.put(999, "after_snapshot");
        }
        {
            KVStore<int, std::string> db(DIR);
            db.recover();
            std::string v;
            CHECK(db.get(0, v) && v == "v0", "int key 0 恢复");
            CHECK(db.get(49, v) && v == "v49", "int key 49 恢复");
            CHECK(db.get(999, v) && v == "after_snapshot", "WAL 中的 int key 恢复");
            CHECK_EQ(db.size(), 51, "int key 总数");

            // 范围查询仍是【数值序】而不是字典序
            auto rows = db.scan(10, 20);
            CHECK_EQ(rows.size(), 10, "int scan [10,20) 应有 10 条");
            CHECK(rows.front().first == 10 && rows.back().first == 19, "int scan 顺序正确");
        }
        rmtree(DIR);
    }

    std::cerr << "\n==================================================" << std::endl;
    std::cerr << " 通过: " << g_pass << "   失败: " << g_fail << std::endl;
    std::cerr << (g_fail == 0 ? " ✅ 全部通过" : " ❌ 有失败项") << std::endl;
    std::cerr << "==================================================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
