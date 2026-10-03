// Linux 客户端的网络连接与加密封装。
//
// 和 Windows 客户端共用同一套协议层（dchat_protocol），差异只在 socket 和终端：
//   - socket 走 POSIX（socket_util.h）
//   - 握手流程、ENC 行格式、TOFU 指纹比对全部复用协议层的实现
//
// ⚠️ 一个容易踩的点：会话加密的 nonce 是**双方各自维护的计数器**，不随包发送。
// 发一条计数器 +1，收一条 +1。所以**一行都不能丢**——丢一行之后两边计数器错位，
// 后面每一条都解不开。协议层之所以敢这么设计，是因为 TCP 是可靠有序的。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

#include "crypto.h"
#include "socket_util.h"

namespace dchat {

class ClientConnection {
public:
    ClientConnection() = default;
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    /** 连服务器（阻塞，带超时）。成功后自己完成加密握手（失败则退回明文）。 */
    bool Connect(const std::string& host, int port, std::string* error);

    /** 连上之后是否启用了加密。 */
    bool Encrypted() const { return crypto_.active(); }

    /** 服务器公钥（用于 TOFU 指纹比对）；明文连接时为空。 */
    const std::vector<unsigned char>& ServerPublicKey() const { return serverPublicKey_; }

    /** 发一行（自动加密 + 补换行）。 */
    bool SendLine(const std::string& line);

    /**
     * 后台接收线程：每收到完整一行就回调一次（已解密）。
     * 回调在接收线程里执行，别在里面做耗时的界面操作。
     */
    void StartReceiveLoop(std::function<void(const std::string&)> onLine);

    bool Running() const { return running_.load(); }

    /** 主动断开（接收线程会随之退出）。 */
    void Close();

    /** 接收线程是否因为对端关闭/出错而结束。 */
    bool PeerClosed() const { return peerClosed_.load(); }

private:
    bool DoHandshake(std::string* error);
    bool ReadLineWithTimeout(std::string* out, int timeoutMs);

    sock::Handle socket_ = sock::kInvalid;
    std::mutex sendMutex_;  // SendLine 可能被界面线程和接收线程同时调用
    std::atomic<bool> running_{false};
    std::atomic<bool> peerClosed_{false};
    CryptoSession crypto_;
    std::vector<unsigned char> serverPublicKey_;
    std::string readBuffer_;  // 接收线程自己的缓冲，不用加锁
    // 握手期间读到的、不属于握手的行（例如连上就来的 WELCOME）。
    // 攒着，等 StartReceiveLoop 起来按原顺序补出去，不能丢也不能乱序。
    std::vector<std::string> leftover_;
};

}  // namespace dchat
