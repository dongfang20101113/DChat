package com.dongfang20101113.dchat.protocol

/**
 * 表情输入用的 emoji 调色板。
 *
 * ## 为什么 emoji 不需要改协议
 *
 * emoji 就是 Unicode 字符，跟着普通文本走就行——协议是 UTF-8 的，
 * 什么都不用动。这一点和"贴纸"完全不同：贴纸是图片，得走文件通道 +
 * 一个新的标记命令。所以这一块（emoji）成本极低、收益立竿见影。
 *
 * ## 分组是刻意的
 *
 * 一个几十个 emoji 的扁平列表找起来很累，按用途分组后基本一眼就能扫到。
 * 分组也方便以后加"常用"一栏。
 */
object EmojiPalette {

    /** 一组 emoji。[title] 用于无障碍描述，也用于界面上的分组提示。 */
    data class Group(val title: String, val emojis: List<String>)

    val groups: List<Group> = listOf(
        Group(
            "表情",
            listOf(
                "😀", "😄", "😁", "😂", "🤣", "😊", "😍", "🤔",
                "😅", "😭", "😡", "😱", "🥳", "😴", "🤯", "🙄",
            ),
        ),
        Group(
            "手势",
            listOf(
                "👍", "👎", "👌", "✌️", "🙏", "👏", "🤝", "💪",
                "👋", "🤙", "👀", "🫡",
            ),
        ),
        Group(
            "常见",
            listOf(
                "❤️", "🔥", "🎉", "✅", "❌", "⚠️", "💡", "⭐",
                "🚀", "🐛", "☕", "🌙",
            ),
        ),
    )

    /** 所有 emoji 拉平成一个列表（测试和查找用）。 */
    val all: List<String> = groups.flatMap { it.emojis }

    /**
     * 把一个 emoji 插到光标处的文本里。
     *
     * 纯函数：给定原文本和插入位置，返回插入后的文本和新光标位置。
     * 光标位置的边界情况（负数、超出长度）都夹到合法范围——
     * 界面上的光标索引偶尔会因为异步重组而越界，这里兜住比崩掉好。
     */
    fun insert(text: String, emoji: String, cursor: Int): Pair<String, Int> {
        val safeCursor = cursor.coerceIn(0, text.length)
        val before = text.substring(0, safeCursor)
        val after = text.substring(safeCursor)
        return (before + emoji + after) to (safeCursor + emoji.length)
    }

    /**
     * 两个 emoji 之间的最大允许间距（字符数）。
     *
     * 界面上是横向滚动的一条，不设上限时某些字体渲染下会挤成一团。
     */
    const val MAX_PER_ROW: Int = 8
}
