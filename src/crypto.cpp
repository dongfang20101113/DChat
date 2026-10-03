// dchat 传输加密的实现。
//
// 这里只做"把密码学原语拼起来"这件事（握手组装、HKDF、会话计数器），
// 原语本身在 crypto_backend.h 后面：Windows 走 CNG，Linux 走 OpenSSL。
//
// 三端（Windows / Linux / 安卓）必须字节级一致，改动这里任何一步之前先想清楚
// 对端会变成什么——不一致不会报错，只表现为"解出来是乱码"。
#include "crypto.h"

#include "crypto_backend.h"

#include <algorithm>
#include <cstdio>

namespace dchat {

// ---------------------------------------------------------------------------
// 随机数 / ECDH（原语在后端，这里只做类型转换）
// ---------------------------------------------------------------------------

bool RandomBytes(std::size_t count, std::vector<unsigned char>* out) {
    return BackendRandomBytes(count, out);
}

bool GenerateEcdhKeyPair(EcdhKeyPair* out) {
    if (!out) return false;
    BackendEcdhKeyPair pair;
    if (!BackendGenerateEcdhKeyPair(&pair)) return false;
    out->privateBlob = pair.privateBlob;
    out->publicKey = pair.publicKey;
    return true;
}

bool ImportEcdhKeyPair(const std::vector<unsigned char>& privateScalar,
                       const std::vector<unsigned char>& publicKey, EcdhKeyPair* out) {
    if (!out) return false;
    BackendEcdhKeyPair pair;
    if (!BackendImportEcdhKeyPair(privateScalar, publicKey, &pair)) return false;
    out->privateBlob = pair.privateBlob;
    out->publicKey = pair.publicKey;
    return true;
}

bool ComputeSharedSecret(const EcdhKeyPair& mine, const std::vector<unsigned char>& peerPublicKey,
                         std::vector<unsigned char>* out) {
    BackendEcdhKeyPair pair;
    pair.privateBlob = mine.privateBlob;
    pair.publicKey = mine.publicKey;
    // 字节序由后端统一成大端（CNG 返回小端要翻转、OpenSSL 本来就是大端）。
    // 这里不翻转，否则 Linux 端会翻成小端、和另外两端不一致。
    return BackendEcdhSharedSecret(pair, peerPublicKey, out);
}

/**
 * 取出密钥对里的裸私有标量（32 字节）。服务器落盘身份密钥要用，
 * 落盘格式（标量 + 公钥的十六进制）必须两端一致，所以偏移量由后端负责。
 */
bool EcdhPrivateScalar(const EcdhKeyPair& pair, std::vector<unsigned char>* out) {
    BackendEcdhKeyPair backend;
    backend.privateBlob = pair.privateBlob;
    backend.publicKey = pair.publicKey;
    return BackendEcdhPrivateScalar(backend, out);
}

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

bool AesGcmEncrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& plaintext, std::string* out) {
    if (!out) return false;
    out->clear();
    std::vector<unsigned char> input(plaintext.begin(), plaintext.end());
    std::vector<unsigned char> produced;
    if (!BackendAesGcmEncrypt(key, nonce, input, &produced)) return false;
    out->assign(reinterpret_cast<const char*>(produced.data()), produced.size());
    return true;
}

bool AesGcmDecrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& ciphertext, std::string* out) {
    if (!out) return false;
    out->clear();
    std::vector<unsigned char> input(ciphertext.begin(), ciphertext.end());
    std::vector<unsigned char> produced;
    // 认证失败（被篡改 / 密钥不对）在这里就是 false。绝不能把半截明文当成功返回。
    if (!BackendAesGcmDecrypt(key, nonce, input, &produced)) return false;
    out->assign(reinterpret_cast<const char*>(produced.data()), produced.size());
    return true;
}

// ---------------------------------------------------------------------------
// HKDF-SHA256（RFC 5869）
// ---------------------------------------------------------------------------

bool HkdfSha256(const std::vector<unsigned char>& ikm, const std::vector<unsigned char>& salt,
                const std::string& info, std::size_t outBytes,
                std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (outBytes == 0 || outBytes > 255 * 32) return false;  // RFC 5869 的上限

    // extract：PRK = HMAC(salt, IKM)
    // salt 为空时按 RFC 用 32 个零字节
    const std::vector<unsigned char> effectiveSalt =
        salt.empty() ? std::vector<unsigned char>(32, 0) : salt;
    std::vector<unsigned char> prk;
    if (!BackendHmacSha256(effectiveSalt, ikm.data(), ikm.size(), &prk)) return false;

    // expand：T(i) = HMAC(PRK, T(i-1) || info || i)
    std::vector<unsigned char> previous;  // T(0) 是空
    unsigned char counter = 1;
    while (out->size() < outBytes) {
        std::vector<unsigned char> block;
        block.reserve(previous.size() + info.size() + 1);
        block.insert(block.end(), previous.begin(), previous.end());
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter);
        if (!BackendHmacSha256(prk, block.data(), block.size(), &previous)) {
            out->clear();
            return false;
        }
        const std::size_t take = std::min(previous.size(), outBytes - out->size());
        out->insert(out->end(), previous.begin(), previous.begin() + take);
        ++counter;
    }
    return true;
}

SessionKeys DeriveSessionKeys(const std::vector<unsigned char>& sharedSecret,
                              const std::vector<unsigned char>& clientNonce,
                              const std::vector<unsigned char>& serverNonce) {
    SessionKeys keys;
    // salt 把两边的随机数都揉进来：任何一方的随机数变了，派生出的密钥就完全不同，
    // 这样"重放一个旧握手"是没用的。
    std::vector<unsigned char> salt;
    salt.reserve(clientNonce.size() + serverNonce.size());
    salt.insert(salt.end(), clientNonce.begin(), clientNonce.end());
    salt.insert(salt.end(), serverNonce.begin(), serverNonce.end());

    // 两个方向各派生一把：客户端→服务端、服务端→客户端。
    // 用不同的 info 分离，避免两个方向共用一把密钥（否则可以反射攻击）。
    if (!HkdfSha256(sharedSecret, salt, kHkdfInfoClientToServer, kAesKeyBytes, &keys.clientToServer)) {
        return SessionKeys{};
    }
    if (!HkdfSha256(sharedSecret, salt, kHkdfInfoServerToClient, kAesKeyBytes, &keys.serverToClient)) {
        return SessionKeys{};
    }
    return keys;
}

// ---------------------------------------------------------------------------
// CryptoSession
// ---------------------------------------------------------------------------

std::vector<unsigned char> CryptoSession::NonceFor(std::uint64_t counter) {
    // 12 字节 = 4 字节前缀 0 + 8 字节大端计数器。
    // 同一把密钥下计数器只会加不会减，所以 nonce 一定不重复。
    std::vector<unsigned char> nonce(kGcmNonceBytes, 0);
    for (int i = 0; i < 8; ++i) {
        nonce[kGcmNonceBytes - 1 - static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((counter >> (8 * i)) & 0xFF);
    }
    return nonce;
}

bool CryptoSession::Start(const std::vector<unsigned char>& sendKey,
                          const std::vector<unsigned char>& recvKey) {
    if (sendKey.size() != kAesKeyBytes || recvKey.size() != kAesKeyBytes) return false;
    sendKey_ = sendKey;
    recvKey_ = recvKey;
    sendCounter_ = 0;
    recvCounter_ = 0;
    active_ = true;
    return true;
}

bool CryptoSession::Encrypt(const std::string& plaintext, std::string* out) {
    if (!active_) return false;
    const std::vector<unsigned char> nonce = NonceFor(sendCounter_);
    if (!AesGcmEncrypt(sendKey_, nonce, plaintext, out)) return false;
    ++sendCounter_;
    return true;
}

bool CryptoSession::Decrypt(const std::string& ciphertext, std::string* out) {
    if (!active_) return false;
    const std::vector<unsigned char> nonce = NonceFor(recvCounter_);
    if (!AesGcmDecrypt(recvKey_, nonce, ciphertext, out)) return false;
    ++recvCounter_;
    return true;
}

void CryptoSession::Reset() {
    sendKey_.clear();
    recvKey_.clear();
    sendCounter_ = 0;
    recvCounter_ = 0;
    active_ = false;
}

// ---------------------------------------------------------------------------
// 指纹
// ---------------------------------------------------------------------------

std::string PublicKeyFingerprint(const std::vector<unsigned char>& publicKey) {
    std::vector<unsigned char> digest;
    if (!BackendSha256(publicKey.data(), publicKey.size(), &digest)) return std::string();

    // 取前 16 字节，写成 AA:BB:CC:... 的形式，方便人眼比对
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    const std::size_t take = std::min<std::size_t>(16, digest.size());
    for (std::size_t i = 0; i < take; ++i) {
        if (i > 0) out.push_back(':');
        out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace dchat
