package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.VoiceMessage
import com.dongfang20101113.dchat.voice.PlaybackAction
import com.dongfang20101113.dchat.voice.SendDecision
import com.dongfang20101113.dchat.voice.StopReason
import com.dongfang20101113.dchat.voice.VoicePlayback
import com.dongfang20101113.dchat.voice.VoicePlayer
import com.dongfang20101113.dchat.voice.VoiceRecord
import com.dongfang20101113.dchat.voice.VoiceRecorder
import com.dongfang20101113.dchat.voice.VoiceRecordingFlow
import com.dongfang20101113.dchat.voice.afterPlaybackStarts
import com.dongfang20101113.dchat.voice.afterPlaybackStops
import com.dongfang20101113.dchat.voice.bubbleDurationSeconds
import com.dongfang20101113.dchat.voice.decidePlay
import com.dongfang20101113.dchat.voice.playbackFraction
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * 「按住说话 → 发不发得出去」整条链路的单测。
 *
 * ## 为什么值得写这么多
 *
 * 录音这块的失败**在真机上很难复现**：麦克风被别的 App 占着、
 * `MediaRecorder` 报的时长是 0、文件根本没落盘、手指点一下就松……
 * 这些情况在真机上都只表现为"语音没发出去"，看不出是哪一环断的。
 *
 * 所以把决策抽成了 [VoiceRecordingFlow]（见 `VoiceDecisions.kt`），
 * 用假的 [VoiceRecorder] 把每一种情况都摆出来：
 * **不需要麦克风、不需要真机、不需要等 5 分钟**（时间是注入的）。
 */
class VoiceRecordingFlowTest {

    @get:Rule
    val temp = TemporaryFolder()

    /**
     * 假录音机：**真的写文件**，字节数由用例指定。
     *
     * 这一点是刻意的——决策里有一环是"读文件大小"，如果这里假装文件存在
     * 而不真写，`File.length()` 永远返回 0，测试就会在一条现实中不存在的
     * 路径上全绿。
     */
    private class FakeRecorder(
        /** 真正写进文件的字节数（内容不重要，反正没人去播）。 */
        var bytesToWrite: Long = 512,
        /**
         * 文件最终的**长度**。用来造"2 MB 的录音"这种用例时不必真的写 2 MB：
         * `setLength` 是稀疏扩展，磁盘上几乎不占地方，测试也就不会慢。
         */
        var fileBytes: Long = -1,
        var reportedSeconds: Int = 0,
        var failWith: String? = null,
    ) : VoiceRecorder {
        var startedPath: String? = null
        var cancelled = false

        override var lastError: String? = failWith
            private set

        override fun start(targetPath: String): Boolean {
            if (failWith != null) {
                lastError = failWith
                return false
            }
            startedPath = targetPath
            val out = java.io.File(targetPath)
            out.outputStream().use { stream -> stream.write(ByteArray(bytesToWrite.toInt())) }
            if (fileBytes >= 0) {
                // 稀疏扩展：不用真写 2 MB，测试不会因为造数据变慢
                java.io.RandomAccessFile(out, "rw").use { it.setLength(fileBytes) }
            }
            return true
        }

        override fun stop(targetPath: String): Int = reportedSeconds

        override fun cancel() {
            cancelled = true
            startedPath?.let { runCatching { java.io.File(it).delete() } }
        }
    }

    private var clock = 1_000_000L

    private fun flow(recorder: FakeRecorder): VoiceRecordingFlow =
        VoiceRecordingFlow(
            recorder = recorder,
            dir = { temp.root },
            now = { clock },
            sequence = { 1L },
        )

    // ------------------------------------------------------------------
    // 时长判定
    // ------------------------------------------------------------------

    @Test
    fun `录了 3 秒就是 3 秒`() {
        // 时间注入：不用真的等 3 秒
        assertEquals(3, VoiceRecord.finishedSeconds(1_000, 4_000, 0))
    }

    @Test
    fun `容器报不出时长时用墙钟时间兜底`() {
        // MediaRecorder 报 0 是常见现象（个别设备/编码器）。这时如果信它，
        // 一段录得好好的语音会被判成"空的"。
        assertEquals(4, VoiceRecord.finishedSeconds(1_000, 5_000, 0))
    }

    @Test
    fun `容器报的时长更长时以容器为准`() {
        // 手指松开的时刻和编码器收尾的时刻本来就会差一点，取大的那头更接近实际内容
        assertEquals(6, VoiceRecord.finishedSeconds(1_000, 4_000, 6))
    }

    @Test
    fun `接近一秒算一秒`() {
        // 录了 0.999 秒：算 1 秒。否则用户会觉得"我明明按住了，怎么说太短"
        assertEquals(1, VoiceRecord.finishedSeconds(0, 999, 0))
        assertEquals(0, VoiceRecord.finishedSeconds(0, 400, 0))
    }

    @Test
    fun `一秒以下不给发`() {
        assertNotNull(VoiceRecord.whyTooShort(0))
        assertNull(VoiceRecord.whyTooShort(1))
        assertNull(VoiceRecord.whyTooShort(120))
    }

    @Test
    fun `录音计时到上限就不再往上加`() {
        // 停在 5:00，而不是跳到 5:37——那个数字没有意义，反正不会发出去
        assertEquals("5:00", VoiceRecord.displaySeconds(337))
        assertEquals("0:07", VoiceRecord.displaySeconds(7))
        assertEquals("0:00", VoiceRecord.displaySeconds(-3))
    }

    // ------------------------------------------------------------------
    // 整条流程
    // ------------------------------------------------------------------

    @Test
    fun `太短的录音不发_而且文件被删掉`() {
        val recorder = FakeRecorder(bytesToWrite = 200)
        val flow = flow(recorder)
        assertTrue(flow.begin())
        val path = recorder.startedPath!!

        clock += 300                       // 手指点了一下就松
        val decision = flow.finish()

        assertTrue("应该是 TooShort，实际 $decision", decision is SendDecision.TooShort)
        assertEquals("说话时间太短了", (decision as SendDecision.TooShort).reason)
        assertFalse("太短的录音文件必须删掉", java.io.File(path).exists())
    }

    @Test
    fun `够长的录音可以发_大小就是磁盘上的真实字节数`() {
        val recorder = FakeRecorder(bytesToWrite = 4096, reportedSeconds = 3)
        val flow = flow(recorder)

        assertTrue(flow.begin())
        clock += 3_200
        val decision = flow.finish()

        assertTrue("应该是 Send，实际 $decision", decision is SendDecision.Send)
        val send = decision as SendDecision.Send
        assertEquals(3, send.seconds)
        assertEquals("voice-1.m4a", send.fileName)
        // 关键：字节数必须来自真实文件，不是"以为写了多少"
        assertEquals(java.io.File(send.path).length(), send.bytes)
        assertTrue(send.bytes > 0)
    }

    @Test
    fun `超过 2 MB 的录音被拦下_并说明是录音质量的问题`() {
        val recorder = FakeRecorder(fileBytes = VoiceMessage.MAX_BYTES + 1, reportedSeconds = 30)
        val flow = flow(recorder)
        flow.begin()
        clock += 30_000

        val decision = flow.finish()

        assertTrue("应该是 Rejected，实际 $decision", decision is SendDecision.Rejected)
        val reason = (decision as SendDecision.Rejected).reason
        assertTrue("要提示原因：$reason", reason.contains("2 MB"))
        assertTrue("要给出可能的原因：$reason", reason.contains("录音质量"))
        assertEquals("被拦下的录音不该留在缓存里", 0, temp.root.list()?.size ?: 0)
    }

    @Test
    fun `刚好到 2 MB 上限时可以发`() {
        // 边界要测两头：只测"超了会被拒"的话，把上限写成 >= 也能全绿
        val recorder = FakeRecorder(fileBytes = VoiceMessage.MAX_BYTES, reportedSeconds = 30)
        val flow = flow(recorder)
        flow.begin()
        clock += 30_000

        assertTrue(flow.finish() is SendDecision.Send)
    }

    @Test
    fun `超过 5 分钟的录音被拦下_并给出具体时长`() {
        val recorder = FakeRecorder(reportedSeconds = VoiceMessage.MAX_DURATION_SECONDS + 30)
        val flow = flow(recorder)
        flow.begin()
        clock += (VoiceMessage.MAX_DURATION_SECONDS + 30) * 1000L

        val decision = flow.finish()

        assertTrue(decision is SendDecision.Rejected)
        val reason = (decision as SendDecision.Rejected).reason
        assertTrue("要说清最长多少：$reason", reason.contains("5:00"))
        assertTrue("要说清这段多长：$reason", reason.contains("5:30"))
    }

    @Test
    fun `文件没落盘时不发_而不是发一个 0 字节的语音`() {
        val recorder = FakeRecorder(bytesToWrite = 0)
        val flow = flow(recorder)
        flow.begin()
        // 把文件删掉，模拟"MediaRecorder 没写成"
        java.io.File(recorder.startedPath!!).delete()
        clock += 5_000

        val decision = flow.finish()

        assertTrue("应该是 Rejected，实际 $decision", decision is SendDecision.Rejected)
        assertTrue((decision as SendDecision.Rejected).reason.contains("空"))
    }

    @Test
    fun `已经在录的时候再按一次_不会录出第二条`() {
        val recorder = FakeRecorder()
        val flow = flow(recorder)

        assertTrue(flow.begin())
        val first = recorder.startedPath
        assertTrue("重复按下应当被忽略并返回 true", flow.begin())
        assertEquals("不能换文件", first, recorder.startedPath)
    }

    @Test
    fun `没按过就松手_什么都不做`() {
        val flow = flow(FakeRecorder())
        assertEquals(SendDecision.Idle, flow.finish())
    }

    @Test
    fun `取消之后文件没了_松手也不发`() {
        val recorder = FakeRecorder()
        val flow = flow(recorder)
        flow.begin()
        val path = recorder.startedPath!!

        flow.cancel()

        assertTrue(recorder.cancelled)
        assertFalse(java.io.File(path).exists())
        assertEquals("取消之后再松手不该发出任何东西", SendDecision.Idle, flow.finish())
    }

    @Test
    fun `录音本身失败时不留下空文件`() {
        val recorder = FakeRecorder(failWith = "没有麦克风权限")
        val flow = flow(recorder)

        assertFalse(flow.begin())
        assertFalse(flow.isRecording)
        assertNull("失败时不该产生录音状态", flow.elapsedSeconds())
        assertEquals(0, temp.root.list()?.size ?: 0)
    }

    @Test
    fun `录音时长是按时间算的_不是在数滴答`() {
        val flow = flow(FakeRecorder())
        flow.begin()
        clock += 7_400
        assertEquals(7, flow.elapsedSeconds())
        clock += 1_000
        assertEquals(8, flow.elapsedSeconds())
    }
}

/**
 * 播放的取舍：点同一条、点另一条、暂停之后又点，分别该发生什么。
 *
 * 这些规则在真机上的表现是"声音叠在一起"或者"点了没反应"，
 * 靠手点很难覆盖全，所以这里把每一条都钉下来。
 */
class VoicePlaybackTest {

    @Test
    fun `没在播时点一条_从头播`() {
        val action = decidePlay(VoicePlayback(), "F1", "/tmp/a.m4a")
        assertEquals(PlaybackAction.Start("/tmp/a.m4a"), action)
    }

    @Test
    fun `同一条正在播_再点一下是暂停`() {
        val current = VoicePlayback(currentId = "F1", path = "/tmp/a.m4a", durationSeconds = 8)
        assertEquals(PlaybackAction.Pause, decidePlay(current, "F1", "/tmp/a.m4a"))
    }

    @Test
    fun `同一条暂停着_再点一下是继续_而不是从头播`() {
        val current = VoicePlayback(
            currentId = "F1", path = "/tmp/a.m4a", durationSeconds = 8, paused = true,
        )
        assertEquals(PlaybackAction.Resume, decidePlay(current, "F1", "/tmp/a.m4a"))
    }

    @Test
    fun `点另一条_从头播新的_不会两条一起响`() {
        val current = VoicePlayback(currentId = "F1", path = "/tmp/a.m4a", durationSeconds = 8)
        val action = decidePlay(current, "F2", "/tmp/b.m4a")
        assertEquals(PlaybackAction.Start("/tmp/b.m4a"), action)
    }

    @Test
    fun `文件还没下下来_要说清为什么_而不是点了没反应`() {
        val action = decidePlay(VoicePlayback(), "F1", null)
        assertTrue("应该是 Unavailable，实际 $action", action is PlaybackAction.Unavailable)
        assertTrue((action as PlaybackAction.Unavailable).reason.contains("还没下载完"))
    }

    @Test
    fun `开始播放后记下时长_界面才显示得出几秒`() {
        val next = afterPlaybackStarts(VoicePlayback(), "F1", "/tmp/a.m4a", 8)
        assertEquals("F1", next.currentId)
        assertEquals(8, next.durationSeconds)
        assertFalse(next.paused)
        assertTrue(next.isPlaying)
    }

    @Test
    fun `续播时读不到时长_不该把已知时长清成 0`() {
        // 否则气泡上明明写着 0:32，暂停再点一下就变回 0:00
        val current = VoicePlayback(currentId = "F1", path = "/tmp/a.m4a", durationSeconds = 32, paused = true)
        val next = afterPlaybackStarts(current, "F1", "/tmp/a.m4a", 0)
        assertEquals(32, next.durationSeconds)
    }

    @Test
    fun `播完之后回到没有在播_否则界面一直显示暂停按钮`() {
        val playing = VoicePlayback(currentId = "F1", path = "/tmp/a.m4a", durationSeconds = 8)
        for (reason in listOf(StopReason.FINISHED, StopReason.ERROR, StopReason.USER)) {
            val stopped = afterPlaybackStops(playing, reason)
            assertNull("$reason 之后不该还有 currentId", stopped.currentId)
            assertNull(stopped.path)
            assertFalse(stopped.isPlaying)
        }
    }

    @Test
    fun `没播过的语音显示 0 比 0_而不是拿字节数猜`() {
        assertEquals("0:00", bubbleDurationSeconds(0, 0))
        // 播过一次之后显示真实时长
        assertEquals("1:23", bubbleDurationSeconds(0, 83))
    }

    @Test
    fun `进度是算出来的_且夹在 0 到 1 之间`() {
        assertEquals(0f, playbackFraction(0, 8), 0.001f)
        assertEquals(0.5f, playbackFraction(4, 8), 0.001f)
        assertEquals(1f, playbackFraction(99, 8), 0.001f)   // 超出也夹住
        assertEquals(0f, playbackFraction(3, 0), 0.001f)    // 时长未知按 0 处理
    }
}
