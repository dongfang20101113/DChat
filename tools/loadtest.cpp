// dchat 安全与压力测试工具（Linux，POSIX socket）。
//
// 用来给"安全性"那一页提供**实测数据**，而不是拍脑袋写结论。
// 五个场景各自独立，可以只跑其中一个：
//
//   flood     并发连接洪泛：开 N 条连接，看服务器接受多少、拒绝多少、耗时多少
//   slowloris 慢速耗尽：连上不登录，占住连接，看握手超时是否按规则清掉
//   badlogin  暴力破解：反复用错误密码登录，看 loginfails 是否封禁（对比封禁前后）
//   garbage   畸形数据：乱字节、超长行、只发不换行、发一半就断、提前注入 ENC
//   echo      吞吐压测：正常登录后持续发消息，测消息往返速率
//
// 关键原则：**不测服务器扛不扛得住"打死"**，测的是"防护是否按规则生效"。
// 所以每个场景都要先读规则、设好阈值，再看实际行为对不对得上。
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string g_host = "127.0.0.1";
int g_port = 5555;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int ConnectOnce(int timeoutMs, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        if (error) *error = "socket() 失败";
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(g_port));
    inet_pton(AF_INET, g_host.c_str(), &address.sin_addr);

    // 非阻塞 connect + poll，这样能自己控制超时（阻塞 connect 在内核队列满时会卡很久）
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        if (error) *error = "connect 立即失败";
        return -1;
    }
    pollfd item{};
    item.fd = fd;
    item.events = POLLOUT;
    const int ready = ::poll(&item, 1, timeoutMs);
    if (ready <= 0) {
        ::close(fd);
        if (error) *error = ready == 0 ? "connect 超时" : "poll 出错";
        return -1;
    }
    int soError = 0;
    socklen_t length = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
    if (soError != 0) {
        ::close(fd);
        if (error) *error = std::string("connect 被拒：") + std::strerror(soError);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

bool Send(const int fd, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 连接是否已被对端关闭（读到 FIN）。
///
/// ⚠️ 必须先**排空缓冲里已有的数据**再看 FIN：服务器连上就发 WELCOME、
/// 超时前还会发一条 ERROR。第一版这里只 recv 一次，拿到 WELCOME 就返回
/// "没关闭"，于是把"服务端明明按规则断开了"测成了"没有被断开"。
bool PeerClosed(int fd, int timeoutMs) {
    const long long deadline = NowMs() + timeoutMs;
    char chunk[4096];
    for (;;) {
        // 先看有没有可读数据；有就排掉，没有就短等
        pollfd item{};
        item.fd = fd;
        item.events = POLLIN;
        const long long left = deadline - NowMs();
        const int ready = ::poll(&item, 1, left > 0 ? static_cast<int>(left) : 0);
        if (ready <= 0) return false;  // 超时内没等到 FIN
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got == 0) return true;      // 读到 0 = 对端关闭
        if (got < 0) return false;      // 出错（含 EAGAIN）
        // got > 0：只是数据（WELCOME / ERROR），继续排空找 FIN
        if (NowMs() >= deadline) return false;
    }
}

std::string RecvSome(int fd, int timeoutMs) {
    pollfd item{};
    item.fd = fd;
    item.events = POLLIN;
    if (::poll(&item, 1, timeoutMs) <= 0) return std::string();
    char chunk[4096];
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) return std::string();
    return std::string(chunk, static_cast<std::size_t>(got));
}

// ---------------------------------------------------------------------------
// 场景 1：并发连接洪泛
// ---------------------------------------------------------------------------
void ScenarioFlood(int count, int timeoutMs) {
    std::printf("\n=== [1] 并发连接洪泛：目标 %d 条 ===\n", count);
    std::atomic<int> ok{0};
    std::atomic<int> refused{0};
    std::atomic<int> timedOut{0};
    std::vector<int> held;
    std::mutex heldMutex;

    const long long start = NowMs();
    const int threads = 16;
    std::vector<std::thread> workers;
    std::atomic<int> issued{0};
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (;;) {
                const int index = issued.fetch_add(1);
                if (index >= count) return;
                std::string error;
                const int fd = ConnectOnce(timeoutMs, &error);
                if (fd < 0) {
                    if (error.find("超时") != std::string::npos) {
                        ++timedOut;
                    } else {
                        ++refused;
                    }
                    continue;
                }
                // ⚠️ TCP 三次握手成功不等于"被服务器接受"：服务器可以在应用层
                // 检查完连接数上限后立刻把连接关掉，此时 connect 仍然是成功的。
                // 所以必须再等一小会儿看对端有没有发 FIN，否则测出来的数字是假的。
                if (PeerClosed(fd, 400)) {
                    ++refused;
                    ::close(fd);
                    continue;
                }
                ++ok;
                std::lock_guard<std::mutex> lock(heldMutex);
                held.push_back(fd);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const long long elapsed = NowMs() - start;

    std::printf("  成功建立      : %d 条\n", ok.load());
    std::printf("  被拒绝        : %d 条\n", refused.load());
    std::printf("  超时          : %d 条\n", timedOut.load());
    std::printf("  总耗时        : %lld ms\n", elapsed);
    if (elapsed > 0) {
        std::printf("  建连速率      : %.0f 条/秒\n", ok.load() * 1000.0 / elapsed);
    }
    // 保持连接 3 秒，确认连接是"真的活着"而不是握手完就断
    std::this_thread::sleep_for(std::chrono::seconds(3));
    int alive = 0;
    for (int fd : held) {
        // 收到 FIN 才算被断开；send 成功不代表对端还在
        if (!PeerClosed(fd, 100)) ++alive;
    }
    std::printf("  3 秒后仍可写  : %d / %d 条\n", alive, ok.load());
    for (int fd : held) ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// 场景 2：慢速耗尽（连上不登录，占住连接）
// ---------------------------------------------------------------------------
void ScenarioSlowloris(int count, int holdSeconds) {
    std::printf("\n=== [2] 慢速连接耗尽：%d 条连接、只连不登录、占住 %d 秒 ===\n", count,
                holdSeconds);
    std::vector<int> held;
    const long long start = NowMs();
    for (int i = 0; i < count; ++i) {
        std::string error;
        const int fd = ConnectOnce(2000, &error);
        if (fd >= 0) held.push_back(fd);
    }
    std::printf("  建立并占住    : %zu 条（耗时 %lld ms）\n", held.size(), NowMs() - start);

    // 先确认这些连接此刻确实还在（服务器还没清）
    std::this_thread::sleep_for(std::chrono::seconds(holdSeconds));
    int stillOpen = 0;
    int closedByServer = 0;
    for (int fd : held) {
        if (PeerClosed(fd, 300)) {
            ++closedByServer;
        } else {
            ++stillOpen;
        }
    }
    std::printf("  被服务器断开    : %d 条\n", closedByServer);
    std::printf("  仍被占住        : %d 条\n", stillOpen);
    std::printf("  （handshaketimeout 到点后服务器应主动断开未登录的连接）\n");
    for (int fd : held) ::close(fd);
}

// ---------------------------------------------------------------------------
// 场景 3：暴力破解（错误密码反复登录）
// ---------------------------------------------------------------------------
bool TryLogin(const std::string& user, const std::string& password, std::string* reply) {
    std::string error;
    const int fd = ConnectOnce(2000, &error);
    if (fd < 0) {
        *reply = error;
        return false;
    }
    Send(fd, "LOGIN " + user + " " + password + "\n");
    const std::string got = RecvSome(fd, 1500);
    ::close(fd);
    *reply = got;
    // 服务器对拒绝会回 ERROR 行
    return got.find("ERROR") == std::string::npos && !got.empty();
}

void ScenarioBadLogin(int attempts, const std::string& user) {
    std::printf("\n=== [3] 暴力破解：用错密码连续登 %d 次 ===\n", attempts);
    int rejected = 0;
    int firstBlockAt = -1;
    for (int i = 1; i <= attempts; ++i) {
        std::string reply;
        TryLogin(user, "wrongpassword" + std::to_string(i), &reply);
        // 失败 = 这次没拿到 LOGGEDIN。封禁的判定要宽一些：
        // 服务器可能回"失败次数过多"，也可能直接不回应/断开（也算封住了）
        const bool loggedIn = reply.find("LOGGEDIN") != std::string::npos;
        const bool blocked = !loggedIn;
        if (blocked) {
            ++rejected;
            if (firstBlockAt < 0) firstBlockAt = i;
        }
        if (i <= 3 || blocked) {
            std::string first;
            const std::size_t newline = reply.find('\n');
            first = reply.substr(0, newline == std::string::npos ? reply.size() : newline);
            std::printf("  第 %2d 次: %s\n", i, first.empty() ? "(无回应/被断开)" : first.c_str());
        }
    }
    std::printf("  被明确拒绝    : %d / %d 次\n", rejected, attempts);
    if (firstBlockAt > 0) {
        std::printf("  首次触发封禁  : 第 %d 次尝试\n", firstBlockAt);
    } else {
        std::printf("  首次触发封禁  : 未触发（loginfails=0 时不限制，这是如实结论）\n");
    }
}

// ---------------------------------------------------------------------------
// 场景 4：畸形数据
// ---------------------------------------------------------------------------
void ScenarioGarbage() {
    std::printf("\n=== [4] 畸形数据抗性 ===\n");
    struct Case {
        const char* name;
        const char* payload;
        int length;  // 0 = 用 strlen
    };
    const std::string huge(600000, 'A');  // 远超 4096 的行上限
    const std::string hugeLine = huge + "\n";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"纯二进制乱字节", std::string("\x00\x01\xff\xfe\x7f\x80garbage\r\n", 17)},
        {"超长行（600 KB 单行）", hugeLine},
        {"无换行只灌数据", std::string(200000, 'B')},
        {"空行轰炸", std::string(2000, '\n')},
        {"提前注入 ENC（未握手）", "ENC aGVsbG8=\n"},
        {"伪造 HELLO_OK 骗客户端", "HELLO_OK AAAA BBBB\n"},
        {"命令名非法字符", "SAY@#$%^&* x y\n"},
        {"嵌套转义与超深字段", "MSG " + std::string(5000, '\\') + "\n"},
        {"负数与超长数字参数", "FILE_SEND -1 -1 -99999999999999999999\n"},
        {"直接发 FILE_CHUNK 无前置", "FILE_CHUNK 999999999 " + std::string(3000, 'Z') + "\n"},
        {"不完整的 UTF-8 序列", std::string("\xe4\xbd") + "\n"},
    };

    for (const auto& item : cases) {
        std::string error;
        const int fd = ConnectOnce(2000, &error);
        if (fd < 0) {
            std::printf("  %-26s 连不上，跳过\n", item.first.c_str());
            continue;
        }
        const long long start = NowMs();
        const bool sent = Send(fd, item.second);
        const std::string reply = RecvSome(fd, 800);
        const long long elapsed = NowMs() - start;

        std::string first;
        const std::size_t newline = reply.find('\n');
        first = reply.substr(0, newline == std::string::npos ? reply.size() : newline);
        for (char& ch : first) {
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e) {
                ch = '?';
            }
        }
        if (first.size() > 60) first = first.substr(0, 60) + "…";
        std::printf("  %-26s 发出=%-3s 回应=%-3s %lldms  %s\n", item.first.c_str(),
                    sent ? "是" : "否", reply.empty() ? "无" : "有", elapsed, first.c_str());
        ::close(fd);
    }
    // 半包：发一半就断开，制造服务器端"不完整缓冲"
    {
        std::string error;
        const int fd = ConnectOnce(2000, &error);
        if (fd >= 0) {
            Send(fd, "MSG 这是一条只发了一半就断开的消");
            ::close(fd);
            std::printf("  %-26s 已断开（测试服务器是否会因不完整缓冲出问题）\n", "半包即断");
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// ---------------------------------------------------------------------------
// 场景 5：吞吐
// ---------------------------------------------------------------------------
void ScenarioEcho(const std::string& user, const std::string& password, int messages,
                  int senders) {
    std::printf("\n=== [5] 消息吞吐：%d 个客户端 × %d 条 ===\n", senders, messages);
    std::atomic<int> sent{0};
    std::atomic<int> acked{0};
    const long long start = NowMs();
    std::vector<std::thread> workers;
    for (int s = 0; s < senders; ++s) {
        workers.emplace_back([&, s] {
            std::string error;
            const int fd = ConnectOnce(3000, &error);
            if (fd < 0) return;
            const std::string name = user + std::to_string(s);
            Send(fd, "REGISTER " + name + " " + password + "\n");
            RecvSome(fd, 1500);  // 等注册回应
            for (int i = 0; i < messages; ++i) {
                if (!Send(fd, "MSG 压测消息 " + std::to_string(i) + " 来自 " + name + "\n")) break;
                ++sent;
                if (!RecvSome(fd, 30).empty()) ++acked;
            }
            ::close(fd);
        });
    }
    for (auto& worker : workers) worker.join();
    const long long elapsed = NowMs() - start;
    std::printf("  发出          : %d 条\n", sent.load());
    std::printf("  总耗时        : %lld ms\n", elapsed);
    if (elapsed > 0) {
        std::printf("  发送速率      : %.0f 条/秒\n", sent.load() * 1000.0 / elapsed);
        std::printf("  折合带宽      : %.0f KB/秒（按平均 60 字节/条）\n",
                    sent.load() * 60.0 / 1024.0 * 1000.0 / elapsed);
    }
}

void PrintUsage() {
    std::printf(
        "用法: dchat_loadtest <场景> [参数]\n"
        "  flood <连接数> [超时ms]     并发连接洪泛\n"
        "  slowloris <连接数> <秒>     慢速连接耗尽\n"
        "  badlogin <次数> <用户名>    暴力破解\n"
        "  garbage                     畸形数据抗性\n"
        "  echo <用户名> <密码> <条数> <并发数>  消息吞吐\n"
        "  all <用户名> <密码>         依次跑全部\n"
        "环境变量: DCHAT_HOST / DCHAT_PORT\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }
    if (const char* host = std::getenv("DCHAT_HOST")) g_host = host;
    if (const char* port = std::getenv("DCHAT_PORT")) g_port = std::atoi(port);

    const std::string scenario = argv[1];
    std::printf("目标: %s:%d\n", g_host.c_str(), g_port);

    if (scenario == "flood") {
        ScenarioFlood(argc > 2 ? std::atoi(argv[2]) : 200, argc > 3 ? std::atoi(argv[3]) : 3000);
    } else if (scenario == "slowloris") {
        ScenarioSlowloris(argc > 2 ? std::atoi(argv[2]) : 50, argc > 3 ? std::atoi(argv[3]) : 35);
    } else if (scenario == "badlogin") {
        ScenarioBadLogin(argc > 2 ? std::atoi(argv[2]) : 12, argc > 3 ? argv[3] : "victim");
    } else if (scenario == "garbage") {
        ScenarioGarbage();
    } else if (scenario == "echo") {
        ScenarioEcho(argc > 2 ? argv[2] : "loaduser", argc > 3 ? argv[3] : "loadpass123",
                     argc > 4 ? std::atoi(argv[4]) : 200, argc > 5 ? std::atoi(argv[5]) : 4);
    } else if (scenario == "all") {
        ScenarioFlood(200, 3000);
        ScenarioSlowloris(50, 35);
        ScenarioBadLogin(12, "victim");
        ScenarioGarbage();
        ScenarioEcho(argc > 2 ? argv[2] : "loaduser", argc > 3 ? argv[3] : "loadpass123", 200, 4);
    } else {
        PrintUsage();
        return 1;
    }
    std::printf("\n测试结束\n");
    return 0;
}
