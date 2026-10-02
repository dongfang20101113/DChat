package com.dongfang20101113.dchat.protocol

/**
 * 贴纸（sticker）的协议约定。
 *
 * ## 为什么不新增一条传输通道
 *
 * 贴纸就是**小图片**，所以完全复用现成的文件通道：先按普通文件上传，
 * 只是多带一个"这是贴纸"的标记。好处是：
 *
 * - 服务端几乎不用改（只是把标记透传下去）
 * - 断点、限速、大小限制、过期清理这些**已经做好且测过的机制全部复用**
 * - 不需要为图片再发明一套分块协议
 *
 * ## 线上格式（在现有命令上**追加**一个可选字段）
 *
 *     C -> S   FILE_SEND <本地ID> <文件名B64> <字节数> [sticker]
 *     S -> C   FILE_OFFER <时间> <昵称> <文件ID> <文件名B64> <字节数> [缩略图0/1] [sticker0/1]
 *
 * ⚠️ 两个标记都**只能追加在末尾**，不能插到中间：
 * 老客户端按位置读到第 5 个字段就停了，插入会让它们把标记当成尺寸解析。
 * 这是"只追加不重排"这条约定在文件通道上的又一次应用
 * （RULES 行当初也是这么扩的）。
 *
 * ## 贴纸和普通文件的区别只在渲染
 *
 * 收到贴纸后客户端做两件不同的事：
 *   1. **自动下载**（普通文件要用户点「下载」）——贴纸很小，等用户点就失去意义了
 *   2. **内联画成大图**（普通文件显示成卡片）
 *
 * 传输层完全一样，所以这里只需要一个标记位。
 */
object StickerProtocol {

    /** `FILE_SEND` / `FILE_OFFER` 上表示"这是贴纸"的标记词。 */
    const val MARKER = "sticker"

    /**
     * 贴纸的大小上限（字节）。
     *
     * **和文档大小限制是两回事**：`documentsize` 管的是"用户能传多大的文件"，
     * 那是给压缩包、视频用的；贴纸是聊天里随手发的，超过 512 KB 就说明
     * 用户选错了图（比如发了一张照片当贴纸），应当提示而不是默默传。
     */
    const val MAX_STICKER_BYTES: Long = 512L * 1024

    /**
     * 允许当贴纸的扩展名。
     *
     * 只收图片：贴纸的意义就是"一眼看到"，收个 zip 当贴纸毫无意义。
     * 用扩展名判断而不是嗅探文件头，是因为**在客户端选完文件的那一刻就要给出提示**，
     * 那时还没读内容；服务端也不做二次校验（它不认识图片格式，
     * 而且恶意用户本来就能改扩展名——真正的防线是"贴纸也会被当普通文件存下来"，
     * 不会有额外的执行风险）。
     */
    private val ALLOWED_EXTENSIONS = setOf(
        "png", "jpg", "jpeg", "gif", "webp", "bmp",
    )

    /** 这个文件名能不能当贴纸。[fileName] 是**已经清理过**的文件名。 */
    fun looksLikeImage(fileName: String): Boolean {
        val dot = fileName.lastIndexOf('.')
        if (dot <= 0 || dot == fileName.length - 1) return false
        return fileName.substring(dot + 1).lowercase() in ALLOWED_EXTENSIONS
    }

    /**
     * 这个文件能不能作为贴纸发出？返回 `null` 表示可以，否则是给用户看的原因。
     *
     * 纯函数：界面在用户选完文件后立刻调用，不用等上传完再被服务端拒绝。
     */
    fun whyCannotBeSticker(fileName: String, sizeBytes: Long): String? {
        if (!looksLikeImage(fileName)) {
            return "贴纸只能是图片（png / jpg / gif / webp / bmp）"
        }
        if (sizeBytes <= 0) return "这个文件是空的"
        if (sizeBytes > MAX_STICKER_BYTES) {
            val limitKb = MAX_STICKER_BYTES / 1024
            val actualKb = sizeBytes / 1024
            return "贴纸最大 ${limitKb} KB，这张有 ${actualKb} KB——选一张小一点的图，或者按普通文件发"
        }
        return null
    }

    /**
     * 判断**某一个字段**是不是贴纸标记。
     *
     * 刻意只收一个字段而不是整行：`FILE_OFFER` 在解析时时间前缀已经被剥掉了，
     * 用"整行的第 7 位"这种写法会让调用方算错索引——这种错位不会报错，
     * 只会让贴纸静默地变成普通文件。让调用方明确指定是哪个字段，就不会错。
     *
     * **`null`（字段不存在）一律当"不是贴纸"**：老服务器根本不会发这个标记，
     * 那种情况下按普通文件卡片显示才是对的（用户点一下还能下载）。
     */
    fun isStickerFlag(word: String?): Boolean {
        if (word == null) return false
        return word == "1" || word.equals(MARKER, ignoreCase = true)
    }

    /** 生成 `FILE_SEND` 末尾该追加的标记（不是贴纸就返回空串）。 */
    fun fileSendSuffix(isSticker: Boolean): String = if (isSticker) " $MARKER" else ""

    /** 生成 `FILE_OFFER` 里表示贴纸的那一位。 */
    fun offerFlag(isSticker: Boolean): String = if (isSticker) "1" else "0"
}
