package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.AttachmentKind
import com.dongfang20101113.dchat.protocol.VoiceMessage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 语音消息的协议约定。
 *
 * 两件事要盯死：
 *
 * 1. **和已发布的贴纸格式兼容**——种类字段是同一格，`1` 必须仍然是贴纸。
 *    改错了会让老客户端上的贴纸全变成文件卡片。
 * 2. **时长和格式的边界**——录音这种东西很容易产生"超大文件"
 *    （高质量录音一分钟就十几 MB），必须在本地就拦住。
 */
class VoiceMessageTest {

    // ------------------------------------------------------------------
    // ★ 种类字段：和贴纸格式共存
    // ------------------------------------------------------------------

    @Test
    fun `★ 贴纸的写法必须保持兼容`() {
        // 这一格已经发出去了，`1` 永远是贴纸
        assertEquals(AttachmentKind.STICKER, VoiceMessage.parseKind("1"))
        assertEquals(AttachmentKind.STICKER, VoiceMessage.parseKind("sticker"))
        assertEquals(AttachmentKind.STICKER, VoiceMessage.parseKind("STICKER"))
    }

    @Test
    fun `普通文件的写法也不变`() {
        assertEquals(AttachmentKind.FILE, VoiceMessage.parseKind("0"))
        assertEquals(AttachmentKind.FILE, VoiceMessage.parseKind(null))
        assertEquals(AttachmentKind.FILE, VoiceMessage.parseKind(""))
        assertEquals(AttachmentKind.FILE, VoiceMessage.parseKind("莫名其妙的值"))
    }

    @Test
    fun `语音能被认出来`() {
        assertEquals(AttachmentKind.VOICE, VoiceMessage.parseKind("voice"))
        assertEquals(AttachmentKind.VOICE, VoiceMessage.parseKind("VOICE"))
        assertEquals(AttachmentKind.VOICE, VoiceMessage.parseKind("Voice"))
    }

    @Test
    fun `种类不会出现"既贴纸又语音"`() {
        // 用同一格表达种类就是为了消掉这种没有意义的组合
        val kinds = listOf("0", "1", "sticker", "voice", null, "xyz").map { VoiceMessage.parseKind(it) }
        for (kind in kinds) {
            assertTrue("必须是三者之一：$kind", kind in AttachmentKind.entries)
        }
    }

    @Test
    fun `上传时只有非普通文件才追加标记`() {
        assertEquals("", VoiceMessage.fileSendSuffix(AttachmentKind.FILE))
        assertEquals(" 1", VoiceMessage.fileSendSuffix(AttachmentKind.STICKER))
        assertEquals(" voice", VoiceMessage.fileSendSuffix(AttachmentKind.VOICE))
    }

    // ------------------------------------------------------------------
    // 时长格式化
    // ------------------------------------------------------------------

    @Test
    fun `时长格式是 分不补零、秒补零`() {
        assertEquals("0:00", VoiceMessage.formatDuration(0))
        assertEquals("0:07", VoiceMessage.formatDuration(7))
        assertEquals("0:59", VoiceMessage.formatDuration(59))
        assertEquals("1:00", VoiceMessage.formatDuration(60))
        assertEquals("1:23", VoiceMessage.formatDuration(83))
        assertEquals("5:00", VoiceMessage.formatDuration(300))
        assertEquals("10:05", VoiceMessage.formatDuration(605))
    }

    @Test
    fun `负数时长显示成 0 而不是负数`() {
        // 界面上偶尔会拿到 -1（表示"时长未知"）
        assertEquals("0:00", VoiceMessage.formatDuration(-1))
        assertEquals("0:00", VoiceMessage.formatDuration(-999))
    }

    // ------------------------------------------------------------------
    // 格式判断
    // ------------------------------------------------------------------

    @Test
    fun `常见录音格式都能发`() {
        for (name in listOf("a.m4a", "a.aac", "a.ogg", "a.opus", "a.3gp", "a.amr", "A.M4A")) {
            assertTrue("$name 应当可以", VoiceMessage.looksLikeAudio(name))
        }
    }

    @Test
    fun `不压缩的 wav 刻意不收`() {
        // 一分钟 wav 就是 5 MB 以上，走公网太奢侈，而且完全没必要
        assertFalse(VoiceMessage.looksLikeAudio("a.wav"))
    }

    @Test
    fun `非音频一律拒绝`() {
        for (name in listOf("a.png", "a.zip", "a.pdf", "noext", "trailing.", "")) {
            assertFalse("$name 不该被当成语音", VoiceMessage.looksLikeAudio(name))
        }
    }

    @Test
    fun `★ mp4 必须被接受（它是 Android 录音的正常输出）`() {
        // 我一开始把 mp4 写进了"该拒绝"的列表，测试红了。
        // 查下来是**测试写错了**：Android 的 MediaRecorder 录 AAC 音频时，
        // 输出容器就是 mp4（或 m4a）。拒绝它等于拒绝掉本机录出来的语音。
        //
        // 代价是有人可能把一段视频当语音发——但那是他自己的选择，
        // 而且大小上限（2 MB）会挡住绝大多数视频。
        assertTrue(VoiceMessage.looksLikeAudio("recording.mp4"))
        assertNull(VoiceMessage.whyCannotSend("recording.mp4", 200 * 1024, 15))
    }

    // ------------------------------------------------------------------
    // 发送前的本地校验
    // ------------------------------------------------------------------

    @Test
    fun `正常录音可以发`() {
        assertNull(VoiceMessage.whyCannotSend("a.m4a", 40 * 1024, 12))
    }

    @Test
    fun `空录音被拦`() {
        assertNotNull(VoiceMessage.whyCannotSend("a.m4a", 1000, 0))
        assertNotNull(VoiceMessage.whyCannotSend("a.m4a", 0, 5))
    }

    @Test
    fun `超长录音要给出具体时长`() {
        val reason = VoiceMessage.whyCannotSend("a.m4a", 100 * 1024, 600)
        assertNotNull("超长必须被拦", reason)
        assertTrue("要说清上限：$reason", reason!!.contains("5:00"))
        assertTrue("也要给出实际时长：$reason", reason.contains("10:00"))
    }

    @Test
    fun `超大文件要提醒可能是音质设太高`() {
        // 录音最容易出的问题就是"文件莫名其妙很大"，
        // 只说"太大"用户不知道怎么改，要给出可操作的原因
        val reason = VoiceMessage.whyCannotSend("a.m4a", 5 * 1024 * 1024, 30)
        assertNotNull(reason)
        assertTrue("要提示原因：$reason", reason!!.contains("录音质量"))
    }

    @Test
    fun `刚好到边界都放行`() {
        assertNull(
            VoiceMessage.whyCannotSend(
                "a.m4a",
                VoiceMessage.MAX_BYTES,
                VoiceMessage.MAX_DURATION_SECONDS,
            ),
        )
    }

    @Test
    fun `格式不对的提示要说清支持哪些`() {
        val reason = VoiceMessage.whyCannotSend("a.wav", 1024, 5)
        assertNotNull(reason)
        assertTrue("要列出支持的格式：$reason", reason!!.contains("m4a"))
    }
}
