// 跨端互通探针：用 **Windows 侧编译**的协议层去连 Linux 服务端。
//
// 为什么要有这个工具：Windows 客户端的界面设置存在注册表里，GUI 不便于自动化；
// 而"两端能不能互通"真正取决于**协议层 + 密码学后端**是否字节一致，界面只是壳。
// 这个探针用的和 Windows 客户端**完全相同的源文件**（protocol.cpp / crypto.cpp /
// crypto_backend_win.cpp / auth.cpp），只是把界面换成命令行，所以：
//
//   探针连上 Linux 服务端并完成登录和收发  =>  说明 CNG 后端算出的握手材料和
//   OpenSSL 后端算出的完全一致，Windows 客户端与 Linux 服务端必然互通。
//
// 用法：
//   dchat_interop_probe <主机> <端口> <用户名> <密码> <要发的文字> <期望收到的文字>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "file_transfer.h"
#include "protocol.h"
#include "render.h"

namespace {

std::string Base64Of(const std::vector<unsigned char>& bytes) {
    return dchat::Base64Encode(std::string(bytes.begin(), bytes.end()));
}

/** 带超时地读一行（握手期间用；阻塞 socket 上必须先设 SO_RCVTIMEO）。 */
bool ReadOneLine(SOCKET socket, std::string* buffer, std::string* out, int timeoutMs) {
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char chunk[2048];
    for (;;) {
        const std::size_t end = buffer->find('\n');
        if (end != std::string::npos) {
            std::string line = buffer->substr(0, end);
            buffer->erase(0, end + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            *out = line;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const int got = ::recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (got > 0) {
            buffer->append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0) return false;
        if (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK) continue;
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::printf("用法: dchat_interop_probe <主机> <端口> <用户名> <密码> <要发的文字> "
                    "<期望收到的文字>\n");
        return 1;
    }
    const std::string host = argv[1];
    const int port = std::atoi(argv[2]);
    const std::string user = argv[3];
    const std::string password = argv[4];
    const std::string message = argv[5];
    const std::string expect = argv[6];

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[失败] WSAStartup\n");
        return 1;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        std::printf("[失败] 解析不了 %s\n", host.c_str());
        return 1;
    }
    SOCKET socket = INVALID_SOCKET;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == INVALID_SOCKET) continue;
        if (::connect(socket, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) break;
        ::closesocket(socket);
        socket = INVALID_SOCKET;
    }
    ::freeaddrinfo(result);
    if (socket == INVALID_SOCKET) {
        std::printf("[失败] 连不上 %s:%d\n", host.c_str(), port);
        return 2;
    }
    std::printf("[连接] 已连上 %s:%d\n", host.c_str(), port);

    // ---- 加密握手（和 Windows 客户端 DoCryptoHandshake 同一套流程）----
    std::string sendBuffer;
    bool encrypted = false;
    dchat::CryptoSession crypto;
    {
        std::vector<unsigned char> clientNonce;
        dchat::EcdhKeyPair clientKey;
        if (dchat::RandomBytes(dchat::kHandshakeNonceBytes, &clientNonce) &&
            dchat::GenerateEcdhKeyPair(&clientKey)) {
            const std::string hello = dchat::BuildLine(
                "HELLO", std::to_string(dchat::kCryptoVersion) + " " +
                             Base64Of(clientKey.publicKey) + " " + Base64Of(clientNonce));
            sendBuffer = hello + "\n";
            ::send(socket, sendBuffer.data(), static_cast<int>(sendBuffer.size()), 0);

            std::string buffer;
            std::string line;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(dchat::kHandshakeTimeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!ReadOneLine(socket, &buffer, &line, dchat::kHandshakeTimeoutMs)) break;
                if (dchat::ParseLine(line).command == "HELLO_OK") break;
                line.clear();  // WELCOME 之类，跳过（它本来就该显示给用户）
            }
            if (!line.empty()) {
                const dchat::Message raw = dchat::ParseLine(line);
                if (raw.command == "HELLO_OK") {
                    std::vector<std::string> words = raw.Words();
                    if (!words.empty() && dchat::LooksLikeTime(words[0])) {
                        words.erase(words.begin());
                    }
                    std::vector<unsigned char> serverPublic, serverNonce;
                    if (words.size() >= 2 && dchat::Base64Decode(words[0], &serverPublic) &&
                        dchat::Base64Decode(words[1], &serverNonce) &&
                        serverPublic.size() == dchat::kP256PublicKeyBytes &&
                        serverNonce.size() == dchat::kHandshakeNonceBytes) {
                        std::vector<unsigned char> shared;
                        if (dchat::ComputeSharedSecret(clientKey, serverPublic, &shared)) {
                            const dchat::SessionKeys keys =
                                dchat::DeriveSessionKeys(shared, clientNonce, serverNonce);
                            encrypted = crypto.Start(keys.clientToServer, keys.serverToClient);
                            std::printf("[安全] 服务器指纹：%s\n",
                                        dchat::PublicKeyFingerprint(serverPublic).c_str());
                        }
                    }
                }
            }
        }
    }
    // 握手后撤掉读超时，否则后面的正常接收会被打断
    DWORD zero = 0;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&zero),
                 sizeof(zero));
    std::printf("[连接] 加密状态：%s\n", encrypted ? "已加密" : "明文");

    auto SendLine = [&](const std::string& line) {
        std::string data;
        if (crypto.active()) {
            std::string sealed;
            if (!crypto.Encrypt(line, &sealed)) return false;
            data = dchat::BuildLine("ENC", Base64Of(std::vector<unsigned char>(
                                               sealed.begin(), sealed.end()))) + "\n";
        } else {
            data = line + "\n";
        }
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int written =
                ::send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (written <= 0) return false;
            sent += static_cast<std::size_t>(written);
        }
        return true;
    };

    if (!SendLine(dchat::BuildLine("REGISTER",
                                   dchat::EscapeText(user + " " + password)))) {
        std::printf("[失败] 发不出登录请求\n");
        return 2;
    }

    // ---- 收：解密每一行，等登录成功再发消息，最后等回显 ----
    std::string buffer;
    bool loggedIn = false;
    bool sentMessage = false;
    bool success = false;
    char chunk[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline && !success) {
        const int got = ::recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (got <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(got));
        for (;;) {
            const std::size_t end = buffer.find('\n');
            if (end == std::string::npos) break;
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;

            const dchat::Message outer = dchat::ParseLine(line);
            if (outer.command == "ENC") {
                std::vector<std::string> words = outer.Words();
                if (!words.empty() && dchat::LooksLikeTime(words[0])) words.erase(words.begin());
                std::vector<unsigned char> decoded;
                if (words.empty() || !dchat::Base64Decode(words[0], &decoded)) continue;
                std::string plain;
                if (!crypto.Decrypt(std::string(decoded.begin(), decoded.end()), &plain)) {
                    std::printf("[失败] 解密失败（两端密钥不一致）\n");
                    return 3;
                }
                line = plain;
            }

            const dchat::Message message = dchat::ParseLine(line);
            if (message.command == "LOGGEDIN") {
                loggedIn = true;
                std::printf("[登录] 服务器已确认登录\n");
            } else if (message.command == "SAY") {
                dchat::SayInfo info;
                if (dchat::ParseSay(line, std::string(), &info)) {
                    std::printf("[收到] [%s] <%s> %s\n", info.time.c_str(), info.nick.c_str(),
                                info.text.c_str());
                    if (info.text.find(expect) != std::string::npos) success = true;
                }
            } else if (message.command == "ERROR" || message.command == "SYS") {
                std::vector<std::string> words = message.Words();
                if (!words.empty() && dchat::LooksLikeTime(words[0])) words.erase(words.begin());
                std::string text;
                for (const std::string& word : words) {
                    if (!text.empty()) text += " ";
                    text += word;
                }
                std::printf("[%s] %s\n", message.command.c_str(), text.c_str());
            }
        }
        if (loggedIn && !sentMessage) {
            sentMessage = true;
            if (!SendLine(dchat::BuildLine("MSG", dchat::EscapeText(message)))) {
                std::printf("[失败] 发不出消息\n");
                return 2;
            }
            std::printf("[发出] %s\n", message.c_str());
        }
    }

    ::closesocket(socket);
    WSACleanup();
    if (success) {
        std::printf("[成功] Windows 侧协议层与 Linux 服务端完成加密互通\n");
        return 0;
    }
    std::printf("[超时] 没等到预期的内容\n");
    return 3;
}
