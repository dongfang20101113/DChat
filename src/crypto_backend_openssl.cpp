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
#include <memory>
#include <cstring>

namespace dchat {
namespace {

constexpr std::size_t kCoordinateBytes = 32;
constexpr std::size_t kPublicKeyBytes = 64;

/**
 * OpenSSL 侧的"密钥对"：**直接持有 EVP_PKEY**，不用私有标量重建。
 *
 * 踩过的坑：一开始我把私钥存成裸标量、公钥用 EVP_PKEY_get_octet_string_param 导出，
 * 算共享密钥时再用"标量 + 公钥"重建密钥对。结果是重建出来的公私钥**不是一对**
 * （实测两边算出的共享密钥不同），握手会静默失败。CNG 那边能这么干是因为它把整个
 * blob 原样存取；OpenSSL 这边直接留着 EVP_PKEY 最省事也最不可能错。
 *
 * 对外仍然只暴露"裸标量 + 裸 X||Y"，那才是跨端约定。
 */
struct OpenSslKey {
    EVP_PKEY* key = nullptr;
    ~OpenSslKey() {
        if (key) EVP_PKEY_free(key);
    }
};

/**
 * 存活的 EVP_PKEY 表。
 *
 * 用 `std::vector<std::unique_ptr<...>>` 而不是 `vector<Holder>`：后者在扩容时
 * 会重新分配，虽然会移动 Holder、EVP_PKEY* 的值本身没变，但**一旦有人拿着
 * store 里的引用/指针就会失效**。用 unique_ptr 让 key 的地址永远稳定。
 */
std::vector<std::unique_ptr<OpenSslKey>>& KeyStore() {
    static std::vector<std::unique_ptr<OpenSslKey>> store;
    return store;
}

/** 把一把 EVP_PKEY 交给 store 长期持有，返回它的索引（1 起）。 */
std::size_t RememberKey(EVP_PKEY* key) {
    auto holder = std::make_unique<OpenSslKey>();
    holder->key = key;
    KeyStore().push_back(std::move(holder));
    return KeyStore().size();
}

/** 从私有 blob（存的是 store 索引）取回 EVP_PKEY；拿不到返回 nullptr。 */
EVP_PKEY* LookupKey(const std::vector<unsigned char>& blob) {
    if (blob.size() != 8) return nullptr;
    std::uint64_t index = 0;
    for (int i = 0; i < 8; ++i) index = (index << 8) | blob[static_cast<std::size_t>(i)];
    auto& store = KeyStore();
    if (index == 0 || index > store.size()) return nullptr;
    OpenSslKey* holder = store[static_cast<std::size_t>(index - 1)].get();
    return holder ? holder->key : nullptr;
}

/** 把索引编成 8 字节 blob。 */
std::vector<unsigned char> EncodeIndex(std::size_t index) {
    std::vector<unsigned char> blob(8, 0);
    for (int i = 0; i < 8; ++i) {
        blob[static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((static_cast<std::uint64_t>(index) >> ((7 - i) * 8)) & 0xFF);
    }
    return blob;
}
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
    // 优先用"拆开的坐标"参数，拿到的就是纯 X||Y；
    // 拿不到再退回未压缩点格式（0x04 || X || Y）自己剥掉头。
    std::size_t length = 0;
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_EC_PUB_X, nullptr, 0, &length) == 1 &&
        length == kCoordinateBytes) {
        std::vector<unsigned char> x(length);
        std::vector<unsigned char> y(length);
        std::size_t xLen = length, yLen = length;
        if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_EC_PUB_X, x.data(), x.size(),
                                            &xLen) == 1 &&
            EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_EC_PUB_Y, y.data(), y.size(),
                                            &yLen) == 1 &&
            xLen == kCoordinateBytes && yLen == kCoordinateBytes) {
            out->clear();
            out->insert(out->end(), x.begin(), x.end());
            out->insert(out->end(), y.begin(), y.end());
            return true;
        }
    }

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

/** 用"标量 + 公钥"造一把完整的 EC 私钥。失败返回 nullptr。 */
EVP_PKEY* BuildPrivateKey(const std::vector<unsigned char>& scalar,
                          const std::vector<unsigned char>& publicXY) {
    // ⚠️ 字节序：跨端约定的"私钥标量"是 **CNG 那种小端存放**（BCRYPT_ECCPRIVATE_BLOB 里
    // 就是小端），而 OpenSSL 的 BIGNUM 是大端。不反转的话两边"同一个标量"其实是两个数：
    // 导入能成功、ECDH 也能算出 32 字节，但结果和 Windows 端完全不同（静默不一致，
    // 握手永远成不了，还没有任何报错）。实测 fixture 就是先被这个坑住的。
    unsigned char scalarBytes[kCoordinateBytes];
    for (std::size_t i = 0; i < kCoordinateBytes; ++i) {
        scalarBytes[i] = scalar[kCoordinateBytes - 1 - i];
    }
    BIGNUM* bn = BN_bin2bn(scalarBytes, static_cast<int>(sizeof(scalarBytes)), nullptr);
    if (!bn) return nullptr;
    const bool padded = BN_bn2binpad(bn, scalarBytes, static_cast<int>(sizeof(scalarBytes))) ==
                        static_cast<int>(sizeof(scalarBytes));
    BN_free(bn);
    if (!padded) return nullptr;

    // point 必须是**具名变量**：曾经写成 MakeUncompressed(...) 的临时对象，
    // 它在构造 OSSL_PARAM 之前就被销毁，参数指向已释放内存，直接段错误。
    // 公钥**分开给 X / Y**，而不是给一个未压缩点：
    // 实测给 OSSL_PKEY_PARAM_PUB_KEY（未压缩点）时 EVP_PKEY_fromdata 会接受，
    // 但用标量 + 这个公钥算出的共享密钥和 CNG 对不上（导入端静默不一致）。
    // 分开给坐标是 OpenSSL 文档里 EC 导入的标准做法。
    std::vector<unsigned char> x(publicXY.begin(), publicXY.begin() + 32);
    std::vector<unsigned char> y(publicXY.begin() + 32, publicXY.end());
    OSSL_PARAM params[5];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                 const_cast<char*>("P-256"), 0);
    params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_PRIV_KEY, scalarBytes,
                                        sizeof(scalarBytes));
    params[2] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_EC_PUB_X, x.data(), x.size());
    params[3] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_EC_PUB_Y, y.data(), y.size());
    params[4] = OSSL_PARAM_construct_end();

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!ctx) return nullptr;
    EVP_PKEY* key = nullptr;
    const int ok = EVP_PKEY_fromdata_init(ctx) == 1 &&
                   EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_KEYPAIR, params) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (ok != 1 || !key) return nullptr;
    return key;
}

/** 用点造一把公钥。失败返回 nullptr。 */
EVP_PKEY* BuildPublicKey(const std::vector<unsigned char>& publicXY) {
    std::vector<unsigned char> point = MakeUncompressed(publicXY);  // 非 const：OSSL_PARAM 要的是 void*
    OSSL_PARAM params[3];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                 const_cast<char*>("P-256"), 0);
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                                  point.size());
    params[2] = OSSL_PARAM_construct_end();

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!ctx) return nullptr;
    EVP_PKEY* key = nullptr;
    const int ok = EVP_PKEY_fromdata_init(ctx) == 1 &&
                   EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (ok != 1 || !key) return nullptr;
    return key;
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

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!ctx) return false;
    bool ok = EVP_PKEY_keygen_init(ctx) == 1;
    if (ok) {
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char*>("P-256"), 0);
        params[1] = OSSL_PARAM_construct_end();
        ok = EVP_PKEY_CTX_set_params(ctx, params) == 1;
    }
    EVP_PKEY* key = nullptr;
    if (ok) ok = EVP_PKEY_keygen(ctx, &key) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !key) return false;

    // 公钥：裸 X||Y（对外约定）
    if (!PublicKeyXY(key, &out->publicKey)) {
        EVP_PKEY_free(key);
        return false;
    }
    // 私钥：把 EVP_PKEY 本身存起来，privateBlob 只放索引。
    // 不重建密钥对（重建出来的公私钥配不上，实测过）。
    out->privateBlob = EncodeIndex(RememberKey(key));
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

    // 调用方保证标量和公钥是一对（和 CNG 那边的契约一致）
    EVP_PKEY* key = BuildPrivateKey(privateScalar, publicKey);
    if (!key) return false;
    out->privateBlob = EncodeIndex(RememberKey(key));
    out->publicKey = publicKey;
    return true;
}
bool BackendEcdhSharedSecret(const BackendEcdhKeyPair& mine,
                             const std::vector<unsigned char>& peerPublicKey,
                             std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (peerPublicKey.size() != kPublicKeyBytes) return false;

    // 自己的密钥直接从 store 取回（生成/导入时已经放好，且公私钥天然匹配）
    EVP_PKEY* mineKey = LookupKey(mine.privateBlob);
    if (!mineKey) return false;

    PkeyHandle peerKey;
    peerKey.key = BuildPublicKey(peerPublicKey);
    if (!peerKey.key) return false;

    EVP_PKEY_CTX* derive = EVP_PKEY_CTX_new_from_pkey(nullptr, mineKey, nullptr);
    if (!derive) return false;
    std::vector<unsigned char> secret;
    bool derived = false;
    if (EVP_PKEY_derive_init(derive) == 1 &&
        EVP_PKEY_derive_set_peer(derive, peerKey.key) == 1) {
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
bool BackendEcdhPrivateScalar(const BackendEcdhKeyPair& pair,
                              std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    EVP_PKEY* key = LookupKey(pair.privateBlob);
    if (!key) return false;
    BIGNUM* scalar = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &scalar) != 1 || !scalar) return false;
    out->assign(kCoordinateBytes, 0);
    const int written = BN_bn2binpad(scalar, out->data(), static_cast<int>(out->size()));
    BN_free(scalar);
    if (written != static_cast<int>(kCoordinateBytes)) {
        out->clear();
        return false;
    }
    return true;
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
