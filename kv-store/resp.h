/* ************************************************************************
> File Name:     resp.h
> Description:   [P6] RESP 协议编解码器
>
> RESP = REdis Serialization Protocol，Redis 的客户端/服务端协议。
>
> 为什么选 RESP 而不是自己发明协议：
>   1. 有现成客户端 —— redis-cli 可以直接连我们的服务，演示效果拉满
>   2. 协议简单清晰，文本 + 长度前缀，容易实现正确
>   3. 面试时可以说「我实现了 Redis 协议的一个子集」
>
> 协议格式（本项目用到的 5 种类型）：
>   Simple String   +OK\r\n
>   Error           -ERR unknown command\r\n
>   Integer         :1000\r\n
>   Bulk String     $6\r\nfoobar\r\n        ($-1\r\n 表示 nil)
>   Array           *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n   (*-1\r\n 表示 nil 数组)
>
> 核心难点：TCP 是【字节流】，没有消息边界。
>   一次 read() 可能拿到：
>     - 半条命令（"*2\r\n$3\r\nGE"）
>     - 一条半命令（"*2\r\n...\r\n*1\r\n$4\r\nPING\r\n" 的前半）
>     - 正好 N 条命令
>   所以必须实现一个【状态机解析器】，把收到的字节喂进去，
>   能解析出几条就返回几条，剩下的留在缓冲区等下次。
>
> 编译：C++17
 ************************************************************************/

#ifndef RESP_H
#define RESP_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace resp {

// 协议里的一些常量
constexpr char kCRLF[] = "\r\n";
constexpr size_t kCRLFLen = 2;

// 单条命令的参数上限，防御恶意/超大请求
constexpr size_t kMaxArgs = 1024;
constexpr size_t kMaxBulkLen = 512ull * 1024 * 1024;   // 512MB，和 Redis 默认一致

/* ===========================================================================
 * 编码：把数据打包成 RESP 字节流
 * =========================================================================== */
class Encoder {
public:
    // +OK\r\n
    static std::string simple_string(const std::string& s) {
        return "+" + s + "\r\n";
    }

    // -ERR xxx\r\n
    static std::string error(const std::string& msg) {
        return "-" + msg + "\r\n";
    }

    // :123\r\n
    static std::string integer(long long v) {
        return ":" + std::to_string(v) + "\r\n";
    }

    // $6\r\nfoobar\r\n
    static std::string bulk_string(const std::string& s) {
        return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
    }

    // $-1\r\n  —— nil（key 不存在时返回这个）
    static std::string nil() {
        return "$-1\r\n";
    }

    // *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
    static std::string array(const std::vector<std::string>& items) {
        std::string out = "*" + std::to_string(items.size()) + "\r\n";
        for (const auto& s : items) out += bulk_string(s);
        return out;
    }

    // *-1\r\n —— nil 数组
    static std::string nil_array() {
        return "*-1\r\n";
    }

    // 空数组 *0\r\n
    static std::string empty_array() {
        return "*0\r\n";
    }
};

/* ===========================================================================
 * 解码：增量式解析器
 * ---------------------------------------------------------------------------
 * 用法：
 *     Parser p;
 *     p.feed(bytes, len);              // 收到数据就喂进去
 *     std::vector<Command> cmds;
 *     while (p.next(cmds)) { ... }     // 能解析几条就取几条
 *
 * 设计要点：
 *   - 内部维护一个 buffer，不完整的部分留在里面等下次 feed
 *   - 解析成功才消费 buffer，失败/不完整则原样保留（可重试）
 *   - 对协议错误返回 false 并置 error 标志，让上层关闭连接
 * =========================================================================== */
struct Command {
    std::vector<std::string> args;      // args[0] 是命令名（如 "SET"）

    const std::string& name() const { return args[0]; }
    size_t size() const { return args.size(); }
};

class Parser {
public:
    Parser() : _error(false), _error_msg("") {}

    // 喂入新收到的字节
    void feed(const char* data, size_t len) {
        _buf.append(data, len);
    }
    void feed(const std::string& s) {
        _buf.append(s);
    }

    bool has_error() const { return _error; }
    const std::string& error_msg() const { return _error_msg; }
    size_t buffered() const { return _buf.size(); }

    /* -----------------------------------------------------------------------
     * 尝试解析一条完整命令
     *   返回 true  -> out 里是一条完整命令，已从缓冲区消费
     *   返回 false -> 数据不完整（等下次 feed），或协议错误
     *
     * 注意：区分「不完整」和「协议错误」很重要：
     *   不完整 -> 继续等数据
     *   协议错误 -> 关闭连接（否则会被恶意数据撑爆内存）
     *
     * 怎么区分：靠 has_error()，【不是】靠 pos。
     *   parse_one 只在【解析成功】时才写 pos；
     *   失败时（不管是不完整还是协议错误）它根本不碰 pos，
     *   所以调用方那个 pos 变量在失败路径上永远是 0，无法用来判断。
     *   真正的依据是内部 _error 标志：
     *     _error == false -> 数据不完整，等下次 feed
     *     _error == true  -> 协议错误，调用方应关闭连接
     * ----------------------------------------------------------------------- */
    bool next(Command& out) {
        if (_error) return false;

        // 循环：跳过空命令（例如 redis-cli --pipe 发的裸 \r\n 空行），
        // 直到解析出一条真正的命令、或数据不完整、或出错。
        while (true) {
            size_t pos = 0;          // 仅当 parse_one 成功时才被赋值
            if (!parse_one(pos, out)) {
                // 失败原因看 has_error()，不看 pos（见上面的说明）。
                return false;
            }

            // 消费掉已解析的字节
            _buf.erase(0, pos);

            // 空命令（无参数）：跳过，继续解析下一条。
            // 绝不能把空 args 交给上层 —— execute() 会访问 args[0]，那是越界。
            if (!out.args.empty()) return true;

            if (_buf.empty()) return false;   // 没有更多数据了
        }
    }

    // 一次性取出所有能解析的命令
    std::vector<Command> drain() {
        std::vector<Command> out;
        Command c;
        while (next(c)) out.push_back(c);
        return out;
    }

private:
    /* -----------------------------------------------------------------------
     * 解析一条命令
     *
     * 契约（很重要，调用方依赖它）：
     *   成功 -> 返回 true，并把「消费到的字节数」写进 pos
     *   失败 -> 返回 false，【不修改 pos】，失败原因记录在 _error 里
     *
     *   也就是说 pos 只在成功路径上有意义。想判断失败原因，
     *   请用 has_error()，不要看 pos。
     *
     * 支持两种入站格式（Redis 客户端两种都会发）：
     *   1. 标准多行格式：*N\r\n$len\r\narg\r\n...
     *   2. 内联格式：    SET key value\r\n   （redis-cli 交互模式可能用）
     * ----------------------------------------------------------------------- */
    bool parse_one(size_t& pos, Command& out) {
        if (_buf.empty()) return false;

        // ---- 内联命令（不以 * 开头）----
        if (_buf[0] != '*') {
            return parse_inline(pos, out);
        }

        // ---- 标准数组格式 ----
        // 注意：p 从 1 开始，跳过开头的 '*'
        size_t p = 1;

        // 解析 *N
        long long nargs;
        if (!read_line_int(p, nargs)) return false;      // 不完整
        if (nargs <= 0 || static_cast<size_t>(nargs) > kMaxArgs) {
            return set_error("invalid multibulk length");
        }

        out.args.clear();
        out.args.reserve(static_cast<size_t>(nargs));

        for (long long i = 0; i < nargs; i++) {
            // 每个参数必须是 $len\r\n<data>\r\n
            if (p >= _buf.size() || _buf[p] != '$') {
                // 可能是还没收全，也可能真错了。
                // 如果连 '$' 都没到，说明不完整；到了但不是 '$' 才是错误。
                if (p >= _buf.size()) return false;
                return set_error("expected '$'");
            }
            p++;   // 跳过 '$'

            long long len;
            if (!read_line_int(p, len)) return false;
            if (len < 0 || static_cast<size_t>(len) > kMaxBulkLen) {
                return set_error("invalid bulk length");
            }

            // 数据 + CRLF 是否都已到达
            size_t need = static_cast<size_t>(len) + kCRLFLen;
            if (p + need > _buf.size()) return false;    // 不完整

            out.args.emplace_back(_buf, p, static_cast<size_t>(len));
            p += need;
        }

        pos = p;
        return true;
    }

    /* -----------------------------------------------------------------------
     * 内联命令：一行文本，空格分隔
     * 例如 "PING\r\n" 或 "SET foo bar\r\n"
     * ----------------------------------------------------------------------- */
    bool parse_inline(size_t& pos, Command& out) {
        size_t nl = _buf.find("\r\n");
        if (nl == std::string::npos) {
            // 单行太长还不换行，认为异常
            if (_buf.size() > 64 * 1024) return set_error("inline command too long");
            return false;                                 // 不完整
        }

        std::string line = _buf.substr(0, nl);
        pos = nl + kCRLFLen;

        out.args.clear();
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (i >= line.size()) break;
            size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
            out.args.push_back(line.substr(start, i - start));
        }

        // 空行（只有 \r\n）不是错误，直接跳过。
        // 实测 redis-cli --pipe 在发完命令后、发 ECHO 之前会发一个空行，
        // 如果这里报错会导致整个管道被判定失败（errors: 1）。
        if (out.args.empty()) {
            return true;      // pos 已前进，相当于消费掉这个空行
        }
        return true;
    }

    /* -----------------------------------------------------------------------
     * 从位置 p 读一个「以 \r\n 结尾的整数」，成功后 p 指向 \r\n 之后
     * 返回 false 表示数据不完整
     * ----------------------------------------------------------------------- */
    bool read_line_int(size_t& p, long long& value) {
        // 跳过前导的类型字符（'*' 或 '$'），由调用方已处理
        size_t nl = _buf.find("\r\n", p);
        if (nl == std::string::npos) {
            // 防止一个超长的"数字"把内存吃光
            if (_buf.size() - p > 64) return set_error("integer too long");
            return false;                                 // 不完整
        }

        std::string num = _buf.substr(p, nl - p);
        if (num.empty()) return set_error("empty integer");

        // 严格校验：只允许可选负号 + 数字
        size_t idx = 0;
        bool neg = false;
        if (num[0] == '-') { neg = true; idx = 1; }
        if (idx >= num.size()) return set_error("invalid integer");
        for (size_t i = idx; i < num.size(); i++) {
            if (num[i] < '0' || num[i] > '9') return set_error("invalid integer");
        }

        long long v = 0;
        for (size_t i = idx; i < num.size(); i++) {
            v = v * 10 + (num[i] - '0');
            if (v > (1LL << 40)) return set_error("integer overflow");
        }
        value = neg ? -v : v;
        p = nl + kCRLFLen;
        return true;
    }

    bool set_error(const std::string& msg) {
        _error = true;
        _error_msg = msg;
        return false;
    }

    std::string _buf;
    bool _error;
    std::string _error_msg;
};

}  // namespace resp

#endif  // RESP_H
