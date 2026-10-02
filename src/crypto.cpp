// dchat 传输加密的实现（Windows CNG / bcrypt.dll）
//
// 这里只做"把系统提供的密码学原语拼起来"这件事：
//   随机数   BCryptGenRandom
//   ECDH     BCryptOpenAlgorithmProvider(ECDH_P256) + BCryptSecretAgreement
//   SHA-256  BCryptCreateHash
//   HMAC     BCryptCreateHash(HMAC 标志)
//   AES-GCM  BCryptEncrypt/BCryptDecrypt + BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO
#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace dchat {

namespace {

// P-256 的曲线参数：坐标 32 字节
constexpr ULONG kP256CoordinateBytes = 32;
// BCRYPT_ECCKEY_BLOB 的魔数（"ECK1" / "ECK2"，小端读出来是这两个值）
constexpr ULONG kEcdhPublicP256Magic = 0x314B4345;
constexpr ULONG kEcdhPrivateP256Magic = 0x324B4345;

// BCRYPT_KDF_RAW_SECRET：不做任何 KDF，直接要原始的共享密钥。
// 微软文档里的值就是字符串 "TRUNCATE"。MinGW 的 bcrypt.h 没有这个宏，
// 所以自己定义一份（Windows SDK 里定义时也带了这个条件判断）。
#ifndef BCRYPT_KDF_RAW_SECRET
constexpr wchar_t kKdfRawSecret[] = L"TRUNCATE";
#else
constexpr const wchar_t* kKdfRawSecret = BCRYPT_KDF_RAW_SECRET;
#endif

// ---- 极简 RAII：CNG 的句柄都必须显式关，异常路径上很容易漏 ----
struct AlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~AlgHandle() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
    bool Open(LPCWSTR algorithm, ULONG flags = 0) {
        return NT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, algorithm, nullptr, flags));
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~HashHandle() {
        if (handle) BCryptDestroyHash(handle);
    }
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~KeyHandle() {
        if (handle) BCryptDestroyKey(handle);
    }
};

struct SecretHandle {
    BCRYPT_SECRET_HANDLE handle = nullptr;
    ~SecretHandle() {
        if (handle) BCryptDestroySecret(handle);
    }
};

// BCRYPT_ECCKEY_BLOB 的布局：{ ULONG dwMagic; ULONG cbKey; } 后面跟 X、Y
std::vector<unsigned char> MakeEccPublicBlob(const std::vector<unsigned char>& xy) {
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + xy.size(), 0);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = kEcdhPublicP256Magic;
    header->cbKey = kP256CoordinateBytes;
    std::copy(xy.begin(), xy.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    return blob;
}

/** 从导出的 ECCPUBLIC_BLOB 里取出裸的 X||Y。 */
bool ExtractPublicXY(const std::vector<unsigned char>& blob, std::vector<unsigned char>* out) {
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB)) return false;
    const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (header->cbKey != kP256CoordinateBytes) return false;
    const std::size_t need = sizeof(BCRYPT_ECCKEY_BLOB) + kP256CoordinateBytes * 2;
    if (blob.size() < need) return false;
    out->assign(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.begin() + need);
    return true;
}

/** 一个缓冲区大小的 SHA-256。 */
bool Sha256(const unsigned char* data, std::size_t length, std::vector<unsigned char>* out) {
    AlgHandle alg;
    if (!alg.Open(BCRYPT_SHA256_ALGORITHM)) return false;
    DWORD objectBytes = 0, digestBytes = 0, produced = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                                      &produced, 0))) {
        return false;
    }
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_HASH_LENGTH,
                                      reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes),
                                      &produced, 0))) {
        return false;
    }
    std::vector<unsigned char> object(objectBytes);
    HashHandle hash;
    if (!NT_SUCCESS(BCryptCreateHash(alg.handle, &hash.handle, object.data(), objectBytes, nullptr,
                                     0, 0))) {
        return false;
    }
    if (!NT_SUCCESS(BCryptHashData(hash.handle, const_cast<PUCHAR>(data),
                                   static_cast<ULONG>(length), 0))) {
        return false;
    }
    out->assign(digestBytes, 0);
    return NT_SUCCESS(BCryptFinishHash(hash.handle, out->data(), digestBytes, 0));
}

/** HMAC-SHA256（HKDF 的底座）。 */
bool HmacSha256(const std::vector<unsigned char>& key, const unsigned char* data,
                std::size_t length, std::vector<unsigned char>* out) {
    AlgHandle alg;
    if (!alg.Open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    DWORD objectBytes = 0, digestBytes = 0, produced = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                                      &produced, 0))) {
        return false;
    }
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_HASH_LENGTH,
                                      reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes),
                                      &produced, 0))) {
        return false;
    }
    std::vector<unsigned char> object(objectBytes);
    HashHandle hash;
    if (!NT_SUCCESS(BCryptCreateHash(alg.handle, &hash.handle, object.data(), objectBytes,
                                     const_cast<PUCHAR>(key.data()),
                                     static_cast<ULONG>(key.size()), 0))) {
        return false;
    }
    if (!NT_SUCCESS(BCryptHashData(hash.handle, const_cast<PUCHAR>(data),
                                   static_cast<ULONG>(length), 0))) {
        return false;
    }
    out->assign(digestBytes, 0);
    return NT_SUCCESS(BCryptFinishHash(hash.handle, out->data(), digestBytes, 0));
}

/** 准备一个 AES-GCM 的算法句柄（已设好链模式）。 */
bool OpenAesGcm(AlgHandle* alg) {
    if (!alg->Open(BCRYPT_AES_ALGORITHM)) return false;
    // GCM 是认证加密模式，必须显式设置链模式，否则默认是 CBC
    const wchar_t* mode = BCRYPT_CHAIN_MODE_GCM;
    return NT_SUCCESS(BCryptSetProperty(alg->handle, BCRYPT_CHAINING_MODE,
                                        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                                        sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
}

}  // namespace

// ---------------------------------------------------------------------------
// 随机数
// ---------------------------------------------------------------------------

bool RandomBytes(std::size_t count, std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(count, 0);
    if (count == 0) return true;
    return NT_SUCCESS(BCryptGenRandom(nullptr, out->data(), static_cast<ULONG>(count),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// ---------------------------------------------------------------------------
// ECDH
// ---------------------------------------------------------------------------

bool GenerateEcdhKeyPair(EcdhKeyPair* out) {
    if (!out) return false;
    out->privateBlob.clear();
    out->publicKey.clear();

    AlgHandle alg;
    if (!alg.Open(BCRYPT_ECDH_P256_ALGORITHM)) return false;

    KeyHandle key;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(alg.handle, &key.handle, 256, 0))) return false;
    if (!NT_SUCCESS(BCryptFinalizeKeyPair(key.handle, 0))) return false;

    // 私钥：导出成不透明 blob 留着后面算共享密钥
    DWORD privateBytes = 0;
    if (!NT_SUCCESS(BCryptExportKey(key.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0,
                                    &privateBytes, 0))) {
        return false;
    }
    out->privateBlob.assign(privateBytes, 0);
    if (!NT_SUCCESS(BCryptExportKey(key.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                    out->privateBlob.data(), privateBytes, &privateBytes, 0))) {
        out->privateBlob.clear();
        return false;
    }

    // 公钥：导出后只留裸的 X||Y
    DWORD publicBytes = 0;
    if (!NT_SUCCESS(BCryptExportKey(key.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0,
                                    &publicBytes, 0))) {
        out->privateBlob.clear();
        return false;
    }
    std::vector<unsigned char> publicBlob(publicBytes);
    if (!NT_SUCCESS(BCryptExportKey(key.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, publicBlob.data(),
                                    publicBytes, &publicBytes, 0))) {
        out->privateBlob.clear();
        return false;
    }
    if (!ExtractPublicXY(publicBlob, &out->publicKey)) {
        out->privateBlob.clear();
        return false;
    }
    return true;
}

bool ComputeSharedSecret(const EcdhKeyPair& mine, const std::vector<unsigned char>& peerPublicKey,
                         std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (mine.privateBlob.empty() || peerPublicKey.size() != kP256PublicKeyBytes) return false;

    AlgHandle alg;
    if (!alg.Open(BCRYPT_ECDH_P256_ALGORITHM)) return false;

    // 自己的私钥
    KeyHandle privateKey;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        &privateKey.handle,
                                        const_cast<PUCHAR>(mine.privateBlob.data()),
                                        static_cast<ULONG>(mine.privateBlob.size()), 0))) {
        return false;
    }

    // 对方的公钥（长度已经检查过；不是曲线上的合法点会被 ImportKeyPair 拒掉）
    const std::vector<unsigned char> peerBlob = MakeEccPublicBlob(peerPublicKey);
    KeyHandle publicKey;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                        &publicKey.handle,
                                        const_cast<PUCHAR>(peerBlob.data()),
                                        static_cast<ULONG>(peerBlob.size()), 0))) {
        return false;
    }

    SecretHandle secret;
    if (!NT_SUCCESS(BCryptSecretAgreement(privateKey.handle, publicKey.handle, &secret.handle, 0))) {
        return false;
    }

    DWORD secretBytes = 0;
    if (!NT_SUCCESS(BCryptDeriveKey(secret.handle, kKdfRawSecret, nullptr, nullptr, 0,
                                    &secretBytes, 0))) {
        return false;
    }
    out->assign(secretBytes, 0);
    if (!NT_SUCCESS(BCryptDeriveKey(secret.handle, kKdfRawSecret, nullptr, out->data(),
                                    secretBytes, &secretBytes, 0))) {
        out->clear();
        return false;
    }

    // ⚠️ 跨平台的关键一步：Windows 的 BCRYPT_KDF_RAW_SECRET 返回的是
    // **小端序**的 x 坐标，而 Java 的 KeyAgreement.generateSecret() 是**大端序**。
    // 不翻转的话两端的共享密钥完全相同地不同，握手永远成功不了（而且不会有任何报错，
    // 只表现为"解出来是乱码"）。翻转后统一成大端。
    std::reverse(out->begin(), out->end());
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
    if (!HmacSha256(effectiveSalt, ikm.data(), ikm.size(), &prk)) return false;

    // expand：T(i) = HMAC(PRK, T(i-1) || info || i)
    std::vector<unsigned char> previous;  // T(0) 是空
    unsigned char counter = 1;
    while (out->size() < outBytes) {
        std::vector<unsigned char> block;
        block.reserve(previous.size() + info.size() + 1);
        block.insert(block.end(), previous.begin(), previous.end());
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter);
        if (!HmacSha256(prk, block.data(), block.size(), &previous)) {
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

    if (!HkdfSha256(sharedSecret, salt, kHkdfInfoClientToServer, kAesKeyBytes,
                    &keys.clientToServer)) {
        return SessionKeys{};
    }
    if (!HkdfSha256(sharedSecret, salt, kHkdfInfoServerToClient, kAesKeyBytes,
                    &keys.serverToClient)) {
        return SessionKeys{};
    }
    return keys;
}

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

bool AesGcmEncrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& plaintext, std::string* out) {
    if (!out) return false;
    out->clear();
    if (key.size() != kAesKeyBytes) return false;
    if (nonce.size() != kGcmNonceBytes) return false;

    AlgHandle alg;
    if (!OpenAesGcm(&alg)) return false;

    KeyHandle keyHandle;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg.handle, &keyHandle.handle, nullptr, 0,
                                               const_cast<PUCHAR>(key.data()),
                                               static_cast<ULONG>(key.size()), 0))) {
        return false;
    }

    std::vector<unsigned char> tag(kGcmTagBytes, 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    std::vector<unsigned char> buffer(plaintext.size() + kGcmTagBytes);
    ULONG produced = 0;
    const PUCHAR input = plaintext.empty()
                             ? nullptr
                             : reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data()));
    if (!NT_SUCCESS(BCryptEncrypt(keyHandle.handle, input, static_cast<ULONG>(plaintext.size()),
                                  &info, nullptr, 0, buffer.data(),
                                  static_cast<ULONG>(buffer.size()), &produced, 0))) {
        return false;
    }
    buffer.resize(produced);

    // 输出 = 密文 || 标签（和 Java 的 Cipher.doFinal 顺序一致）
    out->assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    out->append(reinterpret_cast<const char*>(tag.data()), tag.size());
    return true;
}

bool AesGcmDecrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& nonce,
                   const std::string& ciphertext, std::string* out) {
    if (!out) return false;
    out->clear();
    if (key.size() != kAesKeyBytes) return false;
    if (nonce.size() != kGcmNonceBytes) return false;
    if (ciphertext.size() < kGcmTagBytes) return false;

    const std::size_t bodyBytes = ciphertext.size() - kGcmTagBytes;
    std::vector<unsigned char> tag(ciphertext.begin() + static_cast<std::ptrdiff_t>(bodyBytes),
                                   ciphertext.end());

    AlgHandle alg;
    if (!OpenAesGcm(&alg)) return false;

    KeyHandle keyHandle;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg.handle, &keyHandle.handle, nullptr, 0,
                                               const_cast<PUCHAR>(key.data()),
                                               static_cast<ULONG>(key.size()), 0))) {
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    std::vector<unsigned char> buffer(bodyBytes > 0 ? bodyBytes : 1);
    ULONG produced = 0;
    const PUCHAR input =
        bodyBytes == 0 ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data()));
    const NTSTATUS status =
        BCryptDecrypt(keyHandle.handle, input, static_cast<ULONG>(bodyBytes), &info, nullptr, 0,
                      buffer.data(), static_cast<ULONG>(buffer.size()), &produced, 0);
    // 认证失败（被篡改 / 密钥不对）就是这里失败。绝不能把半截明文当成功返回。
    if (!NT_SUCCESS(status)) return false;

    out->assign(reinterpret_cast<const char*>(buffer.data()), produced);
    return true;
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
    if (!Sha256(publicKey.data(), publicKey.size(), &digest)) return std::string();

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
