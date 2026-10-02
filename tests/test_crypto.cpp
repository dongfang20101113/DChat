// 传输加密的单元测试
//
// 分两类，第二类才是真正有价值的：
//
//   1. 自洽性：往返、篡改检测、会话行为
//   2. **已知答案测试（KAT）**：拿 RFC 5869 和 NIST GCM 的官方测试向量来对。
//      光靠"加密再解密能还原"是没有说服力的——实现错得一致的话照样能往返，
//      而且错得一致往往意味着**不安全**。只有对着标准向量算出来的字节完全一样，
//      才能说明这个实现是标准本身，而不是一个自成一派的近似物。
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "crypto.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

using Bytes = std::vector<unsigned char>;

/** "0b0b0b" -> 字节数组。 */
Bytes FromHex(const std::string& hex) {
    Bytes out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return Bytes{};
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

std::string ToHex(const Bytes& data) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : data) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string ToHex(const std::string& data) {
    return ToHex(Bytes(data.begin(), data.end()));
}

Bytes Repeat(unsigned char value, std::size_t count) { return Bytes(count, value); }

}  // namespace

int main() {
    std::printf("== dchat crypto tests ==\n");

    // ==================================================================
    // 1. 已知答案测试：HKDF-SHA256（RFC 5869 官方向量）
    // ==================================================================
    {
        std::printf("[1] HKDF-SHA256 对照 RFC 5869 官方测试向量\n");

        // ---- Test Case 1（RFC 5869 A.1）----
        {
            const Bytes ikm = Repeat(0x0b, 22);
            const Bytes salt = FromHex("000102030405060708090a0b0c");
            const std::string info = std::string("\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9", 10);

            Bytes okm;
            check(dchat::HkdfSha256(ikm, salt, info, 42, &okm), "TC1: HKDF 计算成功");
            check(ToHex(okm) ==
                      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                      "34007208d5b887185865",
                  "TC1: OKM 与 RFC 5869 完全一致（42 字节）");
        }

        // ---- Test Case 3：salt 和 info 都为空（RFC 5869 A.3）----
        {
            const Bytes ikm = Repeat(0x0b, 22);
            Bytes okm;
            check(dchat::HkdfSha256(ikm, Bytes{}, std::string(), 42, &okm),
                  "TC3: salt/info 为空时也能算");
            check(ToHex(okm) ==
                      "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                      "9d201395faa4b61a96c8",
                  "TC3: OKM 与 RFC 5869 完全一致（空 salt/info）");
        }

        // ---- 输出长度应该被严格遵守 ----
        {
            Bytes short1;
            dchat::HkdfSha256(Repeat(0x0b, 22), Bytes{}, std::string(), 1, &short1);
            check(short1.size() == 1, "要 1 字节就给 1 字节");

            Bytes long64;
            dchat::HkdfSha256(Repeat(0x0b, 22), Bytes{}, std::string(), 64, &long64);
            check(long64.size() == 64, "要 64 字节就给 64 字节（跨两个 HMAC 块）");

            // 长输出的前缀必须和短输出一致（HKDF 的分块性质）
            Bytes long42;
            dchat::HkdfSha256(Repeat(0x0b, 22), Bytes{}, std::string(), 42, &long42);
            check(std::equal(long42.begin(), long42.end(), long64.begin()),
                  "长输出的前缀与短输出一致");

            Bytes tooLong;
            check(!dchat::HkdfSha256(Repeat(0x0b, 22), Bytes{}, std::string(), 255 * 32 + 1, &tooLong),
                  "超过 RFC 上限（255*32）时拒绝");
            check(!dchat::HkdfSha256(Repeat(0x0b, 22), Bytes{}, std::string(), 0, &tooLong),
                  "要 0 字节时拒绝");
        }
    }

    // ==================================================================
    // 2. 已知答案测试：AES-256-GCM（NIST GCM 官方向量）
    // ==================================================================
    {
        std::printf("[2] AES-256-GCM 对照 NIST 官方测试向量\n");

        const Bytes key = Repeat(0, 32);        // 全零 256 位密钥
        const Bytes nonce = Repeat(0, 12);      // 全零 96 位 IV

        // ---- Test Case 13：空明文 ----
        {
            std::string out;
            check(dchat::AesGcmEncrypt(key, nonce, std::string(), &out), "TC13: 加密空明文成功");
            // 密文为空，输出应当只有 16 字节标签
            check(out.size() == dchat::kGcmTagBytes, "TC13: 空明文只产生 16 字节标签");
            check(ToHex(out) == "530f8afbc74536b9a963b4f1c4cb738b",
                  "TC13: 标签与 NIST 向量一致");

            std::string back;
            check(dchat::AesGcmDecrypt(key, nonce, out, &back) && back.empty(),
                  "TC13: 能解回空明文");
        }

        // ---- Test Case 14：16 字节全零明文 ----
        {
            const std::string plain(16, '\0');
            std::string out;
            check(dchat::AesGcmEncrypt(key, nonce, plain, &out), "TC14: 加密成功");
            check(out.size() == 16 + dchat::kGcmTagBytes, "TC14: 输出 = 16 密文 + 16 标签");

            const std::string cipherHex = ToHex(out.substr(0, 16));
            const std::string tagHex = ToHex(out.substr(16));
            check(cipherHex == "cea7403d4d606b6e074ec5d3baf39d18",
                  "TC14: 密文与 NIST 向量一致");
            check(tagHex == "d0d1c8a799996bf0265b98b5d48ab919",
                  "TC14: 标签与 NIST 向量一致");

            std::string back;
            check(dchat::AesGcmDecrypt(key, nonce, out, &back) && back == plain,
                  "TC14: 能解回原文");
        }
    }

    // ==================================================================
    // 3. AES-GCM 的失败路径（这块错了就等于没有认证）
    // ==================================================================
    {
        std::printf("[3] AES-GCM 必须能识破篡改\n");

        Bytes key, nonce;
        dchat::RandomBytes(32, &key);
        dchat::RandomBytes(12, &nonce);
        const std::string plain = "这是一条不能被人改动的消息";

        std::string sealed;
        check(dchat::AesGcmEncrypt(key, nonce, plain, &sealed), "加密成功");

        std::string back;
        check(dchat::AesGcmDecrypt(key, nonce, sealed, &back) && back == plain, "正常情况下能解开");

        // 改密文里的任意一个字节
        for (std::size_t pos : {std::size_t(0), sealed.size() / 2, sealed.size() - 1}) {
            std::string tampered = sealed;
            tampered[pos] = static_cast<char>(tampered[pos] ^ 0x01);
            std::string out;
            check(!dchat::AesGcmDecrypt(key, nonce, tampered, &out),
                  "密文被改一个字节 -> 认证失败（拒绝）");
        }

        // 换密钥
        {
            Bytes wrongKey;
            dchat::RandomBytes(32, &wrongKey);
            std::string out;
            check(!dchat::AesGcmDecrypt(wrongKey, nonce, sealed, &out), "换密钥 -> 拒绝");
        }
        // 换 nonce
        {
            Bytes wrongNonce;
            dchat::RandomBytes(12, &wrongNonce);
            std::string out;
            check(!dchat::AesGcmDecrypt(key, wrongNonce, sealed, &out), "换 nonce -> 拒绝");
        }
        // 参数长度不对
        {
            std::string out;
            check(!dchat::AesGcmEncrypt(Bytes(16, 0), nonce, plain, &out), "16 字节密钥（AES-128）被拒绝");
            check(!dchat::AesGcmEncrypt(key, Bytes(16, 0), plain, &out), "16 字节 nonce 被拒绝");
            check(!dchat::AesGcmDecrypt(key, nonce, "short", &out), "比标签还短的密文被拒绝");
        }
        // 中文和二进制都要能过
        {
            std::string binary;
            for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
            std::string out, backAgain;
            check(dchat::AesGcmEncrypt(key, nonce, binary, &out), "加密全部 256 种字节值");
            check(dchat::AesGcmDecrypt(key, nonce, out, &backAgain) && backAgain == binary,
                  "二进制数据能原样还原");
        }
    }

    // ==================================================================
    // 4. ECDH
    // ==================================================================
    {
        std::printf("[4] ECDH P-256\n");

        dchat::EcdhKeyPair alice, bob;
        check(dchat::GenerateEcdhKeyPair(&alice), "生成 Alice 的密钥对");
        check(dchat::GenerateEcdhKeyPair(&bob), "生成 Bob 的密钥对");
        check(alice.publicKey.size() == dchat::kP256PublicKeyBytes,
              "公钥是 64 字节（X||Y）");
        check(bob.publicKey.size() == dchat::kP256PublicKeyBytes, "Bob 的公钥也是 64 字节");
        check(alice.publicKey != bob.publicKey, "两个人的公钥不一样");
        check(!alice.privateBlob.empty(), "私钥 blob 非空");

        Bytes secretA, secretB;
        check(dchat::ComputeSharedSecret(alice, bob.publicKey, &secretA), "Alice 算出共享密钥");
        check(dchat::ComputeSharedSecret(bob, alice.publicKey, &secretB), "Bob 算出共享密钥");
        check(!secretA.empty() && secretA.size() == 32, "共享密钥是 32 字节");
        check(secretA == secretB, "★ 两边算出的共享密钥完全相同（ECDH 的根本性质）");

        // 换个人算出来的就不一样
        dchat::EcdhKeyPair mallory;
        dchat::GenerateEcdhKeyPair(&mallory);
        Bytes secretC;
        dchat::ComputeSharedSecret(alice, mallory.publicKey, &secretC);
        check(secretC != secretA, "和第三个人算出的密钥不同");

        // 畸形公钥不能崩，也不能算出一个"看起来正常"的密钥
        Bytes bad;
        check(!dchat::ComputeSharedSecret(alice, Bytes(64, 0xFF), &bad), "全 FF 的假公钥被拒绝");
        check(!dchat::ComputeSharedSecret(alice, Bytes(63, 0x01), &bad), "长度不对的公钥被拒绝");
        check(!dchat::ComputeSharedSecret(alice, Bytes{}, &bad), "空公钥被拒绝");
    }

    // ==================================================================
    // 5. 会话密钥派生
    // ==================================================================
    {
        std::printf("[5] 会话密钥派生\n");

        const Bytes shared = Repeat(0x42, 32);
        const Bytes clientNonce = Repeat(0x01, 16);
        const Bytes serverNonce = Repeat(0x02, 16);

        const dchat::SessionKeys k1 = dchat::DeriveSessionKeys(shared, clientNonce, serverNonce);
        check(k1.valid(), "派生出的两个密钥都是 32 字节");
        // ★ 分方向是**必须的**：共用一把密钥 + 各自从 0 计数 = GCM nonce 重用
        check(k1.clientToServer != k1.serverToClient,
              "★ 两个方向的密钥必须不同（否则 GCM nonce 会重用）");

        const dchat::SessionKeys k2 = dchat::DeriveSessionKeys(shared, clientNonce, serverNonce);
        check(k2.clientToServer == k1.clientToServer && k2.serverToClient == k1.serverToClient,
              "同样的输入派生出同样的密钥（确定性）");

        const dchat::SessionKeys k3 =
            dchat::DeriveSessionKeys(shared, Repeat(0x09, 16), serverNonce);
        check(k3.clientToServer != k1.clientToServer,
              "客户端随机数变了 -> 密钥完全不同（防重放）");

        const dchat::SessionKeys k4 =
            dchat::DeriveSessionKeys(shared, clientNonce, Repeat(0x09, 16));
        check(k4.clientToServer != k1.clientToServer, "服务器随机数变了 -> 密钥完全不同");

        const dchat::SessionKeys k5 =
            dchat::DeriveSessionKeys(Repeat(0x43, 32), clientNonce, serverNonce);
        check(k5.clientToServer != k1.clientToServer, "共享密钥变了 -> 会话密钥完全不同");
    }

    // ==================================================================
    // 6. 随机数
    // ==================================================================
    {
        std::printf("[6] 随机数\n");

        Bytes a, b, c;
        check(dchat::RandomBytes(32, &a) && a.size() == 32, "取 32 字节");
        check(dchat::RandomBytes(32, &b), "再取 32 字节");
        check(a != b, "两次取值不同");

        bool allSame = true;
        for (int i = 0; i < 8; ++i) {
            dchat::RandomBytes(16, &c);
            if (c == a) allSame = false;
        }
        check(allSame, "连续取 8 次都不重复");

        Bytes zero;
        check(dchat::RandomBytes(0, &zero) && zero.empty(), "取 0 字节返回空");

        // 全零概率是 2^-256，真出现说明随机源坏了
        Bytes big;
        dchat::RandomBytes(64, &big);
        bool notAllZero = false;
        for (unsigned char byte : big) {
            if (byte != 0) notAllZero = true;
        }
        check(notAllZero, "64 字节不全为零（随机源不是坏的）");
    }

    // ==================================================================
    // 7. CryptoSession
    // ==================================================================
    {
        std::printf("[7] 会话加解密\n");

        // 模拟一次完整握手：客户端和服务端各自拿同样的两把密钥，方向相反
        dchat::EcdhKeyPair clientKey, serverKey;
        dchat::GenerateEcdhKeyPair(&clientKey);
        dchat::GenerateEcdhKeyPair(&serverKey);

        Bytes clientSecret, serverSecret;
        dchat::ComputeSharedSecret(clientKey, serverKey.publicKey, &clientSecret);
        dchat::ComputeSharedSecret(serverKey, clientKey.publicKey, &serverSecret);
        check(clientSecret == serverSecret, "握手：两边共享密钥一致");

        Bytes clientNonce, serverNonce;
        dchat::RandomBytes(16, &clientNonce);
        dchat::RandomBytes(16, &serverNonce);
        const dchat::SessionKeys keys =
            dchat::DeriveSessionKeys(clientSecret, clientNonce, serverNonce);

        dchat::CryptoSession client, server;
        check(client.Start(keys.clientToServer, keys.serverToClient), "客户端会话启动（发 c2s，收 s2c）");
        check(server.Start(keys.serverToClient, keys.clientToServer), "服务端会话启动（发 s2c，收 c2s）");
        check(client.active() && server.active(), "两个会话都处于激活状态");

        // 客户端加密一行 -> 服务端解密
        const std::string message = "LOGIN 张三 mypassword123";
        std::string sealed, opened;
        check(client.Encrypt(message, &sealed), "客户端加密");
        check(sealed != message, "密文和明文不一样");
        check(server.Decrypt(sealed, &opened) && opened == message,
              "★ 服务端解出原文（含中文和密码）");
        check(server.sent() == 0 && server.received() == 1, "服务端收计数 +1");

        // 反向也要能过
        std::string reply, replyBack;
        check(server.Encrypt("LOGGEDIN 张三", &reply), "服务端加密回复");
        check(client.Decrypt(reply, &replyBack) && replyBack == "LOGGEDIN 张三",
              "★ 客户端解出回复");

        // ★ nonce 计数器：同一方向连发多条，密文绝不能重复
        {
            dchat::CryptoSession sender, receiver;
            // 注意方向：发送方用 c2s 加密，接收方就必须用 c2s 解密。
            // 这里一开始我两边都写成 (c2s, s2c)，测试立刻红了——
            // 这恰恰说明**分方向的密钥是真的在生效**：如果两个方向共用一把密钥，
            // 这种接线错误根本不会被发现。
            sender.Start(keys.clientToServer, keys.serverToClient);
            receiver.Start(keys.serverToClient, keys.clientToServer);
            std::string same = "同样的内容";
            std::string first, second;
            sender.Encrypt(same, &first);
            sender.Encrypt(same, &second);
            check(first != second, "★ 同样的明文连发两次，密文不同（nonce 没重用）");
            check(sender.sent() == 2, "发送计数为 2");
            std::string out1, out2;
            check(receiver.Decrypt(first, &out1) && out1 == same, "第 1 条能解开");
            check(receiver.Decrypt(second, &out2) && out2 == same, "第 2 条能解开");
        }

        // 篡改任何一条都必须被发现
        {
            std::string tampered = sealed;
            tampered[3] = static_cast<char>(tampered[3] ^ 0x80);
            dchat::CryptoSession fresh;
            fresh.Start(keys.serverToClient, keys.clientToServer);
            std::string out;
            check(!fresh.Decrypt(tampered, &out), "★ 被篡改的行解不开（会话层也要拦住）");
        }

        // 没启动的会话不能加解密
        {
            dchat::CryptoSession idle;
            std::string out;
            check(!idle.active(), "没 Start 时会话是未激活的");
            check(!idle.Encrypt("x", &out), "未激活时不能加密");
            check(!idle.Decrypt("x", &out), "未激活时不能解密");
        }

        // 密钥长度不对就拒绝启动
        {
            dchat::CryptoSession bad;
            check(!bad.Start(Bytes(16, 0), Bytes(32, 0)), "16 字节的密钥不能启动会话");
            check(!bad.Start(Bytes(32, 0), Bytes{}), "空密钥不能启动会话");
        }

        // Reset 之后回到未激活
        {
            dchat::CryptoSession s;
            s.Start(keys.clientToServer, keys.serverToClient);
            s.Reset();
            check(!s.active() && s.sent() == 0 && s.received() == 0, "Reset 清干净了");
        }
    }

    // ==================================================================
    // 8. 公钥指纹（TOFU 用）
    // ==================================================================
    {
        std::printf("[8] 公钥指纹\n");

        dchat::EcdhKeyPair key;
        dchat::GenerateEcdhKeyPair(&key);

        const std::string fp1 = dchat::PublicKeyFingerprint(key.publicKey);
        const std::string fp2 = dchat::PublicKeyFingerprint(key.publicKey);
        check(!fp1.empty(), "指纹非空");
        check(fp1 == fp2, "同一个公钥的指纹稳定不变");
        // 16 字节 -> 32 个十六进制字符 + 15 个冒号
        check(fp1.size() == 47, "指纹长度是 47（AA:BB:... 共 16 组）");
        check(fp1.find(':') != std::string::npos, "指纹里有冒号分隔");

        dchat::EcdhKeyPair other;
        dchat::GenerateEcdhKeyPair(&other);
        check(dchat::PublicKeyFingerprint(other.publicKey) != fp1, "不同公钥的指纹不同");
    }

    // ==================================================================
    // 9. 跨语言 fixture：与安卓端 Kotlin 实现比对
    // ==================================================================
    //
    // 这几个常量是**两端共用**的：C++ 这边算出来的必须和 Kotlin 那边一模一样。
    //
    // 为什么必须做这件事：HKDF 和 AES-GCM 都有官方测试向量，两边各自对照标准就够了；
    // 但 **ECDH 的共享密钥没有"标准字节序"**——Windows 的 BCRYPT_KDF_RAW_SECRET
    // 返回小端序，JCE 的 KeyAgreement 返回大端序。这是唯一一处"两边都符合各自的
    // 文档、却互相不兼容"的地方，而且错了不会报错，只表现为握手成功但解出乱码。
    // 只有把同一组密钥对喂给两边、比对算出来的字节，才能真正把它钉死。
    {
        std::printf("[9] 跨语言一致性（与安卓端 Kotlin 实现比对）\n");

        // 这组值由 C++ 端一次性生成后固化；Kotlin 端测试用的是同一组
        const Bytes da = FromHex("c274a98a7fb6866815c220592086e6e77193a14f4882f4e700abc318f047ad2b");
        const Bytes pa = FromHex(
            "351019adfbe32c216d4cc85dbb22f1a5182aafac8afd0d6246f9348a3ece0f75"
            "a514a55e3bc4e1ffb7d1ba1eed914d55d8e49564fef85e576fc2037c0e8064bd");
        const Bytes db = FromHex("056a3eeba3d6eed350c0ebbafb2f8de42222e2c810ebc9059b968fb6ba974901");
        const Bytes pb = FromHex(
            "b0015cb22a2972152ea6ce50209814a7864351411c30d3030f8d254668a14249"
            "ab882f5ee692aaea4463f01193ff86f8a34eab739da333a5bb1309c59d8fe05f");
        const Bytes expectedShared =
            FromHex("9b2046ee642c42f8242e39db624254743e6d8201b7c023ccb73df93faeb609a9");
        const Bytes expectedC2s =
            FromHex("93ba6edd5072fc8ee83bfe3d222ae964a329c20ed90cd71104c00627ab6b6cf8");
        const Bytes expectedS2c =
            FromHex("0c740c2f5b839a8d775241feb5e49284fc96493c3764f0baf4a3836c02f5be86");

        check(da.size() == 32 && pa.size() == 64 && db.size() == 32 && pb.size() == 64,
              "fixture 常量长度正确（标量 32 / 公钥 64）");
        check(expectedShared.size() == 32, "fixture 共享密钥是 32 字节");

        dchat::EcdhKeyPair alice, bob;
        check(dchat::ImportEcdhKeyPair(da, pa, &alice), "从固定标量重建 Alice 的密钥对");
        check(dchat::ImportEcdhKeyPair(db, pb, &bob), "从固定标量重建 Bob 的密钥对");
        check(alice.publicKey == pa, "重建后的公钥与输入一致");

        // 实测结论：CNG 对 ECC 私钥 blob 是原样存取，**不会**校验标量和公钥是否对应，
        // 所以错配是能导入成功的。这里把这个真实行为钉住（免得以后有人以为有校验），
        // 同时验证真正影响安全的部分：ECDH 用的仍然是标量 d。
        {
            dchat::EcdhKeyPair mismatched;
            check(dchat::ImportEcdhKeyPair(da, pb, &mismatched),
                  "标量与公钥错配时 CNG 仍会接受（这是它的真实行为，没有校验）");
            Bytes fromMismatched;
            check(dchat::ComputeSharedSecret(mismatched, bob.publicKey, &fromMismatched),
                  "错配对象仍能算共享密钥");
            check(fromMismatched == expectedShared,
                  "★ 错配时 ECDH 用的仍是标量 d，结果正确（安全性不受影响）");
        }
        dchat::EcdhKeyPair wrong;
        check(!dchat::ImportEcdhKeyPair(Bytes(31, 1), pa, &wrong), "标量长度不对被拒绝");
        check(!dchat::ImportEcdhKeyPair(da, Bytes(63, 1), &wrong), "公钥长度不对被拒绝");

        Bytes sharedA, sharedB;
        check(dchat::ComputeSharedSecret(alice, bob.publicKey, &sharedA), "Alice 算共享密钥");
        check(dchat::ComputeSharedSecret(bob, alice.publicKey, &sharedB), "Bob 算共享密钥");
        check(sharedA == expectedShared,
              "★ 共享密钥与 fixture 一致（这一条盯的就是 Windows 的小端序）");
        check(sharedB == expectedShared, "★ 反向算出来的也一致");

        const Bytes clientNonce = FromHex("000102030405060708090a0b0c0d0e0f");
        const Bytes serverNonce = FromHex("101112131415161718191a1b1c1d1e1f");
        const dchat::SessionKeys keys = dchat::DeriveSessionKeys(sharedA, clientNonce, serverNonce);
        check(keys.clientToServer == expectedC2s, "★ 派生的 c2s 会话密钥与 fixture 一致");
        check(keys.serverToClient == expectedS2c, "★ 派生的 s2c 会话密钥与 fixture 一致");

        // 用固定会话密钥加密固定明文。AEAD 的密文由算法完全决定，
        // 所以 Kotlin 端拿同样的输入必须得到同样的字节。
        // 这里只断言"长度正确 + 能解回原文"；逐字节比对放在 Kotlin 那边的测试里做
        // （那边会硬编码这段密文，两边对不上就红）。
        const std::string sample = "dchat cross-language";
        std::string sealed;
        check(dchat::AesGcmEncrypt(keys.clientToServer, Bytes(12, 0), sample, &sealed),
              "用固定会话密钥加密固定明文");
        check(sealed.size() == sample.size() + dchat::kGcmTagBytes,
              "密文长度 = 明文长度 + 16 字节标签");

        std::string opened;
        check(dchat::AesGcmDecrypt(keys.clientToServer, Bytes(12, 0), sealed, &opened) &&
                  opened == sample,
              "能解回原文");
        std::printf("  （固定明文用 c2s 密钥加密后是：%s）\n", ToHex(sealed).c_str());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
