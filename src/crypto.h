// dchat 传输加密：ECDH P-256 握手 + AES-256-GCM 会话加密
//
// ## 设计原则
//
// **不发明任何密码学原语**。ECDH、SHA-256、HMAC、AES-GCM 全部调用系统自带的
// 实现（Windows 上是 CNG / bcrypt.dll，安卓上是 JCE）。这个文件里自己写的只有
// HKDF 的拼接流程（HKDF 本身就是"拿 HMAC 拼出来"的构造，见 RFC 5869）。
//
// ## 握手流程（明文完成，之后全部加密）
//
//     C -> S   HELLO <版本> <客户端公钥B64> <客户端随机数B64>
//     S -> C   HELLO_OK <服务器公钥B64> <服务器随机数B64>
//     ---- 双方各自算 ----
//     shared = ECDH(自己的私钥, 对方的公钥)                    32 字节
//     c2s    = HKDF(shared, salt=客户端随机数||服务器随机数, info="dchat-v1-c2s")
//     s2c    = HKDF(shared, salt=..., info="dchat-v1-s2c")
//
// 之后每一行都包成 `ENC <base64(nonce12 || 密文 || tag16)>`。
//
// ## 为什么密钥要分方向
//
// GCM 的 nonce **在同一把密钥下绝不能重复**，否则明文可被恢复、认证也会失效。
// 如果两个方向共用一把密钥、各自的计数器都从 0 开始，第 0 条消息就撞了。
// 所以按方向派生两把独立密钥，各自从 0 计数就是安全的。
//
// ## 这个方案保护什么、不保护什么
//
// 保护：**被动窃听**（公网上抓包看不到密码和聊天内容）、**篡改**（GCM 自带认证）。
// 不保护：**主动中间人**。裸 ECDH 没有身份认证，攻击者若能劫持连接，
// 可以分别和两边完成握手。要挡住它需要证书或预共享密钥。
// 客户端侧用 **TOFU**（首次连接记住服务器公钥指纹，之后变了就警告）来缓解，
// 这是 SSH 的做法，比什么都没有强得多，但仍不是完整的 PKI。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dchat {

// ---- 尺寸常量 ----
/** P-256 公钥的裸表示：X || Y，各 32 字节大端。 */
inline constexpr std::size_t kP256PublicKeyBytes = 64;
/** AES-256 密钥长度。 */
inline constexpr std::size_t kAesKeyBytes = 32;
/** GCM 的 nonce 长度（标准就是 12 字节）。 */
inline constexpr std::size_t kGcmNonceBytes = 12;
/** GCM 认证标签长度。 */
inline constexpr std::size_t kGcmTagBytes = 16;
/** 握手随机数长度。 */
inline constexpr std::size_t kHandshakeNonceBytes = 16;

/** 握手协议版本。将来改流程就加这个数，双方对不上就退回明文。 */
inline constexpr int kCryptoVersion = 1;

/** HKDF 的 info 字符串（分方向）。 */
inline constexpr const char* kHkdfInfoClientToServer = "dchat-v1-c2s";
inline constexpr const char* kHkdfInfoServerToClient = "dchat-v1-s2c";

/** ECDH 密钥对。[privateBlob] 是平台内部表示，不透明、不上线。 */
struct EcdhKeyPair {
    std::vector<unsigned char> privateBlob;
    /** 64 字节 X||Y，可以直接上线。 */
    std::vector<unsigned char> publicKey;
};

/** 一对会话密钥（按方向分开，避免 GCM nonce 重用）。 */
struct SessionKeys {
    std::vector<unsigned char> clientToServer;
    std::vector<unsigned char> serverToClient;

    bool valid() const {
        return clientToServer.size() == kAesKeyBytes && serverToClient.size() == kAesKeyBytes;
    }
};

/** 生成随机字节（用系统的密码学随机源）。 */
bool RandomBytes(std::size_t count, std::vector<unsigned char>* out);

/** 生成 P-256 密钥对。 */
bool GenerateEcdhKeyPair(EcdhKeyPair* out);

/**
 * 用自己的私钥和对方的公钥（64 字节 X||Y）算出共享密钥。
 *
 * 公钥长度不对、或者不是曲线上的合法点，都返回 false。
 */
bool ComputeSharedSecret(const EcdhKeyPair& mine, const std::vector<unsigned char>& peerPublicKey,
                         std::vector<unsigned char>* out);

/**
 * HKDF-SHA256（RFC 5869）：extract + expand。
 *
 * 自己拼是因为要用到 HMAC-SHA256 这个原语，而它由系统提供；
 * HKDF 本身只是"两次 HMAC 的构造"，不涉及任何密码学设计。
 */
bool HkdfSha256(const std::vector<unsigned char>& ikm, const std::vector<unsigned char>& salt,
                const std::string& info, std::size_t outBytes,
                std::vector<unsigned char>* out);

/**
 * 从共享密钥和双方的握手随机数派生出**两把**会话密钥。
 *
 * salt 用"客户端随机数 || 服务器随机数"，这样两边的随机数都参与进来，
 * 任何一方都能确认对方是新的（防止重放旧的握手）。
 */
SessionKeys DeriveSessionKeys(const std::vector<unsigned char>& sharedSecret,
                              const std::vector<unsigned char>& clientNonce,
                              const std::vector<unsigned char>& serverNonce);

/** AES-256-GCM 加密。输出是 `密文 || 16 字节标签`。 */
bool AesGcmEncrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& plaintext, std::string* out);

/**
 * AES-256-GCM 解密。
 *
 * **认证失败（数据被篡改 / 密钥不对 / nonce 不对）返回 false**，
 * 调用方必须当成硬错误处理——绝不能把半截明文当成功。
 */
bool AesGcmDecrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& ciphertext, std::string* out);

/**
 * 一条连接上的加解密会话。
 *
 * nonce 用**计数器**而不是随机数：同一把密钥下计数器保证不重复，
 * 而随机数只是"重复概率很低"。配合上面按方向分密钥，就没有重用风险。
 */
class CryptoSession {
public:
    /** 两个方向各自的密钥。顺序无所谓，调用方按自己的方向传即可。 */
    bool Start(const std::vector<unsigned char>& sendKey,
               const std::vector<unsigned char>& recvKey);

    bool active() const { return active_; }

    /** 加密一行（只加密内容，`ENC ` 前缀由调用方加）。 */
    bool Encrypt(const std::string& plaintext, std::string* out);

    /** 解密一行。返回 false 表示认证失败，调用方应当断开连接。 */
    bool Decrypt(const std::string& ciphertext, std::string* out);

    void Reset();

    /** 已经发了/收了多少条（调试用）。 */
    std::uint64_t sent() const { return sendCounter_; }
    std::uint64_t received() const { return recvCounter_; }

private:
    static std::vector<unsigned char> NonceFor(std::uint64_t counter);

    std::vector<unsigned char> sendKey_;
    std::vector<unsigned char> recvKey_;
    std::uint64_t sendCounter_ = 0;
    std::uint64_t recvCounter_ = 0;
    bool active_ = false;
};

/**
 * 公钥指纹：SHA-256 的前 16 字节的十六进制，每两字节用冒号隔开。
 *
 * 给 TOFU 用：客户端第一次连服务器时记下它，以后变了就警告用户。
 */
std::string PublicKeyFingerprint(const std::vector<unsigned char>& publicKey);

}  // namespace dchat
