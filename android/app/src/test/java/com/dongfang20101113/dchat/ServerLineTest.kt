package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.base64Encode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 服务器 → 客户端命令的强类型解析测试。
 *
 * **重点覆盖 README 协议表里漏掉的 6 个命令**：`ANNOUNCE`、`RULES`、
 * `FILE_THUMB`、`FILE_THUMB_GET`、`FILE_THUMB_DATA`、`FILE_THUMB_END`。
 * 如果照文档写客户端，公告会变成未知行、缩略图功能完全接不上。
 */
class ServerLineTest {

    @Test
    fun `RULES 不带时间戳也能解析_这是最容易踩的一个`() {
        // RULES 和 FILE_* 一样属于控制行，服务器发的时候没有套时间前缀。
        // 如果按"先剥 hh:mm"的套路解析，第一段 64 会被当成时间而丢掉，整条解析失败。
        val line = ServerLine.parse("RULES 64 500 1")
        assertTrue("应解析成 Rules，实际 $line", line is ServerLine.Rules)
        line as ServerLine.Rules
        assertEquals(64, line.documentSizeMb)
        assertEquals(500, line.chatIntervalMs)
        assertTrue(line.keepChatHistory)
    }

    @Test
    fun `RULES 的布尔字段支持 0 和 false`() {
        val zero = ServerLine.parse("RULES 128 0 0") as ServerLine.Rules
        assertFalse(zero.keepChatHistory)
        assertEquals(0, zero.chatIntervalMs)

        val word = ServerLine.parse("RULES 128 0 false") as ServerLine.Rules
        assertFalse(word.keepChatHistory)
    }

    @Test
    fun `ANNOUNCE 是公告而不是 SAY`() {
        val line = ServerLine.parse("ANNOUNCE 21:05 服务器维护")
        assertTrue(line is ServerLine.Announce)
        line as ServerLine.Announce
        assertEquals("21:05", line.time)
        assertEquals("服务器维护", line.text)
    }

    @Test
    fun `SAY 解析出气泡需要的全部信息`() {
        val line = ServerLine.parse("SAY 21:05 alice 你好 @bob", selfNick = "bob")
        assertTrue(line is ServerLine.Say)
        line as ServerLine.Say
        assertEquals("alice", line.info.nick)
        assertEquals("你好 @bob", line.info.text)
        assertTrue("提到我了", line.info.mention)
        assertFalse(line.info.own)
    }

    @Test
    fun `FILE_OFFER 解析昵称_文件ID_文件名和大小`() {
        val name = base64Encode("季度报告.pdf".toByteArray(Charsets.UTF_8))
        val line = ServerLine.parse("FILE_OFFER 21:05 alice F1 $name 1048576")
        assertTrue(line is ServerLine.FileOffer)
        line as ServerLine.FileOffer
        assertEquals("21:05", line.time)
        assertEquals("alice", line.nick)
        assertEquals("F1", line.fileId)
        assertEquals("季度报告.pdf", line.fileName)
        assertEquals(1048576L, line.size)
        assertFalse("没有第 5 个字段就是没有缩略图", line.hasThumbnail)
    }

    @Test
    fun `FILE_OFFER 第 5 个字段为 1 表示带缩略图`() {
        val name = base64Encode("视频.mp4".toByteArray(Charsets.UTF_8))
        val line = ServerLine.parse("FILE_OFFER 21:05 bob F2 $name 2048 1") as ServerLine.FileOffer
        assertTrue(line.hasThumbnail)
        assertEquals("视频.mp4", line.fileName)
    }

    @Test
    fun `FILE_BEGIN 和 FILE_DATA 和 FILE_END 组成一次完整下载`() {
        val name = base64Encode("a.txt".toByteArray(Charsets.UTF_8))
        val begin = ServerLine.parse("FILE_BEGIN F9 $name 5") as ServerLine.FileBegin
        assertEquals("F9", begin.fileId)
        assertEquals("a.txt", begin.fileName)
        assertEquals(5L, begin.size)

        val payload = byteArrayOf(1, 2, 3, 4, 5)
        val data = ServerLine.parse("FILE_DATA F9 ${base64Encode(payload)}") as ServerLine.FileData
        assertEquals("F9", data.fileId)
        assertTrue(payload.contentEquals(data.data))

        val end = ServerLine.parse("FILE_END F9") as ServerLine.FileEnd
        assertEquals("F9", end.fileId)
    }

    @Test
    fun `FILE_THUMB_DATA 和 FILE_THUMB_END 能解析_这两个文档里没有`() {
        val payload = byteArrayOf(9, 8, 7)
        val data = ServerLine.parse("FILE_THUMB_DATA F3 ${base64Encode(payload)}") as ServerLine.FileThumbData
        assertEquals("F3", data.fileId)
        assertTrue(payload.contentEquals(data.data))

        val end = ServerLine.parse("FILE_THUMB_END F3") as ServerLine.FileThumbEnd
        assertEquals("F3", end.fileId)
    }

    @Test
    fun `FILE_FAIL 保留中文原因`() {
        val line = ServerLine.parse("FILE_FAIL F7 文件不存在或已经过期") as ServerLine.FileFail
        assertEquals("F7", line.fileId)
        assertEquals("文件不存在或已经过期", line.reason)
    }

    @Test
    fun `KNOWN 不进聊天记录但能拿到名单`() {
        val line = ServerLine.parse("KNOWN 21:05 alice,bob,carol") as ServerLine.Known
        assertEquals(listOf("alice", "bob", "carol"), line.nicks)
        assertEquals("21:05", line.time)
    }

    @Test
    fun `NAMES 名单会过滤空项和占位文案`() {
        val line = ServerLine.parse("NAMES 21:05 alice,,bob,") as ServerLine.Names
        assertEquals(listOf("alice", "bob"), line.nicks)

        val empty = ServerLine.parse("NAMES 21:05 (暂时没人设置昵称)") as ServerLine.Names
        assertTrue(empty.nicks.isEmpty())
    }

    @Test
    fun `坏 Base64 不会崩_退回 Unknown 并保留原文`() {
        val raw = "FILE_DATA F1 !!!not-base64!!!"
        val line = ServerLine.parse(raw)
        assertTrue("坏数据应退回 Unknown，实际 $line", line is ServerLine.Unknown)
        assertEquals(raw, (line as ServerLine.Unknown).raw)
    }

    @Test
    fun `字段不全的行退回 Unknown`() {
        assertTrue(ServerLine.parse("FILE_OFFER 21:05 alice") is ServerLine.Unknown)
        assertTrue(ServerLine.parse("RULES 64") is ServerLine.Unknown)
        assertTrue(ServerLine.parse("FILE_BEGIN F1") is ServerLine.Unknown)
    }

    @Test
    fun `完全未知的命令保留原文`() {
        val raw = "BRAND_NEW_CMD 21:05 内容"
        val line = ServerLine.parse(raw)
        assertTrue(line is ServerLine.Unknown)
        assertEquals(raw, (line as ServerLine.Unknown).raw)
    }

    @Test
    fun `空行解析成 Unknown 而不是崩`() {
        assertTrue(ServerLine.parse("") is ServerLine.Unknown)
        assertTrue(ServerLine.parse("   ") is ServerLine.Unknown)
    }
}
