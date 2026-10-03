// 密码学原语的 **Windows 后端**：CNG（bcrypt.dll）。
//
// 这些函数原来长在 crypto.cpp 里，为了给 Linux 端让路搬了出来。
// 逻辑一行没改——三端（Windows / Linux / 安卓）必须字节级一致，
// 动这里的任何一步都要先想清楚对端会变成什么。
#include "crypto_backend.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace dchat {
namespace {

// P-256 的坐标是 32 字节
constexpr ULONG kP256CoordinateBytes = 32;
// BCRYPT_ECCKEY_BLOB 的魔数（"ECK1" / "ECK2"，小端读出来是这两个值）
constexpr ULONG kEcdhPublicP256Magic = 0x314B4345;
constexpr ULONG kEcdhPrivateP256Magic = 0x324B4345;

// BCRYPT_KDF_RAW_SECRET：不做任何 KDF，直接要原始共享密钥。
// 微软文档里的值就是字符串 "TRUNCATE"；MinGW 的 bcrypt.h 没有这个宏，自己定义一份。
#ifndef BCRYPT_KDF_RAW_SECRET
constexpr wchar_t kKdfRawSecret[] = L"TRUNCATE";
#else
constexpr const wchar_t* kKdfRawSecret = BCRYPT_KDF_RAW_SECRET;
#endif

// ---- 极简 RAII：CNG 句柄必须显式关，异常路径上很容易漏 ----
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

std::vector<unsigned char> MakeEccPublicBlob(const std::vector<unsigned char>& xy) {
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + xy.size(), 0);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = kEcdhPublicP256Magic;
    header->cbKey = kP256CoordinateBytes;
    std::copy(xy.begin(), xy.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    return blob;
}

bool ExtractPublicXY(const std::vector<unsigned char>& blob, std::vector<unsigned char>* out) {
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB)) return false;
    const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (header->cbKey != kP256CoordinateBytes) return false;
    const std::size_t need = sizeof(BCRYPT_ECCKEY_BLOB) + kP256CoordinateBytes * 2;
    if (blob.size() < need) return false;
    out->assign(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.begin() + need);
    return true;
}

bool OpenAesGcm(AlgHandle* alg) {
    if (!alg->Open(BCRYPT_AES_ALGORITHM)) return false;
    // GCM 是认证加密模式，必须显式设置链模式，否则默认是 CBC
    const wchar_t* mode = BCRYPT_CHAIN_MODE_GCM;
    return NT_SUCCESS(BCryptSetProperty(alg->handle, BCRYPT_CHAINING_MODE,
                                        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                                        sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
}

}  // namespace

bool BackendSha256(const unsigned char* data, std::size_t length,
                   std::vector<unsigned char>* out) {
    if (!out) return false;
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

bool BackendHmacSha256(const std::vector<unsigned char>& key, const unsigned char* data,
                       std::size_t length, std::vector<unsigned char>* out) {
    if (!out) return false;
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

bool BackendPbkdf2Sha256(const std::string& password, const std::vector<unsigned char>& salt,
                         int iterations, std::size_t outBytes,
                         std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(outBytes, 0);
    if (outBytes == 0 || iterations <= 0) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.empty() ? nullptr : salt.data()), static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations), out->data(), static_cast<ULONG>(out->size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        out->clear();
        return false;
    }
    return true;
}

bool BackendRandomBytes(std::size_t count, std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(count, 0);
    if (count == 0) return true;
    return NT_SUCCESS(BCryptGenRandom(nullptr, out->data(), static_cast<ULONG>(count),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool BackendGenerateEcdhKeyPair(BackendEcdhKeyPair* out) {
    if (!out) return false;
    out->privateBlob.clear();
    out->publicKey.clear();

    AlgHandle alg;
    if (!alg.Open(BCRYPT_ECDH_P256_ALGORITHM)) return false;

    KeyHandle key;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(alg.handle, &key.handle, 256, 0))) return false;
    if (!NT_SUCCESS(BCryptFinalizeKeyPair(key.handle, 0))) return false;

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

bool BackendImportEcdhKeyPair(const std::vector<unsigned char>& privateScalar,
                              const std::vector<unsigned char>& publicKey,
                              BackendEcdhKeyPair* out) {
    if (!out) return false;
    out->privateBlob.clear();
    out->publicKey.clear();
    if (privateScalar.size() != kP256CoordinateBytes) return false;
    if (publicKey.size() != 64) return false;

    // ECCPRIVATE_BLOB 布局：header(8) + X(32) + Y(32) + d(32)
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64 + kP256CoordinateBytes, 0);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = kEcdhPrivateP256Magic;
    header->cbKey = kP256CoordinateBytes;
    std::copy(publicKey.begin(), publicKey.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    std::copy(privateScalar.begin(), privateScalar.end(),
              blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB) + 64);

    AlgHandle alg;
    if (!alg.Open(BCRYPT_ECDH_P256_ALGORITHM)) return false;
    KeyHandle key;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.handle,
                                        blob.data(), static_cast<ULONG>(blob.size()), 0))) {
        return false;
    }
    // CNG 对 ECC 私钥 blob 是**原样存取**：既不校验标量和公钥是否对应，也不会用标量重算公钥。
    // 所以这个函数的契约是"调用方必须传一对真正匹配的标量和公钥"（测试固定向量、读回服务器身份密钥）。
    out->privateBlob = blob;
    out->publicKey = publicKey;
    return true;
}

bool BackendEcdhSharedSecret(const BackendEcdhKeyPair& mine,
                             const std::vector<unsigned char>& peerPublicKey,
                             std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (mine.privateBlob.empty() || peerPublicKey.size() != 64) return false;

    AlgHandle alg;
    if (!alg.Open(BCRYPT_ECDH_P256_ALGORITHM)) return false;

    KeyHandle privateKey;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        &privateKey.handle,
                                        const_cast<PUCHAR>(mine.privateBlob.data()),
                                        static_cast<ULONG>(mine.privateBlob.size()), 0))) {
        return false;
    }
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

    // ⚠️ 跨平台的关键一步：Windows 的 BCRYPT_KDF_RAW_SECRET 返回的是**小端序** x 坐标，
    // 而 Java / OpenSSL 是**大端序**。不翻转的话两端共享密钥"相同地不同"，
    // 握手永远成功不了，而且没有任何报错，只表现为"解出来是乱码"。翻转后统一成大端。
    std::reverse(out->begin(), out->end());
    return true;
}

bool BackendEcdhPrivateScalar(const BackendEcdhKeyPair& pair,
                              std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    // CNG 的 ECCPRIVATE_BLOB 布局：header(8) + X(32) + Y(32) + d(32)，标量在最后 32 字节
    constexpr std::size_t kScalarOffset = sizeof(BCRYPT_ECCKEY_BLOB) + kP256CoordinateBytes * 2;
    if (pair.privateBlob.size() < kScalarOffset + kP256CoordinateBytes) return false;
    out->assign(pair.privateBlob.begin() + static_cast<std::ptrdiff_t>(kScalarOffset),
                pair.privateBlob.begin() +
                    static_cast<std::ptrdiff_t>(kScalarOffset + kP256CoordinateBytes));
    return true;
}

bool BackendAesGcmEncrypt(const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& nonce,
                          const std::vector<unsigned char>& plaintext,
                          std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (key.size() != 32 || nonce.size() != 12) return false;

    AlgHandle alg;
    if (!OpenAesGcm(&alg)) return false;
    KeyHandle keyHandle;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg.handle, &keyHandle.handle, nullptr, 0,
                                               const_cast<PUCHAR>(key.data()),
                                               static_cast<ULONG>(key.size()), 0))) {
        return false;
    }

    std::vector<unsigned char> tag(16, 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    std::vector<unsigned char> buffer(plaintext.size() + 16);
    ULONG produced = 0;
    const PUCHAR input =
        plaintext.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<unsigned char*>(plaintext.data()));
    if (!NT_SUCCESS(BCryptEncrypt(keyHandle.handle, input, static_cast<ULONG>(plaintext.size()),
                                  &info, nullptr, 0, buffer.data(),
                                  static_cast<ULONG>(buffer.size()), &produced, 0))) {
        return false;
    }
    buffer.resize(produced);

    // 输出 = 密文 || tag（和 Java 的 Cipher.doFinal 顺序一致）
    out->assign(buffer.begin(), buffer.end());
    out->insert(out->end(), tag.begin(), tag.end());
    return true;
}

bool BackendAesGcmDecrypt(const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& nonce,
                          const std::vector<unsigned char>& input,
                          std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (key.size() != 32 || nonce.size() != 12) return false;
    if (input.size() < 16) return false;

    const std::size_t bodyBytes = input.size() - 16;
    std::vector<unsigned char> tag(input.begin() + static_cast<std::ptrdiff_t>(bodyBytes),
                                   input.end());

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
    const PUCHAR raw =
        bodyBytes == 0 ? nullptr : const_cast<PUCHAR>(input.data());
    const NTSTATUS status =
        BCryptDecrypt(keyHandle.handle, raw, static_cast<ULONG>(bodyBytes), &info, nullptr, 0,
                      buffer.data(), static_cast<ULONG>(buffer.size()), &produced, 0);
    // 认证失败（被篡改 / 密钥不对）就是这里失败。绝不能把半截明文当成功返回。
    if (!NT_SUCCESS(status)) return false;
    out->assign(buffer.begin(), buffer.begin() + produced);
    return true;
}

}  // namespace dchat
