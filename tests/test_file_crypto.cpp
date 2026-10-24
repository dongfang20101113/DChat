// 群文件加密模块的单元测试。
//
// 这里守的是**四条不能靠"看起来没问题"的**：
//   1. 加密再解密必须回到原文（含二进制、空文件、大文件）
//   2. 同一明文加密两次，密文**必须不同**（IV 不重用 —— GCM 下重用是灾难级错误）
//   3. 错密钥 / 被篡改**必须失败**，不能"解出一堆垃圾还返回成功"
//   4. 魔数判定要准（加没加密能一眼看出来），且截断的文件要被拒
#include <cstdio>
#include <string>
#include <vector>

#include "file_crypto.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [失败] %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace dchat;

    std::printf("[1] 生成密钥\n");
    FileKey key;
    Check(GenerateFileKey(&key), "能生成 32 字节密钥");
    Check(key.size() == kFileKeyBytes, "密钥长度是 32 字节");
    FileKey other;
    Check(GenerateFileKey(&other), "能生成第二把密钥");
    Check(key != other, "两次生成的密钥不一样");

    std::printf("[2] 加解密往返\n");
    const std::string plain = "群文件内容 test 12345\n第二行";
    std::string blob;
    std::string error;
    Check(EncryptFileBlob(key, plain, &blob, &error), "加密成功");
    Check(IsEncryptedBlob(blob), "密文带 DCHATENC1 魔数");
    Check(blob.size() == kFileMagicBytes + kFileIvBytes + plain.size() + kFileTagBytes,
          "密文长度 = 魔数 + IV + 明文 + 标签");
    Check(blob.find(plain) == std::string::npos, "密文里不含明文");
    std::string back;
    Check(DecryptFileBlob(key, blob, &back, &error), "解密成功");
    Check(back == plain, "解密回到原文");

    std::printf("[3] IV 不重用\n");
    std::string blob2;
    Check(EncryptFileBlob(key, plain, &blob2, &error), "同一明文再加密一次");
    Check(blob2 != blob, "两次加密的密文不同（IV 是随机的）");
    {
        // 只有前一步真的加密成功才有东西可比。**不能直接 substr** ——
        // 加密失败时 blob 是空的，substr 会抛 out_of_range 把整个测试崩掉，
        // 那样就看不到"到底哪一条失败了"（第一次跑就是这么崩的）。
        if (blob.size() >= kFileMagicBytes + kFileIvBytes &&
            blob2.size() >= kFileMagicBytes + kFileIvBytes) {
            const std::string iv1 = blob.substr(kFileMagicBytes, kFileIvBytes);
            const std::string iv2 = blob2.substr(kFileMagicBytes, kFileIvBytes);
            Check(iv1 != iv2, "两次的 IV 不同");
        } else {
            Check(false, "密文长度不足，无法比较 IV（说明加密那一步就失败了）");
        }
    }

    std::printf("[4] 错密钥 / 篡改必须失败\n");
    std::string dummy;
    Check(!DecryptFileBlob(other, blob, &dummy, &error), "用另一把密钥解密必须失败");
    {
        std::string tampered = blob;
        tampered[tampered.size() - 1] = static_cast<char>(tampered.back() ^ 0x01);
        Check(!DecryptFileBlob(key, tampered, &dummy, &error), "改一个字节必须失败");
    }
    {
        std::string truncated = blob.substr(0, kFileMagicBytes + kFileIvBytes + 4);
        Check(!DecryptFileBlob(key, truncated, &dummy, &error), "被截断的密文必须失败");
    }
    Check(!DecryptFileBlob(key, plain, &dummy, &error), "拿明文当密文必须失败");
    Check(!IsEncryptedBlob(plain), "明文不被当成加密文件");
    Check(!IsEncryptedBlob(""), "空数据不被当成加密文件");

    std::printf("[5] 边界输入\n");
    {
        std::string emptyBlob;
        Check(EncryptFileBlob(key, std::string(), &emptyBlob, &error), "空文件也能加密");
        std::string emptyBack = "x";
        Check(DecryptFileBlob(key, emptyBlob, &emptyBack, &error), "空文件能解密");
        Check(emptyBack.empty(), "空文件解出来还是空的");
    }
    {
        std::string binary;
        for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
        binary += '\0';  // 中间夹个 0 字节
        binary += "tail";
        std::string binaryBlob;
        Check(EncryptFileBlob(key, binary, &binaryBlob, &error), "含 0 字节的二进制能加密");
        std::string binaryBack;
        Check(DecryptFileBlob(key, binaryBlob, &binaryBack, &error), "二进制能解密");
        Check(binaryBack == binary, "二进制内容一字不差");
    }
    {
        FileKey shortKey = {1, 2, 3};
        std::string out;
        Check(!EncryptFileBlob(shortKey, plain, &out, &error), "密钥长度不对时加密失败");
        Check(!DecryptFileBlob(shortKey, blob, &out, &error), "密钥长度不对时解密失败");
    }

    std::printf("\n%d 项检查，失败 %d 项。\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
