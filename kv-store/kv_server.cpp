/* ************************************************************************
> File Name:     kv_server.cpp
> Description:   [P6] 基于 epoll 的 KV 服务器，对外提供 RESP 协议
>
> 架构：
>
>     redis-cli / 任意 Redis 客户端
>               │  RESP over TCP
>               ▼
>     ┌─────────────────────────────────────────┐
>     │  KvServer                               │
>     │                                         │
>     │  main thread:  accept() 新连接           │
>     │       │                                 │
>     │       ▼                                 │
>     │  epoll (LT 模式) 监听所有连接            │
>     │       │                                 │
>     │       ├── 可读 → 每连接 Parser.feed()    │
>     │       │         → drain() 取命令         │
>     │       │         → 执行 → 写回响应        │
>     │       └── 可写 → 发送未写完的响应缓冲     │
>     └─────────────────────────────────────────┘
>               │
>               ▼
>        KVStore (跳表 + WAL)
>
> 支持命令：
>     PING                 → +PONG
>     SET key value        → +OK
>     GET key              → $len\r\nvalue  /  $-1（不存在）
>     DEL key              → :1 / :0
>     EXISTS key           → :1 / :0
>     KEYS                 → 数组，全部 key（演示用，生产应避免）
>     SCAN begin end       → 数组，范围查询 [begin, end)  ← P3 的能力
>     COUNT begin end      → :n，区间内元素个数          ← P3 的能力
>     DBSIZE               → :n
>     SNAPSHOT             → +OK，触发快照压缩 WAL       ← P4 的能力
>     INFO                 → 数组，服务状态
>     COMMAND              → 空数组（让 redis-cli 兼容）
>     QUIT                 → +OK 后关闭连接
>
> 编译：
>   g++ kv_server.cpp -o bin/kv_server --std=c++17 -pthread -O2
>
> 运行：
>   ./bin/kv_server [端口] [数据目录]     默认 6380 /tmp/kvdata
>   redis-cli -p 6380 ping
 ************************************************************************/

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "kvstore.h"
#include "resp.h"

// ---------------------------------------------------------------------------
// 每个连接的上下文
// ---------------------------------------------------------------------------
struct Connection {
    int fd;
    resp::Parser parser;          // 每连接一个解析器（保存半条命令）
    std::string out_buf;          // 待发送的响应（可能一次写不完）
    size_t out_sent = 0;          // 已经写出去多少
    bool want_close = false;      // 发完就关（QUIT）

    explicit Connection(int f) : fd(f) {}

    bool has_pending_output() const { return out_sent < out_buf.size(); }

    void append(const std::string& s) { out_buf += s; }
};

// ---------------------------------------------------------------------------
class KvServer {
public:
    KvServer(int port, const std::string& data_dir)
        : _port(port), _data_dir(data_dir), _listen_fd(-1), _epfd(-1),
          _store(data_dir), _running(false), _total_cmds(0), _total_conns(0) {}

    ~KvServer() { stop(); }

    /* -----------------------------------------------------------------------
     * 启动：建 socket、bind、listen、建 epoll
     * ----------------------------------------------------------------------- */
    bool start() {
        // 恢复数据（快照 + WAL）
        size_t n = _store.recover(true);
        std::cout << "[server] 恢复 " << n << " 条记录，当前 size = "
                  << _store.size() << std::endl;

        // ---- 创建监听 socket ----
        _listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (_listen_fd < 0) {
            std::cerr << "[server] socket() 失败: " << strerror(errno) << std::endl;
            return false;
        }

        int opt = 1;
        setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//设置这个参数避免tcp连接断开重连后的timewait

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(_port));

        if (::bind(_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[server] bind(" << _port << ") 失败: " << strerror(errno) << std::endl;
            return false;
        }

        if (::listen(_listen_fd, SOMAXCONN) < 0) {
            std::cerr << "[server] listen() 失败: " << strerror(errno) << std::endl;
            return false;
        }

        set_nonblocking(_listen_fd);

        // ---- 创建 epoll ----
        _epfd = ::epoll_create1(0);
        if (_epfd < 0) {
            std::cerr << "[server] epoll_create1() 失败: " << strerror(errno) << std::endl;
            return false;
        }

        // 监听 socket 注册到 epoll
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = _listen_fd;
        if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, _listen_fd, &ev) < 0) {
            std::cerr << "[server] epoll_ctl(listen) 失败" << std::endl;
            return false;
        }

        _running = true;
        std::cout << "[server] 监听 0.0.0.0:" << _port
                  << "，数据目录 " << _data_dir << std::endl;
        std::cout << "[server] 测试: redis-cli -p " << _port << " ping" << std::endl;
        return true;
    }

    /* -----------------------------------------------------------------------
     * 事件循环
     * ----------------------------------------------------------------------- */
    void run() {
        constexpr int kMaxEvents = 1024;
        std::vector<epoll_event> events(kMaxEvents);

        while (_running) {
            int n = ::epoll_wait(_epfd, events.data(), kMaxEvents, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::cerr << "[server] epoll_wait 失败: " << strerror(errno) << std::endl;
                break;
            }

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                if (fd == _listen_fd) {
                    accept_new();
                } else {
                    if (ev & (EPOLLHUP | EPOLLERR)) {
                        close_conn(fd);
                        continue;
                    }
                    if (ev & EPOLLIN)  on_readable(fd);
                    if (ev & EPOLLOUT) on_writable(fd);
                    // on_readable 可能已关闭连接
                    if (_conns.find(fd) == _conns.end()) continue;
                    if (_conns[fd]->want_close && !_conns[fd]->has_pending_output()) {
                        close_conn(fd);
                    }
                }
            }
        }
    }

    void stop() {
        _running = false;
        for (auto& kv : _conns) ::close(kv.first);
        _conns.clear();
        if (_listen_fd >= 0) { ::close(_listen_fd); _listen_fd = -1; }
        if (_epfd >= 0) { ::close(_epfd); _epfd = -1; }
    }

private:
    /* -----------------------------------------------------------------------
     * 接受新连接
     * ----------------------------------------------------------------------- */
    void accept_new() {
        // 一次循环尽量多 accept（水平触发下也建议这样做，减少 epoll_wait 次数）
        while (true) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            int cfd = ::accept(_listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 没有更多了
                if (errno == EINTR) continue;
                std::cerr << "[server] accept 失败: " << strerror(errno) << std::endl;
                break;
            }

            set_nonblocking(cfd);
            // 关闭 Nagle：KV 场景都是小包往返，禁用 Nagle 降低延迟
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            auto conn = std::make_shared<Connection>(cfd);
            _conns[cfd] = conn;

            epoll_event ev{};
            ev.events = EPOLLIN;               // 需要写时再动态加 EPOLLOUT
            ev.data.fd = cfd;
            if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
                std::cerr << "[server] epoll_ctl(add conn) 失败" << std::endl;
                ::close(cfd);
                _conns.erase(cfd);
                continue;
            }
            _total_conns++;

            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            std::cout << "[server] 新连接 fd=" << cfd << " from " << ip << ":"
                      << ntohs(peer.sin_port) << "（当前 " << _conns.size() << " 个连接）"
                      << std::endl;
        }
    }

    /* -----------------------------------------------------------------------
     * 可读：读数据 → 解析 → 执行 → 写响应
     * ----------------------------------------------------------------------- */
    void on_readable(int fd) {
        auto it = _conns.find(fd);
        if (it == _conns.end()) return;
        auto conn = it->second;

        char buf[16384];
        while (true) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn->parser.feed(buf, static_cast<size_t>(n));
                // 继续读，直到 EAGAIN（把内核缓冲里的数据一次读完）
                if (static_cast<size_t>(n) < sizeof(buf)) break;
                continue;
            }
            if (n == 0) {                     // 对端关闭
                close_conn(fd);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close_conn(fd);
            return;
        }

        // ---- 协议错误：回错误并关闭 ----
        if (conn->parser.has_error()) {
            conn->append(resp::Encoder::error("ERR Protocol error: " + conn->parser.error_msg()));
            conn->want_close = true;
            flush_output(conn);
            return;
        }

        // ---- 执行所有已解析出的命令 ----
        resp::Command cmd;
        while (conn->parser.next(cmd)) {
            _total_cmds++;
            std::string reply = execute(cmd);
            conn->append(reply);
            if (cmd.name() == "QUIT") {
                conn->want_close = true;
                break;
            }
        }

        flush_output(conn);
    }

    /* -----------------------------------------------------------------------
     * 可写：把没发完的响应继续发出去
     * ----------------------------------------------------------------------- */
    void on_writable(int fd) {
        auto it = _conns.find(fd);
        if (it == _conns.end()) return;
        flush_output(it->second);
    }

    /* -----------------------------------------------------------------------
     * 尝试把 conn->out_buf 里剩余的数据写出去
     *
     * 关键点：TCP 写可能「只写一部分」（内核发送缓冲满了），
     * 所以必须记录 out_sent，并在没写完时注册 EPOLLOUT 等下次机会。
     * 这是网络编程里非常容易漏的地方 —— 漏了就会丢响应或卡住。
     * ----------------------------------------------------------------------- */
    void flush_output(const std::shared_ptr<Connection>& conn) {
        while (conn->has_pending_output()) {
            size_t remain = conn->out_buf.size() - conn->out_sent;
            ssize_t n = ::write(conn->fd, conn->out_buf.data() + conn->out_sent, remain);
            if (n > 0) {
                conn->out_sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // 内核缓冲满：注册 EPOLLOUT，等可写再继续
                mod_epoll(conn->fd, EPOLLIN | EPOLLOUT);
                return;
            }
            if (n < 0 && errno == EINTR) continue;
            // 真错误：关闭连接
            close_conn(conn->fd);
            return;
        }

        // 全部写完：清理缓冲，取消 EPOLLOUT 关注
        conn->out_buf.clear();
        conn->out_sent = 0;
        if (_conns.find(conn->fd) != _conns.end()) {
            mod_epoll(conn->fd, EPOLLIN);
        }
        if (conn->want_close) close_conn(conn->fd);
    }

    void mod_epoll(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    void close_conn(int fd) {
        auto it = _conns.find(fd);
        if (it == _conns.end()) return;
        ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        _conns.erase(it);
    }

    static void set_nonblocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* -----------------------------------------------------------------------
     * 命令分发与执行
     * ----------------------------------------------------------------------- */
    std::string execute(const resp::Command& cmd) {
        // Redis 命令名【大小写不敏感】：redis-cli 实际发的是小写 "ping"/"set"。
        // 这里统一转成大写再分发，否则所有命令都会 unknown。
        std::string name = to_upper(cmd.name());

        // ---------------- PING ----------------
        if (name == "PING") {
            if (cmd.size() == 1) return resp::Encoder::simple_string("PONG");
            return resp::Encoder::bulk_string(cmd.args[1]);
        }

        // ---------------- SET key value ----------------
        // [P2] key 现在是任意字符串，不再要求整数
        if (name == "SET") {
            if (cmd.size() != 3) return arity_error("SET");
            if (!_store.put(cmd.args[1], cmd.args[2])) {
                return resp::Encoder::error("ERR 写入失败（WAL 不可写）");
            }
            return resp::Encoder::simple_string("OK");
        }

        // ---------------- GET key ----------------
        if (name == "GET") {
            if (cmd.size() != 2) return arity_error("GET");
            std::string v;
            if (!_store.get(cmd.args[1], v)) return resp::Encoder::nil();
            return resp::Encoder::bulk_string(v);
        }

        // ---------------- DEL key [key ...] ----------------
        if (name == "DEL") {
            if (cmd.size() < 2) return arity_error("DEL");
            long long deleted = 0;
            for (size_t i = 1; i < cmd.size(); i++) {
                std::string v;
                if (_store.get(cmd.args[i], v)) {
                    _store.del(cmd.args[i]);
                    deleted++;
                }
            }
            return resp::Encoder::integer(deleted);
        }

        // ---------------- EXISTS key ----------------
        if (name == "EXISTS") {
            if (cmd.size() != 2) return arity_error("EXISTS");
            std::string v;
            return resp::Encoder::integer(_store.get(cmd.args[1], v) ? 1 : 0);
        }

        // ---------------- KEYS ----------------
        // 演示用：返回全部 key。生产环境应改用 SCAN 游标，避免一次拉回太多
        if (name == "KEYS") {
            std::vector<std::string> out;
            auto all = _store.scan(std::string(), std::string(1, '\xff'));
            out.reserve(all.size());
            for (auto& kv : all) out.push_back(kv.first);
            return resp::Encoder::array(out);
        }

        // ---------------- SCAN begin end  ← P3 的能力 ----------------
        // [P2] 现在按【字典序】范围查询，可以这样用：
        //     SCAN user: user;        —— 所有以 "user:" 开头的 key
        //     SCAN a m                —— 字典序 [a, m)
        if (name == "SCAN") {
            if (cmd.size() != 3) return arity_error("SCAN begin end");
            auto rows = _store.scan(cmd.args[1], cmd.args[2]);
            std::vector<std::string> out;
            out.reserve(rows.size() * 2);
            for (auto& kv : rows) {
                out.push_back(kv.first);
                out.push_back(kv.second);
            }
            return resp::Encoder::array(out);
        }

        // ---------------- COUNT begin end  ← P3 的能力 ----------------
        if (name == "COUNT") {
            if (cmd.size() != 3) return arity_error("COUNT begin end");
            auto rows = _store.scan(cmd.args[1], cmd.args[2]);
            return resp::Encoder::integer(static_cast<long long>(rows.size()));
        }

        // ---------------- DBSIZE ----------------
        if (name == "DBSIZE") {
            return resp::Encoder::integer(_store.size());
        }

        // ---------------- SNAPSHOT  ← P4 的能力 ----------------
        if (name == "SNAPSHOT") {
            if (!_store.snapshot()) return resp::Encoder::error("ERR 快照失败");
            return resp::Encoder::simple_string("OK");
        }

        // ---------------- INFO ----------------
        if (name == "INFO") {
            std::vector<std::string> out;
            out.push_back("server");
            out.push_back("kv_server (skiplist + WAL)");
            out.push_back("keys");
            out.push_back(std::to_string(_store.size()));
            out.push_back("connections");
            out.push_back(std::to_string(_conns.size()));
            out.push_back("total_connections");
            out.push_back(std::to_string(_total_conns));
            out.push_back("total_commands");
            out.push_back(std::to_string(_total_cmds));
            out.push_back("wal_bytes");
            out.push_back(std::to_string(_store.wal_bytes()));
            return resp::Encoder::array(out);
        }

        // ---------------- CONFIG GET <param> ----------------
        // redis-benchmark 启动时会发 CONFIG GET save / appendonly 来探测服务端配置，
        // 不实现会打印 "ERROR: ERR unknown command 'CONFIG'" 警告。
        // 我们没有可配置项，返回空数组即可（表示"该项无配置"）。
        if (name == "CONFIG") {
            return resp::Encoder::empty_array();
        }

        // ---------------- ECHO message ----------------
        // redis-cli --pipe 模式在结束时发 ECHO <magic> 来确认与服务端同步，
        // 不实现它会导致 --pipe 报 "errors: 1"。很多客户端库也会用 ECHO 做心跳。
        if (name == "ECHO") {
            if (cmd.size() != 2) return arity_error("ECHO");
            return resp::Encoder::bulk_string(cmd.args[1]);
        }

        // ---------------- SELECT db ----------------
        // 客户端连上来常会先发 SELECT 0。我们只有单库，
        // 对 db=0 返回 OK，其它库返回错误（诚实告知不支持）。
        if (name == "SELECT") {
            if (cmd.size() != 2) return arity_error("SELECT");
            if (cmd.args[1] == "0") return resp::Encoder::simple_string("OK");
            return resp::Encoder::error("ERR DB index is out of range (only db 0 exists)");
        }

        // ---------------- COMMAND ----------------
        // redis-cli 连接时会发 COMMAND DOCS 探测能力，返回空数组让它继续
        if (name == "COMMAND") {
            return resp::Encoder::empty_array();
        }

        // ---------------- QUIT ----------------
        if (name == "QUIT") {
            return resp::Encoder::simple_string("OK");
        }

        return resp::Encoder::error("ERR unknown command '" + name + "'");
    }

    /* -----------------------------------------------------------------------
     * [P2] parse_key / int_error 已删除
     * -----------------------------------------------------------------------
     * 原版这里强制 key 必须是整数，否则报
     *     "ERR key must be an integer (this build uses int keys)"
     * 现在 key 是任意字符串，交给 KVStore<std::string, std::string> 处理，
     * 不需要在协议层做类型校验了。
     * ----------------------------------------------------------------------- */

    std::string arity_error(const std::string& usage) {
        return resp::Encoder::error("ERR wrong number of arguments for '" + usage + "'");
    }

    // 命令名转大写（Redis 命令大小写不敏感）
    static std::string to_upper(const std::string& s) {
        std::string r = s;
        for (char& c : r) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return r;
    }

    int _port;
    std::string _data_dir;
    int _listen_fd;
    int _epfd;
    std::map<int, std::shared_ptr<Connection>> _conns;
    // [P2] key 类型从 int 改成 std::string，支持 SET username alice
    KVStore<std::string, std::string> _store;
    bool _running;
    long long _total_cmds;
    long long _total_conns;
};

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 6380;
    std::string dir = (argc > 2) ? argv[2] : "/tmp/kvdata";

    // 忽略 SIGPIPE：向已关闭的连接写数据时，不要让进程被信号杀死
    ::signal(SIGPIPE, SIG_IGN);

    // 确保数据目录存在
    std::string mk = "mkdir -p '" + dir + "'";
    if (system(mk.c_str()) != 0) {
        std::cerr << "无法创建数据目录 " << dir << std::endl;
        return 1;
    }

    KvServer server(port, dir);
    if (!server.start()) return 1;
    server.run();
    return 0;
}
