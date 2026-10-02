package com.dongfang20101113.dchat.protocol

/**
 * 服务器身份的可信状态判定（TOFU：Trust On First Use）。
 *
 * ## 这是在解决什么问题
 *
 * ECDH 本身是匿名的：它保证"这条通道没有第三方在被动偷听"，
 * 但**不保证对面真的是你以为的那台服务器**。攻击者如果能劫持连接，
 * 可以分别和两边各做一次握手，全程转发——双方都以为自己在和对方说话。
 *
 * TOFU 的思路（SSH 就是这么做的）：**第一次**连接时把服务器公钥指纹记下来，
 * 以后每次连接都比对。指纹变了就大声警告——因为正常服务器换密钥是极少见的事，
 * 而中间人攻击必然导致指纹变化。
 *
 * ## 它挡住了什么、挡不住什么
 *
 * | 场景 | TOFU |
 * | --- | --- |
 * | 服务器换了密钥（重装/换机）| ✅ 会被发现（虽然是无害的变化）|
 * | 连过一次之后再被中间人劫持 | ✅ **会被发现** |
 * | **第一次连接就被劫持** | ❌ 挡不住（这正是"首次使用"的代价）|
 *
 * 最后一行是 TOFU 的固有局限，不是实现缺陷。要补上它只能靠
 * 带外核对指纹（比如当面念给对方听），或者用证书体系。
 */
sealed interface TrustDecision {
    /** 这条连接没加密——谈不上信任，用户应当知道自己在裸奔。 */
    data object NotEncrypted : TrustDecision

    /** 第一次连这台服务器（或者服务器以前没启用加密）：记下指纹。 */
    data class FirstUse(val fingerprint: String) : TrustDecision

    /** 和上次记下的一致：一切正常。 */
    data class Unchanged(val fingerprint: String) : TrustDecision

    /**
     * **指纹变了**。可能是服务器换了密钥，也可能有人在中间。
     *
     * 界面必须把这件事**显眼地**告诉用户，而不是悄悄接受新指纹——
     * 悄悄接受等于 TOFU 完全没做。
     */
    data class Changed(val previous: String, val current: String) : TrustDecision
}

/**
 * 根据"上次记下的指纹"和"这次连上的指纹"判断该信任到什么程度。
 *
 * 纯函数：不碰存储、不碰网络，所以各种分支都能在 JVM 单测里跑。
 *
 * @param known 上次为这个服务器记下的指纹（null 表示没记过）
 * @param current 这次握手拿到的指纹（null 表示这次连接没加密）
 */
fun decideTrust(known: String?, current: String?): TrustDecision {
    // 没加密：谈不上信任。即使以前记过指纹也一样——
    // 服务器突然"不支持加密"本身就可能是降级攻击。
    if (current.isNullOrEmpty()) return TrustDecision.NotEncrypted

    if (known.isNullOrEmpty()) return TrustDecision.FirstUse(current)

    return if (known.equals(current, ignoreCase = true)) {
        TrustDecision.Unchanged(current)
    } else {
        TrustDecision.Changed(previous = known, current = current)
    }
}

/** 指纹的展示形式：每两组之间加个分隔，方便人眼分段比对。 */
fun formatFingerprint(fingerprint: String): String = fingerprint

/**
 * 给用户看的一句话结论。
 *
 * 措辞上刻意分开"首次"和"变了"：前者是正常的，后者需要用户认真对待，
 * 不能都用同一句"已连接"糊过去。
 */
fun describeTrust(decision: TrustDecision): String = when (decision) {
    is TrustDecision.NotEncrypted -> "未加密：这条连接是明文，公网上会被看到"
    is TrustDecision.FirstUse -> "首次连接，已记下服务器指纹 ${decision.fingerprint}"
    is TrustDecision.Unchanged -> "服务器身份已确认（指纹未变）"
    is TrustDecision.Changed ->
        "⚠ 服务器指纹变了！之前是 ${decision.previous}，现在是 ${decision.current}。" +
            "可能只是服务器换了密钥，也可能有人在中间。请先跟服务器管理员核对。"
}
