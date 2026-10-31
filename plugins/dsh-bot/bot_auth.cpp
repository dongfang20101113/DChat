#include "bot_auth.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "crypto.h"
#include "file_transfer.h"
#include "protocol.h"
#include "socket_util.h"

namespace dchat {
namespace dshbot {
namespace {

constexpr int kHandshakeNonceBytes = 32;

/** 连到服务器（带超时）。 */
sock::Handle ConnectTo(const std::string& host, int port, std::string* error) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        if (error != nullptr) *error = "解析不了主机：" + host;
        return sock::kInvalid;
    }
    sockaddr_in address{};
    std::memcpy(&address, result->ai_addr, sizeof(address));
    ::freeaddrinfo(result);

    const sock::Handle handle = sock::CreateTcp();
    if (handle == sock::kInvalid) {
        if (error != nullptr) *error = "建 socket 失败";
        return sock::kInvalid;
    }
    sock::SetNoSigpipe(handle);
    sock::SetReceiveTimeout(handle, 3000);
    if (::connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) *error = "连不上 " + host + ":" + portText;
        sock::Close(handle);
        return sock::kInvalid;
    }
    sock::SetNoDelay(handle);
    return handle;
}

/**
 * 带缓冲的按行读。
 *
 * **必须缓冲**：一次 recv 很可能同时拿到好几行（HELLO_OK 和随后的提示常在一个包
 * 里），不缓冲就会把第一行之后的内容全丢掉 —— 表现是"握手成功但永远等不到
 * LOGGEDIN"。
 */
struct LineReader {
    sock::Handle handle = sock::kInvalid;
    std::string buffer;

    bool Next(std::string* out, int timeoutMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            const std::size_t newline = buffer.find('\n');
            if (newline != std::string::npos) {
                *out = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!out->empty() && out->back() == '\r') out->pop_back();
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            char chunk[512];
            const std::ptrdiff_t got = sock::Receive(handle, chunk, sizeof(chunk));
            if (got == 0) return false;
            if (got < 0) {
                if (sock::WouldBlock()) continue;
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }
};

void Send(sock::Handle handle, CryptoSession* crypto, const std::string& line) {
    std::string payload = line;
    if (crypto != nullptr && crypto->active()) {
        std::string sealed;
        if (!crypto->Encrypt(line, &sealed)) return;
        payload = "ENC " + Base64Encode(sealed);
    }
    payload += "\n";
    sock::SendAll(handle, payload.data(), payload.size());
}

/** 解一行（加密时是 `ENC <b64>`）。 */
bool DecodeLine(CryptoSession* crypto, const std::string& raw, std::string* plain) {
    *plain = raw;
    if (crypto == nullptr || !crypto->active()) return true;
    const Message outer = ParseLine(raw);
    if (outer.command != "ENC") return false;
    std::vector<unsigned char> bytes;
    if (!Base64Decode(outer.rest, &bytes)) return false;
    const std::string sealed(bytes.begin(), bytes.end());
    return crypto->Decrypt(sealed, plain);
}

}  // namespace

PasswordCheck VerifyDchatPassword(const std::string& host, int port, const std::string& nick,
                                  const std::string& password, int timeoutMs) {
    PasswordCheck check;
    if (nick.empty() || password.empty()) {
        check.error = "昵称和密码都不能为空";
        return check;
    }
    sock::Startup();
    const sock::Handle handle = ConnectTo(host, port, &check.error);
    if (handle == sock::kInvalid) return check;
    LineReader reader;
    reader.handle = handle;

    CryptoSession crypto;
    // ---- 加密握手（服务器支持就加密，和不加密的服务端也能用）----
    {
        std::vector<unsigned char> clientNonce;
        EcdhKeyPair clientKey;
        if (RandomBytes(kHandshakeNonceBytes, &clientNonce) &&
            GenerateEcdhKeyPair(&clientKey)) {
            Send(handle, nullptr,
                 BuildLine("HELLO", std::to_string(kCryptoVersion) + " " +
                                        Base64Encode(clientKey.publicKey.data(),
                                                     clientKey.publicKey.size()) +
                                        " " + Base64Encode(clientNonce.data(), clientNonce.size())));
            std::string line;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!reader.Next(&line, 500)) continue;
                if (ParseLine(line).command != "HELLO_OK") continue;
                const Message raw = ParseLine(line);
                std::vector<std::string> words = raw.Words();
                if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
                if (words.size() < 2) break;
                std::vector<unsigned char> serverPublic;
                std::vector<unsigned char> serverNonce;
                if (!Base64Decode(words[0], &serverPublic) ||
                    !Base64Decode(words[1], &serverNonce) ||
                    serverPublic.size() != kP256PublicKeyBytes ||
                    serverNonce.size() != kHandshakeNonceBytes) {
                    break;
                }
                std::vector<unsigned char> shared;
                if (!ComputeSharedSecret(clientKey, serverPublic, &shared)) break;
                const SessionKeys keys = DeriveSessionKeys(shared, clientNonce, serverNonce);
                crypto.Start(keys.clientToServer, keys.serverToClient);
                break;
            }
        }
    }

    Send(handle, &crypto, BuildLine("LOGIN", nick + " " + password));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string raw;
        if (!reader.Next(&raw, 300)) continue;
        std::string plain;
        if (!DecodeLine(&crypto, raw, &plain)) continue;
        const Message message = ParseLine(plain);
        if (message.command == "LOGGEDIN") {
            check.ok = true;
            break;
        }
        if (message.command == "ERROR") {
            // 注意顺序：**先看"昵称已被占用"** —— 服务器是先校验密码再查重名，
            // 所以那句话同样证明密码是对的（用户在线时也要能登进管理界面）。
            if (message.rest.find("已被占用") != std::string::npos) {
                check.ok = true;
                check.nickTaken = true;
                break;
            }
            // 其余 ERROR 一律当"登录没通过"，把服务器的原话带给用户。
            // 以前这里只认"密码错误"四个字，别的（比如"密码至少 6 个字符"）
            // 会漏到超时分支，用户看到"服务器没有回应"，完全误导。
            check.error = message.rest;
            // 服务器给用户看的行都带 "hh:mm " 前缀，登录框里不需要它
            if (check.error.size() > 6 && check.error[2] == ':' && check.error[5] == ' ') {
                check.error = check.error.substr(6);
            }
            break;
        }
    }
    if (!check.ok && check.error.empty()) check.error = "服务器没有回应（超时）";
    // 服务器在登录期间就把连接关了（比如连续失败被限流）也要说清楚
    if (!check.ok && check.error.empty()) check.error = "登录没有通过";
    Send(handle, &crypto, BuildLine("QUIT"));
    sock::Close(handle);
    return check;
}

}  // namespace dshbot
}  // namespace dchat
