package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.countTextLines
import com.dongfang20101113.dchat.protocol.escapeText
import com.dongfang20101113.dchat.protocol.parseLine
import com.dongfang20101113.dchat.protocol.unescapeText
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.reduce
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 阶段 1 新增功能：多行文本转义 + 服务器限制（maxtextlen / maxtextlines）。
 *
 * ## 最重要的一条约束：两端必须逐字节一致
 *
 * 转义规则在 C++ 端（`protocol.cpp` 的 `EscapeText`/`UnescapeText`/`CountTextLines`）
 * 和这里各实现了一遍——**两边的测试向量是同一批**。
 * 只要有一边改了规则而另一边没改，服务端的行数统计就会和客户端对不上，
 * 所以这里的用例是照着 C++ 测试（`tests/test_protocol.cpp` 第 6~10 组）抄的。
 */
class TextLimitsTest {

    // ------------------------------------------------------------------
    // 转义
    // ------------------------------------------------------------------

    @Test
    fun `转义_只有真正的特殊字符才会被改`() {
        assertEquals("abc", escapeText("abc"))
        assertEquals("a\\nb", escapeText("a\nb"))          // 换行 -> 反斜杠 n
        assertEquals("a\\nb", escapeText("a\r\nb"))        // CRLF 归一成一个换行
        assertEquals("a\\nb", escapeText("a\rb"))          // 单独的 CR 也算换行
        assertEquals("a\\\\b", escapeText("a\\b"))         // 反斜杠翻倍
        assertEquals("", escapeText(""))
    }

    @Test
    fun `转义_字面的反斜杠加 n 不能被当成换行`() {
        // 用户真的想打「\n」这两个字符时，必须转义成 \\n，否则会被还原成换行
        assertEquals("a\\\\nb", escapeText("a\\nb"))
        assertEquals("a\\nb", unescapeText("a\\\\nb"))
    }

    @Test
    fun `反转义_不认识的转义原样保留`() {
        // 这条很重要：老客户端发来的 Windows 路径不能被吃掉字符
        assertEquals("C:\\x", unescapeText("C:\\x"))
        assertEquals("tail\\", unescapeText("tail\\"))
        assertEquals("abc", unescapeText("abc"))
    }

    @Test
    fun `转义再还原必须和原文一模一样`() {
        val samples = listOf(
            "hello",
            "第一行\n第二行",
            "a\\b",
            "路径 C:\\Users\\test",
            "混合 \\ 和 \n 都有",
            "\n\n开头两个换行",
            "结尾换行\n",
            "emoji 🎉 也要活下来",
        )
        for (sample in samples) {
            assertEquals("「$sample」往返后不一致", sample, unescapeText(escapeText(sample)))
        }
    }

    @Test
    fun `转义后的文本里绝不能有真换行`() {
        // 有真换行就会破坏行式协议的分帧
        val samples = listOf("a\nb", "a\r\nb", "多行\n文本\n在这里", "\r")
        for (sample in samples) {
            val escaped = escapeText(sample)
            assertTrue("「$sample」转义后仍有换行", !escaped.contains('\n') && !escaped.contains('\r'))
        }
    }

    @Test
    fun `多行文本走完整条线协议之后不能丢信息`() {
        // 这是端到端的形态：转义 -> buildLine -> 线上 -> parseLine -> 还原
        val multi = "第一行\n第二行\n第三行"
        val line = com.dongfang20101113.dchat.protocol.makeMessage(escapeText(multi))

        assertTrue("线上不该有真换行", !line.contains('\n'))
        val parsed = parseLine(line)
        assertEquals("MSG", parsed.command)
        assertEquals(multi, unescapeText(parsed.rest))
        assertEquals(3, countTextLines(parsed.rest))
    }

    // ------------------------------------------------------------------
    // 行数统计
    // ------------------------------------------------------------------

    @Test
    fun `行数统计`() {
        assertEquals(0, countTextLines(""))
        assertEquals(1, countTextLines("abc"))
        assertEquals(2, countTextLines("a\\nb"))
        assertEquals(3, countTextLines("a\\nb\\nc"))
    }

    @Test
    fun `行数统计_反斜杠不算换行`() {
        // \\nb 是「字面反斜杠 + n」，只有 1 行
        assertEquals(1, countTextLines("a\\\\nb"))
        // \\ 后面再接一个真换行 \\n，就是 2 行
        assertEquals(2, countTextLines("a\\\\\\nb"))
        // 结尾的孤立反斜杠不能让它崩，也不算换行
        assertEquals(1, countTextLines("end\\"))
    }

    @Test
    fun `行数统计_开头的换行`() {
        // "\n" 单独一个转义换行 = 两个空行
        assertEquals(2, countTextLines("\\n"))
    }

    // ------------------------------------------------------------------
    // RULES 行的解析（含向后兼容）
    // ------------------------------------------------------------------

    @Test
    fun `新服务器的 RULES 有 8 个字段`() {
        val line = ServerLine.parse("RULES 64 500 1 128 256 1000 10", "我")
        assertTrue("应解析成 Rules，实际 $line", line is ServerLine.Rules)
        line as ServerLine.Rules
        assertEquals(64, line.documentSizeMb)
        assertEquals(500, line.chatIntervalMs)
        assertTrue(line.keepChatHistory)
        assertEquals(128, line.uploadRateKbps)
        assertEquals(256, line.downloadRateKbps)
        assertEquals(1000, line.maxTextLength)
        assertEquals(10, line.maxTextLines)
    }

    @Test
    fun `老服务器只发 3 个字段_新字段按不限制处理`() {
        // 这是向后兼容的关键：连老服务器时不能解析失败，也不能把限制弄成非 0
        val line = ServerLine.parse("RULES 32 0 0", "我")
        assertTrue("3 字段的老格式也必须能解析，实际 $line", line is ServerLine.Rules)
        line as ServerLine.Rules
        assertEquals(32, line.documentSizeMb)
        assertEquals(0, line.uploadRateKbps)
        assertEquals(0, line.downloadRateKbps)
        assertEquals(0, line.maxTextLength)
        assertEquals(0, line.maxTextLines)
    }

    @Test
    fun `字段缺失或写坏时退化成不限制_而不是解析失败`() {
        val line = ServerLine.parse("RULES 64 0 0 notanumber 256", "我")
        assertTrue(line is ServerLine.Rules)
        line as ServerLine.Rules
        assertEquals("坏字段应该退化成 0（不限制）", 0, line.uploadRateKbps)
        assertEquals("后面的好字段仍要读出来", 256, line.downloadRateKbps)
    }

    @Test
    fun `RULES 会更新界面状态里的限制`() {
        val s = ChatState().reduce(ServerLine.parse("RULES 128 0 0 64 32 20 3", "我"), "21:05")
        assertEquals(128, s.maxFileMb)
        assertEquals(64, s.uploadRateKbps)
        assertEquals(32, s.downloadRateKbps)
        assertEquals(20, s.maxTextLength)
        assertEquals(3, s.maxTextLines)
        assertTrue("RULES 是控制行，不该进聊天记录", s.items.isEmpty())
    }

    // ------------------------------------------------------------------
    // 发送前的本地校验
    // ------------------------------------------------------------------

    @Test
    fun `没有限制时什么都能发`() {
        val s = ChatState()   // 默认全部 0 = 不限制
        assertNull(s.whyCannotSend(escapeText("随便多长都行".repeat(1000))))
        assertNull(s.whyCannotSend(escapeText("多\n行\n也\n行")))
    }

    @Test
    fun `空消息不能发`() {
        assertNotNull(ChatState().whyCannotSend(""))
    }

    @Test
    fun `超过字符数上限会被本地拦住`() {
        val s = ChatState(maxTextLength = 10)
        assertNull("刚好 10 个字符应该放行", s.whyCannotSend(escapeText("0123456789")))
        val why = s.whyCannotSend(escapeText("01234567890"))
        assertNotNull("11 个字符应该被拦", why)
        assertTrue("提示里要说清限制和实际值：$why", why!!.contains("10") && why.contains("11"))
    }

    @Test
    fun `字符数按 Unicode 码点算_emoji 只算一个`() {
        // "🎉" 在 UTF-16 里是两个 char，直接用 String.length 会算成 2
        val s = ChatState(maxTextLength = 1)
        assertNull("一个 emoji 应该算 1 个字符", s.whyCannotSend(escapeText("🎉")))
        assertNotNull("两个 emoji 就超了", s.whyCannotSend(escapeText("🎉🎉")))
    }

    @Test
    fun `超过行数上限会被本地拦住`() {
        val s = ChatState(maxTextLines = 3)
        assertNull("3 行应该放行", s.whyCannotSend(escapeText("a\nb\nc")))
        val why = s.whyCannotSend(escapeText("a\nb\nc\nd"))
        assertNotNull("4 行应该被拦", why)
        assertTrue("提示里要说清限制和实际值：$why", why!!.contains("3") && why.contains("4"))
    }

    @Test
    fun `字符数和行数同时超限时先报字符数`() {
        val s = ChatState(maxTextLength = 2, maxTextLines = 1)
        val why = s.whyCannotSend(escapeText("aaaa\nbbbb"))
        assertNotNull(why)
        assertTrue("应当先报长度：$why", why!!.contains("字符") || why.contains("太长"))
    }

    @Test
    fun `本地校验用的是转义前的字符数_和转义无关`() {
        // 换行转义后是 2 个字符，但限制数的是"还原后"的 1 个换行
        val s = ChatState(maxTextLength = 3)
        assertNull("「a↵b」是 3 个字符，应该放行", s.whyCannotSend(escapeText("a\nb")))
    }
}
