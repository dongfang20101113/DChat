package com.dongfang20101113.dchat.voice

import com.dongfang20101113.dchat.protocol.VoiceMessage

/**
 * 语音功能的**纯逻辑部分**：录音状态的判定、播放的取舍。
 *
 * ## 为什么单独一个文件
 *
 * `MediaRecorder` / `MediaPlayer` 在 JVM 单测里跑不起来（它们是 native 的），
 * 但这块真正容易出错的地方根本不是"调不调得动 API"，而是**决策**：
 *
 *  - 这一秒的录音够不够长、要不要发出去？
 *  - 已经在录了又按一次怎么办？
 *  - 点第二条语音时，第一条要不要停？
 *  - 暂停之后那一秒算不算"还在播"？
 *
 * 所以这里只有数据类和纯函数，**一行 Android API 都没有**，能全部在 JVM 上测；
 * 真正碰硬件的那层藏在 [VoiceRecorder] / [VoicePlayer] 两个接口后面
 * （实现见 `AndroidVoice.kt`）。
 */

// ---------------------------------------------------------------------------
// 录音
// ---------------------------------------------------------------------------

/**
 * 一次录音的当前状态。
 *
 * [elapsedSeconds] 只在录音过程中有意义；按下那一刻还没开始计，
 * 所以先给 [notYetStarted]（界面上显示 `0:00`）。
 */
data class RecordingState(
    val filePath: String,
    val startedAtMillis: Long,
    val elapsedSeconds: Int,
) {
    companion object {
        /** 按下按钮、文件已创建但计时还没开始。 */
        fun notYetStarted(filePath: String, startedAtMillis: Long): RecordingState =
            RecordingState(filePath, startedAtMillis, 0)
    }
}

object VoiceRecord {

    /** 比这还短的录音直接丢掉——手指在屏幕上点一下就会产生一条，全是噪音。 */
    const val MIN_SEND_SECONDS: Int = 1

    /** 留一点余量取整：录了 0.999 秒按 1 秒算，免得用户觉得"明明按住了却说太短"。 */
    private const val ROUND_HALF_UP_MILLIS: Long = 500

    /**
     * 这一轮录音的最终时长（秒）。
     *
     * 取「墙钟时间」和「容器自己报的时长」里**较大**的那个：
     * 正常情况两者接近；`MediaRecorder` 偶尔会报 0（个别设备/编码器），
     * 那时墙钟时间才是可信的——否则一段录得好好的语音会被判成"空的"。
     */
    fun finishedSeconds(startedAtMillis: Long, endedAtMillis: Long, reportedSeconds: Int): Int {
        val wallClock = ((endedAtMillis - startedAtMillis) + ROUND_HALF_UP_MILLIS) / 1000
        return maxOf(wallClock, reportedSeconds.toLong()).toInt().coerceAtLeast(0)
    }

    /** 太短、不给发的原因；够长返回 null。 */
    fun whyTooShort(seconds: Int): String? {
        if (seconds >= MIN_SEND_SECONDS) return null
        return "说话时间太短了"
    }

    /**
     * 这次录音能不能发出去？返回 null 表示可以，否则是**给用户看的原因**。
     *
     * 时长、格式、大小的规则全部来自 [VoiceMessage]——那是服务端也会执行的一套，
     * 这里只是把"录音"这个场景的数字喂给它，**不重复定义一遍阈值**。
     */
    fun whyCannotSend(fileName: String, sizeBytes: Long, seconds: Int): String? {
        whyTooShort(seconds)?.let { return it }
        return VoiceMessage.whyCannotSend(fileName, sizeBytes, seconds)
    }

    /**
     * 界面上的录音计时文字。
     *
     * 超过上限时不再往上加——让它停在 `5:00`（上限）上，配合"到点自动发送"的提示，
     * 比继续跳到 `5:37` 更清楚：那个数字没有意义，反正不会发出去。
     */
    fun displaySeconds(elapsedSeconds: Int, maxSeconds: Int = VoiceMessage.MAX_DURATION_SECONDS): String =
        VoiceMessage.formatDuration(elapsedSeconds.coerceIn(0, maxSeconds))
}

/** 「按住说话」录出来的文件放在这里（App 私有缓存，不需要存储权限）。 */
object VoiceFiles {

    /**
     * 录音文件名。
     *
     * **用 m4a 而不是 wav**：`MediaRecorder` 的 MPEG_4 + AAC 出来的就是这个后缀，
     * 一分钟只占几百 KB；wav 一分钟 5 MB 起，走公网纯粹是浪费。
     */
    fun fileName(sequence: Long): String = "voice-$sequence.m4a"

    /** 这条录音算不算"文件通道能传的东西"（扩展名在白名单里）。 */
    fun isSupported(fileName: String): Boolean = VoiceMessage.looksLikeAudio(fileName)
}

// ---------------------------------------------------------------------------
// 一次录音的完整流程
// ---------------------------------------------------------------------------

/** 一次录音的归宿。 */
sealed interface SendDecision {

    /** 压根没在录（重复松手、以及松手前已经取消过）。 */
    data object Idle : SendDecision

    /** 太短：文件已删，只给用户一句提示。 */
    data class TooShort(val seconds: Int, val reason: String) : SendDecision

    /** 不够格发出去（超时长 / 超大 / 格式不对）：文件已删，[reason] 是给用户看的原因。 */
    data class Rejected(val reason: String) : SendDecision

    /** 可以发了。 */
    data class Send(
        val path: String,
        val fileName: String,
        val bytes: Long,
        val seconds: Int,
    ) : SendDecision
}

/**
 * 「按住说话」→「松手」这条流程的**全部决策**。
 *
 * ## 为什么单独抽出来
 *
 * 这段逻辑原来长在会话里，和 socket、状态流、Android 上下文缠在一起，
 * 单测根本碰不到它——只能"装到手机上按一下试试"，而它偏偏是最容易出问题的一段：
 * 录太短会不会发出去？超时长有没有拦？文件没落盘时会不会把大小算成 0？
 *
 * 抽出来之后，配上假的 [VoiceRecorder]（写多少字节、报多少时长都可以指定），
 * 整条链路能在 JVM 上跑完，**不需要麦克风、不需要真机、不需要网络**。
 *
 * 时间也是注入的（[now]），所以"录了 5 分钟"这种用例不用真的等 5 分钟。
 */
class VoiceRecordingFlow(
    private val recorder: VoiceRecorder,
    /** 录音文件放哪儿；由调用方给（Android 上是 App 私有缓存目录）。 */
    private val dir: () -> java.io.File,
    /** 当前时间（毫秒）。注入是为了让单测能瞬间"录"五分钟。 */
    private val now: () -> Long = System::currentTimeMillis,
    private val sequence: () -> Long = { System.nanoTime() },
) {

    private var current: RecordingState? = null

    /** 正在录吗。 */
    val isRecording: Boolean get() = current != null

    /**
     * 按下。返回 false 表示没录起来（原因在 [VoiceRecorder.lastError]）。
     *
     * 已经在录时**不做任何事、返回 true**：界面上快速点两下、
     * 或者无障碍服务重复触发，都不该产生第二条录音。
     */
    fun begin(): Boolean {
        if (current != null) return true
        val target = java.io.File(dir(), VoiceFiles.fileName(sequence()))
        val startedAt = now()
        if (!recorder.start(target.absolutePath)) {
            runCatching { target.delete() }
            return false
        }
        current = RecordingState.notYetStarted(target.absolutePath, startedAt)
        return true
    }

    /** 到点了该录多久（给界面上的计时用）；没在录返回 null。 */
    fun elapsedSeconds(): Int? {
        val state = current ?: return null
        return ((now() - state.startedAtMillis) / 1000).toInt().coerceAtLeast(0)
    }

    /**
     * 松手。决定这次录音是发出去、还是删掉。
     *
     * 无论走哪条分支，**都不会留下没用的文件**：太短、超限都会删。
     * 这一点很重要——录音文件在缓存里悄悄堆积，用户唯一的感受就是
     * "这 App 怎么这么占空间"。
     */
    fun finish(): SendDecision {
        val state = current ?: return SendDecision.Idle
        current = null

        val reported = recorder.stop(state.filePath)
        val endedAt = now()
        // 墙钟时间兜底：MediaRecorder 不提供"录了多久"，从文件里读时长也可能失败
        val wallClock = ((endedAt - state.startedAtMillis) / 1000).toInt().coerceAtLeast(0)
        val seconds = VoiceRecord.finishedSeconds(state.startedAtMillis, endedAt, maxOf(reported, wallClock))

        val file = java.io.File(state.filePath)
        val tooShort = VoiceRecord.whyTooShort(seconds)
        if (tooShort != null) {
            runCatching { file.delete() }
            return SendDecision.TooShort(seconds, tooShort)
        }

        val bytes = if (file.exists()) file.length() else 0L
        val reason = VoiceRecord.whyCannotSend(file.name, bytes, seconds)
        if (reason != null) {
            runCatching { file.delete() }
            return SendDecision.Rejected(reason)
        }

        return SendDecision.Send(
            path = file.absolutePath,
            fileName = file.name,
            bytes = bytes,
            seconds = seconds,
        )
    }

    /** 用户按了「取消」：文件删掉、不发。 */
    fun cancel() {
        val state = current ?: return
        current = null
        recorder.cancel()
        runCatching { java.io.File(state.filePath).delete() }
    }

    /** 录到上限时收尾用：先看还有没有在录。 */
    fun peek(): RecordingState? = current
}

// ---------------------------------------------------------------------------
// 播放
// ---------------------------------------------------------------------------

/**
 * 一次播放的取舍结果。
 *
 * 用密封类而不是"一个路径 + 几个布尔"是有意的：调用方必须**逐个分支写出来**，
 * 将来加一种动作（比如"跳到某一秒"）编译器会立刻把漏掉的分支指出来，
 * 而不是安静地走错分支。
 */
sealed interface PlaybackAction {

    /** 现在没有在播：从头开始播这条。 */
    data class Start(val path: String) : PlaybackAction

    /** 就是这条在播：暂停。 */
    data object Pause : PlaybackAction

    /** 就是这条暂停着：从暂停处继续。 */
    data object Resume : PlaybackAction

    /** 播不了（多半是文件还没下下来），把原因告诉用户。 */
    data class Unavailable(val reason: String) : PlaybackAction
}

/**
 * 正在播放的语音。
 *
 * [paused] 和 [currentId] 是**两件事**：暂停时 [currentId] 还在（气泡上仍然显示这一条），
 * 只是没在往前走。把"暂停"表示成 `currentId = null` 的话，界面就没法把
 * 标题和进度留在那条气泡上了。
 */
data class VoicePlayback(
    val currentId: String? = null,
    val path: String? = null,
    val durationSeconds: Int = 0,
    val paused: Boolean = false,
) {
    val isPlaying: Boolean get() = currentId != null && !paused
}

/** 播放停止的原因，交给调用方决定要不要提示用户。 */
enum class StopReason { FINISHED, ERROR, USER }

/**
 * 「点一条语音气泡」时到底该干什么。
 *
 * 规则与微信一致，逐条都有理由：
 *
 *  1. **同一条正在播** → 暂停；再点一次 → 从暂停处继续（而不是从头开始）。
 *  2. **同一条已暂停** → 继续。
 *  3. **另一条** → 停掉旧的、从头播新的。两条同时响是谁也听不清的噪音。
 *  4. **文件还没下下来**（[path] 为 null）→ 什么都不做，但要说清为什么，
 *     否则用户看到的是"点了没反应"。
 */
fun decidePlay(current: VoicePlayback, fileId: String, path: String?): PlaybackAction = when {
    current.currentId == fileId && !current.paused -> PlaybackAction.Pause
    current.currentId == fileId && current.paused -> PlaybackAction.Resume
    path == null -> PlaybackAction.Unavailable("这条语音还没下载完，下载好再点")
    else -> PlaybackAction.Start(path)
}

/**
 * 播放开始/继续之后的状态。
 *
 * [durationSeconds] 传 0 表示"读不出来"，此时**保留**上一次读到的时长：
 * 续播时不该因为一次读取失败就把气泡上的 `0:32` 变回 `0:00`。
 */
fun afterPlaybackStarts(
    current: VoicePlayback,
    fileId: String,
    path: String,
    durationSeconds: Int,
): VoicePlayback = VoicePlayback(
    currentId = fileId,
    path = path,
    durationSeconds = if (durationSeconds > 0) durationSeconds else current.durationSeconds,
    paused = false,
)

/**
 * 播放结束（自然放完 / 出错 / 用户点了停止）之后的状态。
 *
 * 三种情况都必须**回到"没有在播"**：否则界面会一直显示暂停按钮，
 * 而用户再点一下只会得到一个什么都不做的"暂停"。
 */
fun afterPlaybackStops(current: VoicePlayback, reason: StopReason): VoicePlayback = when (reason) {
    StopReason.FINISHED, StopReason.ERROR, StopReason.USER -> current.copy(
        currentId = null,
        path = null,
        durationSeconds = 0,
        paused = false,
    )
}

/**
 * 这条语音在气泡上该显示多长。
 *
 * 优先用播放器读出来的**真实时长**：协议里 `FILE_OFFER` 只带字节数，
 * 没有时长字段（改协议要动两端和已经发出去的历史消息，不值当）。
 * 还没播过的语音就先显示 `0:00`——**不猜**：拿字节数按码率估出来的时长
 * 一旦偏了，用户会觉得"明明写着 3 秒，怎么放了 6 秒"。
 */
fun bubbleDurationSeconds(playbackSeconds: Int, totalSeconds: Int): String =
    VoiceMessage.formatDuration(if (totalSeconds > 0) totalSeconds else playbackSeconds)

/** 播放进度（0..1），给气泡上的进度条用。时长未知时按 0 处理。 */
fun playbackFraction(currentSeconds: Int, totalSeconds: Int): Float {
    if (totalSeconds <= 0) return 0f
    return (currentSeconds.toFloat() / totalSeconds).coerceIn(0f, 1f)
}
