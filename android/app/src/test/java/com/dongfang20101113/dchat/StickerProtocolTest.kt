package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.StickerProtocol
import com.dongfang20101113.dchat.protocol.base64Encode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 贴纸的协议约定。
 *
 * 真正要盯住的是**向后兼容**：标记位是追加在末尾的第 7 个字段，
 * 老服务器不会发它。如果解析时把"缺字段"当成错误，用户连老服务器就收不到文件了——
 * 那是最糟糕的一类兼容性事故（新客户端连不上老服务器）。
 */
class StickerProtocolTest {

    // ------------------------------------------------------------------
    // 文件类型判断
    // ------------------------------------------------------------------

    @Test
    fun `常见图片格式都能当贴纸`() {
        for (name in listOf("a.png", "a.jpg", "a.jpeg", "a.gif", "a.webp", "a.bmp")) {
            assertTrue("$name 应当可以", StickerProtocol.looksLikeImage(name))
        }
    }

    @Test
    fun `大小写不敏感`() {
        assertTrue(StickerProtocol.looksLikeImage("PHOTO.PNG"))
        assertTrue(StickerProtocol.looksLikeImage("Photo.JpEg"))
    }

    @Test
    fun `不是图片的一律拒绝`() {
        for (name in listOf("a.zip", "a.exe", "a.txt", "a.mp4", "a.pdf")) {
            assertFalse("$name 不该被当成贴纸", StickerProtocol.looksLikeImage(name))
        }
    }

    @Test
    fun `没有扩展名或者只有点的情况不能崩`() {
        assertFalse(StickerProtocol.looksLikeImage("noext"))
        assertFalse(StickerProtocol.looksLikeImage("trailing."))
        assertFalse(StickerProtocol.looksLikeImage(""))
        assertFalse(StickerProtocol.looksLikeImage(".png"))  // 隐藏文件，没有主名
    }

    @Test
    fun `中间有点的文件名取最后一段扩展名`() {
        assertTrue(StickerProtocol.looksLikeImage("my.photo.v2.png"))
        assertFalse(StickerProtocol.looksLikeImage("archive.tar.gz"))
    }

    // ------------------------------------------------------------------
    // 大小限制
    // ------------------------------------------------------------------

    @Test
    fun `正常大小的图片可以当贴纸`() {
        assertNull(StickerProtocol.whyCannotBeSticker("a.png", 100 * 1024))
    }

    @Test
    fun `刚好到上限可以`() {
        assertNull(StickerProtocol.whyCannotBeSticker("a.png", StickerProtocol.MAX_STICKER_BYTES))
    }

    @Test
    fun `超过上限要给出具体数字`() {
        val reason = StickerProtocol.whyCannotBeSticker("a.png", StickerProtocol.MAX_STICKER_BYTES + 1)
        assertNotNull("超限必须被拦", reason)
        assertTrue("要说清上限：$reason", reason!!.contains("512"))
        assertTrue("也要给出实际大小：$reason", reason.contains("KB"))
    }

    @Test
    fun `空文件和非图片各有各的提示`() {
        val empty = StickerProtocol.whyCannotBeSticker("a.png", 0)
        assertNotNull(empty)
        assertTrue(empty!!.contains("空"))

        val wrongType = StickerProtocol.whyCannotBeSticker("a.zip", 1024)
        assertNotNull(wrongType)
        assertTrue("要说清允许哪些格式：$wrongType", wrongType!!.contains("图片"))
    }

    @Test
    fun `提示里要给出退路`() {
        // 用户选了一张大图当贴纸，不能只说"太大"就完了——
        // 他其实可以按普通文件发出去
        val reason = StickerProtocol.whyCannotBeSticker("big.png", 5 * 1024 * 1024)
        assertTrue("要告诉用户还能按普通文件发：$reason", reason!!.contains("普通文件"))
    }

    // ------------------------------------------------------------------
    // 线上格式
    // ------------------------------------------------------------------

    @Test
    fun `标记只追加在末尾`() {
        assertEquals("", StickerProtocol.fileSendSuffix(false))
        assertEquals(" sticker", StickerProtocol.fileSendSuffix(true))
        assertEquals("0", StickerProtocol.offerFlag(false))
        assertEquals("1", StickerProtocol.offerFlag(true))
    }

    @Test
    fun `解析 FILE_OFFER 的贴纸标记`() {
        // 注意：这个函数只认**一个字段**，索引由调用方负责（见 isStickerFlag 的注释）
        assertFalse("字段不存在时必须是普通文件", StickerProtocol.isStickerFlag(null))
        assertFalse(StickerProtocol.isStickerFlag("0"))
        assertTrue(StickerProtocol.isStickerFlag("1"))
        // 也接受文字标记，方便人工构造和调试
        assertTrue(StickerProtocol.isStickerFlag("sticker"))
        assertTrue(StickerProtocol.isStickerFlag("STICKER"))
        assertFalse(StickerProtocol.isStickerFlag(""))
        assertFalse(StickerProtocol.isStickerFlag("2"))
    }

    // ------------------------------------------------------------------
    // ★ 向后兼容：这是最容易出事的地方
    // ------------------------------------------------------------------

    @Test
    fun `★ 老服务器发的 FILE_OFFER 不会被误判成贴纸`() {
        val name = base64Encode("照片.png".toByteArray(Charsets.UTF_8))
        // 老服务器只会发到第 5 或第 6 个字段
        for (line in listOf(
            "FILE_OFFER 21:05 小明 F1 $name 2048",
            "FILE_OFFER 21:05 小明 F1 $name 2048 1",
        )) {
            val parsed = ServerLine.parse(line, "我")
            assertTrue("应当解析成 FileOffer：$line", parsed is ServerLine.FileOffer)
            val offer = parsed as ServerLine.FileOffer
            assertEquals("老格式的文件名要读对", "照片.png", offer.fileName)
            assertEquals(2048L, offer.size)
            assertFalse("老格式绝不能当成贴纸：$line", offer.isSticker)
        }
    }

    @Test
    fun `★ 新服务器发的贴纸能被认出来`() {
        val name = base64Encode("开心.png".toByteArray(Charsets.UTF_8))
        val parsed = ServerLine.parse("FILE_OFFER 21:05 小明 F9 $name 4096 0 1", "我")
        assertTrue(parsed is ServerLine.FileOffer)
        val offer = parsed as ServerLine.FileOffer
        assertTrue("应当认出这是贴纸", offer.isSticker)
        assertEquals("开心.png", offer.fileName)
        assertEquals(4096L, offer.size)
        // 不能被贴纸标记影响原有的解析结果
        assertFalse("缩略图标记是 0，不该被读成有缩略图", offer.hasThumbnail)
    }

    @Test
    fun `贴纸和缩略图标记互不干扰`() {
        val name = base64Encode("a.png".toByteArray(Charsets.UTF_8))
        val withThumb = ServerLine.parse("FILE_OFFER 21:05 小明 F1 $name 100 1 1", "我")
        val offer = withThumb as ServerLine.FileOffer
        assertTrue("两个标记都为 1 时都要认出来", offer.hasThumbnail && offer.isSticker)
    }
}
