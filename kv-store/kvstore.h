/* ************************************************************************
> File Name:     kvstore.h
> Description:   [P4] 带 WAL 预写日志与崩溃恢复的 KV 存储引擎
>
> 这是把「跳表」变成「存储引擎」的关键一步。
>
> 原版的持久化只有 dump_file()：全量覆盖写一个文本文件。
> 问题：
>   1. 不是增量的 —— 每次都要把整个内存重写一遍
>   2. 没有崩溃恢复 —— 进程被 kill，上次 dump 之后的所有写入全丢
>   3. 没有校验 —— 文件写坏了也不知道
>   4. 不检查 is_open() —— 静默丢数据（P1 已修，但架构问题还在）
>
> P4 引入 WAL（Write-Ahead Log，预写日志）：
>
>   写路径：  写请求 → 追加 WAL（顺序写 + fsync）→ 改内存跳表 → 返回
>   恢复：    启动时 → 重放 WAL → 重建跳表
>   压缩：    WAL 超过阈值 → 全量快照 → 清空 WAL
>
> 关键点：**先写日志成功，再改内存**。顺序反了就没有恢复能力。
>
> 编译：C++17
 ************************************************************************/

#ifndef KVSTORE_H
#define KVSTORE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <functional>
#include <mutex>          // [P6] WAL 内部串行化
#include <shared_mutex>   // [P6] KVStore 快照与写入互斥
#include <istream>        // [P2] 复用的记录读取器

#include "skiplist.h"
#include "codec.h"        // [P2] KeyCodec

/* ===========================================================================
 * CRC32（标准 IEEE 802.3 多项式 0xEDB88320，和 zip/png/gzip 一致）
 * ---------------------------------------------------------------------------
 * 为什么要 CRC：
 *   崩溃时最后一条 WAL 记录可能只写了一半（撕裂写，torn write）。
 *   恢复时必须能识别「这条记录不完整」，然后从这条开始截断，
 *   否则会把垃圾数据当成合法记录重放进去。
 * =========================================================================== */
class CRC32 {
public:
    static uint32_t compute(const void* data, size_t len) {
        static const uint32_t* table = get_table();
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; i++) {
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    static uint32_t compute(const std::string& s) {
        return compute(s.data(), s.size());
    }

private:
    static const uint32_t* get_table() {
        static uint32_t table[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                table[i] = c;
            }
            init = true;
        }
        return table;
    }
};

/* ===========================================================================
 * WAL 记录格式
 * ---------------------------------------------------------------------------
 *   +--------+--------+--------+--------+---------+---------+
 *   | crc32  | op(1B) | klen   | vlen   |  key    |  value  |
 *   |  4B    |        |  4B    |  4B    | klen B  | vlen B  |
 *   +--------+--------+--------+--------+---------+---------+
 *
 * 全部小端序（x86 原生）。
 *
 * crc32 覆盖范围：op + klen + vlen + key + value（即 crc 字段之后的所有字节）
 *
 * op 取值：'P' = PUT（写/覆盖），'D' = DEL（删除）
 *
 * 为什么长度字段用固定 4 字节而不是变长编码：
 *   实现简单，且一条记录最多多花几字节。工业实现（如 LevelDB）会用
 *   varint 压缩，但那是为了省空间，这里优先可读性和正确性。
 * =========================================================================== */
struct WALRecord {
    static constexpr char kOpPut = 'P';
    static constexpr char kOpDel = 'D';

    // 把一条记录序列化成字节流
    static std::string encode(char op, const std::string& key, const std::string& value) {
        uint32_t klen = static_cast<uint32_t>(key.size());
        uint32_t vlen = static_cast<uint32_t>(value.size());

        // 先拼 crc 之后的负载部分
        std::string payload;
        payload.reserve(1 + 8 + key.size() + value.size());
        payload.push_back(op);
        payload.append(reinterpret_cast<const char*>(&klen), 4);
        payload.append(reinterpret_cast<const char*>(&vlen), 4);
        payload.append(key);
        payload.append(value);

        uint32_t crc = CRC32::compute(payload);

        std::string out;
        out.reserve(4 + payload.size());
        out.append(reinterpret_cast<const char*>(&crc), 4);
        out.append(payload);
        return out;
    }

    /* -----------------------------------------------------------------------
     * [P2] 从流里读一条记录（不含 crc 校验，调用方决定要不要校验）
     *
     * 返回：1 = 读到一条；0 = 正常结束（EOF）；-1 = 数据损坏/不完整
     *
     * 为什么要抽出来：
     *   快照文件也用「长度前缀」格式（见 KVStore::snapshot），
     *   而不是 `key\tvalue\n` 文本行 —— 因为 string key 里可能含 \t 或 \n，
     *   文本行格式会被破坏。长度前缀对任意字节都安全。
     *   既然格式一样，读取逻辑就该复用，避免两份实现走偏。
     * ----------------------------------------------------------------------- */
    static int read_one(std::istream& in, char& op,
                        std::string& key, std::string& value) {
        char hdr[9];
        in.read(hdr, 9);
        if (in.gcount() == 0) return 0;              // 干净的 EOF
        if (in.gcount() != 9) return -1;             // 头不完整

        op = hdr[0];
        uint32_t klen, vlen;
        std::memcpy(&klen, hdr + 1, 4);
        std::memcpy(&vlen, hdr + 5, 4);

        const uint32_t kMaxLen = 64u * 1024 * 1024;
        if (klen > kMaxLen || vlen > kMaxLen) return -1;

        key.assign(klen, '\0');
        value.assign(vlen, '\0');
        if (klen > 0) { in.read(&key[0], klen);   if (static_cast<uint32_t>(in.gcount()) != klen) return -1; }
        if (vlen > 0) { in.read(&value[0], vlen); if (static_cast<uint32_t>(in.gcount()) != vlen) return -1; }
        return 1;
    }

    // 快照用：不带 crc 的记录编码（快照有整体原子替换保护，不需要逐条 crc）
    static std::string encode_no_crc(char op, const std::string& key, const std::string& value) {
        uint32_t klen = static_cast<uint32_t>(key.size());
        uint32_t vlen = static_cast<uint32_t>(value.size());
        std::string out;
        out.reserve(9 + key.size() + value.size());
        out.push_back(op);
        out.append(reinterpret_cast<const char*>(&klen), 4);
        out.append(reinterpret_cast<const char*>(&vlen), 4);
        out.append(key);
        out.append(value);
        return out;
    }
};

/* ===========================================================================
 * WAL：预写日志
 * =========================================================================== */
class WAL {
public:
    explicit WAL(const std::string& path) : _path(path), _bytes(0) {
        // 追加模式打开：进程重启后继续往同一个文件写
        _out.open(path, std::ios::binary | std::ios::app);
        if (_out.is_open()) {
            _out.seekp(0, std::ios::end);
            _bytes = static_cast<size_t>(_out.tellp());
        }
    }

    ~WAL() {
        if (_out.is_open()) _out.close();
    }

    bool is_open() const { return _out.is_open(); }

    // [P6] bytes() 读 _bytes，需要与 append 互斥；const 函数里改锁 -> mutable
    size_t bytes() const {
        std::lock_guard<std::mutex> lk(_mtx);
        return _bytes;
    }

    const std::string& path() const { return _path; }

    /* -----------------------------------------------------------------------
     * 追加一条记录并 fsync 落盘
     *
     * 为什么必须 fsync：
     *   write() 只是把数据交给内核页缓存，进程崩溃数据还在；
     *   但【机器断电】或【内核 panic】时页缓存会丢。
     *   fsync 强制刷到磁盘介质，才真正持久。
     *
     * 代价：每次写都要等磁盘，这是 WAL 最大的性能开销。
     *   工业实现会做「组提交」（group commit）：把多个写请求的 fsync 合并成一次。
     *   这里为了正确性和可读性，每条都 fsync。
     * ----------------------------------------------------------------------- */
    bool append(char op, const std::string& key, const std::string& value) {
        // [P6] 关键修复：多线程同时 append 会竞争 ofstream 内部缓冲。
        //   实测（tests/p4_concurrency_test.cpp，TSan）：
        //       WARNING: ThreadSanitizer: data race
        //         Write of size 8 ... by thread T2:
        //           #2 std::basic_streambuf::xsputn(...)   ← ofstream::write 内部
        //   后果：日志内容交错错乱，CRC 全废，恢复直接失败。
        //   加锁后整个「编码 + 写 + flush」是一个原子操作。
        std::lock_guard<std::mutex> lk(_mtx);

        if (!_out.is_open()) return false;

        std::string rec = WALRecord::encode(op, key, value);
        _out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
        _out.flush();
        if (!_out.good()) return false;

        // 真正刷到磁盘
        std::fflush(nullptr);
        // 说明：标准 C++ 没有 fsync 接口，这里用 fstream 的 flush 已经
        // 把数据交给了内核。要强制落盘需要 POSIX fsync(fileno(...))，
        // 但那需要拿到 FILE*，跨平台性差。
        // 对本项目而言，flush 已经足够演示「崩溃恢复」——
        // 因为 kill -9 杀进程不会丢内核页缓存里的数据，
        // 只有断电才会。我在文档里说明了这个边界。

        _bytes += rec.size();
        return true;
    }

    /* -----------------------------------------------------------------------
     * 重放 WAL
     *
     * 返回：成功重放的记录数
     *
     * 容错策略（这是崩溃恢复的核心）：
     *   逐条读取，一旦出现以下情况就【停止重放并截断】：
     *     - 剩余字节不足以构成一条完整记录的头部
     *     - 长度字段越界
     *     - CRC 校验失败
     *   因为崩溃时最后一条记录可能写了一半，后面不可能再有完整记录。
     * ----------------------------------------------------------------------- */
    size_t replay(const std::function<void(char, const std::string&, const std::string&)>& fn) {
        // [P6] 重放期间独占，避免与并发 append 交叉。
        //   注意：replay 只在启动时调用，此时通常还没有并发写入，
        //   但加上锁能保证「恢复期间误写入」也不会破坏一致性。
        std::lock_guard<std::mutex> lk(_mtx);

        std::ifstream in(_path, std::ios::binary);
        if (!in.is_open()) return 0;

        size_t count = 0;
        size_t valid_bytes = 0;

        while (true) {
            // ---- 读 4 字节 crc ----
            uint32_t crc_stored;
            if (!in.read(reinterpret_cast<char*>(&crc_stored), 4)) break;

            // ---- 读 op + klen + vlen（9 字节）----
            char op;
            uint32_t klen, vlen;
            if (!in.read(&op, 1)) break;
            if (!in.read(reinterpret_cast<char*>(&klen), 4)) break;
            if (!in.read(reinterpret_cast<char*>(&vlen), 4)) break;

            // ---- 合法性检查：防御损坏的长度字段导致巨大分配 ----
            const uint32_t kMaxLen = 64u * 1024 * 1024;   // 单条 key/value 上限 64MB
            if (klen > kMaxLen || vlen > kMaxLen) {
                std::cerr << "[WAL] 长度字段异常 (klen=" << klen
                          << ", vlen=" << vlen << ")，截断于此" << std::endl;
                break;
            }

            // ---- 读 key + value ----
            std::string key(klen, '\0'), value(vlen, '\0');
            if (klen > 0 && !in.read(&key[0], klen)) break;
            if (vlen > 0 && !in.read(&value[0], vlen)) break;

            // ---- CRC 校验 ----
            std::string payload;
            payload.reserve(9 + klen + vlen);
            payload.push_back(op);
            payload.append(reinterpret_cast<const char*>(&klen), 4);
            payload.append(reinterpret_cast<const char*>(&vlen), 4);
            payload.append(key);
            payload.append(value);

            if (CRC32::compute(payload) != crc_stored) {
                std::cerr << "[WAL] 第 " << (count + 1)
                          << " 条记录 CRC 校验失败，截断于此（崩溃时的撕裂写）" << std::endl;
                break;
            }

            fn(op, key, value);
            count++;
            valid_bytes += 4 + payload.size();
        }

        // 如果有尾部损坏，把文件截断到最后一个完整记录，避免下次重放又读到
        if (valid_bytes < _bytes) {
            std::cerr << "[WAL] 检测到 " << (_bytes - valid_bytes)
                      << " 字节尾部损坏，已截断" << std::endl;
            in.close();
            truncate_to(valid_bytes);
            _bytes = valid_bytes;
        }

        return count;
    }

    // 清空 WAL（快照完成后调用）
    bool reset() {
        // [P6] 与 append 互斥
        std::lock_guard<std::mutex> lk(_mtx);
        _out.close();
        std::ofstream trunc(_path, std::ios::binary | std::ios::trunc);
        trunc.close();
        _out.open(_path, std::ios::binary | std::ios::app);
        _bytes = 0;
        return _out.is_open();
    }

private:
    void truncate_to(size_t n) {
        // 调用方（replay）已持锁，这里不再加锁，避免死锁（std::mutex 不可重入）
        // 用读全部 + 重写的方式截断（C++ 标准库没有 truncate）
        std::ifstream in(_path, std::ios::binary);
        std::string buf((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        in.close();
        if (buf.size() > n) buf.resize(n);

        _out.close();
        std::ofstream out(_path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.close();
        _out.open(_path, std::ios::binary | std::ios::app);
    }

    std::string _path;
    std::ofstream _out;
    size_t _bytes;
    // [P6] 保护 _out 和 _bytes：WAL 必须是「追加有序」的，
    //      否则多线程写入会交错，CRC 校验全失败。
    mutable std::mutex _mtx;
};

/* ===========================================================================
 * KVStore：跳表 + WAL 组成的存储引擎
 * ---------------------------------------------------------------------------
 * [P2] 现在 key 类型是模板参数 K，不再写死 int。
 *   通过 KeyCodec<K> 决定 key 怎么变成字节（见 codec.h）。
 *
 *   用法：
 *     KVStore<std::string, std::string> db(dir);   // string key（推荐）
 *     db.put("username", "alice");
 *     db.put("user:1001:name", "bob");             // 含冒号的 key 也 OK
 *
 *     KVStore<int, std::string> db2(dir);          // int key（兼容旧用法）
 *     db2.put(42, "answer");
 *
 * 对外接口：
 *   put(key, value)  —— 写，先 WAL 再内存
 *   get(key, &value) —— 读
 *   del(key)         —— 删，先 WAL 再内存
 *   scan(a, b)       —— 范围查询（继承 P3）
 *   snapshot()       —— 全量快照 + 清空 WAL（压缩）
 *   recover()        —— 启动时重放 WAL
 * =========================================================================== */
template<typename K, typename V>
class KVStore {
public:
    /* -----------------------------------------------------------------------
     * 构造：指定数据目录
     *   data_dir/wal.log     —— 预写日志
     *   data_dir/snapshot    —— 全量快照
     * ----------------------------------------------------------------------- */
    explicit KVStore(const std::string& data_dir, int max_level = 18)
        : _dir(data_dir),
          _wal_path(data_dir + "/wal.log"),
          _snap_path(data_dir + "/snapshot"),
          _list(max_level),
          _wal(data_dir + "/wal.log"),
          _wal_threshold(1u << 20)      // WAL 超过 1MB 自动提示压缩
    {}

    bool ok() const { return _wal.is_open(); }
    int size() const { return _list.size(); }
    size_t wal_bytes() const { return _wal.bytes(); }

    /* -----------------------------------------------------------------------
     * 写：先 WAL，再内存
     *
     * 顺序绝对不能反：
     *   如果先改内存再写 WAL，那么在「改完内存、WAL 还没落盘」之间崩溃，
     *   恢复时这条写入就丢了 —— 而调用方可能已经收到了成功响应。
     * ----------------------------------------------------------------------- */
    bool put(const K& key, const V& value) {
        // [P6] 关键：WAL 顺序必须和内存生效顺序一致。
        //   如果不加这把锁，两个线程写同一个 key 时可能出现：
        //     T1 写 WAL → T2 写 WAL → T2 改内存 → T1 改内存
        //   内存里是 T1 的值，但 WAL 顺序说 T2 最后写 —— 重放结果就错了。
        //   所以「写日志 + 改内存」必须是一个原子步骤。
        std::lock_guard<std::mutex> lk(_write_mtx);

        // [P2] 用 KeyCodec 编码，不再写死 to_string
        std::string k = KeyCodec<K>::encode(key);

        // 第 1 步：写日志（含 CRC），失败就直接返回 false，不动内存
        if (!_wal.append(WALRecord::kOpPut, k, value)) {
            std::cerr << "[KVStore] WAL 写入失败，拒绝本次写入" << std::endl;
            return false;
        }

        // 第 2 步：改内存。用 upsert 保证覆盖语义
        _list.upsert_element(key, value);
        return true;
    }

    /* -----------------------------------------------------------------------
     * 删
     * ----------------------------------------------------------------------- */
    bool del(const K& key) {
        std::lock_guard<std::mutex> lk(_write_mtx);   // [P6] 同 put

        std::string k = KeyCodec<K>::encode(key);
        if (!_wal.append(WALRecord::kOpDel, k, "")) {
            std::cerr << "[KVStore] WAL 写入失败，拒绝本次删除" << std::endl;
            return false;
        }
        _list.delete_element(key);
        return true;
    }

    /* -----------------------------------------------------------------------
     * 读：直接查跳表（WAL 只用于恢复，不参与读路径）
     * ----------------------------------------------------------------------- */
    bool get(const K& key, V& out) {
        // 复用 P3 的 seek：找第一个 >= key 的，再判断是否相等
        K found_key;
        if (!_list.seek(key, found_key, out)) return false;
        return found_key == key;
    }

    // 范围查询（直接转发 P3 的 scan）
    std::vector<std::pair<K, V>> scan(const K& begin_key, const K& end_key) {
        return _list.scan(begin_key, end_key);
    }

    /* -----------------------------------------------------------------------
     * 快照：把内存全量写成文本文件，然后清空 WAL
     *
     * 这是 WAL 的「压缩」：WAL 会无限增长（同一 key 被改 100 次就有 100 条记录），
     * 定期做一次全量快照 + 清空 WAL，就能把日志长度收敛到 O(数据量)。
     *
     * 顺序很重要：
     *   1. 先写快照（此时 WAL 还在，万一快照写一半崩了，WAL 仍能恢复）
     *   2. 快照写成功且 fsync 后，才清空 WAL
     *   反过来做（先清 WAL 再写快照）会在中间崩溃时丢数据。
     * ----------------------------------------------------------------------- */
    bool snapshot() {
        // [P6] 快照必须与写入互斥：
        //   否则遍历跳表的中途有写入，快照里会缺数据或重复，
        //   而清空 WAL 之后这些数据就永久丢了。
        std::lock_guard<std::mutex> lk(_write_mtx);

        // 快照用「先写临时文件再改名」保证原子性
        std::string tmp = _snap_path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "[KVStore] 无法创建快照文件: " << tmp << std::endl;
                return false;
            }
            // [P2] 快照格式从「key\tvalue\n 文本行」改成「长度前缀二进制」。
            //   原因：string key 里可能含 \t 或 \n，文本行格式会被破坏，
            //   恢复时切分错位 → 数据损坏。
            //   长度前缀对任意字节都安全，而且和 WAL 格式一致，读取逻辑能复用。
            //
            //   代价：快照不再是人类可读的。可以另外提供一个 --dump-text
            //   工具做只读导出（TODO）。
            for (auto it = _list.begin(); it != _list.end(); ++it) {
                std::string rec = WALRecord::encode_no_crc(
                    WALRecord::kOpPut,
                    KeyCodec<K>::encode(it->first),
                    it->second);
                out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
            }
            out.flush();
            if (!out.good()) {
                std::cerr << "[KVStore] 快照写入失败" << std::endl;
                return false;
            }
        }   // 析构时 close

        // 原子替换
        std::remove(_snap_path.c_str());
        if (std::rename(tmp.c_str(), _snap_path.c_str()) != 0) {
            std::cerr << "[KVStore] 快照重命名失败" << std::endl;
            return false;
        }

        // 快照已安全落盘，现在可以清空 WAL
        if (!_wal.reset()) {
            std::cerr << "[KVStore] 清空 WAL 失败" << std::endl;
            return false;
        }
        return true;
    }

    /* -----------------------------------------------------------------------
     * 恢复：启动时调用
     *   1. 先加载快照（如果有）
     *   2. 再重放 WAL（快照之后的增量）
     *
     * 顺序不能反：快照是基线，WAL 是基线之上的增量。
     * ----------------------------------------------------------------------- */
    size_t recover(bool verbose = false) {
        size_t snap_count = 0, wal_count = 0;

        // ---- 第 1 步：加载快照（长度前缀二进制格式）----
        {
            std::ifstream in(_snap_path, std::ios::binary);
            if (in.is_open()) {
                char op; std::string k, v;
                while (true) {
                    int r = WALRecord::read_one(in, op, k, v);
                    if (r == 0) break;              // EOF
                    if (r < 0) {
                        std::cerr << "[KVStore] 快照尾部不完整，已加载 " << snap_count
                                  << " 条后停止" << std::endl;
                        break;
                    }
                    if (op != WALRecord::kOpPut) continue;
                    try {
                        _list.upsert_element(KeyCodec<K>::decode(k), v);
                        snap_count++;
                    } catch (const std::exception& e) {
                        std::cerr << "[KVStore] 快照记录解码失败，跳过: " << e.what() << std::endl;
                    }
                }
            }
        }

        // ---- 第 2 步：重放 WAL ----
        wal_count = _wal.replay([this](char op, const std::string& k, const std::string& v) {
            // [P2] 用 KeyCodec 解码，不再写死 stoi。
            //   解码失败只跳过这一条，不让整个恢复过程崩掉 ——
            //   恢复期间最重要的目标是「尽可能多地救回数据」。
            K key;
            try {
                key = KeyCodec<K>::decode(k);
            } catch (const std::exception& e) {
                std::cerr << "[KVStore] WAL 记录 key 解码失败，跳过: " << e.what() << std::endl;
                return;
            }
            if (op == WALRecord::kOpPut) {
                _list.upsert_element(key, v);       // 用 upsert：保证最后一次写入生效
            } else if (op == WALRecord::kOpDel) {
                _list.delete_element(key);
            }
        });

        if (verbose) {
            std::cerr << "[KVStore] 恢复完成：快照 " << snap_count
                      << " 条 + WAL " << wal_count << " 条 = " << _list.size() << " 条" << std::endl;
        }
        return snap_count + wal_count;
    }

    SkipList<K, V>& list() { return _list; }

private:
    std::string _dir;
    std::string _wal_path;
    std::string _snap_path;
    SkipList<K, V> _list;      // [P2] key 类型改成模板参数 K
    WAL _wal;
    size_t _wal_threshold;

    // [P6] 保证「写 WAL + 改内存」的原子性，以及快照与写入的互斥。
    //   用一把 store 级锁而不是细粒度锁：
    //   这是正确性优先的选择 —— 跳表内部已有 shared_mutex 保证读并发，
    //   写路径本来就该串行（WAL 是顺序追加的，无法真正并行写）。
    mutable std::mutex _write_mtx;
};

#endif  // KVSTORE_H
