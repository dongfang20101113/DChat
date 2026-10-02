package com.dongfang20101113.dchat.voice

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import androidx.core.content.ContextCompat
import java.io.File

/**
 * 真正碰硬件的那一层：`MediaRecorder` 录音、`MediaPlayer` 放音。
 *
 * 所有"该不该录、该不该发、该不该停前一条"的判断都在
 * [VoiceDecisions] 那个纯逻辑文件里；这里只负责把 API 调对，
 * 以及**把异常翻译成人话**。
 *
 * ## 两个必须记住的 API 陷阱
 *
 * 1. `MediaRecorder` 的调用顺序**不能错**：`setAudioSource` → `setOutputFormat` →
 *    `setAudioEncoder` → `prepare` → `start`。顺序错了不是"行为怪异"，而是直接
 *    `IllegalStateException` 崩掉。
 * 2. 录完必须调 `stop()` 才会把 MP4 的 `moov`（索引）写进文件。**没调 stop 的
 *    m4a 是个放不出来的残文件**——看着有几百 KB，播放器一打开就报错。
 */

/** 录音机的 Android 实现。 */
class AndroidVoiceRecorder(private val context: Context) : VoiceRecorder {

    private var recorder: MediaRecorder? = null
    private var targetPath: String? = null

    override var lastError: String? = null
        private set

    override fun start(targetPath: String): Boolean {
        lastError = null

        // 权限没给就直说。MediaRecorder.start() 在没权限时会抛 SecurityException，
        // 但那条消息对用户毫无意义。
        val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        if (!granted) {
            lastError = "没有麦克风权限"
            return false
        }

        // 上一次没收拾干净的，先收拾掉，否则 new MediaRecorder() 会让上一台机器
        // 一直占着麦克风（表现为"新录音里什么都听不到"）。
        releaseQuietly()

        var created: MediaRecorder? = null
        return try {
            created = newRecorder()
            created.setAudioSource(MediaRecorder.AudioSource.MIC)
            created.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            created.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            created.setAudioChannels(1)              // 人声单声道足够，体积还小一半
            created.setAudioSamplingRate(44100)
            created.setAudioEncodingBitRate(64_000)  // 64 kbps：一分钟约 480 KB，远低于 2 MB 上限
            created.setOutputFile(targetPath)
            created.prepare()
            created.start()
            recorder = created
            this.targetPath = targetPath
            true
        } catch (e: Exception) {
            // 麦克风被别的 App 占着、设备没有录音硬件、路径不可写……都归到这一类
            lastError = describe(e)
            runCatching { created?.release() }
            runCatching { File(targetPath).delete() }
            recorder = null
            this.targetPath = null
            false
        }
    }

    override fun stop(targetPath: String): Int {
        val current = recorder ?: return 0
        recorder = null
        this.targetPath = null
        return try {
            current.stop()                                  // 这一句才把 moov 写进文件
            current.release()
            readDurationSeconds(targetPath)
        } catch (e: Exception) {
            // 录得太短时 stop() 会抛 RuntimeException——文件作废，交给上层按"太短"处理
            lastError = describe(e)
            runCatching { current.release() }
            0
        }
    }

    override fun cancel() {
        val current = recorder
        val path = targetPath
        recorder = null
        targetPath = null
        current?.let {
            runCatching { it.stop() }      // 先 stop 再 release，避免残留半截文件
            runCatching { it.release() }
        }
        path?.let { runCatching { File(it).delete() } }
    }

    private fun newRecorder(): MediaRecorder =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) MediaRecorder(context)
        else @Suppress("DEPRECATION") MediaRecorder()

    /**
     * 从刚录好的文件里读时长。
     *
     * `MediaRecorder` **没有**"录了多久"这个属性（只有 `getMaxAmplitude` 之类），
     * 只能读文件。读不出来返回 0——调用方会用墙钟时间兜底（见
     * [VoiceRecord.finishedSeconds]），不会因此把一段正常录音判成空的。
     */
    private fun readDurationSeconds(path: String): Int = try {
        val retriever = android.media.MediaMetadataRetriever()
        try {
            retriever.setDataSource(path)
            val ms = retriever
                .extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_DURATION)
                ?.toLongOrNull() ?: 0L
            (ms / 1000).toInt().coerceAtLeast(0)
        } finally {
            runCatching { retriever.release() }
        }
    } catch (_: Exception) {
        0
    }

    private fun releaseQuietly() {
        recorder?.let {
            runCatching { it.reset() }
            runCatching { it.release() }
        }
        recorder = null
        targetPath = null
    }

    private fun describe(e: Exception): String = when (e) {
        is SecurityException -> "没有麦克风权限"
        else -> e.message?.takeIf { it.isNotBlank() } ?: "录音失败"
    }
}

/** 播放器的 Android 实现。 */
class AndroidVoicePlayer(private val context: Context) : VoicePlayer {

    private var player: MediaPlayer? = null
    private var duration = 0

    /** 放完之后通知谁——由会话层接上，用来把界面上的"正在播"收回去。 */
    var onFinished: (() -> Unit)? = null

    /** 播放中途出错时通知谁。 */
    var onFailed: ((String) -> Unit)? = null

    override var lastError: String? = null
        private set

    /**
     * 开始播放 [path]。
     *
     * @return 音频总时长（秒）；**负数表示没播成**（原因在 [lastError]），
     *         `0` 表示播上了但读不出时长——这两件事必须分开，
     *         否则"读不出时长的短音频"会被当成播放失败。
     */
    override fun play(path: String): Int {
        stop()
        lastError = null
        return try {
            val mp = MediaPlayer()
            mp.setAudioAttributes(
                android.media.AudioAttributes.Builder()
                    .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                    .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            mp.setDataSource(path)
            mp.setOnCompletionListener { onFinished?.invoke() }
            mp.setOnErrorListener { _, what, extra ->
                lastError = "播放出错（$what/$extra）"
                onFailed?.invoke(lastError!!)
                true    // 返回 true 表示"我自己处理了"，系统就不会再往上层抛
            }
            mp.prepare()
            duration = (mp.duration / 1000).coerceAtLeast(0)
            mp.start()
            player = mp
            duration
        } catch (e: Exception) {
            lastError = describe(e)
            runCatching { player?.release() }
            player = null
            duration = 0
            // 用**负数**明确表示"没播成"：0 是个合法的时长（读不出时长的短音频），
            // 用它表示失败会让调用方以为"播上了，只是没有时长"。
            -1
        }
    }

    override fun pause() {
        runCatching { player?.takeIf { it.isPlaying }?.pause() }
    }

    override fun resume() {
        runCatching { player?.start() }
    }

    override fun stop() {
        player?.let {
            runCatching { it.stop() }
            runCatching { it.release() }
        }
        player = null
        duration = 0
    }

    override fun currentSeconds(): Int =
        runCatching { (player?.currentPosition ?: 0) / 1000 }.getOrDefault(0).coerceAtLeast(0)

    private fun describe(e: Exception): String = when (e) {
        is java.io.IOException -> "音频文件读不了（可能没下载完）"
        else -> e.message?.takeIf { it.isNotBlank() } ?: "播放失败"
    }
}
