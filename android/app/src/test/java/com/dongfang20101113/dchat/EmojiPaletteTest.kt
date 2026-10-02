package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.EmojiPalette
import com.dongfang20101113.dchat.protocol.countTextLines
import com.dongfang20101113.dchat.protocol.escapeText
import com.dongfang20101113.dchat.protocol.unescapeText
import com.dongfang20101113.dchat.protocol.utf8CharCount
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * emoji 调色板。
 *
 * 这里真正要盯住的是**和服务器文本限制的配合**：
 * 一个 emoji 在 UTF-16 里可能是两个 char、在 UTF-8 里是四个字节，
 * 但服务端按 **Unicode 码点**计数。算错了会导致"明明没超却被拒"，
 * 或者"显示出来才发现超了"。这条链路在 TextLimitsTest 里也验过，
 * 这里再确认一遍 emoji 是能安全穿过转义和长度统计的。
 */
class EmojiPaletteTest {

    @Test
    fun `调色板非空且分组都有内容`() {
        assertTrue("至少要有一组", EmojiPalette.groups.isNotEmpty())
        for (group in EmojiPalette.groups) {
            assertTrue("「${group.title}」不该是空的", group.emojis.isNotEmpty())
        }
        assertEquals(
            "all 应该是所有分组的拉平结果",
            EmojiPalette.groups.sumOf { it.emojis.size },
            EmojiPalette.all.size,
        )
    }

    @Test
    fun `调色板里没有重的 emoji`() {
        val duplicates = EmojiPalette.all.groupingBy { it }.eachCount().filter { it.value > 1 }
        assertTrue("重复的 emoji 会让用户以为点错了：$duplicates", duplicates.isEmpty())
    }

    @Test
    fun `调色板里没有空串或纯空白`() {
        for (emoji in EmojiPalette.all) {
            assertFalse("不该有空项", emoji.isBlank())
        }
    }

    // ------------------------------------------------------------------
    // 插入
    // ------------------------------------------------------------------

    @Test
    fun `插到末尾`() {
        val (text, cursor) = EmojiPalette.insert("你好", "😀", 2)
        assertEquals("你好😀", text)
        assertEquals(4, cursor)  // "😀" 在 UTF-16 里是 2 个 char
    }

    @Test
    fun `插到中间`() {
        val (text, cursor) = EmojiPalette.insert("你好世界", "😀", 2)
        assertEquals("你好😀世界", text)
        assertEquals(4, cursor)
    }

    @Test
    fun `插到开头`() {
        val (text, cursor) = EmojiPalette.insert("你好", "🎉", 0)
        assertEquals("🎉你好", text)
        assertEquals(2, cursor)
    }

    @Test
    fun `空文本也能插`() {
        val (text, cursor) = EmojiPalette.insert("", "🚀", 0)
        assertEquals("🚀", text)
        assertEquals(2, cursor)
    }

    @Test
    fun `光标越界会被夹住而不是崩掉`() {
        // 界面上的光标索引偶尔会因为异步重组而越界，必须兜住
        val (negative, _) = EmojiPalette.insert("abc", "😀", -5)
        assertEquals("😀abc", negative)

        val (beyond, _) = EmojiPalette.insert("abc", "😀", 999)
        assertEquals("abc😀", beyond)
    }

    // ------------------------------------------------------------------
    // 和服务器的文本限制配合
    // ------------------------------------------------------------------

    @Test
    fun `emoji 只算一个字符`() {
        // 服务端按 Unicode 码点计数，一个 emoji 就是 1
        assertEquals(1, utf8CharCount("😀"))
        assertEquals(3, utf8CharCount("😀a🎉"))
        // 但 UTF-16 长度是 2 —— 直接用 String.length 会算多
        assertEquals(2, "😀".length)
    }

    @Test
    fun `emoji 能安全穿过转义`() {
        val text = "第一行 😀\n第二行 🎉"
        val escaped = escapeText(text)

        assertFalse("转义后不该有真换行", escaped.contains('\n'))
        assertEquals("还原后必须一字不差", text, unescapeText(escaped))
        assertEquals("行数统计不受 emoji 影响", 2, countTextLines(escaped))
        // 服务端统计的是"还原后"的码点数
        assertEquals(utf8CharCount(text), utf8CharCount(unescapeText(escaped)))
    }

    @Test
    fun `一段全是 emoji 的消息长度按码点算`() {
        val emojis = EmojiPalette.all.take(10).joinToString("")
        assertEquals("10 个 emoji 就是 10 个码点", 10, utf8CharCount(emojis))
        assertTrue("UTF-16 长度会更长", emojis.length > 10)
    }

    @Test
    fun `带变体选择符的 emoji 也能穿过往返`() {
        // ❤️ 这类是"基础字符 + 变体选择符"的组合，容易被截断弄坏
        val withVariation = "❤️"
        assertEquals(withVariation, unescapeText(escapeText(withVariation)))
        assertTrue("这类 emoji 至少占一个码点", utf8CharCount(withVariation) >= 1)
    }

    // ------------------------------------------------------------------
    // 布局常量
    // ------------------------------------------------------------------

    @Test
    fun `每行上限是合理的`() {
        assertTrue("上限太小会显得很碎", EmojiPalette.MAX_PER_ROW >= 6)
        assertTrue("上限太大在窄屏上会挤", EmojiPalette.MAX_PER_ROW <= 12)
    }
}
