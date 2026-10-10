// 多会话协议的端到端探针：直接说协议，不依赖任何界面。
//
// 为什么需要它：三端界面都还没改的时候，服务端的房间逻辑**没有任何办法验证**——
// 单测只能证明纯函数是对的，证明不了"命令真的接上了、消息真的发对了人"。
// 这个探针用最笨的方式把协议说一遍，于是下面这些最容易出错的事都能被钉住：
//   * 老客户端（不说 rooms1）会不会意外收到群聊/私聊的消息
//   * 发了 GCREATE 之后 ROOMLIST 里到底有没有这个群
//   * 群里发的 RMSG 带没带房间号、大厅的 SAY 有没有变样
//   * 不在群里的人能不能拿文件 ID 把别人的附件下下来
//
// 用法：
//   room_probe <主机> <端口> <昵称> <密码> <login|register> [no-features]
// 命令从 **stdin** 一行一条读，支持两个指令：
//   @sleep <毫秒>    等一会儿
//   @drain <毫秒>    只收不发
// 其余每一行原样发给服务器。服务器发来的每一行原样打到 stdout，
// 自己发出去的行以 `# sent:` 开头，方便对着看。
//
// 第 6 个参数传 no-features 就**不声明 rooms1** —— 用来扮演"老客户端"，
// 验证服务端对老客户端的行为与升级前一模一样。不传这个参数时**必须**声明，
// 否则整套多会话命令都会被服务端按"你的客户端不支持"拒掉。
// ⚠️ socket_util.h 必须**第一个**包含：它内部要在 windows.h 之前包进 winsock2.h，
// 而 <string> 这类标准库头在某些工具链下会间接拉进 windows.h（于是老 winsock.h
// 定义了 _WINSOCKAPI_，winsock2.h 被它自己的保护宏挡掉，SOCKET 全不认识）。
// 这个坑 socket_util.h 的注释里写得很详细，这里只是照做。
#include "socket_util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "protocol.h"

namespace {

dchat::sock::Handle g_sock = dchat::sock::kInvalid;
dchat::LineBuffer g_buffer;

/** 收一段时间的数据并打印。返回 false 表示连接已经断了。 */
bool Pump(int totalMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMs);
    char chunk[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        const std::ptrdiff_t got = dchat::sock::Receive(g_sock, chunk, sizeof(chunk));
        if (got > 0) {
            g_buffer.Append(chunk, static_cast<std::size_t>(got));
            std::string line;
            while (g_buffer.PopLine(&line)) {
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
            }
            continue;
        }
        if (got == 0) {
            std::printf("# -- 连接已被服务器关闭 --\n");
            std::fflush(stdout);
            return false;
        }
        // 超时（接收超时设成了 200ms）不是错误，只是"这会儿没数据"
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/**
 * 原样发一行（自己补换行）。
 *
 * ⚠️ 这里**不能**走 dchat::BuildLine：BuildLine 的第一个参数是"命令名"，
 * 它会用一个字符白名单去校验（只允许字母/数字/下划线）。把整行
 * "GCREATE mygroup xxx=" 当命令名传进去，会因为含空格和 '=' 被判定非法，
 * **静默返回空串**，于是探针看起来"发送失败"、而服务器那边什么都没收到。
 * （这个坑真实踩过一次：e2e 全线报"发送失败"，查了半天才发现在探针自己身上。）
 */
bool SendRaw(const std::string& line) {
    return dchat::sock::SendAll(g_sock, (line + "\n").data(), line.size() + 1);
}

/** 拼一条协议行再发（用于登录这类字段规整的命令）。 */
bool SendLine(const std::string& command, const std::string& rest = std::string()) {
    const std::string line = dchat::BuildLine(command, rest);
    if (line.empty()) return false;
    return SendRaw(line);
}

/** 等某一行出现（或超时）。返回是否等到。 */
bool WaitFor(const std::string& needle, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char chunk[4096];
    std::string seen;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::ptrdiff_t got = dchat::sock::Receive(g_sock, chunk, sizeof(chunk));
        if (got > 0) {
            g_buffer.Append(chunk, static_cast<std::size_t>(got));
            std::string line;
            while (g_buffer.PopLine(&line)) {
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
                seen += line;
                seen += "\n";
                if (line.find(needle) != std::string::npos) return true;
            }
            continue;
        }
        if (got == 0) {
            std::printf("# -- 连接已被服务器关闭 --\n");
            std::fflush(stdout);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)seen;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "用法：room_probe <主机> <端口> <昵称> <密码> <login|register> [no-features]\n"
                     "命令从 stdin 一行一条读（# 开头是注释，@sleep/@drain 见文件头）\n");
        return 2;
    }
    const std::string host = argv[1];
    const int port = std::atoi(argv[2]);
    const std::string nick = argv[3];
    const std::string password = argv[4];
    const std::string mode = argv[5];
    const bool noFeatures = (argc >= 7 && std::string(argv[6]) == "no-features");

    if (!dchat::sock::Startup()) {
        std::fprintf(stderr, "WSAStartup 失败\n");
        return 2;
    }

    const dchat::sock::Address address = dchat::sock::MakeAddress(host, port);
    g_sock = dchat::sock::CreateTcp();
    if (g_sock == dchat::sock::kInvalid) {
        std::fprintf(stderr, "建 socket 失败\n");
        return 2;
    }
    dchat::sock::SetNoSigpipe(g_sock);
    dchat::sock::SetNoDelay(g_sock);
    // 接收超时设短一点：整个探针靠"收不到就当前没数据"来推进，
    // 超时长一点只会让每一步都变慢
    dchat::sock::SetReceiveTimeout(g_sock, 200);

    if (::connect(g_sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "连接 %s:%d 失败（错误码 %d）\n", host.c_str(), port,
                     dchat::sock::LastError());
        return 2;
    }

    // 1) 等 WELCOME
    if (!WaitFor("WELCOME", 3000)) {
        std::fprintf(stderr, "没等到 WELCOME\n");
        return 2;
    }

    // 2) 登录 / 注册
    if (!SendLine(mode == "register" ? "REGISTER" : "LOGIN", nick + " " + password)) {
        std::fprintf(stderr, "发送登录请求失败\n");
        return 2;
    }
    if (!WaitFor("LOGGEDIN", 4000)) {
        std::fprintf(stderr, "登录失败（没有收到 LOGGEDIN）\n");
        return 2;
    }
    Pump(150);  // 把 WELCOME/NAMES/RULES 之类的尾巴收干净

    // 3) 声明多会话能力（**这一步就是"新客户端"和"老客户端"的分界线**）
    if (noFeatures) {
        std::printf("# -- 本次不声明 rooms1：扮演老客户端 --\n");
        std::fflush(stdout);
    } else {
        SendLine("FEATURES", "rooms1");
        Pump(300);
    }

    // 4) 跑 stdin 里的脚本
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("@sleep", 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(line.c_str() + 6)));
            continue;
        }
        if (line.rfind("@drain", 0) == 0) {
            Pump(std::atoi(line.c_str() + 6));
            continue;
        }
        std::printf("# sent: %s\n", line.c_str());
        std::fflush(stdout);
        if (!SendRaw(line)) {
            std::fprintf(stderr, "发送失败：%s\n", line.c_str());
            break;
        }
        Pump(250);  // 每条命令之后收一下回应，输出顺序才好对
    }

    Pump(500);  // 收尾
    SendRaw("QUIT");
    Pump(200);
    dchat::sock::Close(g_sock);
    dchat::sock::Cleanup();
    return 0;
}
