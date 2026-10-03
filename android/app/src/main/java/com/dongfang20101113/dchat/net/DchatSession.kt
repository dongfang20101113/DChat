package com.dongfang20101113.dchat.net

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import com.dongfang20101113.dchat.data.SettingsStore
import com.dongfang20101113.dchat.protocol.AttachmentKind
import com.dongfang20101113.dchat.protocol.ChatColor
import com.dongfang20101113.dchat.protocol.FILE_CHUNK_BYTES
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.VoiceMessage
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.protocol.copyWithLimit
import com.dongfang20101113.dchat.protocol.decideTrust
import com.dongfang20101113.dchat.protocol.escapeText
import com.dongfang20101113.dchat.protocol.TrustDecision
import com.dongfang20101113.dchat.protocol.makeFileChunk
import com.dongfang20101113.dchat.protocol.makeFileEnd
import com.dongfang20101113.dchat.protocol.makeFileGet
import com.dongfang20101113.dchat.protocol.makeFileSend
import com.dongfang20101113.dchat.protocol.makeLogin
import com.dongfang20101113.dchat.protocol.makeMessage
import com.dongfang20101113.dchat.protocol.makeQuit
import com.dongfang20101113.dchat.protocol.makeRegister
import com.dongfang20101113.dchat.protocol.makeUniqueName
import com.dongfang20101113.dchat.protocol.sanitizeFileName
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.FileState
import com.dongfang20101113.dchat.ui.Stage
import com.dongfang20101113.dchat.ui.reduce
import com.dongfang20101113.dchat.voice.AndroidVoicePlayer
import com.dongfang20101113.dchat.voice.AndroidVoiceRecorder
import com.dongfang20101113.dchat.voice.PlaybackAction
import com.dongfang20101113.dchat.voice.SendDecision
import com.dongfang20101113.dchat.voice.StopReason
import com.dongfang20101113.dchat.voice.VoicePlayback
import com.dongfang20101113.dchat.voice.VoicePlayer
import com.dongfang20101113.dchat.voice.VoiceRecorder
import com.dongfang20101113.dchat.voice.VoiceRecordingFlow
import com.dongfang20101113.dchat.voice.afterPlaybackStarts
import com.dongfang20101113.dchat.voice.afterPlaybackStops
import com.dongfang20101113.dchat.voice.decidePlay
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.time.LocalTime
import java.time.format.DateTimeFormatter
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

/**
 * 进程级会话：**连接和聊天状态都属于进程，不属于 Activity**。
 *
 * ## 为什么必须是单例
 *
 * 之前的实现把 [DchatConnection] 挂在 `AndroidViewModel` 的 `viewModelScope` 上。
 * 结果：**点「📎」打开系统文件选择器 → MainActivity 被 stop → 系统内存紧张时销毁它 →
 * ViewModel 被清除 → viewModelScope 取消 → socket 被连带关闭 → 界面弹回连接页**。
 * 用户看到的就是"打开文件上传几秒钟后自动退出服务器"。
 *
 * 同样的道理，旋转屏幕、切换深色模式、Activity 重建都会触发这个问题。
 *
 * 把连接和状态放到 Application 作用域之后，这些事件都拿不走它：
 * Activity 重建后重新 `collect` 一下就有完整状态（聊天记录、在线名单、未读数都还在）。
 *
 * ## 局限（要诚实说明）
 *
 * 这只保住了 **Activity** 的生命周期，保不住**进程**。如果系统把整个 App 进程回收了
 * （低内存、厂商省电策略），连接仍然会断。要连进程一起保住，得用前台服务
 * （会常驻一条通知），目前没做。
 */
object DchatSession {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    val connection: DchatConnection by lazy { DchatConnection(scope) }

    private lateinit var appContext: Context
    private lateinit var settings: SettingsStore

    private val _state = MutableStateFlow(ChatState())
    val state: StateFlow<ChatState> = _state.asStateFlow()

    /** 正在下载的文件：fileId → 累积字节。 */
    private val incoming = mutableMapOf<String, ByteArrayOutputStream>()

    /** 下载中待写的文件名（FILE_BEGIN 给的）。 */
    private val pendingNames = mutableMapOf<String, String>()

    // ---- 语音消息（阶段 5）----
    //
    // 录音/播放的真实实现挂在接口后面（[recorder] / [voicePlayer] 是 var），
    // 决策部分（什么时候该停、太短要不要发）在 [VoiceRecordingFlow] 里，
    // 单测直接测那个类，不需要麦克风、不需要真机、也不需要网络。
    //
    // 默认先给空实现：这样比如单元测试里只 init 了一半就用到会话时，
    // 拿到的是"什么也不做"而不是 `lateinit` 未初始化崩溃。

    /** 录音机。init 时替换成 Android 实现。 */
    var recorder: VoiceRecorder = NoopVoiceRecorder

    /** 播放器。init 时替换成 Android 实现。 */
    var voicePlayer: VoicePlayer = NoopVoicePlayer

    /**
     * 「按住说话」的整条决策流程。
     *
     * `by lazy` 是为了让它拿到**当时的** [recorder]：单测会在建好会话之后
     * 换成自己的假实现，早绑定就会一直用那个空实现。
     */
    private val recFlow: VoiceRecordingFlow by lazy {
        VoiceRecordingFlow(recorder = recorder, dir = { voiceDir() })
    }

    /** 录音计时协程；停录时取消。 */
    private var recordTimer: Job? = null

    /** 播放进行态（内存里，界面要的部分同步进 [ChatState]）。 */
    private var playback = VoicePlayback()

    @Volatile private var initialised = false

    /** 由 [com.dongfang20101113.dchat.DchatApp] 在 Application.onCreate 里调用。 */
    fun init(context: Context) {
        if (initialised) return
        initialised = true
        appContext = context.applicationContext
        settings = SettingsStore(appContext)

        // 语音的真实实现：录音走 MediaRecorder、放音走 MediaPlayer
        recorder = AndroidVoiceRecorder(appContext)
        voicePlayer = AndroidVoicePlayer(appContext).also { player ->
            // 播完 / 播放出错都要把界面上的"正在播"收回去，否则气泡会一直显示暂停按钮
            player.onFinished = { stopVoice(StopReason.FINISHED) }
            player.onFailed = { reason -> stopVoice(StopReason.ERROR, reason) }
        }

        scope.launch {
            connection.lines.collect { raw ->
                val line = ServerLine.parse(raw, _state.value.selfNick)
                handleFileSideEffects(line)
                _state.value = _state.value.reduce(line, nowTime())

                // **贴纸和语音自动下载**，普通文件不自动下。
                // 判断走的是 ChatItem 上那条被测试钉死的规则（kind != FILE && OFFERED），
                // 而不是在这里另写一份 —— 两份判断迟早会不一致。
                val item = (_state.value.items.lastOrNull() as? ChatItem.FileItem)
                if (line is ServerLine.FileOffer && item?.fileId == line.fileId &&
                    item.shouldAutoDownload
                ) {
                    requestDownload(line.fileId)
                }
            }
        }

        scope.launch {
            connection.state.collect { st -> onConnectionState(st) }
        }

        scope.launch {
            val prefs = settings.connection.first()
            _state.value = _state.value.copy(host = prefs.host, port = prefs.port)
        }
    }

    private fun onConnectionState(st: ConnectionState) {
        _state.value = when (st) {
            is ConnectionState.Disconnected -> _state.value.copy(
                connected = false, connecting = false, stage = Stage.CONNECT,
            )

            is ConnectionState.Connecting -> _state.value.copy(
                connected = false, connecting = true, connectionError = null,
            )

            is ConnectionState.Connected -> _state.value.copy(
                connected = true, connecting = false, connectionError = null,
                stage = Stage.AUTH,
            )

            is ConnectionState.Failed -> _state.value.copy(
                connected = false, connecting = false, connectionError = st.reason,
                stage = Stage.CONNECT,
            )

            // 意外掉线：保留聊天记录，只提示原因，让用户一眼看出"不是我点错的"
            is ConnectionState.Lost -> _state.value.copy(
                connected = false, connecting = false, connectionError = st.reason,
                stage = Stage.CONNECT,
            )
        }
    }

    // ------------------------------------------------------------------
    // 连接
    // ------------------------------------------------------------------

    fun updateHost(host: String) { _state.value = _state.value.copy(host = host) }
    fun updatePort(port: Int) { _state.value = _state.value.copy(port = port) }

    fun connect() {
        val s = _state.value
        // 重连前清掉上次的错误提示，否则会一直挂在界面上
        _state.value = s.copy(connectionError = null, trust = TrustDecision.NotEncrypted)
        scope.launch {
            settings.saveConnection(s.host, s.port)
            if (!connection.connect(s.host, s.port)) return@launch

            // ---- TOFU：比对服务器指纹 ----
            // 加密本身不保证"对面是那台服务器"，只保证"这条通道没被偷听"。
            // 记住首次见到的指纹、以后每次比对，才能发现中间人。
            val current = connection.serverFingerprint
            val known = settings.trustedFingerprint(s.host, s.port)
            val decision = decideTrust(known, current)
            _state.value = _state.value.copy(trust = decision)

            // 首次见到就记下来。**指纹变了不自动覆盖**——
            // 那等于把警告变成了静默接受，TOFU 就白做了。
            // 必须由用户核对后调用 acceptNewFingerprint() 才更新。
            if (decision is TrustDecision.FirstUse) {
                settings.saveTrustedFingerprint(s.host, s.port, decision.fingerprint)
            }
        }
    }

    /**
     * 用户核对过之后，接受服务器换的新指纹。
     *
     * 只有 [TrustDecision.Changed] 状态下才有意义——这时界面应当已经把
     * 新旧指纹都显示出来了，用户确认"确实是我们自己换的密钥"。
     */
    fun acceptNewFingerprint() {
        val s = _state.value
        val changed = s.trust as? TrustDecision.Changed ?: return
        scope.launch {
            settings.saveTrustedFingerprint(s.host, s.port, changed.current)
            _state.value = _state.value.copy(trust = TrustDecision.FirstUse(changed.current))
        }
    }

    /** 忘掉这台服务器的指纹，下次连接按"首次"处理（用于用户确认服务器换过密钥）。 */
    fun forgetFingerprint() {
        val s = _state.value
        scope.launch {
            settings.forgetTrustedFingerprint(s.host, s.port)
            _state.value = _state.value.copy(trust = TrustDecision.NotEncrypted)
        }
    }

    /** 用户主动断开。 */
    fun disconnect() {
        cancelVoiceRecording()
        stopVoice(StopReason.USER)
        scope.launch { connection.send(makeQuit()) }
        connection.disconnect()
        incoming.clear()
        pendingNames.clear()
        _state.value = ChatState(host = _state.value.host, port = _state.value.port)
    }

    // ------------------------------------------------------------------
    // 登录 / 注册
    // ------------------------------------------------------------------

    fun login(nick: String, password: String) {
        _state.value = _state.value.copy(loggingIn = true, authError = null)
        scope.launch {
            if (!connection.send(makeLogin(nick, password))) {
                _state.value = _state.value.copy(loggingIn = false, authError = "发送失败，连接可能已断开")
            }
        }
    }

    fun register(nick: String, password: String) {
        _state.value = _state.value.copy(loggingIn = true, authError = null)
        scope.launch {
            // 服务端注册成功后**不会自动登录**，桌面端也是注册完再发一次 LOGIN
            if (!connection.send(makeRegister(nick, password))) {
                _state.value = _state.value.copy(loggingIn = false, authError = "发送失败，连接可能已断开")
            }
        }
    }

    // ------------------------------------------------------------------
    // 聊天
    // ------------------------------------------------------------------

    fun sendMessage(text: String) {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return
        // 本机指令（/chatcolor 等）就地处理，不发到服务器——
        // 它们只是把色码放进输入框、或往记录区写帮助，不需要联网。
        // 和桌面端 client.cpp 的 HandleLocalCommand 是同一条约定。
        if (handleLocalCommand(trimmed)) return
        // 换行必须先转义才能过行式协议（否则会被 buildLine 丢掉），
        // 服务端的 maxtextlen / maxtextlines 也是按转义后的形态统计的。
        val escaped = escapeText(trimmed)
        scope.launch {
            if (!connection.send(makeMessage(escaped))) {
                _state.value = _state.value.reduce(
                    ServerLine.Error("", "消息发送失败，连接可能已断开"), nowTime(),
                )
            }
        }
    }

    /**
     * 本机指令：`/chatcolor`（也认 `/charcolor`、`/chatcolour`）。
     *
     * - 不带参数或 `help`：把色码对照表写进聊天记录（方便对照着打）
     * - `choose`：请界面层弹取色盘（这里只翻一个标志位）
     *
     * 返回 true 表示这条已经处理掉了，不要再发服务器。
     */
    private fun handleLocalCommand(text: String): Boolean {
        if (!text.startsWith("/")) return false
        val body = text.substring(1)
        val name = body.substringBefore(' ').lowercase()
        val argument = if (body.contains(' ')) body.substringAfter(' ').trim() else ""
        if (name !in setOf("chatcolor", "charcolor", "chatcolour", "color", "colour")) return false

        when (argument.lowercase()) {
            "", "help", "?", "列表" -> _state.value = _state.value.reduce(
                ServerLine.Sys("", ChatColor.helpText()),
                nowTime(),
            )
            "choose", "pick", "色板" -> _state.value = _state.value.copy(showColorPicker = true)
            else -> _state.value = _state.value.reduce(
                ServerLine.Error("", "用法：/chatcolor help 看色码表，/chatcolor choose 弹色板"),
                nowTime(),
            )
        }
        return true
    }

    /** 取色盘选完颜色后由界面调用：把色码记下来，界面负责放进输入框。 */
    fun rememberPickedColor(code: String) {
        _state.value = _state.value.copy(pendingColorCode = code, showColorPicker = false)
    }

    /** 取色盘被关掉（没选）。 */
    fun dismissColorPicker() {
        _state.value = _state.value.copy(showColorPicker = false)
    }

    /** 色码已经放进输入框了，清掉待处理标记（否则下一轮又会插一次）。 */
    fun clearPendingColorCode() {
        _state.value = _state.value.copy(pendingColorCode = null)
    }

    fun markRead() { _state.value = _state.value.copy(unread = 0) }

    // ------------------------------------------------------------------
    // 文件：下载
    // ------------------------------------------------------------------

    fun requestDownload(fileId: String) {
        incoming[fileId] = ByteArrayOutputStream()
        scope.launch { connection.send(makeFileGet(fileId)) }
    }

    private fun handleFileSideEffects(line: ServerLine) {
        when (line) {
            is ServerLine.FileBegin -> {
                incoming[line.fileId] = ByteArrayOutputStream()
                pendingNames[line.fileId] = line.fileName
            }

            is ServerLine.FileData -> incoming[line.fileId]?.write(line.data)

            is ServerLine.FileEnd -> {
                val bytes = incoming.remove(line.fileId)?.toByteArray() ?: return
                val rawName = pendingNames.remove(line.fileId) ?: "file"
                saveReceivedFile(line.fileId, rawName, bytes)
            }

            is ServerLine.FileFail -> {
                incoming.remove(line.fileId)
                pendingNames.remove(line.fileId)
            }

            else -> Unit
        }
    }

    private fun saveReceivedFile(fileId: String, rawName: String, bytes: ByteArray) {
        scope.launch(Dispatchers.IO) {
            val dir = receivedDir(appContext)
            val name = sanitizeFileName(rawName)
            val existing = dir.list()?.toSet() ?: emptySet()
            val unique = makeUniqueName(name) { it in existing }
            val target = File(dir, unique)
            try {
                target.writeBytes(bytes)
                updateFileCard(fileId) {
                    it.copy(state = FileState.DONE, progress = 100, savedPath = target.absolutePath)
                }
            } catch (e: Exception) {
                updateFileCard(fileId) {
                    it.copy(state = FileState.FAILED, note = "写入失败：${e.message}")
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 文件：发送
    // ------------------------------------------------------------------

    /**
     * 选好文件后调用。
     *
     * **内存占用是常数（约 64 KB）**，与文件大小无关——这一点很重要：
     * 之前的实现遇到"provider 不返回文件大小"时会 `readBytes()` 把整个文件读进内存，
     * 选一个大视频就直接 OOM 崩掉。现在无论大小未知还是已知，都先流式复制到缓存文件
     * 并边读边计数，超过上限立刻中止，然后再从缓存文件分块发送。
     */
    fun sendFile(uri: Uri) {
        val s = _state.value
        if (s.selfNick.isEmpty()) return

        scope.launch(Dispatchers.IO) {
            val name = queryDisplayName(appContext, uri) ?: "file"
            val limit = minOf(s.maxFileMb.toLong() * 1024 * 1024, MAX_SEND_BYTES)
            val spool = File(appContext.cacheDir, "outgoing-${System.nanoTime()}.tmp")

            val total = try {
                spoolOutgoing(appContext, uri, spool, limit)
            } catch (e: Exception) {
                spool.delete()
                appendLocalError("读取文件失败：${e.message}")
                return@launch
            }

            if (total < 0) {
                spool.delete()
                appendLocalError("文件超过上限（当前服务器允许 ${s.maxFileMb} MB）")
                return@launch
            }
            if (total == 0L) {
                spool.delete()
                appendLocalError("这个文件是空的，发不了")
                return@launch
            }

            try {
                uploadSpoolFile(spool, sanitizeFileName(name), total, AttachmentKind.FILE)
            } finally {
                spool.delete()
            }
        }
    }

    /**
     * 把已经落盘的 [spool] 走文件通道发给服务器。
     *
     * **字节数由调用方传入**：语音是从缓存文件发的，选文件是先流式落到缓存再发的，
     * 两条路都不该在这里再 `File.length()` 一次（统计口径必须和真正发出去的字节一致）。
     *
     * [kind] 决定对方看到的是文件卡片、贴纸还是语音气泡。它作为 `FILE_SEND`
     * 的**最后一格**发出去；服务端只透传、不解释。
     */
    private suspend fun uploadSpoolFile(
        spool: File,
        safeName: String,
        total: Long,
        kind: AttachmentKind,
    ) {
        val transferId = "T" + UUID.randomUUID().toString().substring(0, 12)
        val nameBase64 = base64Encode(safeName.toByteArray(Charsets.UTF_8))

        try {
            if (!connection.send(
                    makeFileSend(transferId, nameBase64, total, VoiceMessage.fileSendSuffix(kind).trim()),
                )
            ) {
                appendLocalError("发送失败：连接已断开")
                return
            }

            var sent = 0L
            val buffer = ByteArray(FILE_CHUNK_BYTES)
            spool.inputStream().use { stream ->
                while (true) {
                    val read = stream.read(buffer)
                    if (read <= 0) break
                    val piece = if (read == buffer.size) buffer else buffer.copyOf(read)
                    if (!connection.send(makeFileChunk(transferId, base64Encode(piece)))) {
                        appendLocalError("发送中断：连接已断开")
                        return
                    }
                    sent += read
                }
            }
            connection.send(makeFileEnd(transferId))
            _state.value = _state.value.reduce(
                ServerLine.Sys("", "已发送文件：$safeName（${sent / 1024} KB）"), nowTime(),
            )
        } catch (e: Exception) {
            appendLocalError("发送失败：${e.message}")
        }
    }

    // ------------------------------------------------------------------
    // 语音消息：录音
    //
    // 决策（太短丢掉 / 超限拦截 / 大小校验）全在 [VoiceRecordingFlow] 里，
    // 那是纯逻辑、有单测；这里只负责接线：状态流、计时协程、把文件发出去。
    // ------------------------------------------------------------------

    /** 「按住说话」按下时调用。 */
    fun beginVoiceRecording() {
        if (_state.value.selfNick.isEmpty()) return         // 没登录不能发
        if (recFlow.isRecording) return                     // 已经在录了，忽略重复按下

        if (!recFlow.begin()) {
            appendLocalError(recorder.lastError ?: "录音失败")
            return
        }
        _state.value = _state.value.copy(recording = true, recordingSeconds = 0)

        // 计时并**到点自动收尾**：录音是有上限的（默认 5 分钟，见 VoiceMessage），
        // 到点还不停，用户会一直录到文件超限、最后被服务端拒绝——白录。
        recordTimer = scope.launch {
            while (isActive) {
                delay(RECORD_TICK_MILLIS)
                val elapsed = recFlow.elapsedSeconds() ?: return@launch
                _state.value = _state.value.copy(recordingSeconds = elapsed)

                if (elapsed >= VoiceMessage.MAX_DURATION_SECONDS) {
                    _state.value = _state.value.reduce(
                        ServerLine.Sys(
                            "",
                            "语音最长 ${VoiceMessage.formatDuration(VoiceMessage.MAX_DURATION_SECONDS)}，已自动发送",
                        ),
                        nowTime(),
                    )
                    finishVoiceRecording()
                    return@launch
                }
            }
        }
    }

    /** 「按住说话」松手时调用：太短就丢掉，够长就发。 */
    fun finishVoiceRecording() {
        recordTimer?.cancel()
        recordTimer = null
        _state.value = _state.value.copy(recording = false, recordingSeconds = 0)

        when (val decision = recFlow.finish()) {
            is SendDecision.Idle -> Unit      // 没在录（重复松手），什么都不做

            is SendDecision.TooShort -> appendLocalNotice("${decision.reason}，没发出去")

            is SendDecision.Rejected -> appendLocalError(decision.reason)

            is SendDecision.Send -> {
                val file = File(decision.path)
                scope.launch(Dispatchers.IO) {
                    try {
                        uploadSpoolFile(file, decision.fileName, decision.bytes, AttachmentKind.VOICE)
                    } finally {
                        // 发完就删：服务器已经存了一份，本地再留一份只会吃缓存。
                        // 想听自己刚说的那条，去下载服务器上那一份——和 QQ/微信的行为一致。
                        runCatching { file.delete() }
                    }
                }
            }
        }
    }

    /** 用户反悔（松手前滑走）时调用：不留文件、不发。 */
    fun cancelVoiceRecording() {
        recordTimer?.cancel()
        recordTimer = null
        recFlow.cancel()
        _state.value = _state.value.copy(recording = false, recordingSeconds = 0)
    }

    /** 录音文件放 App 私有缓存：不需要存储权限，系统也不会把它当"用户文件"备份走。 */
    private fun voiceDir(): File {
        val dir = File(appContext.cacheDir, "voice")
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    // ------------------------------------------------------------------
    // 语音消息：播放
    // ------------------------------------------------------------------

    /**
     * 点一条语音气泡。
     *
     * 同一条在播 → 暂停；同一条暂停 → 继续；另一条 → 停掉旧的从头播。
     * 取舍逻辑在 [decidePlay] 里，是纯函数、有单测。
     */
    fun toggleVoice(fileId: String, path: String?) {
        when (val action = decidePlay(playback, fileId, path)) {
            is PlaybackAction.Pause -> {
                voicePlayer.pause()
                playback = playback.copy(paused = true)
                publishVoiceState()
            }

            is PlaybackAction.Resume -> {
                voicePlayer.resume()
                playback = playback.copy(paused = false)
                publishVoiceState()
                startPlaybackTicker()
            }

            is PlaybackAction.Start -> startVoiceFile(fileId, action.path)

            is PlaybackAction.Unavailable -> appendLocalNotice(action.reason)
        }
    }

    /** 停掉正在播的语音（切换、断开连接、播完）。 */
    fun stopVoice(reason: StopReason = StopReason.USER, message: String? = null) {
        if (playback.currentId == null) return
        voicePlayer.stop()
        playback = afterPlaybackStops(playback, reason)
        publishVoiceState()
        if (message != null) appendLocalError(message)
    }

    private fun startVoiceFile(fileId: String, path: String) {
        voicePlayer.stop()                                  // 同一时刻只响一条
        val seconds = voicePlayer.play(path)
        // 约定：负数 = 没播成；0 = 播上了但读不出时长（短音频很常见，不算失败）
        if (seconds < 0) {
            playback = afterPlaybackStops(playback, StopReason.ERROR)
            publishVoiceState()
            appendLocalError(voicePlayer.lastError ?: "这条语音放不出来")
            return
        }
        playback = afterPlaybackStarts(playback, fileId, path, seconds)
        publishVoiceState()
        startPlaybackTicker()
    }

    /**
     * 每 250 毫秒读一次播放进度。
     *
     * 250 而不是每帧：`getCurrentPosition()` 是跨进程调用，按帧问会把主线程拖卡；
     * 而进度条是按秒走的，250 毫秒的余量已经够跟手了。
     */
    private fun startPlaybackTicker() {
        val id = playback.currentId ?: return
        scope.launch {
            while (isActive) {
                delay(PLAYBACK_TICK_MILLIS)
                if (playback.currentId != id || playback.paused) return@launch
                val seconds = voicePlayer.currentSeconds()
                if (seconds != _state.value.playingSeconds) {
                    _state.value = _state.value.copy(playingSeconds = seconds)
                }
            }
        }
    }

    /** 把内存里的 [playback] 同步到界面状态。 */
    private fun publishVoiceState() {
        _state.value = _state.value.copy(
            playingVoiceId = playback.currentId,
            playingSeconds = 0,
            playingDurationSeconds = playback.durationSeconds,
        )
    }

    private fun appendLocalNotice(text: String) {
        _state.value = _state.value.reduce(ServerLine.Sys("", text), nowTime())
    }

    /**
     * 把选中的内容流式复制到 [target] 并返回字节数。
     *
     * 返回 **-1** 表示超过 [limit]（已中止，调用方负责删掉 [target]）。
     * 内存占用是常数（一个 64 KB 缓冲区），**不会**把整个文件读进内存。
     */
    private fun spoolOutgoing(ctx: Context, uri: Uri, target: File, limit: Long): Long {
        val stream = ctx.contentResolver.openInputStream(uri) ?: return 0L
        stream.use { input ->
            target.outputStream().use { out ->
                return copyWithLimit(input, out, limit)
            }
        }
    }

    private fun queryDisplayName(ctx: Context, uri: Uri): String? = try {
        var name: String? = null
        ctx.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && cursor.moveToFirst()) name = cursor.getString(idx)
        }
        name
    } catch (_: Exception) {
        null
    }

    // ------------------------------------------------------------------

    private fun appendLocalError(text: String) {
        _state.value = _state.value.reduce(ServerLine.Error("", text), nowTime())
    }

    private fun updateFileCard(fileId: String, transform: (com.dongfang20101113.dchat.ui.ChatItem.FileItem) -> com.dongfang20101113.dchat.ui.ChatItem.FileItem) {
        val list = _state.value.items.map { item ->
            if (item is com.dongfang20101113.dchat.ui.ChatItem.FileItem && item.fileId == fileId) {
                transform(item)
            } else {
                item
            }
        }
        _state.value = _state.value.copy(items = list)
    }

    private fun nowTime(): String = LocalTime.now().format(TIME_FORMAT)

    private val TIME_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")

    /** 收到文件放这里：`Android/data/<包名>/files/Download/dchat/`，不需要存储权限。 */
    fun receivedDir(ctx: Context): File {
        val base = ctx.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: ctx.filesDir
        val dir = File(base, "dchat")
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    /** 每个 App 自己发文件时的硬上限，避免误选超大文件把缓存撑爆。 */
    private const val MAX_SEND_BYTES: Long = 512L * 1024 * 1024

    /** 录音计时的刷新间隔：界面上是按秒走的，250 毫秒足够跟手，也不会白烧电。 */
    private const val RECORD_TICK_MILLIS: Long = 250

    /** 播放进度的刷新间隔。理由见 [startPlaybackTicker]。 */
    private const val PLAYBACK_TICK_MILLIS: Long = 250
}

/** 没接上真实录音机时的空实现：什么都不做，也永远不报"成功"。 */
private object NoopVoiceRecorder : VoiceRecorder {
    override fun start(targetPath: String): Boolean = false
    override fun stop(targetPath: String): Int = 0
    override fun cancel() = Unit
    override val lastError: String? = "录音功能还没准备好"
}

/** 没接上真实播放器时的空实现。 */
private object NoopVoicePlayer : VoicePlayer {
    override fun play(path: String): Int = 0
    override fun pause() = Unit
    override fun resume() = Unit
    override fun stop() = Unit
    override fun currentSeconds(): Int = 0
    override val lastError: String? = "播放功能还没准备好"
}
