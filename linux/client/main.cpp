// dchat Linux 客户端。
//
// 两种用法：
//   1. 批处理模式（自动化 / 脚本 / 真机验证用）：
//        dchat-client --host 127.0.0.1 --port 5555 --user alice --pass secret \
//                     --send "你好" --expect-say "你好" --timeout 5
//      连上 → 握手 → 登录 → 发一条 → 等到收到包含指定文字的行 → 退出并打印结果。
//      **这一模式是给"跨端互通"验证用的**：Windows 服务端 + Linux 客户端能跑通，
//      就说明协议、加密、账号三条线两端一致。
//   2. 交互模式（不带 --send 时）：连上登录后进终端聊天界面（见 ui.cpp）。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "file_transfer.h"
#include "net.h"
#include "protocol.h"
#include "files.h"
#include "terminal.h"
#include "ui.h"
#include "render.h"  // SayInfo / ParseSay：把 SAY 行拆成昵称与正文
#include "trust.h"

namespace {

const char* const kKnownServersPath = "dchat-known-servers.txt";

struct Options {
    std::string host = "127.0.0.1";
    int port = 5555;
    std::string user;
    std::string password;
    bool wantRegister = false;  // --register：没有账号就注册
    std::string send;
    std::string expectSay;  // 收到包含这段文字的 SAY 就算成功
    std::string expectAny;  // 收到任何一行包含这段文字就算成功
    int timeoutSec = 5;
    bool insecureOk = false;  // 指纹变了也继续（只在测试里用）
};

void PrintUsage() {
    std::printf(
        "用法: dchat-client [选项]\n"
        "  --host <地址>        服务器地址（默认 127.0.0.1）\n"
        "  --port <端口>        服务器端口（默认 5555）\n"
        "  --user <用户名>      登录用户名\n"
        "  --pass <密码>        密码\n"
        "  --register           没账号就注册（注册成功即登录）\n"
        "  --send <文字>        登录后发一条消息\n"
        "  --expect-say <文字>  收到包含这段文字的 SAY 就算成功（批处理模式）\n"
        "  --expect-any <文字>  收到任何包含这段文字的行就算成功\n"
        "  --timeout <秒>       等回复的超时（默认 5）\n"
        "  --insecure-ok        指纹变了也继续（只给测试用）\n");
}

bool ParseArgs(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (arg == "--host") {
            if (!next(&options->host)) return false;
        } else if (arg == "--port") {
            std::string text;
            if (!next(&text)) return false;
            options->port = std::atoi(text.c_str());
        } else if (arg == "--user") {
            if (!next(&options->user)) return false;
        } else if (arg == "--pass") {
            if (!next(&options->password)) return false;
        } else if (arg == "--send") {
            if (!next(&options->send)) return false;
        } else if (arg == "--expect-say") {
            if (!next(&options->expectSay)) return false;
        } else if (arg == "--expect-any") {
            if (!next(&options->expectAny)) return false;
        } else if (arg == "--timeout") {
            std::string text;
            if (!next(&text)) return false;
            options->timeoutSec = std::atoi(text.c_str());
        } else if (arg == "--register") {
            options->wantRegister = true;
        } else if (arg == "--insecure-ok") {
            options->insecureOk = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            std::printf("未知参数：%s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::string ReadFileOrEmpty(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

/** 命令行下的 TOFU 检查：一致就过，变了就警告并中止（除非 --insecure-ok）。 */
bool CheckTrust(const Options& options, const dchat::ClientConnection& connection) {
    if (!connection.Encrypted()) {
        std::printf("[安全] 这次连接**没有加密**（服务器不支持或握手失败）\n");
        return true;
    }
    const std::string fingerprint = dchat::PublicKeyFingerprint(connection.ServerPublicKey());
    const std::string key = dchat::KnownServerKey(options.host, options.port);
    const std::string content = ReadFileOrEmpty(kKnownServersPath);
    const dchat::TrustDecision decision =
        dchat::DecideTrust(dchat::LookupKnownFingerprint(content, key), fingerprint);
    std::printf("[安全] %s\n", dchat::DescribeTrust(decision).c_str());
    if (decision.kind == dchat::TrustKind::Changed && !options.insecureOk) {
        std::printf("[安全] 指纹变了，已中止（确认无误后加 --insecure-ok 可继续）\n");
        return false;
    }
    if (decision.kind == dchat::TrustKind::FirstUse ||
        decision.kind == dchat::TrustKind::Changed) {
        WriteFile(kKnownServersPath,
                  dchat::UpsertKnownFingerprint(content, key, fingerprint));
    }
    return true;
}

/** 把服务器发来的一行渲染成终端文字。返回 false 表示这行不用显示。 */
bool RenderLine(const std::string& line, std::string* out) {
    const dchat::Message message = dchat::ParseLine(line);
    if (message.command == "ENC" || message.command == "PONG" ||
        message.command == "RULES" || message.command == "KNOWN") {
        return false;  // 控制行，不给人看
    }
    if (!message.command.compare(0, 5, "FILE_")) return false;

    if (message.command == "SAY") {
        dchat::SayInfo info;
        if (dchat::ParseSay(line, std::string(), &info)) {
            *out = "[" + info.time + "] <" + info.nick + "> " + info.text;
            return true;
        }
    }
    std::vector<std::string> words = message.Words();
    if (!words.empty() && dchat::LooksLikeTime(words[0])) words.erase(words.begin());
    std::string rest;
    for (const std::string& word : words) {
        if (!rest.empty()) rest += " ";
        rest += word;
    }
    *out = "[" + message.command + "] " + rest;
    return true;
}

/** 接上之后的公共流程：登录（可选注册）+ 收发。 */
int RunSession(const Options& options, dchat::ClientConnection* connection) {
    if (!options.user.empty()) {
        // 登录；如果要求注册就先注册（注册成功服务器会直接当成登录）
        const std::string command = options.wantRegister ? "REGISTER" : "LOGIN";
        if (!connection->SendLine(dchat::BuildLine(
                command, dchat::EscapeText(options.user + " " + options.password)))) {
            std::printf("[失败] 发送登录请求失败\n");
            return 2;
        }
    }

    bool loggedIn = false;
    bool gotSay = false;
    bool sentMessage = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(options.timeoutSec);
    std::string pending;
    std::mutex pendingMutex;
    bool done = false;

    connection->StartReceiveLoop([&](const std::string& line) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        if (done) return;
        std::string rendered;
        const bool show = RenderLine(line, &rendered);
        if (show) std::printf("%s\n", rendered.c_str());
        std::fflush(stdout);

        const dchat::Message message = dchat::ParseLine(line);
        if (message.command == "LOGGEDIN") loggedIn = true;
        if (message.command == "ERROR") {
            // 登录失败就说清楚，别让人以为是网络问题
            std::vector<std::string> words = message.Words();
            if (!words.empty() && dchat::LooksLikeTime(words[0])) words.erase(words.begin());
            std::string reason;
            for (const std::string& word : words) {
                if (!reason.empty()) reason += " ";
                reason += word;
            }
            std::printf("[失败] 服务器拒绝了：%s\n", reason.c_str());
            done = true;
            return;
        }
        if (!options.expectSay.empty() && message.command == "SAY") {
            if (rendered.find(options.expectSay) != std::string::npos) {
                gotSay = true;
                done = true;
            }
        }
        if (!options.expectAny.empty() && rendered.find(options.expectAny) != std::string::npos) {
            gotSay = true;
            done = true;
        }
    });

    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (done) break;
        }
        // 登录成功后发一次消息。**发送放在主循环里**，不在接收回调里做：
        // 回调跑在接收线程上，在里面发消息容易自己等自己。
        if (loggedIn && !sentMessage && !options.send.empty()) {
            sentMessage = true;
            if (!connection->SendLine(dchat::BuildLine("MSG", dchat::EscapeText(options.send)))) {
                std::printf("[失败] 发送失败（连接可能已断开）\n");
                return 2;
            }
            std::printf("[发出] %s\n", options.send.c_str());
            std::fflush(stdout);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const bool waitingForReply = !options.expectSay.empty() || !options.expectAny.empty();
    if (waitingForReply) {
        std::printf(gotSay ? "[成功] 收到了预期的内容\n" : "[超时] 没等到预期的内容\n");
        return gotSay ? 0 : 3;
    }
    if (!options.user.empty() && !loggedIn) {
        std::printf("[超时] 没能完成登录\n");
        return 3;
    }
    std::printf("[完成]\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage();
        return 1;
    }
    if (!dchat::sock::Startup()) {
        std::printf("socket 初始化失败\n");
        return 1;
    }

    dchat::ClientConnection connection;
    std::string error;
    std::printf("[连接] %s:%d\n", options.host.c_str(), options.port);
    if (!connection.Connect(options.host, options.port, &error)) {
        std::printf("[失败] %s\n", error.c_str());
        return 2;
    }
    std::printf("[连接] 已连上（%s）\n", connection.Encrypted() ? "已加密" : "明文");
    if (!CheckTrust(options, connection)) return 4;

    // 没给 --send 就进交互模式；给了就是批处理（脚本 / 自动化验证用）
    if (options.send.empty() && options.user.empty()) {
        std::printf("[提示] 没有指定 --user，将直接进交互模式（如果服务器要求登录，"
                    "请重新带上 --user/--pass）\n");
    }
    if (options.send.empty()) {
        dchat::Terminal terminal;
        dchat::FileTransfers files(&connection);
        dchat::ChatUi ui(&connection, &terminal, &files);
        connection.StartReceiveLoop([&ui, &files, &terminal](const std::string& line) {
            // 文件相关的行先给文件模块，它认领了就不到界面去
            if (files.HandleLine(line, [&ui](const std::string& text) {
                    ui.ShowStatus(text);
                })) {
                return;
            }
            ui.HandleServerLine(line);
        });
        // 交互模式下由主循环负责发送登录请求
        if (!options.user.empty()) {
            const std::string command = options.wantRegister ? "REGISTER" : "LOGIN";
            if (!connection.SendLine(dchat::BuildLine(
                    command, dchat::EscapeText(options.user + " " + options.password)))) {
                std::printf("[失败] 发送登录请求失败\n");
                connection.Close();
                dchat::sock::Cleanup();
                return 2;
            }
        }
        const int uiResult = ui.Run();
        connection.Close();
        dchat::sock::Cleanup();
        return uiResult;
    }

    const int result = RunSession(options, &connection);
    connection.Close();
    dchat::sock::Cleanup();
    return result;
}
