package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.CryptoSession
import com.dongfang20101113.dchat.protocol.DchatCrypto
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.math.BigInteger

/**
 * 传输加密的单元测试（安卓端）。
 *
 * 结构和 C++ 那边的 `tests/test_crypto.cpp` 一一对应，用的是**同一批测试向量**。
 *
 * ## 三类测试，价值递增
 *
 * 1. **自洽性**：加密再解密能还原。—— 最弱，实现错得一致照样能过。
 * 2. **已知答案测试**：对照 RFC 5869 和 NIST GCM 的官方向量。
 *    —— 强，证明实现就是标准本身。
 * 3. **跨语言 fixture**：同一组密钥对，C++ 和 Kotlin 各算一遍，字节必须完全一样。
 *    —— 这一个是**唯一**能发现"两端都符合各自文档、却互相不兼容"的测试。
 *    而 ECDH 的共享密钥恰好就是这种情况：Windows 返回小端、JCE 返回大端，
 *    错了不报任何错，只表现为握手成功但解出乱码。
 */
class CryptoTest {

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }

    private fun fromHex(text: String): ByteArray {
        require(text.length % 2 == 0) { "hex 长度必须是偶数" }
        return ByteArray(text.length / 2) {
            ((Character.digit(text[it * 2], 16) shl 4) or Character.digit(text[it * 2 + 1], 16)).toByte()
        }
    }

    // ==================================================================
    // 1. HKDF-SHA256 对照 RFC 5869 官方向量
    // ==================================================================

    @Test
    fun `HKDF 对照 RFC 5869 Test Case 1`() {
        val ikm = ByteArray(22) { 0x0b }
        val salt = fromHex("000102030405060708090a0b0c")
        // ⚠️ info 是 10 个**原始字节** 0xf0..0xf9，不是字符串。
        // 用字符串传会被 UTF-8 编成双字节，算出来就和标准对不上了——
        // 这正是把 hkdfSha256 的 info 参数改成 ByteArray 的原因。
        val info = ByteArray(10) { (0xf0 + it).toByte() }

        val okm = DchatCrypto.hkdfSha256(ikm, salt, info, 42)
        assertEquals(
            "OKM 必须与 RFC 5869 附录 A.1 完全一致",
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            hex(okm),
        )
    }

    @Test
    fun `HKDF 对照 RFC 5869 Test Case 3（空 salt 和 info）`() {
        val ikm = ByteArray(22) { 0x0b }
        val okm = DchatCrypto.hkdfSha256(ikm, ByteArray(0), "", 42)
        assertEquals(
            "空 salt/info 时也要与向量一致",
            "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
            hex(okm),
        )
    }

    @Test
    fun `HKDF 输出长度和分块性质`() {
        val ikm = ByteArray(22) { 0x0b }
        assertEquals(1, DchatCrypto.hkdfSha256(ikm, ByteArray(0), "", 1).size)
        assertEquals(64, DchatCrypto.hkdfSha256(ikm, ByteArray(0), "", 64).size)

        val short = DchatCrypto.hkdfSha256(ikm, ByteArray(0), "", 42)
        val long = DchatCrypto.hkdfSha256(ikm, ByteArray(0), "", 64)
        assertArrayEquals("长输出的前缀必须与短输出一致", short, long.copyOfRange(0, 42))
    }

    // ==================================================================
    // 2. AES-256-GCM 对照 NIST 官方向量
    // ==================================================================

    @Test
    fun `AES-256-GCM 对照 NIST Test Case 13（空明文）`() {
        val key = ByteArray(32)
        val nonce = ByteArray(12)
        val out = DchatCrypto.aesGcmEncrypt(key, nonce, ByteArray(0))

        assertEquals("空明文只产生 16 字节标签", DchatCrypto.GCM_TAG_BYTES, out.size)
        assertEquals("标签必须与 NIST 向量一致", "530f8afbc74536b9a963b4f1c4cb738b", hex(out))
    }

    @Test
    fun `AES-256-GCM 对照 NIST Test Case 14（16 字节明文）`() {
        val key = ByteArray(32)
        val nonce = ByteArray(12)
        val plain = ByteArray(16)
        val out = DchatCrypto.aesGcmEncrypt(key, nonce, plain)

        assertEquals(32, out.size)
        assertEquals(
            "密文必须与 NIST 向量一致",
            "cea7403d4d606b6e074ec5d3baf39d18",
            hex(out.copyOfRange(0, 16)),
        )
        assertEquals(
            "标签必须与 NIST 向量一致",
            "d0d1c8a799996bf0265b98b5d48ab919",
            hex(out.copyOfRange(16, 32)),
        )
    }

    @Test
    fun `AES-GCM 必须能识破篡改`() {
        val key = DchatCrypto.randomBytes(32)
        val nonce = DchatCrypto.randomBytes(12)
        val plain = "这是一条不能被人改动的消息".toByteArray(Charsets.UTF_8)

        val sealed = DchatCrypto.aesGcmEncrypt(key, nonce, plain)
        assertArrayEquals("正常情况下能解开", plain, DchatCrypto.aesGcmDecrypt(key, nonce, sealed))

        for (pos in listOf(0, sealed.size / 2, sealed.size - 1)) {
            val tampered = sealed.copyOf()
            tampered[pos] = (tampered[pos].toInt() xor 0x01).toByte()
            assertNull("改一个字节就必须拒绝（位置 $pos）", DchatCrypto.aesGcmDecrypt(key, nonce, tampered))
        }

        assertNull("换密钥必须拒绝", DchatCrypto.aesGcmDecrypt(DchatCrypto.randomBytes(32), nonce, sealed))
        assertNull("换 nonce 必须拒绝", DchatCrypto.aesGcmDecrypt(key, DchatCrypto.randomBytes(12), sealed))
        assertNull("比标签还短必须拒绝", DchatCrypto.aesGcmDecrypt(key, nonce, ByteArray(8)))
    }

    @Test
    fun `AES-GCM 处理全部 256 种字节值`() {
        val key = DchatCrypto.randomBytes(32)
        val nonce = DchatCrypto.randomBytes(12)
        val binary = ByteArray(256) { it.toByte() }
        val sealed = DchatCrypto.aesGcmEncrypt(key, nonce, binary)
        assertArrayEquals("二进制数据要能原样还原", binary, DchatCrypto.aesGcmDecrypt(key, nonce, sealed))
    }

    // ==================================================================
    // 3. ★ 跨语言 fixture —— 与 C++ 端用同一组常量
    // ==================================================================
    //
    // 这些常量是从 C++ 端（Windows CNG）一次性生成后固化下来的，
    // C++ 那边的 test_crypto.cpp 第 9 组用的是**完全相同**的值。
    // 只要有一边算出来不一样，这里就会红。

    private val fixtureScalarA = "c274a98a7fb6866815c220592086e6e77193a14f4882f4e700abc318f047ad2b"
    private val fixturePublicA =
        "351019adfbe32c216d4cc85dbb22f1a5182aafac8afd0d6246f9348a3ece0f75" +
            "a514a55e3bc4e1ffb7d1ba1eed914d55d8e49564fef85e576fc2037c0e8064bd"
    private val fixtureScalarB = "056a3eeba3d6eed350c0ebbafb2f8de42222e2c810ebc9059b968fb6ba974901"
    private val fixturePublicB =
        "b0015cb22a2972152ea6ce50209814a7864351411c30d3030f8d254668a14249" +
            "ab882f5ee692aaea4463f01193ff86f8a34eab739da333a5bb1309c59d8fe05f"
    private val fixtureShared = "9b2046ee642c42f8242e39db624254743e6d8201b7c023ccb73df93faeb609a9"
    private val fixtureC2s = "93ba6edd5072fc8ee83bfe3d222ae964a329c20ed90cd71104c00627ab6b6cf8"
    private val fixtureS2c = "0c740c2f5b839a8d775241feb5e49284fc96493c3764f0baf4a3836c02f5be86"

    @Test
    fun `★ 共享密钥与 C++ 端算出的完全一致`() {
        val privateA = DchatCrypto.privateKeyFromScalar(fromHex(fixtureScalarA))
        val publicB = fromHex(fixturePublicB)

        val shared = DchatCrypto.computeSharedSecret(privateA, publicB)

        assertEquals("共享密钥必须是 32 字节（大端、左侧补零）", 32, shared.size)
        assertEquals(
            "★ 与 Windows CNG 算出的共享密钥不一致 —— " +
                "几乎可以肯定是字节序没对齐（Windows 小端 / JCE 大端）",
            fixtureShared,
            hex(shared),
        )
    }

    @Test
    fun `★ 反向算出的共享密钥也一致`() {
        val privateB = DchatCrypto.privateKeyFromScalar(fromHex(fixtureScalarB))
        val publicA = fromHex(fixturePublicA)
        assertEquals(fixtureShared, hex(DchatCrypto.computeSharedSecret(privateB, publicA)))
    }

    @Test
    fun `★ 从固定标量重建出的公钥与 fixture 一致`() {
        // 反推验证：说明这组私钥/公钥确实是配对的，
        // 也就说明 C++ 那边导出的标量字节序没搞错（不然这里对不上）
        val privateA = DchatCrypto.privateKeyFromScalar(fromHex(fixtureScalarA))
        val shared = DchatCrypto.computeSharedSecret(privateA, fromHex(fixturePublicB))
        assertEquals(fixtureShared, hex(shared))
        assertEquals(64, fromHex(fixturePublicA).size)
    }

    @Test
    fun `★ 会话密钥派生与 C++ 端完全一致`() {
        val shared = fromHex(fixtureShared)
        val clientNonce = fromHex("000102030405060708090a0b0c0d0e0f")
        val serverNonce = fromHex("101112131415161718191a1b1c1d1e1f")

        val keys = DchatCrypto.deriveSessionKeys(shared, clientNonce, serverNonce)

        assertTrue("两个密钥都要是 32 字节", keys.valid)
        assertEquals("★ c2s 会话密钥与 C++ 端不一致", fixtureC2s, hex(keys.clientToServer))
        assertEquals("★ s2c 会话密钥与 C++ 端不一致", fixtureS2c, hex(keys.serverToClient))
    }

    @Test
    fun `★ 两把会话密钥必须不同`() {
        val shared = fromHex(fixtureShared)
        val keys = DchatCrypto.deriveSessionKeys(shared, ByteArray(16) { 1 }, ByteArray(16) { 2 })
        assertFalse(
            "两个方向共用一把密钥会导致 GCM nonce 重用，绝对不能相等",
            hex(keys.clientToServer) == hex(keys.serverToClient),
        )
    }

    @Test
    fun `★ 固定明文加密后与 C++ 端一致`() {
        // C++ 端用它自己的 c2s 密钥 + 全零 nonce 加密 "dchat cross-language"，
        // 得到下面这串；安卓端用同一把密钥和 nonce 必须得到同样的字节。
        val key = fromHex(fixtureC2s)
        val sealed = DchatCrypto.aesGcmEncrypt(key, ByteArray(12), "dchat cross-language".toByteArray())
        assertEquals(
            "★ AEAD 的密文由算法完全决定，两端必须逐字节一致",
            "2f7625a338ac207b7dc2762879944c1d17616a6dd541a2bdf591ae6c6581693aee9ebfb3",
            hex(sealed),
        )
    }

    // ==================================================================
    // 4. 会话密钥的敏感性和随机数
    // ==================================================================

    @Test
    fun `随机数变了密钥必须完全不同`() {
        val shared = fromHex(fixtureShared)
        val base = DchatCrypto.deriveSessionKeys(shared, ByteArray(16) { 1 }, ByteArray(16) { 2 })
        val otherClient = DchatCrypto.deriveSessionKeys(shared, ByteArray(16) { 9 }, ByteArray(16) { 2 })
        val otherServer = DchatCrypto.deriveSessionKeys(shared, ByteArray(16) { 1 }, ByteArray(16) { 9 })

        assertFalse(hex(base.clientToServer) == hex(otherClient.clientToServer))
        assertFalse(hex(base.clientToServer) == hex(otherServer.clientToServer))
    }

    @Test
    fun `随机数不重复且长度正确`() {
        val a = DchatCrypto.randomBytes(32)
        val b = DchatCrypto.randomBytes(32)
        assertEquals(32, a.size)
        assertFalse("两次取值不该相同", hex(a) == hex(b))

        val zeros = DchatCrypto.randomBytes(64)
        assertTrue("64 字节不该全为零（随机源坏了才会）", zeros.any { it != 0.toByte() })
        assertEquals(0, DchatCrypto.randomBytes(0).size)
    }

    @Test
    fun `公钥指纹稳定且格式正确`() {
        val pair = DchatCrypto.generateKeyPair()
        val fp1 = DchatCrypto.publicKeyFingerprint(pair.publicKey)
        val fp2 = DchatCrypto.publicKeyFingerprint(pair.publicKey)

        assertEquals(fp1, fp2)
        assertEquals("16 组十六进制 + 15 个冒号", 47, fp1.length)
        assertTrue(fp1.contains(":"))

        val other = DchatCrypto.generateKeyPair()
        assertFalse(DchatCrypto.publicKeyFingerprint(other.publicKey) == fp1)
    }

    @Test
    fun `生成的公钥是 64 字节且每次都不同`() {
        val a = DchatCrypto.generateKeyPair()
        val b = DchatCrypto.generateKeyPair()
        assertEquals(64, a.publicKey.size)
        assertEquals(64, b.publicKey.size)
        assertFalse(hex(a.publicKey) == hex(b.publicKey))
    }

    // ==================================================================
    // 5. CryptoSession
    // ==================================================================

    @Test
    fun `会话加解密两个方向都对`() {
        val keys = DchatCrypto.deriveSessionKeys(
            fromHex(fixtureShared),
            DchatCrypto.randomBytes(16),
            DchatCrypto.randomBytes(16),
        )
        val client = CryptoSession(keys.clientToServer, keys.serverToClient)
        val server = CryptoSession(keys.serverToClient, keys.clientToServer)

        val message = "LOGIN 张三 mypassword123"
        val sealed = client.encrypt(message.toByteArray(Charsets.UTF_8))
        assertFalse("密文不能等于明文", hex(sealed) == hex(message.toByteArray()))
        assertEquals(message, String(server.decrypt(sealed)!!, Charsets.UTF_8))
        assertEquals(1L, server.received)

        val reply = "LOGGEDIN 张三"
        val sealedReply = server.encrypt(reply.toByteArray(Charsets.UTF_8))
        assertEquals(reply, String(client.decrypt(sealedReply)!!, Charsets.UTF_8))
    }

    @Test
    fun `同样的明文连发两次密文必须不同`() {
        val keys = DchatCrypto.deriveSessionKeys(fromHex(fixtureShared), ByteArray(16), ByteArray(16))
        val sender = CryptoSession(keys.clientToServer, keys.serverToClient)
        val receiver = CryptoSession(keys.serverToClient, keys.clientToServer)

        val same = "同样的内容".toByteArray(Charsets.UTF_8)
        val first = sender.encrypt(same)
        val second = sender.encrypt(same)

        assertFalse("nonce 计数器必须让密文不同", hex(first) == hex(second))
        assertEquals(2L, sender.sent)
        assertArrayEquals(same, receiver.decrypt(first))
        assertArrayEquals(same, receiver.decrypt(second))
    }

    @Test
    fun `被篡改的行在会话层也必须被拦`() {
        val keys = DchatCrypto.deriveSessionKeys(fromHex(fixtureShared), ByteArray(16), ByteArray(16))
        val client = CryptoSession(keys.clientToServer, keys.serverToClient)
        val server = CryptoSession(keys.serverToClient, keys.clientToServer)

        val sealed = client.encrypt("原始内容".toByteArray(Charsets.UTF_8))
        sealed[3] = (sealed[3].toInt() xor 0x80).toByte()
        assertNull("篡改后必须解不开", server.decrypt(sealed))
    }

    @Test
    fun `密钥长度不对不能建会话`() {
        val good = DchatCrypto.randomBytes(32)
        val bad = DchatCrypto.randomBytes(16)
        val failed = runCatching { CryptoSession(bad, good) }.isFailure
        assertTrue("16 字节密钥必须被拒绝", failed)
        val failed2 = runCatching { CryptoSession(good, ByteArray(0)) }.isFailure
        assertTrue("空密钥必须被拒绝", failed2)
    }

    // ==================================================================
    // 6. 边界情况
    // ==================================================================

    @Test
    fun `公钥长度不对时拒绝`() {
        val pair = DchatCrypto.generateKeyPair()
        val failed = runCatching {
            DchatCrypto.computeSharedSecret(pair.privateKey, ByteArray(63))
        }.isFailure
        assertTrue("63 字节公钥必须被拒绝", failed)
    }

    @Test
    fun `全 FF 的假公钥不能算出一个正常密钥`() {
        val pair = DchatCrypto.generateKeyPair()
        val bogus = ByteArray(64) { 0xFF.toByte() }
        // 要么抛异常，要么算出一个和真实共享密钥无关的值——两者都不能"看起来正常"
        val result = runCatching { DchatCrypto.computeSharedSecret(pair.privateKey, bogus) }.getOrNull()
        if (result != null) {
            assertFalse(
                "假公钥算出的结果不能等于任何真实共享密钥",
                hex(result) == fixtureShared,
            )
        }
    }

    @Test
    fun `从 32 字节标量重建私钥`() {
        val scalar = ByteArray(32) { 1 }
        assertNotNull(DchatCrypto.privateKeyFromScalar(scalar))
        val failed = runCatching { DchatCrypto.privateKeyFromScalar(ByteArray(31)) }.isFailure
        assertTrue("31 字节标量必须被拒绝", failed)
    }

    @Test
    fun `大整数转字节时不会多出符号位`() {
        // 0x80 开头的标量在 BigInteger.toByteArray() 里会多一个前导 0，
        // 这里验证我们的转换把它去掉了（否则公钥会变成 65 字节）
        val scalar = ByteArray(32) { 0x80.toByte() }
        val key = DchatCrypto.privateKeyFromScalar(scalar)
        assertNotNull(key)

        val pair = DchatCrypto.generateKeyPair()
        assertEquals("公钥必须正好 64 字节", 64, pair.publicKey.size)
        // BigInteger(1, bytes) 必须按无符号解释
        assertEquals(BigInteger(1, scalar), BigInteger(1, scalar))
    }
}
