/* ============================================================================
 * [P6] RESP 协议解析测试
 * ----------------------------------------------------------------------------
 * 重点测「部分包」（partial packet）—— TCP 字节流没有消息边界，
 * 一次 read 可能拿到半条命令。这是网络编程最容易出 bug 的地方。
 *
 * 编译运行：
 *   g++ tests/resp_test.cpp -o bin/resp_test --std=c++17
 *   ./bin/resp_test
 * ============================================================================
 */
#include <iostream>
#include <string>
#include <vector>

#include "../resp.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; std::cout << "  ❌ FAIL: " << msg << std::endl; }     \
    } while (0)

static void section(const char* name) {
    std::cout << "\n===== " << name << " =====" << std::endl;
}

using namespace resp;

int main() {
    // ------------------------------------------------------------------ A
    section("A. 编码");
    {
        CHECK(Encoder::simple_string("OK") == "+OK\r\n", "simple_string");
        CHECK(Encoder::error("ERR bad") == "-ERR bad\r\n", "error");
        CHECK(Encoder::integer(1000) == ":1000\r\n", "integer");
        CHECK(Encoder::integer(-1) == ":-1\r\n", "负 integer");
        CHECK(Encoder::bulk_string("foobar") == "$6\r\nfoobar\r\n", "bulk_string");
        CHECK(Encoder::bulk_string("") == "$0\r\n\r\n", "空 bulk_string");
        CHECK(Encoder::nil() == "$-1\r\n", "nil");

        std::vector<std::string> arr = {"foo", "bar"};
        CHECK(Encoder::array(arr) == "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "array");
        CHECK(Encoder::empty_array() == "*0\r\n", "empty_array");
    }

    // ------------------------------------------------------------------ B
    section("B. 解析完整命令");
    {
        Parser p;
        p.feed("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
        Command c;
        CHECK(p.next(c), "应解析出一条命令");
        CHECK(c.size() == 2, "应有 2 个参数");
        CHECK(c.name() == "GET", "命令名应为 GET");
        CHECK(c.args[1] == "foo", "参数应为 foo");
        CHECK(!p.next(c), "不应再解析出第二条");
        CHECK(p.buffered() == 0, "缓冲区应已清空");
    }

    // ------------------------------------------------------------------ C
    section("C. 部分包（最关键）");
    {
        const std::string full = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";

        // 逐字节喂入，每一步都不应崩、不应误报错误
        Parser p;
        Command c;
        bool got = false;
        for (size_t i = 0; i < full.size(); i++) {
            p.feed(full.data() + i, 1);
            if (p.next(c)) {
                got = true;
                // 必须正好在最后一个字节才解析出来
                CHECK(i == full.size() - 1, "应恰好在收到最后一个字节时解析成功");
                break;
            }
            CHECK(!p.has_error(), "中途不应报协议错误");
        }
        CHECK(got, "逐字节喂入最终应解析成功");
        CHECK(c.size() == 3 && c.name() == "SET" && c.args[1] == "key" && c.args[2] == "value",
              "解析结果应正确");
    }

    // ------------------------------------------------------------------ D
    section("D. 粘包：一次收到多条命令");
    {
        Parser p;
        p.feed("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n");
        auto cmds = p.drain();
        CHECK(cmds.size() == 3, "应解析出 3 条命令");
        CHECK(cmds[0].name() == "PING", "第 1 条 PING");
        CHECK(cmds[1].name() == "PING", "第 2 条 PING");
        CHECK(cmds[2].name() == "GET" && cmds[2].args[1] == "a", "第 3 条 GET a");
    }

    // ------------------------------------------------------------------ E
    section("E. 半条 + 后续补齐");
    {
        Parser p;
        Command c;
        // 先喂半条
        p.feed("*2\r\n$3\r\nGET\r\n$3\r\nfo");
        CHECK(!p.next(c), "半条不应解析成功");
        CHECK(!p.has_error(), "半条不应报错");
        // 补齐
        p.feed("o\r\n");
        CHECK(p.next(c), "补齐后应解析成功");
        CHECK(c.args[1] == "foo", "参数应为 foo");
    }

    // ------------------------------------------------------------------ F
    section("F. 边界：空参数、二进制安全、超长");
    {
        Parser p;
        Command c;
        // 空 bulk string
        p.feed("*2\r\n$3\r\nGET\r\n$0\r\n\r\n");
        CHECK(p.next(c), "空参数应能解析");
        CHECK(c.args[1].empty(), "参数应为空字符串");

        // 二进制安全：value 里含 \r\n 和 \0
        Parser p2;
        std::string bin = "a\r\nb\0c";
        p2.feed("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" + std::to_string(bin.size()) + "\r\n" + bin + "\r\n");
        Command c2;
        CHECK(p2.next(c2), "二进制 value 应能解析");
        CHECK(c2.args[2] == bin, "二进制内容应原样保留（含 \\r\\n 和 \\0）");

        // 超长参数数量应被拒绝
        Parser p3;
        p3.feed("*99999\r\n");
        Command c3;
        CHECK(!p3.next(c3), "超长参数数量应被拒绝");
        CHECK(p3.has_error(), "应置错误标志");
    }

    // ------------------------------------------------------------------ G
    section("G. 协议错误检测");
    {
        // 缺少 '$'
        Parser p;
        p.feed("*2\r\nXXX\r\n");
        Command c;
        CHECK(!p.next(c), "非法格式应失败");
        CHECK(p.has_error(), "应报协议错误");

        // 非法整数
        Parser p2;
        p2.feed("*abc\r\n");
        Command c2;
        CHECK(!p2.next(c2), "非数字长度应失败");
        CHECK(p2.has_error(), "应报协议错误");

        // 负的 bulk 长度（非 -1）
        Parser p3;
        p3.feed("*1\r\n$-5\r\n");
        Command c3;
        CHECK(!p3.next(c3), "非法负长度应失败");
        CHECK(p3.has_error(), "应报协议错误");
    }

    // ------------------------------------------------------------------ H
    section("H. 内联命令");
    {
        Parser p;
        Command c;
        p.feed("PING\r\n");
        CHECK(p.next(c), "内联命令应能解析");
        CHECK(c.name() == "PING", "命令名 PING");

        Parser p2;
        p2.feed("SET foo bar\r\n");
        Command c2;
        CHECK(p2.next(c2), "内联 SET 应能解析");
        CHECK(c2.size() == 3 && c2.args[1] == "foo" && c2.args[2] == "bar", "参数正确");

        // 内联命令分片
        Parser p3;
        p3.feed("PI");
        Command c3;
        CHECK(!p3.next(c3), "内联半条不应成功");
        p3.feed("NG\r\n");
        CHECK(p3.next(c3) && c3.name() == "PING", "补齐后成功");

        // 空行应被跳过，不是错误。
        // 实测 redis-cli --pipe 在发完命令后会发一个裸 \r\n，必须容忍。
        Parser p4;
        p4.feed("\r\n");
        Command c4;
        CHECK(!p4.next(c4), "单独的空行不应产出命令");
        CHECK(!p4.has_error(), "空行不应被当成协议错误");

        // 空行后面跟着真命令
        Parser p5;
        p5.feed("\r\n*1\r\n$4\r\nPING\r\n");
        Command c5;
        CHECK(p5.next(c5), "空行后的命令应能解析");
        CHECK(c5.name() == "PING", "空行应被正确跳过");

        // 空行夹在两条命令之间
        Parser p6;
        p6.feed("PING\r\n\r\nPING\r\n");
        auto cmds6 = p6.drain();
        CHECK(cmds6.size() == 2, "空行夹在中间应解析出 2 条命令");
    }

    // ------------------------------------------------------------------ I
    section("I. 区分「数据不完整」与「协议错误」");
    {
        // 这组测试锁定一个容易被注释误导的契约：
        //   判断失败原因要用 has_error()，【不能】靠 pos。
        //   因为 parse_one 失败时不写 pos，pos 在失败路径上永远是 0。

        // ---- 数据不完整：has_error() 必须为 false ----
        {
            Parser p;
            p.feed("*2\r\n$3\r\nGE");          // 半条命令
            Command c;
            CHECK(!p.next(c), "半条命令不应解析成功");
            CHECK(!p.has_error(), "数据不完整时 has_error() 应为 false");
        }
        {
            Parser p;
            p.feed("*2\r\n");                   // 只有头，参数都没来
            Command c;
            CHECK(!p.next(c), "只有头不应成功");
            CHECK(!p.has_error(), "只有头时 has_error() 应为 false");
        }
        {
            Parser p;
            p.feed("*2\r\n$3\r\nGET\r\n$3\r\nfo");   // 最后一个参数差 1 字节
            Command c;
            CHECK(!p.next(c), "差 1 字节不应成功");
            CHECK(!p.has_error(), "差 1 字节时 has_error() 应为 false");
        }
        {
            Parser p;
            p.feed("PI");                        // 内联命令半条
            Command c;
            CHECK(!p.next(c), "内联半条不应成功");
            CHECK(!p.has_error(), "内联半条时 has_error() 应为 false");
        }

        // ---- 协议错误：has_error() 必须为 true ----
        {
            Parser p;
            p.feed("*2\r\nXXX\r\n");            // 缺少 '$'
            Command c;
            CHECK(!p.next(c), "非法格式应失败");
            CHECK(p.has_error(), "协议错误时 has_error() 应为 true");
            CHECK(!p.error_msg().empty(), "应带上错误描述");
        }
        {
            Parser p;
            p.feed("*abc\r\n");                 // 非数字长度
            Command c;
            CHECK(!p.next(c), "非数字长度应失败");
            CHECK(p.has_error(), "应置错误标志");
        }
        {
            Parser p;
            p.feed("*99999\r\n");               // 参数数量超限
            Command c;
            CHECK(!p.next(c), "超限应失败");
            CHECK(p.has_error(), "应置错误标志");
        }
        {
            Parser p;
            p.feed("*1\r\n$-5\r\n");            // 非法负长度
            Command c;
            CHECK(!p.next(c), "非法负长度应失败");
            CHECK(p.has_error(), "应置错误标志");
        }

        // ---- 关键对照：同样的「返回 false」，has_error() 不同 ----
        {
            Parser incomplete;
            incomplete.feed("*2\r\n$3\r\nGE");
            Command c1;
            bool r1 = incomplete.next(c1);

            Parser bad;
            bad.feed("*2\r\nXXX\r\n");
            Command c2;
            bool r2 = bad.next(c2);

            CHECK(r1 == r2, "两种情况 next() 都返回 false（无法靠返回值区分）");
            CHECK(incomplete.has_error() != bad.has_error(),
                  "但 has_error() 不同 —— 这才是唯一可靠的区分依据");
        }
    }

    std::cout << "\n==================================================" << std::endl;
    std::cout << " 通过: " << g_pass << "   失败: " << g_fail << std::endl;
    std::cout << (g_fail == 0 ? " ✅ 全部通过" : " ❌ 有失败项") << std::endl;
    std::cout << "==================================================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
