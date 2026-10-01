package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.MAX_LINE_BYTES
import com.dongfang20101113.dchat.protocol.MAX_NICK_CHARS
import com.dongfang20101113.dchat.protocol.NickError
import com.dongfang20101113.dchat.protocol.buildLine
import com.dongfang20101113.dchat.protocol.formatTime
import com.dongfang20101113.dchat.protocol.looksLikeTime
import com.dongfang20101113.dchat.protocol.normalizeNick
import com.dongfang20101113.dchat.protocol.parseLine
import com.dongfang20101113.dchat.protocol.utf8CharCount
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 协议层单测。逐条对齐 C++ 端 `test_protocol` 的覆盖点。
 *
 * 这些用例的重点不是"能跑通"，而是**边界**：命令名里的下划线、协议注入、
 * 中文按码点计数、超长行不会把多字节字符切一半。
 */
class DchatProtocolTest {

    // ------------------------------------------------------------------
    // 命令行构造
    // ------------------------------------------------------------------

    @Test
    fun `命令名允许下划线_否则文件协议会变成空行`() {
        // C++ 端专门注释过这个坑：只允许字母的话 BuildLine("FILE_DATA") 返回空串，
        // 转发出去就是一个空行，文件传输整个失效。
        assertEquals("FILE_DATA abc", buildLine("FILE_DATA", "abc"))
        assertEquals("FILE_CHUNK x y", buildLine("FILE_CHUNK", "x y"))
        assertEquals("FILE_THUMB_GET F1", buildLine("FILE_THUMB_GET", "F1"))
    }

    @Test
    fun `命令名会被转成大写`() {
        assertEquals("MSG 你好", buildLine("msg", "你好"))
        assertEquals("FILE_SEND x 1", buildLine("file_send", "x 1"))
    }

    @Test
    fun `非法命令名返回空串_调用方必须当作不要发送`() {
        assertEquals("", buildLine("", "x"))
        assertEquals("", buildLine("BAD CMD", "x"))   // 空格
        assertEquals("", buildLine("BAD-CMD", "x"))   // 连字符
        assertEquals("", buildLine("BAD!CMD", "x"))   // 标点
    }

    @Test
    fun `协议注入被剥掉_换行不会伪造出第二条消息`() {
        assertEquals("MSG 你好世界", buildLine("MSG", "你好\r\n世界"))
        assertEquals("MSG ab", buildLine("MSG", "a\rb"))
        assertEquals("MSG ab", buildLine("MSG", "a\nb"))
    }

    @Test
    fun `没有参数时不会多出一个尾随空格`() {
        assertEquals("LIST", buildLine("LIST"))
        assertEquals("PING", buildLine("PING"))
        assertEquals("QUIT", buildLine("QUIT"))
    }

    @Test
    fun `超长行按 UTF-8 字节截断_且不会切坏多字节字符`() {
        val long = "あ".repeat(3000) // 每个 3 字节，共 9000 字节
        val line = buildLine("MSG", long)
        val bytes = line.toByteArray(Charsets.UTF_8)
        assertTrue("截断后应不超过 $MAX_LINE_BYTES 字节，实际 ${bytes.size}", bytes.size <= MAX_LINE_BYTES)
        // 不能出现替换字符（说明字符被切了一半）
        assertFalse("不应该出现 U+FFFD 替换字符", line.contains('\uFFFD'))
        // 必须能从 UTF-8 正确解回来
        val decoded = String(bytes, Charsets.UTF_8)
        assertFalse(decoded.contains('\uFFFD'))
    }

    // ------------------------------------------------------------------
    // 行解析
    // ------------------------------------------------------------------

    @Test
    fun `解析一行_命令转大写_rest 去首尾空白`() {
        val m = parseLine("  say 21:05 alice hello  ")
        assertEquals("SAY", m.command)
        assertEquals("21:05 alice hello", m.rest)
        assertEquals(listOf("21:05", "alice", "hello"), m.words())
    }

    @Test
    fun `空行和纯空白行解析成空命令`() {
        assertTrue(parseLine("").isEmpty)
        assertTrue(parseLine("   ").isEmpty)
        assertTrue(parseLine("\r\n").isEmpty)
    }

    @Test
    fun `Words 不会产生空词`() {
        val m = parseLine("FILE_SEND   id    name   1024")
        assertEquals(listOf("id", "name", "1024"), m.words())
    }

    // ------------------------------------------------------------------
    // UTF-8 计数
    // ------------------------------------------------------------------

    @Test
    fun `按码点计数_不是按字节也不是按 UTF-16 长度`() {
        assertEquals(3, utf8CharCount("abc"))
        assertEquals(2, utf8CharCount("中文"))
        // emoji 是代理对：UTF-16 长度 2，但码点只有 1
        assertEquals(1, utf8CharCount("😀"))
        assertEquals(0, utf8CharCount(""))
    }

    // ------------------------------------------------------------------
    // 昵称
    // ------------------------------------------------------------------

    @Test
    fun `昵称长度按码点算_12 个中文可以通过`() {
        val twelve = "一二三四五六七八九十一二"
        assertEquals(12, utf8CharCount(twelve))
        assertEquals(twelve to NickError.NONE, normalizeNick(twelve))

        val thirteen = "一二三四五六七八九十一二三"
        assertEquals(13, utf8CharCount(thirteen))
        assertEquals("" to NickError.TOO_LONG, normalizeNick(thirteen))
        assertEquals(12, MAX_NICK_CHARS)
    }

    @Test
    fun `空昵称被拒`() {
        assertEquals("" to NickError.EMPTY, normalizeNick(""))
        assertEquals("" to NickError.EMPTY, normalizeNick("    "))
    }

    @Test
    fun `昵称非法字符被拒`() {
        for (bad in listOf("a b", "a,b", "a:b", "a/b", "a<b", "a>b", "a\"b", "a'b", "a|b", "a\\b")) {
            assertEquals("非法字符应被拒: $bad", NickError.ILLEGAL_CHAR, normalizeNick(bad).second)
        }
    }

    @Test
    fun `昵称允许中文_emoji_下划线_短横线`() {
        for (ok in listOf("小明", "alice_01", "user-name", "玩家😀")) {
            assertEquals("应通过: $ok", NickError.NONE, normalizeNick(ok).second)
        }
    }

    @Test
    fun `昵称首尾空白会被去掉`() {
        assertEquals("alice" to NickError.NONE, normalizeNick("  alice  "))
    }

    // ------------------------------------------------------------------
    // 时间字段
    // ------------------------------------------------------------------

    @Test
    fun `时间格式化补零`() {
        assertEquals("09:05", formatTime(9, 5))
        assertEquals("00:00", formatTime(0, 0))
        assertEquals("23:59", formatTime(23, 59))
    }

    @Test
    fun `时间越界会绕回`() {
        assertEquals("00:00", formatTime(24, 60))
        assertEquals("23:59", formatTime(-1, -1))
        assertEquals("01:30", formatTime(25, 90))
    }

    @Test
    fun `LooksLikeTime 必须严格是 hh mm`() {
        assertTrue(looksLikeTime("09:05"))
        assertTrue(looksLikeTime("23:59"))
        assertFalse("缺前导零不算", looksLikeTime("9:05"))
        assertFalse("小时越界", looksLikeTime("24:00"))
        assertFalse("分钟越界", looksLikeTime("09:60"))
        assertFalse("长度不对", looksLikeTime("09:5"))
        assertFalse("分隔符不对", looksLikeTime("09-05"))
        assertFalse("非数字", looksLikeTime("ab:cd"))
    }
}
