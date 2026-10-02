package com.dongfang20101113.dchat.protocol

/**
 * 语音消息。
 *
 * ## 和贴纸同一套路：复用文件通道
 *
 * 语音就是一段**录音文件**，所以和贴纸一样走现成的文件通道，
 * 只是把"种类"标记设成 `voice` 而不是 `sticker`。
 * 限速、大小限制、过期清理、加密——全部原样复用。
 *
 * ## 为什么把「种类」放在同一格字段里
 *
 * 贴纸那一版把标记放在 `FILE_OFFER` 的**第 6 个字段**（`0`/`1`）。
 * 语音如果另起一格，就会出现"既是贴纸又是语音"这种没有意义的组合，
 * 而且每加一种附件就要往后加一格、越加越长。
 *
 * 所以改成**同一个字段表达种类**：
 *
 *     FILE_OFFER <时间> <昵称> <文件ID> <文件名B64> <字节数> [缩略图] [种类]
 *
 *     种类：0 / file = 普通文件；1 / sticker = 贴纸；voice = 语音消息
 *
 * 这样和已经发出去的贴纸实现**完全兼容**（`1` 仍然是贴纸、`0` 仍然是普通文件），
 * 而以后再加"视频消息""位置"之类也只是多一个取值。
 */
enum class AttachmentKind {
    FILE,
    STICKER,
    VOICE,
    ;

    /** 线上写法。`FILE` 用 `0` 是为了和已经发布的格式保持一致。 */
    val wireValue: String
        get() = when (this) {
            FILE -> "0"
            STICKER -> "1"
            VOICE -> "voice"
        }
}

object VoiceMessage {

    /** 录音文件的最大时长（秒）。 */
    const val MAX_DURATION_SECONDS: Int = 5 * 60

    /** 单个语音文件的大小上限。 */
    const val MAX_BYTES: Long = 2L * 1024 * 1024

    /**
     * 允许的录音格式。
     *
     * 只收 Android `MediaRecorder` 直接能产出的那几种，外加 opus/ogg
     * （桌面端和别处常见）。刻意不收 wav：未压缩的一分钟就是 5 MB 以上，
     * 走公网太奢侈，而且完全没有必要。
     */
    private val ALLOWED_EXTENSIONS = setOf("m4a", "aac", "mp4", "ogg", "opus", "3gp", "amr")

    fun looksLikeAudio(fileName: String): Boolean {
        val dot = fileName.lastIndexOf('.')
        if (dot <= 0 || dot == fileName.length - 1) return false
        return fileName.substring(dot + 1).lowercase() in ALLOWED_EXTENSIONS
    }

    /**
     * 这段录音能不能发？返回 `null` 表示可以，否则是给用户看的原因。
     *
     * 纯函数：录音结束的那一刻就能判断，不用等上传完被服务端拒绝。
     */
    fun whyCannotSend(fileName: String, sizeBytes: Long, durationSeconds: Int): String? {
        if (durationSeconds <= 0) return "这段录音是空的"
        if (durationSeconds > MAX_DURATION_SECONDS) {
            return "语音最长 ${formatDuration(MAX_DURATION_SECONDS)}，这段有 " +
                formatDuration(durationSeconds)
        }
        if (!looksLikeAudio(fileName)) {
            return "这个格式不能当语音发（支持 m4a / aac / ogg / opus 等）"
        }
        if (sizeBytes <= 0) return "录音文件是空的"
        if (sizeBytes > MAX_BYTES) {
            return "语音文件最大 ${MAX_BYTES / 1024 / 1024} MB，这段有 ${formatBytes(sizeBytes)}" +
                "——可能是录音质量设得太高了"
        }
        return null
    }

    /**
     * 把秒数格式化成 `0:07` / `1:23` 这种样子。
     *
     * 和微信一致：**分钟不补零、秒补零**。负数夹成 0——
     * 界面上偶尔会拿到 -1（表示"时长未知"），显示成 `0:00` 比 `-1:-1` 好。
     */
    fun formatDuration(seconds: Int): String {
        val total = if (seconds < 0) 0 else seconds
        return "${total / 60}:${(total % 60).toString().padStart(2, '0')}"
    }

    /**
     * 从 `FILE_OFFER` 的种类字段解析出附件类型。
     *
     * **缺字段一律当普通文件**：老服务器不会发这一格，
     * 那时按文件卡片显示才是对的（用户点一下还能下载）。
     */
    fun parseKind(word: String?): AttachmentKind = when (word?.lowercase()) {
        "1", "sticker" -> AttachmentKind.STICKER
        "voice" -> AttachmentKind.VOICE
        else -> AttachmentKind.FILE
    }

    /** 上传时追加在 `FILE_SEND` 末尾的种类标记。 */
    fun fileSendSuffix(kind: AttachmentKind): String =
        if (kind == AttachmentKind.FILE) "" else " ${kind.wireValue}"
}
