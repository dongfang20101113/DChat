// 群文件加密：把一段明文变成可以落盘的密文（静态加密，不是端到端）。
//
// 设计取舍见 docs/file-encryption.md，这里只强调三条**代码层面必须守住**的：
//
// 1. **每个文件独立随机 IV**。AES-GCM 下 nonce 重用是灾难级的（同一密钥下重用
//    会让攻击者直接恢复明文异或值）。这里绝不用计数器、绝不用固定值。
// 2. **认证失败必须当硬错误**。AesGcmDecrypt 返回 false 就说明数据被改过、密钥不对
//    或者 nonce 不对 —— 绝不能把半截明文当成功交出去。
// 3. **魔数让"加密了没有"可判定**。迁移、排错、回退开关时都靠它，不用猜。
//
// 文件落盘格式：`DCHATENC1`(8) + IV(16) + 密文 + GCM 标签(16)
#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "crypto.h"

namespace dchat {

inline constexpr std::size_t kFileKeyBytes = 32;
inline constexpr std::size_t kFileIvBytes = 12;  // AES-GCM 标准 nonce 长度是 96 位 = 12 字节
inline constexpr std::size_t kFileTagBytes = 16;
inline constexpr std::size_t kFileMagicBytes = 9;  // "DCHATENC1" 是 9 个字符，别数错
/** 不写成 `const char*`：那样每个 TU 一份，还容易被误当宏用。 */
inline constexpr char kFileMagic[kFileMagicBytes + 1] = "DCHATENC1";

using FileKey = std::vector<unsigned char>;

/** 生成一把房间文件密钥（32 字节）。 */
inline bool GenerateFileKey(FileKey* out) {
    if (out == nullptr) return false;
    out->clear();
    if (!RandomBytes(kFileKeyBytes, out)) return false;
    return out->size() == kFileKeyBytes;
}

/** 这段数据是不是我们用 EncryptFileBlob 生成的密文（只看魔数，不做认证）。 */
inline bool IsEncryptedBlob(const std::string& data) {
    if (data.size() < kFileMagicBytes) return false;
    return std::memcmp(data.data(), kFileMagic, kFileMagicBytes) == 0;
}

/** 加密：输出 = 魔数 + 随机 IV + 密文 + 标签。 */
inline bool EncryptFileBlob(const FileKey& key, const std::string& plain, std::string* out,
                            std::string* error) {
    if (out == nullptr) return false;
    if (key.size() != kFileKeyBytes) {
        if (error != nullptr) *error = "密钥长度不是 32 字节";
        return false;
    }
    std::vector<unsigned char> iv;
    if (!RandomBytes(kFileIvBytes, &iv) || iv.size() != kFileIvBytes) {
        if (error != nullptr) *error = "生成随机 IV 失败";
        return false;
    }
    std::string cipher;
    if (!AesGcmEncrypt(key, iv, plain, &cipher)) {
        if (error != nullptr) *error = "AES-GCM 加密失败";
        return false;
    }
    out->clear();
    out->assign(kFileMagic, kFileMagicBytes);
    out->append(reinterpret_cast<const char*>(iv.data()), iv.size());
    out->append(cipher);
    return true;
}

/** 解密。**认证失败一律返回 false**，不要在调用方"容错"。 */
inline bool DecryptFileBlob(const FileKey& key, const std::string& blob, std::string* out,
                            std::string* error) {
    if (out == nullptr) return false;
    if (key.size() != kFileKeyBytes) {
        if (error != nullptr) *error = "密钥长度不是 32 字节";
        return false;
    }
    if (!IsEncryptedBlob(blob)) {
        if (error != nullptr) *error = "这不是加密文件（没有 DCHATENC1 头）";
        return false;
    }
    // 至少要能装下 魔数 + IV + 标签，否则一定是被截断了
    if (blob.size() < kFileMagicBytes + kFileIvBytes + kFileTagBytes) {
        if (error != nullptr) *error = "加密文件被截断";
        return false;
    }
    const std::vector<unsigned char> iv(
        blob.begin() + static_cast<std::ptrdiff_t>(kFileMagicBytes),
        blob.begin() + static_cast<std::ptrdiff_t>(kFileMagicBytes + kFileIvBytes));
    const std::string cipher = blob.substr(kFileMagicBytes + kFileIvBytes);
    if (!AesGcmDecrypt(key, iv, cipher, out)) {
        if (error != nullptr) *error = "解密失败（密钥不对、或文件被改过）";
        return false;
    }
    return true;
}

}  // namespace dchat
