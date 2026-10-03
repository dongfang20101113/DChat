package com.dongfang20101113.dchat.protocol

import androidx.compose.ui.graphics.Color
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 彩色文字代码的解析测试。规则必须和桌面端 `src/chat_color.cpp` 完全一致——
 * 两边不一致的话，同一句话在电脑和手机上颜色会不同。
 */
class ChatColorTest {

    private val default = Color(0xFF123456)
    private fun join(spans: List<ChatColor.Span>) = spans.joinToString("") { it.text }

    @Test
    fun `十六进制色码解析成一段且自己不显示`() {
        val spans = ChatColor.parse("#ff0000红字", default, enabled = true)
        assertEquals(1, spans.size)
        assertEquals("红字", spans[0].text)
        assertEquals(Color(0xFFFF0000), spans[0].color)
        assertTrue(spans[0].hasColor)
    }

    @Test
    fun `新色码覆盖旧色码`() {
        val spans = ChatColor.parse("正常#00ff00绿#0000ff蓝", default, enabled = true)
        assertEquals(3, spans.size)
        assertEquals("正常", spans[0].text)
        assertEquals(default, spans[0].color)
        assertEquals(Color(0xFF00FF00), spans[1].color)
        assertEquals(Color(0xFF0000FF), spans[2].color)
    }

    @Test
    fun `十六进制大小写都认`() {
        val spans = ChatColor.parse("#AbCdEfX", default, enabled = true)
        assertEquals(1, spans.size)
        assertEquals(Color(0xFFABCDEF), spans[0].color)
    }

    @Test
    fun `快捷色码`() {
        val spans = ChatColor.parse("&c红&a绿", default, enabled = true)
        assertEquals(2, spans.size)
        assertEquals(ChatColor.quickColor(12), spans[0].color)
        assertEquals(ChatColor.quickColor(10), spans[1].color)
        assertEquals("红", spans[0].text)
        assertEquals("绿", spans[1].text)
    }

    @Test
    fun `认不出来的 and z 原样显示不吞字`() {
        val spans = ChatColor.parse("&z不认识", default, enabled = true)
        assertEquals("&z不认识", join(spans))
        assertEquals(default, spans[0].color)
        assertFalse(spans[0].hasColor)
    }

    @Test
    fun `两个 and 转义成一个 and`() {
        val spans = ChatColor.parse("a&&b", default, enabled = true)
        assertEquals("a&b", join(spans))
    }

    @Test
    fun `服务器关掉彩色后色码原样显示一个字符都不少`() {
        val text = "#ff0000红&a绿&&x"
        val spans = ChatColor.parse(text, default, enabled = false)
        assertEquals(1, spans.size)
        assertEquals(text, spans[0].text)
        assertEquals(default, spans[0].color)
        assertFalse(spans[0].hasColor)
    }

    @Test
    fun `半截色码原样显示`() {
        assertEquals("#ff00", join(ChatColor.parse("#ff00", default, true)))
        assertEquals("#ff00zz", join(ChatColor.parse("#ff00zz", default, true)))
        assertEquals("结尾一个&", join(ChatColor.parse("结尾一个&", default, true)))
    }

    @Test
    fun `去掉色码只留可见文字`() {
        assertEquals("红绿", ChatColor.stripCodes("#ff0000红&a绿"))
        assertEquals("a&b", ChatColor.stripCodes("a&&b"))
        assertEquals("普通文字", ChatColor.stripCodes("普通文字"))
    }

    @Test
    fun `颜色转十六进制`() {
        assertEquals("#ff0000", ChatColor.toHex(Color(0xFFFF0000)))
        assertEquals("#00abcd", ChatColor.toHex(Color(0xFF00ABCD)))
    }

    @Test
    fun `解析用户输入的色码`() {
        assertEquals(Color(0xFFFF0000), ChatColor.parseHex("#ff0000"))
        assertEquals(Color(0xFF00FF00), ChatColor.parseHex("00ff00"))
        assertEquals(Color(0xFFFF0000), ChatColor.parseHex("#f00"))
        assertEquals(Color(0xFF0000FF), ChatColor.parseHex("  #00F  "))
        assertNull(ChatColor.parseHex("#gg0000"))
        assertNull(ChatColor.parseHex("#ff00"))
        assertNull(ChatColor.parseHex(""))
    }

    @Test
    fun `16 个快捷色互不重复`() {
        val seen = mutableSetOf<Color>()
        for (i in 0 until ChatColor.QUICK_COLOR_COUNT) {
            assertTrue("第 $i 个快捷色重复了", seen.add(ChatColor.quickColor(i)))
        }
        assertEquals(16, seen.size)
    }

    @Test
    fun `快捷色的顺序和桌面端一致`() {
        // 这几个是两端约定死的，改了就两端颜色不同
        assertEquals(Color(0xFF000000), ChatColor.quickColor(0))
        assertEquals(Color(0xFFAA0000), ChatColor.quickColor(4))
        assertEquals(Color(0xFFFF5555), ChatColor.quickColor(12))
        assertEquals(Color(0xFFFFFFFF), ChatColor.quickColor(15))
        assertEquals('c', ChatColor.quickColorDigit(12))
    }

    @Test
    fun `帮助文本列出两种写法和对照表`() {
        val help = ChatColor.helpText()
        assertTrue(help.contains("#rrggbb"))
        assertTrue(help.contains("&a"))
        assertTrue(help.contains("#ff0000"))
    }

    @Test
    fun `色板覆盖常见颜色`() {
        assertTrue(ChatColor.palette.size >= 24)
        val colors = ChatColor.palette.map { it.color }.toSet()
        assertTrue(colors.contains(Color(0xFFFF0000)))
        assertTrue(colors.contains(Color(0xFF0000FF)))
        assertTrue(colors.contains(Color(0xFFFFFFFF)))
        assertTrue(colors.contains(Color(0xFF000000)))
    }

    @Test
    fun `片段数量有上限不会无限增长`() {
        val text = buildString { repeat(500) { append("#ff0000a") } }
        val spans = ChatColor.parse(text, default, enabled = true, maxSpans = 8)
        assertTrue("片段数应当受 maxSpans 限制，实际 ${spans.size}", spans.size <= 10)
        // 剩下的文字不能丢：把所有片段拼起来应当等于原始可见文字
        assertEquals(ChatColor.stripCodes(text), join(spans))
    }
}
