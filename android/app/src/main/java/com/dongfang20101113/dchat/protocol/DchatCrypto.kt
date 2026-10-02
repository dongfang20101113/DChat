package com.dongfang20101113.dchat.protocol

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPrivateKeySpec
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * dchat 传输加密（安卓端实现）。
 *
 * 和 C++ 端 `crypto.h` / `crypto.cpp` **必须逐字节一致**，否则握手会"成功"但解出来是乱码
 * ——这是最难排查的一类问题，所以两边的实现都配了官方测试向量，
 * 还有一条跨语言的互操作测试盯着。
 *
 * ## 用的都是 JCE 自带的原语
 *
 * ECDH（`KeyAgreement` / `secp256r1`）、SHA-256、HMAC-SHA256、AES/GCM 全部来自 JCE。
 * 自己写的只有 HKDF 的拼接流程——HKDF 本身就是"两次 HMAC"的构造（RFC 5869），
 * 不涉及任何密码学设计。
 *
 * ## ⚠️ 字节序：两端最容易对不上的地方
 *
 * Windows CNG 的 `BCRYPT_KDF_RAW_SECRET` 返回的共享密钥是**小端序**，
 * 而 JCE 的 `KeyAgreement.generateSecret()` 返回的是**大端序**（且左侧补零到 32 字节）。
 * C++ 那边做了翻转，这里保持大端不动。**改任何一边都必须同步改另一边。**
 */
object DchatCrypto {

    /** P-256 公钥的裸表示：X || Y，各 32 字节大端。 */
    const val P256_PUBLIC_KEY_BYTES = 64

    /** AES-256 密钥长度。 */
    const val AES_KEY_BYTES = 32

    /** GCM 的 nonce 长度（标准就是 12 字节）。 */
    const val GCM_NONCE_BYTES = 12

    /** GCM 认证标签长度。 */
    const val GCM_TAG_BYTES = 16

    /** 握手随机数长度。 */
    const val HANDSHAKE_NONCE_BYTES = 16

    /** 握手协议版本；双方对不上就退回明文。 */
    const val CRYPTO_VERSION = 1

    /** HKDF 的 info 字符串（分方向）。 */
    const val HKDF_INFO_CLIENT_TO_SERVER = "dchat-v1-c2s"
    const val HKDF_INFO_SERVER_TO_CLIENT = "dchat-v1-s2c"

    private const val CURVE = "secp256r1"
    private const val COORD_BYTES = 32

    private val secureRandom = SecureRandom()

    // ------------------------------------------------------------------
    // 随机数
    // ------------------------------------------------------------------

    fun randomBytes(count: Int): ByteArray {
        require(count >= 0) { "count 不能为负" }
        val out = ByteArray(count)
        if (count > 0) secureRandom.nextBytes(out)
        return out
    }

    // ------------------------------------------------------------------
    // ECDH P-256
    // ------------------------------------------------------------------

    /** 一次性密钥对。[publicKey] 是 64 字节 X||Y，可以直接上线。 */
    class EcdhKeyPair(
        val privateKey: java.security.PrivateKey,
        val publicKey: ByteArray,
    )

    /** 曲线参数。反复取比较贵，缓存一份。 */
    private val p256Params: ECParameterSpec by lazy {
        AlgorithmParameters.getInstance("EC").apply {
            init(ECGenParameterSpec(CURVE))
        }.getParameterSpec(ECParameterSpec::class.java)
    }

    fun generateKeyPair(): EcdhKeyPair {
        val generator = KeyPairGenerator.getInstance("EC")
        generator.initialize(ECGenParameterSpec(CURVE), secureRandom)
        val pair = generator.generateKeyPair()
        val public = pair.public as java.security.interfaces.ECPublicKey
        return EcdhKeyPair(pair.private, encodePoint(public.w))
    }

    /**
     * 用自己的私钥和对方的公钥（64 字节 X||Y）算共享密钥。
     *
     * 返回值是 **32 字节大端**的 x 坐标，和 C++ 端翻转之后的表示一致。
     * 公钥长度不对、或者不是曲线上的合法点，都会抛异常（调用方当作握手失败）。
     */
    fun computeSharedSecret(privateKey: java.security.PrivateKey, peerPublicKey: ByteArray): ByteArray {
        require(peerPublicKey.size == P256_PUBLIC_KEY_BYTES) {
            "公钥必须是 $P256_PUBLIC_KEY_BYTES 字节，实际 ${peerPublicKey.size}"
        }
        val peerKey = KeyFactory.getInstance("EC").generatePublic(
            ECPublicKeySpec(decodePoint(peerPublicKey), p256Params),
        )
        val agreement = KeyAgreement.getInstance("ECDH")
        agreement.init(privateKey)
        agreement.doPhase(peerKey, true)
        val secret = agreement.generateSecret()

        // JCE 返回的已经是固定长度的大端数组；这里再兜一道，
        // 万一将来某个 JDK 实现返回的是不补零的短数组，也能对齐到 32 字节。
        return when {
            secret.size == COORD_BYTES -> secret
            secret.size < COORD_BYTES -> ByteArray(COORD_BYTES - secret.size) + secret
            else -> secret.copyOfRange(secret.size - COORD_BYTES, secret.size)
        }
    }

    /** 从 32 字节大端标量重建私钥（测试和 fixture 用）。 */
    fun privateKeyFromScalar(scalar: ByteArray): java.security.PrivateKey {
        require(scalar.size == COORD_BYTES) { "标量必须是 $COORD_BYTES 字节" }
        return KeyFactory.getInstance("EC").generatePrivate(
            ECPrivateKeySpec(BigInteger(1, scalar), p256Params),
        )
    }

    private fun encodePoint(point: ECPoint): ByteArray =
        toFixed(toBigEndian(point.affineX), COORD_BYTES) + toFixed(toBigEndian(point.affineY), COORD_BYTES)

    private fun decodePoint(raw: ByteArray): ECPoint {
        val x = BigInteger(1, raw.copyOfRange(0, COORD_BYTES))
        val y = BigInteger(1, raw.copyOfRange(COORD_BYTES, P256_PUBLIC_KEY_BYTES))
        return ECPoint(x, y)
    }

    /** BigInteger 的 toByteArray 会带符号位，这里去掉多余的前导零字节。 */
    private fun toBigEndian(value: BigInteger): ByteArray {
        val raw = value.toByteArray()
        var start = 0
        while (start < raw.size - 1 && raw[start] == 0.toByte()) start++
        return raw.copyOfRange(start, raw.size)
    }

    private fun toFixed(bytes: ByteArray, size: Int): ByteArray = when {
        bytes.size == size -> bytes
        bytes.size < size -> ByteArray(size - bytes.size) + bytes
        else -> bytes.copyOfRange(bytes.size - size, bytes.size)
    }

    // ------------------------------------------------------------------
    // HKDF-SHA256（RFC 5869）
    // ------------------------------------------------------------------

    /**
     * HKDF-SHA256（RFC 5869）：extract + expand。
     *
     * [info] 特意用 `ByteArray` 而不是 `String`：HKDF 的 info 在标准里就是**任意二进制**
     * （RFC 5869 的测试向量用的是 0xf0..0xf9 这种非 UTF-8 字节）。用字符串传的话，
     * `toByteArray(UTF_8)` 会把它编成双字节序列，算出来的结果和标准对不上——
     * 而且错得很隐蔽，只有对着官方向量才发现得了。
     */
    fun hkdfSha256(ikm: ByteArray, salt: ByteArray, info: ByteArray, outBytes: Int): ByteArray {
        require(outBytes in 1..(255 * 32)) { "输出长度必须在 1..${255 * 32}" }

        // extract：PRK = HMAC(salt, IKM)；salt 为空时按 RFC 用 32 个零字节
        val effectiveSalt = if (salt.isEmpty()) ByteArray(32) else salt
        val prk = hmacSha256(effectiveSalt, ikm)

        // expand：T(i) = HMAC(PRK, T(i-1) || info || i)
        val out = ByteArray(outBytes)
        var previous = ByteArray(0)
        var counter = 1
        var offset = 0
        while (offset < outBytes) {
            val mac = Mac.getInstance("HmacSHA256")
            mac.init(SecretKeySpec(prk, "HmacSHA256"))
            mac.update(previous)
            mac.update(info)
            mac.update(counter.toByte())
            previous = mac.doFinal()

            val take = minOf(previous.size, outBytes - offset)
            System.arraycopy(previous, 0, out, offset, take)
            offset += take
            counter++
        }
        return out
    }

    /** 字符串形式的 info（内部固定常量用这个就够）。 */
    fun hkdfSha256(ikm: ByteArray, salt: ByteArray, info: String, outBytes: Int): ByteArray =
        hkdfSha256(ikm, salt, info.toByteArray(Charsets.UTF_8), outBytes)

    private fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    // ------------------------------------------------------------------
    // 会话密钥
    // ------------------------------------------------------------------

    class SessionKeys(val clientToServer: ByteArray, val serverToClient: ByteArray) {
        val valid: Boolean
            get() = clientToServer.size == AES_KEY_BYTES && serverToClient.size == AES_KEY_BYTES
    }

    /**
     * 从共享密钥和双方的握手随机数派生**两把**会话密钥。
     *
     * 分方向是必须的：GCM 的 nonce 在同一把密钥下绝不能重复，
     * 两个方向共用一把密钥、各自计数器都从 0 开始的话，第 0 条消息就撞了。
     */
    fun deriveSessionKeys(
        sharedSecret: ByteArray,
        clientNonce: ByteArray,
        serverNonce: ByteArray,
    ): SessionKeys {
        val salt = clientNonce + serverNonce
        return SessionKeys(
            clientToServer = hkdfSha256(sharedSecret, salt, HKDF_INFO_CLIENT_TO_SERVER, AES_KEY_BYTES),
            serverToClient = hkdfSha256(sharedSecret, salt, HKDF_INFO_SERVER_TO_CLIENT, AES_KEY_BYTES),
        )
    }

    // ------------------------------------------------------------------
    // AES-256-GCM
    // ------------------------------------------------------------------

    /** 输出是 `密文 || 16 字节标签`，和 C++ 端、和 JCE 的默认行为都一致。 */
    fun aesGcmEncrypt(key: ByteArray, nonce: ByteArray, plaintext: ByteArray): ByteArray {
        require(key.size == AES_KEY_BYTES) { "AES-256 需要 $AES_KEY_BYTES 字节密钥" }
        require(nonce.size == GCM_NONCE_BYTES) { "nonce 必须是 $GCM_NONCE_BYTES 字节" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
        return cipher.doFinal(plaintext)
    }

    /**
     * 解密。**认证失败（被篡改 / 密钥不对 / nonce 不对）返回 null**，
     * 调用方必须当作硬错误，绝不能把半截明文当成功。
     */
    fun aesGcmDecrypt(key: ByteArray, nonce: ByteArray, ciphertext: ByteArray): ByteArray? {
        if (key.size != AES_KEY_BYTES || nonce.size != GCM_NONCE_BYTES) return null
        if (ciphertext.size < GCM_TAG_BYTES) return null
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
            cipher.doFinal(ciphertext)
        } catch (_: Exception) {
            null  // AEADBadTagException：认证没过
        }
    }

    // ------------------------------------------------------------------
    // 指纹
    // ------------------------------------------------------------------

    /** 公钥指纹：SHA-256 前 16 字节，`AA:BB:...` 形式，给 TOFU 用。 */
    fun publicKeyFingerprint(publicKey: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(publicKey)
        val take = minOf(16, digest.size)
        return (0 until take).joinToString(":") { "%02X".format(digest[it]) }
    }
}

/**
 * 一条连接上的加解密会话。
 *
 * nonce 用**计数器**而不是随机数：同一把密钥下计数器保证不重复，
 * 随机数只是"重复概率很低"。配合按方向分密钥，就没有重用风险。
 */
class CryptoSession(sendKey: ByteArray, recvKey: ByteArray) {

    private val sendKey: ByteArray
    private val recvKey: ByteArray
    private var sendCounter = 0L
    private var recvCounter = 0L

    init {
        require(sendKey.size == DchatCrypto.AES_KEY_BYTES) { "发送密钥必须是 32 字节" }
        require(recvKey.size == DchatCrypto.AES_KEY_BYTES) { "接收密钥必须是 32 字节" }
        this.sendKey = sendKey
        this.recvKey = recvKey
    }

    val sent: Long get() = sendCounter
    val received: Long get() = recvCounter

    fun encrypt(plaintext: ByteArray): ByteArray {
        val out = DchatCrypto.aesGcmEncrypt(sendKey, nonceFor(sendCounter), plaintext)
        sendCounter++
        return out
    }

    fun decrypt(ciphertext: ByteArray): ByteArray? {
        val out = DchatCrypto.aesGcmDecrypt(recvKey, nonceFor(recvCounter), ciphertext) ?: return null
        recvCounter++
        return out
    }

    private fun nonceFor(counter: Long): ByteArray {
        // 12 字节 = 4 字节前缀 0 + 8 字节大端计数器。
        // 同一把密钥下计数器只增不减，所以 nonce 一定不重复。
        val nonce = ByteArray(DchatCrypto.GCM_NONCE_BYTES)
        for (i in 0 until 8) {
            nonce[DchatCrypto.GCM_NONCE_BYTES - 1 - i] = ((counter shr (8 * i)) and 0xFF).toByte()
        }
        return nonce
    }
}
