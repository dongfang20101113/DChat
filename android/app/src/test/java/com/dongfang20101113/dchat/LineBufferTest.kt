package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.LineBuffer
import com.dongfang20101113.dchat.protocol.MAX_LINE_BYTES
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * TCP 字节流切行测试。
 *
 * 半包/粘包是真实网络里**必然发生**的事（一次 recv 拿到的可能是一行的一半，
 * 也可能是五行），桌面端专门为它写了测试；这里逐条对齐。
 */
class LineBufferTest {

    @Test
    fun `半包_先收到半行时不吐出任何行`() {
        val buffer = LineBuffer()
        buffer.append("SAY 21:0".toByteArray(Charsets.UTF_8))
        assertNull("没有换行符，不应该有完整行", buffer.popLine())
        assertFalse(buffer.bad)

        buffer.append("5 alice 你好\n".toByteArray(Charsets.UTF_8))
        assertEquals("SAY 21:05 alice 你好", buffer.popLine())
        assertNull("只有一行，取完就没了", buffer.popLine())
    }

    @Test
    fun `粘包_一次收到多行要能逐行取出`() {
        val buffer = LineBuffer()
        buffer.append("NAMES 21:05 a,b,c\nPONG 21:05\n".toByteArray(Charsets.UTF_8))
        assertEquals("NAMES 21:05 a,b,c", buffer.popLine())
        assertEquals("PONG 21:05", buffer.popLine())
        assertNull(buffer.popLine())
    }

    @Test
    fun `兼容 CRLF 结尾`() {
        val buffer = LineBuffer()
        buffer.append("PING\r\n".toByteArray(Charsets.UTF_8))
        assertEquals("PING", buffer.popLine())
    }

    @Test
    fun `空行也是一个合法的行`() {
        val buffer = LineBuffer()
        buffer.append("\nPING\n".toByteArray(Charsets.UTF_8))
        assertEquals("", buffer.popLine())
        assertEquals("PING", buffer.popLine())
    }

    @Test
    fun `逐字节喂入也能正确切行`() {
        val buffer = LineBuffer()
        val payload = "SAY 21:05 小明 你好世界\n"
        val lines = mutableListOf<String>()
        for (byte in payload.toByteArray(Charsets.UTF_8)) {
            buffer.append(byteArrayOf(byte))
            buffer.popLine()?.let { lines.add(it) }
        }
        assertEquals(listOf("SAY 21:05 小明 你好世界"), lines)
    }

    @Test
    fun `迟迟没有换行且超过上限_判定为异常数据`() {
        val buffer = LineBuffer()
        buffer.append(ByteArray(MAX_LINE_BYTES + 1) { 'a'.code.toByte() })
        assertTrue("无换行且超限应置 bad", buffer.bad)
        assertNull(buffer.popLine())
    }

    @Test
    fun `恰好等于上限且带换行_是合法的`() {
        val buffer = LineBuffer()
        val body = "a".repeat(MAX_LINE_BYTES)
        buffer.append((body + "\n").toByteArray(Charsets.UTF_8))
        val line = buffer.popLine()
        assertEquals(MAX_LINE_BYTES, line?.length)
        assertFalse(buffer.bad)
    }

    @Test
    fun `置 bad 之后不再接受数据`() {
        val buffer = LineBuffer()
        buffer.append(ByteArray(MAX_LINE_BYTES + 1) { 'a'.code.toByte() })
        assertTrue(buffer.bad)
        buffer.append("PING\n".toByteArray(Charsets.UTF_8))
        assertNull("bad 状态下不应吐出任何行", buffer.popLine())
    }

    @Test
    fun `reset 可以清掉 bad 状态`() {
        val buffer = LineBuffer()
        buffer.append(ByteArray(MAX_LINE_BYTES + 1) { 'a'.code.toByte() })
        assertTrue(buffer.bad)
        buffer.reset()
        assertFalse(buffer.bad)
        buffer.append("PING\n".toByteArray(Charsets.UTF_8))
        assertEquals("PING", buffer.popLine())
    }

    @Test
    fun `大量小包不会丢数据_模拟真实分片`() {
        val buffer = LineBuffer()
        val lines = (1..200).map { "SAY 21:05 alice 消息$it" }
        val stream = lines.joinToString("\n") + "\n"
        val bytes = stream.toByteArray(Charsets.UTF_8)

        val got = mutableListOf<String>()
        var offset = 0
        // 每次只喂 7 字节，模拟最恶劣的分片
        while (offset < bytes.size) {
            val end = minOf(offset + 7, bytes.size)
            buffer.append(bytes, offset, end - offset)
            offset = end
            while (true) {
                val line = buffer.popLine() ?: break
                got.add(line)
            }
        }
        assertEquals(lines, got)
    }
}
