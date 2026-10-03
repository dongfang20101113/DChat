// 账号注册洪水测试：模拟"脚本疯狂注册新用户"（配合换 IP）。
//
// 为什么单独测这个：之前所有安全测试都打在**已有账号**的路径上
// （登录失败封禁、连接数上限），注册路径一次都没测过。而换 IP 恰恰能绕开
// 所有基于 IP 的限制（maxconnsperip / loginfails），所以这条路必须单独看。
//
// 五个场景：
//   rate     单线程连续注册，量吞吐与延迟是否随账号数增长而恶化
//   parallel 多线程并发注册（模拟同时换多个 IP）
//   fill     批量灌账号，观察服务端内存、账号文件大小、延迟曲线
//   impact   注册洪水期间，正常用户的登录/发言是否被拖慢（关键：可用性影响）
//   offline  离线生成 payload（不连服务器），只用来核对命令格式
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

int ConnectOnce(int timeoutMs) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(g_port));
    inet_pton(AF_INET, g_host.c_str(), &address.sin_addr);

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
        errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd item{};
    item.fd = fd;
    item.events = POLLOUT;
    if (::poll(&item, 1, timeoutMs) <= 0) {
        ::close(fd);
        return -1;
    }
    int soError = 0;
    socklen_t length = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
    if (soError != 0) {
        ::close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

bool SendAll(int fd, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 等到出现关键字或超时；返回耗时毫秒（-1 = 超时）
long long WaitFor(int fd, const std::string& needle, int timeoutMs) {
    const long long start = NowMs();
    std::string buffer;
    char chunk[4096];
    while (NowMs() - start < timeoutMs) {
        pollfd item{};
        item.fd = fd;
        item.events = POLLIN;
        if (::poll(&item, 1, 200) <= 0) continue;
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) return -1;
        buffer.append(chunk, static_cast<std::size_t>(got));
        if (buffer.find(needle) != std::string::npos) return NowMs() - start;
    }
    return -1;
}

/// 注册一个账号，返回耗时毫秒（-1 = 失败/超时）
long long RegisterOnce(const std::string& name, const std::string& password, int timeoutMs) {
    const int fd = ConnectOnce(timeoutMs);
    if (fd < 0) return -1;
    if (!SendAll(fd, "REGISTER " + name + " " + password + "\n")) {
        ::close(fd);
        return -1;
    }
    const long long cost = WaitFor(fd, "注册成功", timeoutMs);
    ::close(fd);
    return cost;
}

std::string GenName(const char* prefix, long long index) {
    return std::string(prefix) + std::to_string(index);
}

// ---------------------------------------------------------------------------
void ScenarioRate(int count, const char* prefix) {
    std::printf("\n=== [1] 单线程连续注册 %d 个账号 ===\n", count);
    std::printf("  %-8s %-12s %-12s %s\n", "序号", "本次耗时", "累计耗时", "趋势");
    const long long start = NowMs();
    std::vector<long long> costs;
    long long worst = 0;
    int failed = 0;
    for (int i = 1; i <= count; ++i) {
        const long long cost = RegisterOnce(GenName(prefix, i), "pass123456", 8000);
        if (cost < 0) {
            ++failed;
            continue;
        }
        costs.push_back(cost);
        worst = std::max(worst, cost);
        if (i <= 5 || i % 25 == 0 || i == count) {
            std::printf("  %-8d %-12lld %-12lld\n", i, cost, NowMs() - start);
        }
    }
    const long long total = NowMs() - start;
    if (!costs.empty()) {
        long long sum = 0;
        for (long long c : costs) sum += c;
        // 前半段与后半段的平均耗时对比——这是判断"是否随账号数恶化"的关键证据
        const std::size_t half = costs.size() / 2;
        long long firstHalf = 0;
        long long secondHalf = 0;
        for (std::size_t i = 0; i < costs.size(); ++i) {
            if (i < half) firstHalf += costs[i];
            else secondHalf += costs[i];
        }
        const double avgFirst = half ? static_cast<double>(firstHalf) / half : 0;
        const double avgSecond = costs.size() - half
                                     ? static_cast<double>(secondHalf) / (costs.size() - half)
                                     : 0;
        std::printf("\n  成功          : %zu / %d（失败 %d）\n", costs.size(), count, failed);
        std::printf("  总耗时        : %lld ms\n", total);
        std::printf("  注册速率      : %.0f 个/秒\n", costs.size() * 1000.0 / std::max(1LL, total));
        std::printf("  平均耗时      : %.0f ms\n", static_cast<double>(sum) / costs.size());
        std::printf("  最慢一次      : %lld ms\n", worst);
        std::printf("  前半段平均    : %.0f ms\n", avgFirst);
        std::printf("  后半段平均    : %.0f ms  <- 明显变大就说明「账号越多越慢」\n", avgSecond);
    }
}

void ScenarioParallel(int threads, int perThread, const char* prefix) {
    std::printf("\n=== [2] %d 线程并发注册（模拟同时换 %d 个 IP）× 每个 %d 个 ===\n",
                threads, threads, perThread);
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    std::mutex costMutex;
    std::vector<long long> costs;
    const long long start = NowMs();
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < perThread; ++i) {
                const std::string name = std::string(prefix) + "p" + std::to_string(t) + "_" +
                                         std::to_string(i);
                const long long cost = RegisterOnce(name, "pass123456", 10000);
                if (cost < 0) {
                    ++fail;
                } else {
                    ++ok;
                    std::lock_guard<std::mutex> lock(costMutex);
                    costs.push_back(cost);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const long long total = NowMs() - start;
    std::printf("  成功          : %d 个\n", ok.load());
    std::printf("  失败          : %d 个\n", fail.load());
    std::printf("  总耗时        : %lld ms\n", total);
    std::printf("  合计速率      : %.0f 个/秒\n", ok.load() * 1000.0 / std::max(1LL, total));
    if (!costs.empty()) {
        std::sort(costs.begin(), costs.end());
        std::printf("  耗时中位数    : %lld ms\n", costs[costs.size() / 2]);
        std::printf("  耗时 P95      : %lld ms\n", costs[static_cast<std::size_t>(costs.size() * 0.95)]);
        std::printf("  耗时最大      : %lld ms\n", costs.back());
    }
}

void ScenarioImpact(int floodSeconds, const char* prefix) {
    std::printf("\n=== [3] 注册洪水期间的正常用户体验（关键：可用性影响）===\n");
    std::atomic<bool> stop{false};
    std::atomic<int> registered{0};

    // 后台持续注册
    std::thread flood([&] {
        long long index = 0;
        while (!stop.load()) {
            const std::string name = std::string(prefix) + "f" + std::to_string(index++);
            if (RegisterOnce(name, "pass123456", 8000) >= 0) ++registered;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 前台模拟正常用户：注册一次 + 登录 + 发言，量每一步的耗时
    {
        const long long t0 = NowMs();
        const long long cost = RegisterOnce(std::string(prefix) + "normal1", "normalpass1", 15000);
        std::printf("  正常用户注册  : %lld ms（成功=%s）\n", NowMs() - t0, cost >= 0 ? "是" : "否");
    }
    {
        const long long t0 = NowMs();
        const int fd = ConnectOnce(10000);
        if (fd >= 0) {
            SendAll(fd, std::string("LOGIN ") + prefix + "normal1 normalpass1\n");
            const long long cost = WaitFor(fd, "LOGGEDIN", 15000);
            std::printf("  正常用户登录  : %lld ms（成功=%s）\n", NowMs() - t0, cost >= 0 ? "是" : "否");
            const long long t1 = NowMs();
            SendAll(fd, "MSG 洪水期间的正常消息\n");
            const long long cost2 = WaitFor(fd, "洪水期间的正常消息", 15000);
            std::printf("  正常用户发言  : %lld ms（成功=%s）\n", NowMs() - t1,
                        cost2 >= 0 ? "是" : "否");
            ::close(fd);
        } else {
            std::printf("  正常用户登录  : 连不上服务器\n");
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(floodSeconds));
    stop = true;
    flood.join();
    std::printf("\n  洪水期间注册了: %d 个账号\n", registered.load());
}

void PrintUsage() {
    std::printf(
        "用法: dchat_regflood <场景> [参数]\n"
        "  rate <个数> <前缀>            单线程连续注册\n"
        "  parallel <线程> <每线程个数> <前缀>   并发注册（模拟换 IP）\n"
        "  impact <洪水秒数> <前缀>      量洪水期间正常用户的耗时\n"
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
    std::printf("目标: %s:%d\n", g_host.c_str(), g_port);

    const std::string scenario = argv[1];
    if (scenario == "rate") {
        ScenarioRate(argc > 2 ? std::atoi(argv[2]) : 100, argc > 3 ? argv[3] : "bot");
    } else if (scenario == "parallel") {
        ScenarioParallel(argc > 2 ? std::atoi(argv[2]) : 8,
                         argc > 3 ? std::atoi(argv[3]) : 25, argc > 4 ? argv[4] : "bot");
    } else if (scenario == "impact") {
        ScenarioImpact(argc > 2 ? std::atoi(argv[2]) : 3, argc > 3 ? argv[3] : "bot");
    } else {
        PrintUsage();
        return 1;
    }
    std::printf("\n测试结束\n");
    return 0;
}
