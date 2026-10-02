package com.dongfang20101113.dchat.voice

/**
 * 录音 / 播放的**接口层**：真正碰硬件的只有这两个接口的实现。
 *
 * 分成两个文件是有意的：
 *
 *  - `VoiceDecisions.kt` —— 纯函数，一行 Android API 都没有，能在 JVM 上全测；
 *  - 本文件 —— 只放接口；
 *  - `AndroidVoice.kt` —— `MediaRecorder` / `MediaPlayer` 的真实实现。
 *
 * 这样单测里塞一个假实现就能验"按住说话 → 文件多大 → 发不发得出去"的整条链路，
 * 不需要麦克风，也不需要真机。
 */

/** 录音机。实现见 [AndroidVoiceRecorder]。 */
interface VoiceRecorder {

    /**
     * 开始录音，写到 [targetPath]。
     *
     * 失败返回 false，原因放进 [lastError]——调用方只管"成没成、为什么"，
     * 不需要知道 `MediaRecorder` 那个一碰就炸的状态机。
     */
    fun start(targetPath: String): Boolean

    /**
     * 结束并落盘，返回容器自己报的时长（秒）；读不出来返回 0。
     *
     * 要传 [targetPath]：`MediaRecorder` **不提供**"录了多久"这个属性，
     * 只能等文件（MP4 的索引）写完之后再从文件里读时长。
     */
    fun stop(targetPath: String): Int

    /** 放弃这次录音（用户取消 / 太短），并**确保不留残文件**。 */
    fun cancel()

    /** 最近一次失败的原因（给用户看）。 */
    val lastError: String?
}

/** 播放器。实现见 [AndroidVoicePlayer]。 */
interface VoicePlayer {

    /**
     * 开始播放 [path]。已有其他文件在播时，实现负责先停掉它。
     *
     * @return 音频总时长（秒）；读不出来（0 或负数）返回 0
     */
    fun play(path: String): Int

    /** 暂停（能继续）。 */
    fun pause()

    /** 从暂停处恢复。 */
    fun resume()

    /** 彻底停止并释放资源。 */
    fun stop()

    /**
     * 播放到哪儿了（秒）。
     *
     * 一秒问一次就够。**不要每帧去问**：`MediaPlayer.getCurrentPosition()`
     * 是一次跨进程调用，高频轮询会把主线程拖卡。
     */
    fun currentSeconds(): Int

    /** 最近一次失败的原因（给用户看）。 */
    val lastError: String?
}
