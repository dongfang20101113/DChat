// 密码学原语的 **Linux 后端**：OpenSSL 3。
//
// 目标是和 Windows 的 CNG 后端**字节级一致**——三端（Windows / Linux / 安卓）要能互相
// 解开对方的消息。test_crypto 里那批固定向量就是用来钉住这一点的，两端跑同一批向量。
//
// 两处平台差异在这里吸收掉（其他地方看不到）：
//   1. CNG 导出的 ECC 公钥带 BCRYPT_ECCKEY_BLOB 头，这里统一成裸 X||Y
//   2. CNG 的 RAW_SECRET 是小端，OpenSSL 的 derive 是大端 → 所以**这边不需要翻转**
#include "crypto_backend.h"

#include <openssl/core_names.h>  // OSSL_PKEY_PARAM_*
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <openssl/bn.h>
#include <openssl/obj_mac.h>  // NID_X9_62_prime256v1

#include <algorithm>
#include <cstring>

namespace dchat {
namespace {

constexpr std::size_t kCoordinateBytes = 32;
constexpr std::size_t kPublicKeyBytes = 64;

/** EVP_PKEY 的 RAII。 */
struct PkeyHandle {
    EVP_PKEY* key = nullptr;
    ~PkeyHandle() {
        if (key) EVP_PKEY_free(key);
    }
    PkeyHandle() = default;
    PkeyHandle(const PkeyHandle&) = delete;
    PkeyHandle& operator=(const PkeyHandle&) = delete;
};

/** 从 PKEY 里导出裸的 X||Y（各 32 字节大端）。 */
bool PublicKeyXY(EVP_PKEY* key, std::vector<unsigned char>* out) {
    std::size_t length = 0;
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &length) != 1) {
        return false;
    }
    std::vector<unsigned char> point(length);
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(),
                                        &length) != 1) {
        return false;
    }
    // 未压缩点格式：0x04 || X(32) || Y(32)
    if (point.size() == kPublicKeyBytes) {
        out->assign(point.begin(), point.end());
        return true;
    }
    if (point.size() == 1 + kPublicKeyBytes && point[0] == 0x04) {
        out->assign(point.begin() + 1, point.end());
        return true;
    }
    return false;
}

/** 把裸 X||Y 还原成未压缩点格式（导入用）。 */
std::vector<unsigned char> MakeUncompressed(const std::vector<unsigned char>& xy) {
    std::vector<unsigned char> point;
    point.reserve(1 + xy.size());
    point.push_back(0x04);
    point.insert(point.end(), xy.begin(), xy.end());
    return point;
}

}  // namespace

bool BackendSha256(const unsigned char* data, std::size_t length,
                   std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(32, 0);
    unsigned int written = 0;
    if (EVP_Digest(data, length, out->data(), &written, EVP_sha256(), nullptr) != 1) {
        out->clear();
        return false;
    }
    out->resize(written);
    return true;
}

bool BackendHmacSha256(const std::vector<unsigned char>& key, const unsigned char* data,
                       std::size_t length, std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(32, 0);
    unsigned int written = 0;
    // key 为空时 OpenSSL 的 HMAC 会拿 nullptr 当"没有密钥"，所以给一个空数组的地址
    const unsigned char empty = 0;
    const unsigned char* keyData = key.empty() ? &empty : key.data();
    if (HMAC(EVP_sha256(), keyData, static_cast<int>(key.size()), data, length, out->data(),
             &written) == nullptr) {
        out->clear();
        return false;
    }
    out->resize(written);
    return true;
}

bool BackendPbkdf2Sha256(const std::string& password, const std::vector<unsigned char>& salt,
                         int iterations, std::size_t outBytes,
                         std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(outBytes, 0);
    if (outBytes == 0 || iterations <= 0) return false;

    const unsigned char empty = 0;
    const unsigned char* saltData = salt.empty() ? &empty : salt.data();
    // PKCS5_PBKDF2_HMAC 的 out 长度是 int，超过 INT_MAX 的请求直接拒掉
    if (outBytes > 0x7FFFFFFF) {
        out->clear();
        return false;
    }
    const int ok = PKCS5_PBKDF2_HMAC(
        password.c_str(), static_cast<int>(password.size()), saltData,
        static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(outBytes),
        out->data());
    if (ok != 1) {
        out->clear();
        return false;
    }
    return true;
}

bool BackendRandomBytes(std::size_t count, std::vector<unsigned char>* out) {
    if (!out) return false;
    out->assign(count, 0);
    if (count == 0) return true;
    return RAND_bytes(out->data(), static_cast<int>(count)) == 1;
}

bool BackendGenerateEcdhKeyPair(BackendEcdhKeyPair* out) {
    if (!out) return false;
    out->privateBlob.clear();
    out->publicKey.clear();

    // 用 EVP_PKEY_CTX + EVP_PKEY_keygen 生成（比 EVP_EC_gen 更老更广的 API，
    // OpenSSL 3.x 的各个发行版都有；EVP_EC_gen 是 3.0 才加的便利函数，有些环境缺）
    PkeyHandle key;
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!ctx) return false;
        bool ok = EVP_PKEY_keygen_init(ctx) == 1;
        if (ok) {
            // 曲线用 OSSL_PARAM 指定（比 EVP_PKEY_CTX_set_ec_paramgen_curve_nid 更通用）
            OSSL_PARAM params[2];
            params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                         const_cast<char*>("P-256"), 0);
            params[1] = OSSL_PARAM_construct_end();
            ok = EVP_PKEY_CTX_set_params(ctx, params) == 1;
        }
        if (ok) ok = EVP_PKEY_keygen(ctx, &key.key) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (!ok || !key.key) return false;
    }

    // 私钥标量（32 字节大端）留给 import 用；publicKey 存裸 X||Y
    BIGNUM* scalar = nullptr;
    if (EVP_PKEY_get_bn_param(key.key, OSSL_PKEY_PARAM_PRIV_KEY, &scalar) != 1) return false;
    std::vector<unsigned char> buffer(kCoordinateBytes, 0);
    const int written = BN_bn2binpad(scalar, buffer.data(), static_cast<int>(buffer.size()));
    BN_free(scalar);
    if (written != static_cast<int>(kCoordinateBytes)) return false;

    if (!PublicKeyXY(key.key, &out->publicKey)) return false;
    out->privateBlob = buffer;  // OpenSSL 后端里 privateBlob 就是裸标量
    return true;
}

bool BackendImportEcdhKeyPair(const std::vector<unsigned char>& privateScalar,
                              const std::vector<unsigned char>& publicKey,
                              BackendEcdhKeyPair* out) {
    if (!out) return false;
    out->privateBlob.clear();
    out->publicKey.clear();
    if (privateScalar.size() != kCoordinateBytes) return false;
    if (publicKey.size() != kPublicKeyBytes) return false;

    // 故意只做"格式检查"就返回：契约要求调用方传一对真正匹配的标量和公钥。
    // Windows 的 CNG 也是原样存取、不校验，两端行为保持一致（见 crypto_backend_win.cpp 的说明）。
    out->privateBlob = privateScalar;
    out->publicKey = publicKey;
    return true;
}

bool BackendEcdhSharedSecret(const BackendEcdhKeyPair& mine,
                             const std::vector<unsigned char>& peerPublicKey,
                             std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (mine.privateBlob.size() != kCoordinateBytes) return false;
    if (peerPublicKey.size() != kPublicKeyBytes) return false;

    // 自己的密钥：标量 + 自己的公钥拼成一把完整的 EC 私钥
    std::vector<unsigned char> myPoint = MakeUncompressed(mine.publicKey);
    unsigned char scalarBytes[kCoordinateBytes];
    if (BN_bn2binpad(BN_bin2bn(mine.privateBlob.data(),
                               static_cast<int>(mine.privateBlob.size()), nullptr),
                     scalarBytes, static_cast<int>(sizeof(scalarBytes))) !=
        static_cast<int>(sizeof(scalarBytes))) {
        return false;
    }
    OSSL_PARAM mineParams[4];
    mineParams[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char*>("P-256"), 0);
    mineParams[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_PRIV_KEY, scalarBytes,
                                            sizeof(scalarBytes));
    mineParams[2] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, myPoint.data(),
                                                      myPoint.size());
    mineParams[3] = OSSL_PARAM_construct_end();

    PkeyHandle mineKey;
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!ctx) return false;
        const int ok = EVP_PKEY_fromdata_init(ctx) == 1 &&
                       EVP_PKEY_fromdata(ctx, &mineKey.key, EVP_PKEY_KEYPAIR, mineParams) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (ok != 1 || !mineKey.key) return false;
    }

    // 对方的公钥：只需要点、不需要私钥
    std::vector<unsigned char> peerPoint = MakeUncompressed(peerPublicKey);
    OSSL_PARAM peerParams[3];
    peerParams[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char*>("P-256"), 0);
    peerParams[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, peerPoint.data(),
                                                      peerPoint.size());
    peerParams[2] = OSSL_PARAM_construct_end();

    PkeyHandle peerKey;
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!ctx) return false;
        const int ok = EVP_PKEY_fromdata_init(ctx) == 1 &&
                       EVP_PKEY_fromdata(ctx, &peerKey.key, EVP_PKEY_PUBLIC_KEY, peerParams) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (ok != 1 || !peerKey.key) return false;
    }

    // ECDH
    EVP_PKEY_CTX* derive = EVP_PKEY_CTX_new(mineKey.key, nullptr);
    if (!derive) return false;
    std::vector<unsigned char> secret;
    bool derived = false;
    if (EVP_PKEY_derive_init(derive) == 1 && EVP_PKEY_derive_set_peer(derive, peerKey.key) == 1) {
        std::size_t length = 0;
        if (EVP_PKEY_derive(derive, nullptr, &length) == 1 && length > 0) {
            secret.assign(length, 0);
            if (EVP_PKEY_derive(derive, secret.data(), &length) == 1) {
                secret.resize(length);
                derived = true;
            }
        }
    }
    EVP_PKEY_CTX_free(derive);
    if (!derived) return false;

    // ⚠️ 这里**不翻转字节序**：OpenSSL 的 ECDH 输出本来就是大端；
    // Windows 后端翻转是为了把小端的 CNG 结果对齐到大端。两端最终都是大端。
    *out = secret;
    return out->size() == kCoordinateBytes;
}

bool BackendAesGcmEncrypt(const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& nonce,
                          const std::vector<unsigned char>& plaintext,
                          std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (key.size() != 32 || nonce.size() != 12) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    std::vector<unsigned char> cipher(plaintext.size() + 16);
    int written = 0, total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                                  nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1;
    if (ok && !plaintext.empty()) {
        ok = EVP_EncryptUpdate(ctx, cipher.data(), &written, plaintext.data(),
                               static_cast<int>(plaintext.size())) == 1;
        total = written;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, cipher.data() + total, &written) == 1;
        total += written;
    }
    std::vector<unsigned char> tag(16, 0);
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;

    // 输出 = 密文 || tag（和 Java 的 Cipher.doFinal、以及 Windows 后端一致）
    cipher.resize(total);
    out->assign(cipher.begin(), cipher.end());
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

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    std::vector<unsigned char> plain(bodyBytes + 16);
    int written = 0, total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                                  nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1;
    if (ok && bodyBytes > 0) {
        ok = EVP_DecryptUpdate(ctx, plain.data(), &written, input.data(),
                               static_cast<int>(bodyBytes)) == 1;
        total = written;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1;
    }
    if (ok) {
        // 认证失败（被篡改 / 密钥不对）就是这一步返回 0。绝不能把半截明文当成功返回。
        ok = EVP_DecryptFinal_ex(ctx, plain.data() + total, &written) == 1;
        total += written;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    plain.resize(total);
    *out = plain;
    return true;
}

}  // namespace dchat
