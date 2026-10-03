// 密码学原语的**平台后端**接口。
//
// 上层（crypto.cpp 的握手/HKDF/AES-GCM 拼装、auth.cpp 的口令派生）只认这里的函数，
// 具体实现按平台选：Windows 走 CNG（bcrypt.dll），Linux 走 OpenSSL。
//
// **这个接口只有 5 个函数是刻意的**：够用，而且能保证两端行为一致。
// 三个必须字节级一致的地方（不一致会让 Windows、Linux、安卓三方互相解不开）：
//   1. ECDH 公钥的线上格式 —— 裸的 X||Y，各 32 字节大端
//   2. AES-GCM 的密文与 16 字节 tag 的分开存放方式
//   3. PBKDF2-HMAC-SHA256 的派生结果（口令哈希落盘，派生结果一变所有人都登不上）
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dchat {

/** 一次 SHA-256。 */
bool BackendSha256(const unsigned char* data, std::size_t length,
                   std::vector<unsigned char>* out);

/** HMAC-SHA256（HKDF 的底座）。 */
bool BackendHmacSha256(const std::vector<unsigned char>& key, const unsigned char* data,
                       std::size_t length, std::vector<unsigned char>* out);

/** PBKDF2-HMAC-SHA256（口令哈希）。 */
bool BackendPbkdf2Sha256(const std::string& password, const std::vector<unsigned char>& salt,
                         int iterations, std::size_t outBytes,
                         std::vector<unsigned char>* out);

/** 系统随机数。 */
bool BackendRandomBytes(std::size_t count, std::vector<unsigned char>* out);

/** ECDH P-256 的密钥对。privateBlob 是后端自己的不透明格式，只在本后端内使用。 */
struct BackendEcdhKeyPair {
    std::vector<unsigned char> privateBlob;
    std::vector<unsigned char> publicKey;  // 裸 X||Y（64 字节，各 32 字节大端）
};

/** 生成一对 ECDH P-256 密钥。 */
bool BackendGenerateEcdhKeyPair(BackendEcdhKeyPair* out);

/**
 * 用私有标量（32 字节）+ 公钥（64 字节）重建密钥对。
 * 用于"重连时复用同一把密钥"的场景。
 */
bool BackendImportEcdhKeyPair(const std::vector<unsigned char>& privateScalar,
                              const std::vector<unsigned char>& publicKey,
                              BackendEcdhKeyPair* out);

/** 算共享密钥（X 坐标，32 字节，大端）。 */
bool BackendEcdhSharedSecret(const BackendEcdhKeyPair& mine,
                             const std::vector<unsigned char>& peerPublicKey,
                             std::vector<unsigned char>* out);

/**
 * 从密钥对里取出**裸的私有标量**（32 字节大端）。
 *
 * 存在的理由：服务器要把身份密钥落盘，落盘格式是"标量 + 公钥"的十六进制——
 * 这个格式必须两端一致（Windows 上生成的密钥文件要能被 Linux 读）。
 * 以前调用方自己按 `8 + 32 + 32` 这个偏移从 CNG 的 blob 里抠，那是 Windows 专有布局；
 * 抽到这里之后，两边都只认"32 字节标量"这一个约定。
 */
bool BackendEcdhPrivateScalar(const BackendEcdhKeyPair& pair,
                              std::vector<unsigned char>* out);

/**
 * AES-256-GCM 加密。
 * 输出约定（**不能改动，安卓端也是这个顺序**）：密文在前、16 字节 tag 在最后，
 * 和 Java 的 `Cipher.doFinal` 一致。写成"先 tag 后密文"会让三方全部解不开。
 */
bool BackendAesGcmEncrypt(const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& nonce,
                          const std::vector<unsigned char>& plaintext,
                          std::vector<unsigned char>* out);

/** AES-256-GCM 解密：`input` 同样是"密文在前、tag 在后"。 */
bool BackendAesGcmDecrypt(const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& nonce,
                          const std::vector<unsigned char>& input,
                          std::vector<unsigned char>* out);

}  // namespace dchat
