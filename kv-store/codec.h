/* ************************************************************************
> File Name:     codec.h
> Description:   [P2] Key 编解码策略（Codec）
>
> 解决什么问题
> ---------------------------------------------------------------------------
> 原版把 key 类型写死成 int：
>     skiplist.h :  load_file() 里写死 insert_element(stoi(*key), *value)
>     kvstore.h  :  encode_key() 用 std::to_string，recover() 用 std::stoi
>     kv_server  :  parse_key() 强制要求整数，否则报
>                   "ERR key must be an integer (this build uses int keys)"
>
> 后果：`SET username alice` 直接失败 —— 这不像个真正的 KV 服务。
>       README 的「待优化」也承认了这一点。
>
> 怎么解决
> ---------------------------------------------------------------------------
> 把「key 类型 K」和「key 怎么变成字节」这两件事解耦，
> 用策略类（policy）来表达编解码规则：
>
>     KeyCodec<int>::encode(42)          -> "42"
>     KeyCodec<int>::decode("42")        -> 42
>     KeyCodec<std::string>::encode("a") -> "a"      （原样）
>     KeyCodec<std::string>::decode("a") -> "a"
>
> 跳表本身【不需要改】—— 它本来就是 template<typename K, typename V>，
> key 的比较用的是 operator<，int 走数值比较、string 走字典序比较，
> 两者天然都对。真正卡住泛型化的只有「序列化」这一步。
>
> 想支持自定义 key 类型（比如复合 key），只要特化一个 KeyCodec 即可。
>
> 编译：C++17
 ************************************************************************/

#ifndef CODEC_H
#define CODEC_H

#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>

/* ===========================================================================
 * 主模板：默认按「字节原样」处理
 * ---------------------------------------------------------------------------
 * 只对 std::string 有意义。其它类型必须显式特化，
 * 否则会在编译期报「incomplete type」—— 这是有意的：
 * 与其运行期悄悄出错，不如编译期就拦住。
 * =========================================================================== */
template<typename K>
struct KeyCodec;

/* ===========================================================================
 * int：十进制字符串
 * ---------------------------------------------------------------------------
 * 注意：字典序 != 数值序！
 *     "10" < "9"   （字典序）
 *     10   > 9     （数值序）
 *
 * 这一点很重要：跳表内部用 operator< 比较的是【int 本身】，
 * 所以顺序是数值序，正确。
 * Codec 只负责「存盘/写日志时的字节表示」，不参与排序。
 * 所以存成 "10" 不会影响跳表里的顺序。
 *
 * 反过来说：如果你把 key 声明成 std::string 但存的是数字，
 * 那么跳表就按字典序排 —— 这是使用者的选择，不是 bug。
 * =========================================================================== */
template<>
struct KeyCodec<int> {
    static std::string encode(int k) {
        return std::to_string(k);
    }
    static int decode(const std::string& s) {
        // 用 std::stoi 的严格版：必须整串都是数字
        if (s.empty()) throw std::runtime_error("KeyCodec<int>::decode: 空字符串");
        size_t i = 0;
        bool neg = false;
        if (s[0] == '-') { neg = true; i = 1; }
        if (i >= s.size()) throw std::runtime_error("KeyCodec<int>::decode: 非法整数 '" + s + "'");
        long long v = 0;
        for (; i < s.size(); i++) {
            if (s[i] < '0' || s[i] > '9') {
                throw std::runtime_error("KeyCodec<int>::decode: 非法整数 '" + s + "'");
            }
            v = v * 10 + (s[i] - '0');
            if (v > 0x7FFFFFFFLL) {
                throw std::runtime_error("KeyCodec<int>::decode: 溢出 '" + s + "'");
            }
        }
        return static_cast<int>(neg ? -v : v);
    }
};

/* ===========================================================================
 * std::string：原样存储
 * ---------------------------------------------------------------------------
 * string 是「字节容器」，可以装任意内容（含 \0、\r\n、冒号、中文），
 * 所以直接原样用，不做任何转义。
 *
 * 关键点：因为 key 可能包含任意字节，
 * 「key:value」这种文本格式会被 key 里的冒号破坏
 * （比如 key = "user:1001"）。
 * 所以存盘格式必须是【长度前缀】而不是靠分隔符切分。
 * 见 kvstore.h 的 snapshot 格式，以及 skiplist.h 的 rfind 修复。
 * =========================================================================== */
template<>
struct KeyCodec<std::string> {
    static std::string encode(const std::string& k) { return k; }
    static std::string decode(const std::string& s) { return s; }
};

/* ===========================================================================
 * 辅助：判断某个 key 的字节表示是否可以安全地放进「冒号分隔」的文本行
 * ---------------------------------------------------------------------------
 * 仅用于 skiplist.h 的 dump_file/load_file（原版遗留的文本格式）。
 * 含冒号或换行的 key 不能用那个格式，需要走 kvstore 的长度前缀格式。
 * =========================================================================== */
template<typename K>
inline bool key_is_text_safe(const K& k) {
    std::string s = KeyCodec<K>::encode(k);
    return s.find(':') == std::string::npos &&
           s.find('\n') == std::string::npos &&
           s.find('\r') == std::string::npos;
}

#endif  // CODEC_H
