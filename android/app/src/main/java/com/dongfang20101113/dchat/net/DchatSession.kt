package com.dongfang20101113.dchat.net

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import com.dongfang20101113.dchat.data.SettingsStore
import com.dongfang20101113.dchat.protocol.FILE_CHUNK_BYTES
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.protocol.copyWithLimit
import com.dongfang20101113.dchat.protocol.escapeText
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
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.FileState
import com.dongfang20101113.dchat.ui.Stage
import com.dongfang20101113.dchat.ui.reduce
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.time.LocalTime
import java.time.format.DateTimeFormatter
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

    private val transferSeq = AtomicLong(0)

    @Volatile private var initialised = false

    /** 由 [com.dongfang20101113.dchat.DchatApp] 在 Application.onCreate 里调用。 */
    fun init(context: Context) {
        if (initialised) return
        initialised = true
        appContext = context.applicationContext
        settings = SettingsStore(appContext)

        scope.launch {
            connection.lines.collect { raw ->
                val line = ServerLine.parse(raw, _state.value.selfNick)
                handleFileSideEffects(line)
                _state.value = _state.value.reduce(line, nowTime())
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
        _state.value = s.copy(connectionError = null)
        scope.launch {
            settings.saveConnection(s.host, s.port)
            connection.connect(s.host, s.port)
        }
    }

    /** 用户主动断开。 */
    fun disconnect() {
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

            val safeName = sanitizeFileName(name)
            val transferId = "T" + transferSeq.incrementAndGet()
            val nameBase64 = base64Encode(safeName.toByteArray(Charsets.UTF_8))

            try {
                if (!connection.send(makeFileSend(transferId, nameBase64, total))) {
                    appendLocalError("发送失败：连接已断开")
                    return@launch
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
                            return@launch
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
            } finally {
                spool.delete()
            }
        }
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
}
