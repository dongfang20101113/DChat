#include "net.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "file_transfer.h"  // Base64（ENC 行的编解码）
#include "protocol.h"

namespace dchat {
namespace {

/** 带超时的读一行（握手期间用：不能用后台线程，那会打乱计数器的顺序）。 */
bool ReadOneLine(sock::Handle handle, std::string* buffer, std::string* out, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char chunk[1024];
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
        const std::ptrdiff_t got = sock::Receive(handle, chunk, sizeof(chunk));
        if (got > 0) {
            buffer->append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0) return false;  // 对端关了
        if (sock::WouldBlock()) continue;
        return false;
    }
}

std::string Base64Of(const std::vector<unsigned char>& bytes) {
    return Base64Encode(std::string(bytes.begin(), bytes.end()));
}

bool BytesOfBase64(const std::string& text, std::vector<unsigned char>* out) {
    // Base64Decode 的输出是 vector<unsigned char>（不是 string）
    return Base64Decode(text, out);
}

}  // namespace

ClientConnection::~ClientConnection() { Close(); }

bool ClientConnection::Connect(const std::string& host, int port, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        if (error) *error = "解析不了地址：" + host;
        return false;
    }

    sock::Handle handle = sock::kInvalid;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        handle = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (handle == sock::kInvalid) continue;
        if (::connect(handle, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) break;
        sock::Close(handle);
        handle = sock::kInvalid;
    }
    ::freeaddrinfo(result);
    if (handle == sock::kInvalid) {
        if (error) *error = "连不上 " + host + ":" + portText;
        return false;
    }

    socket_ = handle;
    sock::SetNoDelay(socket_);   // 聊天都是小包，禁用 Nagle 才没有人为延迟
    // macOS 上必须设这个：那边没有 MSG_NOSIGNAL，靠 SO_NOSIGPIPE 才不会被
    // 「往已断开的连接写」产生的信号杀掉进程。Linux 上是空操作。
    sock::SetNoSigpipe(socket_);

    // 握手失败**不等于连不上**：老服务器不认识 HELLO 时会把这行当普通聊天内容，
    // 我们等超时之后就当明文连接继续用（Windows 端和安卓端都是这个约定）。
    // 但 socket 真的断了就必须报错——否则后面每条都发不出去，还看不出原因。
    std::string handshakeNote;
    if (!DoHandshake(&handshakeNote) && socket_ == sock::kInvalid) {
        if (error) {
            *error = handshakeNote.empty() ? std::string("连接在握手期间断开") : handshakeNote;
        }
        return false;
    }
    running_ = true;
    return true;
}

bool ClientConnection::DoHandshake(std::string* error) {
    // ⚠️ 握手期必须给 socket 设接收超时。踩过的坑：ReadOneLine 里虽然算了
    // deadline，但阻塞 socket 上的 recv 会一直挂着——超时**永远不会触发**，
    // 客户端会卡在握手读里直到对端关连接。收完再撤掉这个限制，
    // 否则正常的接收线程也会被它打断。
    if (!sock::SetReceiveTimeout(socket_, kHandshakeTimeoutMs)) {
        if (error) *error = "设不了 socket 超时";
        return false;
    }

    std::vector<unsigned char> clientNonce;
    if (!RandomBytes(kHandshakeNonceBytes, &clientNonce)) return false;

    EcdhKeyPair clientKey;
    if (!GenerateEcdhKeyPair(&clientKey)) return false;

    const std::string hello =
        BuildLine("HELLO", std::to_string(kCryptoVersion) + " " + Base64Of(clientKey.publicKey) +
                              " " + Base64Of(clientNonce));
    if (!SendLine(hello)) return false;

    std::string buffer;
    std::string line;
    bool gotHelloOk = false;
    bool gotLine = false;
    // ⚠️ 不能只读一行就下结论：服务器**连上就先发 WELCOME**，第一行往往不是 HELLO_OK。
    // 第一版就是这么错的——读到的 WELCOME 当成"服务器不支持加密"，然后 HELLO_OK
    // 被后面的接收循环吃掉，两边状态彻底错位（服务端日志里能看到
    // "plaintext after handshake, disconnecting"）。
    // 这里循环读到 HELLO_OK 为止，读到的别的行按原样攒起来，握手结束后交给接收循环。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kHandshakeTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - std::chrono::steady_clock::now())
                                              .count());
        if (left <= 0) break;
        if (!ReadOneLine(socket_, &buffer, &line, left)) break;
        gotLine = true;
        if (ParseLine(line).command == "HELLO_OK") {
            gotHelloOk = true;
            break;
        }
        // 不是握手响应（WELCOME / 公告等）：留着，等接收线程起来后照样要显示
        leftover_.push_back(line);
    }
    sock::SetReceiveTimeout(socket_, 0);  // 撤掉：后面的接收线程要一直等
    if (!gotHelloOk) {
        if (error) {
            *error = gotLine ? "服务器不支持加密握手（将走明文）"
                             : "服务器没有回应加密握手（可能是老版本服务器，将走明文）";
        }
        return false;
    }

    const Message raw = ParseLine(line);
    std::vector<std::string> words = raw.Words();
    if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
    if (words.size() < 2) return false;

    std::vector<unsigned char> serverPublic, serverNonce;
    if (!BytesOfBase64(words[0], &serverPublic) || !BytesOfBase64(words[1], &serverNonce)) {
        return false;
    }
    if (serverPublic.size() != kP256PublicKeyBytes ||
        serverNonce.size() != kHandshakeNonceBytes) {
        return false;
    }

    std::vector<unsigned char> shared;
    if (!ComputeSharedSecret(clientKey, serverPublic, &shared)) return false;
    const SessionKeys keys = DeriveSessionKeys(shared, clientNonce, serverNonce);
    if (keys.clientToServer.size() != kAesKeyBytes || keys.serverToClient.size() != kAesKeyBytes) {
        return false;
    }
    // 客户端：发用 c2s、收用 s2c（和服务器正好相反）
    if (!crypto_.Start(keys.clientToServer, keys.serverToClient)) return false;
    serverPublicKey_ = serverPublic;
    return true;
}

bool ClientConnection::ReadLineWithTimeout(std::string* out, int timeoutMs) {
    return ReadOneLine(socket_, &readBuffer_, out, timeoutMs);
}

bool ClientConnection::SendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (socket_ == sock::kInvalid) return false;

    std::string data;
    if (crypto_.active()) {
        std::string sealed;
        if (!crypto_.Encrypt(line, &sealed)) return false;
        data = BuildLine("ENC", Base64Of(std::vector<unsigned char>(sealed.begin(), sealed.end()))) +
               "\n";
    } else {
        data = line + "\n";
    }
    return sock::SendAll(socket_, data.data(), data.size());
}

void ClientConnection::StartReceiveLoop(std::function<void(const std::string&)> onLine) {
    // 握手里读到的非握手行（WELCOME / 公告）先补给它——**顺序不能变**，
    // 而且这些行是明文的（握手期间还没加密）。
    for (const std::string& pending : leftover_) {
        onLine(pending);
    }
    leftover_.clear();

    std::thread([this, onLine] {
        std::string buffer;
        char chunk[4096];
        while (running_.load()) {
            const std::ptrdiff_t got = sock::Receive(socket_, chunk, sizeof(chunk));
            if (got > 0) {
                buffer.append(chunk, static_cast<std::size_t>(got));
                // 一行一行切出来；每行先解密（如果是 ENC）再交给回调。
                // **顺序绝对不能乱**：计数器错位之后所有消息都解不开。
                for (;;) {
                    const std::size_t end = buffer.find('\n');
                    if (end == std::string::npos) break;
                    std::string line = buffer.substr(0, end);
                    buffer.erase(0, end + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                        line.pop_back();
                    }
                    if (line.empty()) continue;

                    const Message outer = ParseLine(line);
                    if (outer.command == "ENC") {
                        std::vector<std::string> words = outer.Words();
                        if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
                        std::vector<unsigned char> decoded;
                        if (words.empty() || !Base64Decode(words[0], &decoded)) continue;
                        const std::string body(decoded.begin(), decoded.end());
                        std::string plain;
                        if (!crypto_.Decrypt(body, &plain)) {
                            // 解不开说明计数器错位或对端有问题，继续读也没意义
                            peerClosed_ = true;
                            running_ = false;
                            return;
                        }
                        onLine(plain);
                        continue;
                    }
                    onLine(line);
                }
                continue;
            }
            if (got == 0) break;  // 对端正常关闭
            if (sock::WouldBlock()) continue;
            break;
        }
        peerClosed_ = true;
        running_ = false;
    }).detach();
}

void ClientConnection::Close() {
    running_ = false;
    if (socket_ != sock::kInvalid) {
        sock::ShutdownWrite(socket_);
        sock::Close(socket_);
        socket_ = sock::kInvalid;
    }
}

}  // namespace dchat
