package com.dongfang20101113.dchat.ui

import android.app.Application
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.dongfang20101113.dchat.data.SettingsStore
import com.dongfang20101113.dchat.net.ConnectionState
import com.dongfang20101113.dchat.net.DchatConnection
import com.dongfang20101113.dchat.protocol.FILE_CHUNK_BYTES
import com.dongfang20101113.dchat.protocol.MAX_FILE_BYTES
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.protocol.chunkFile
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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.time.LocalTime
import java.time.format.DateTimeFormatter
import java.util.concurrent.atomic.AtomicLong

/**
 * 连接、登录、聊天的全部业务动作。
 *
 * 状态迁移本身在 [reduce]（纯函数）里，这里只负责：
 *  - 把网络事件喂给 reduce
 *  - 文件收发的落盘
 *  - 与界面交互的入口
 */
class ChatViewModel(app: Application) : AndroidViewModel(app) {

    private val connection = DchatConnection(viewModelScope)
    private val settings = SettingsStore(app)

    private val _state = MutableStateFlow(ChatState())
    val state: StateFlow<ChatState> = _state.asStateFlow()

    /** 正在下载的文件：fileId → 累积字节。下载完成才落盘。 */
    private val incoming = mutableMapOf<String, ByteArrayOutputStream>()

    /** 下载完成后待写的文件名（FILE_BEGIN 给的名字）。 */
    private val pendingNames = mutableMapOf<String, String>()

    private val transferSeq = AtomicLong(0)

    init {
        // 服务器每一行 → 纯函数 reduce
        viewModelScope.launch {
            connection.lines.collect { raw ->
                val line = ServerLine.parse(raw, _state.value.selfNick)
                handleFileSideEffects(line)
                _state.value = _state.value.reduce(line, nowTime())
            }
        }

        // 连接状态 → 界面
        viewModelScope.launch {
            connection.state.collect { st ->
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
                        connected = false, connecting = false,
                        connectionError = st.reason, stage = Stage.CONNECT,
                    )
                }
            }
        }

        // 记住上次的地址端口
        viewModelScope.launch {
            val prefs = settings.connection.first()
            _state.value = _state.value.copy(host = prefs.host, port = prefs.port)
        }
    }

    // ------------------------------------------------------------------
    // 连接
    // ------------------------------------------------------------------

    fun updateHost(host: String) { _state.value = _state.value.copy(host = host) }
    fun updatePort(port: Int) { _state.value = _state.value.copy(port = port) }

    fun connect() {
        val s = _state.value
        viewModelScope.launch {
            settings.saveConnection(s.host, s.port)
            connection.connect(s.host, s.port)
        }
    }

    fun disconnect() {
        viewModelScope.launch {
            connection.send(makeQuit())
            connection.disconnect()
        }
        incoming.clear()
        pendingNames.clear()
        _state.value = _state.value.copy(
            stage = Stage.CONNECT, selfNick = "", onlineNicks = emptyList(),
            knownNicks = emptyList(), items = emptyList(), unread = 0,
        )
    }

    // ------------------------------------------------------------------
    // 登录 / 注册
    // ------------------------------------------------------------------

    fun login(nick: String, password: String) {
        _state.value = _state.value.copy(loggingIn = true, authError = null)
        viewModelScope.launch {
            if (!connection.send(makeLogin(nick, password))) {
                _state.value = _state.value.copy(loggingIn = false, authError = "发送失败，连接可能已断开")
            }
        }
    }

    fun register(nick: String, password: String) {
        _state.value = _state.value.copy(loggingIn = true, authError = null)
        viewModelScope.launch {
            // 注意：服务端注册成功后**不会自动登录**，桌面端也是注册完再发一次 LOGIN
            if (!connection.send(makeRegister(nick, password))) {
                _state.value = _state.value.copy(loggingIn = false, authError = "发送失败，连接可能已断开")
            }
        }
    }

    // ------------------------------------------------------------------
    // 聊天
    // ------------------------------------------------------------------

    /** 发一条聊天。返回发送是否成功。 */
    fun sendMessage(text: String) {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return
        viewModelScope.launch {
            if (!connection.send(makeMessage(trimmed))) {
                _state.value = _state.value.reduce(
                    ServerLine.Error("", "消息发送失败，连接可能已断开"), nowTime(),
                )
            }
        }
    }

    fun markRead() { _state.value = _state.value.copy(unread = 0) }

    fun noteUnread() { _state.value = _state.value.copy(unread = _state.value.unread + 1) }

    // ------------------------------------------------------------------
    // 文件：下载
    // ------------------------------------------------------------------

    /** 点卡片上的「下载」。 */
    fun requestDownload(fileId: String) {
        incoming[fileId] = ByteArrayOutputStream()
        viewModelScope.launch { connection.send(makeFileGet(fileId)) }
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
                pendingNames.remove(line.fileId)
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
        viewModelScope.launch(Dispatchers.IO) {
            val dir = receivedDir(getApplication())
            val name = sanitizeFileName(rawName)
            val existing = dir.list()?.toSet() ?: emptySet()
            val unique = makeUniqueName(name) { it in existing }
            val target = File(dir, unique)
            try {
                target.writeBytes(bytes)
                _state.value = _state.value.updateFileCard(fileId) {
                    it.copy(state = FileState.DONE, progress = 100, savedPath = target.absolutePath)
                }
            } catch (e: Exception) {
                _state.value = _state.value.updateFileCard(fileId) {
                    it.copy(state = FileState.FAILED, note = "写入失败：${e.message}")
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 文件：发送
    // ------------------------------------------------------------------

    /** 选好文件后调用。会在后台读文件、分块发出去。 */
    fun sendFile(uri: Uri) {
        val s = _state.value
        if (s.selfNick.isEmpty()) return
        viewModelScope.launch(Dispatchers.IO) {
            val ctx = getApplication<Application>()
            val resolved = resolveFile(ctx, uri)
            if (resolved == null) {
                _state.value = _state.value.reduce(ServerLine.Error("", "读取文件失败"), nowTime())
                return@launch
            }
            val (name, size, reader) = resolved

            if (size > MAX_FILE_BYTES) {
                _state.value = _state.value.reduce(
                    ServerLine.Error("", "文件太大（上限 ${MAX_FILE_BYTES / 1024 / 1024} MB）"), nowTime(),
                )
                return@launch
            }
            if (size > s.maxFileMb.toLong() * 1024 * 1024) {
                _state.value = _state.value.reduce(
                    ServerLine.Error("", "超过服务器上限（当前 ${s.maxFileMb} MB）"), nowTime(),
                )
                return@launch
            }

            val transferId = "T" + transferSeq.incrementAndGet()
            val nameBase64 = base64Encode(name.toByteArray(Charsets.UTF_8))
            if (!connection.send(makeFileSend(transferId, nameBase64, size))) return@launch

            val buffer = ByteArray(FILE_CHUNK_BYTES)
            var sent = 0L
            try {
                reader.use { stream ->
                    while (true) {
                        val read = stream.read(buffer)
                        if (read <= 0) break
                        val piece = if (read == buffer.size) buffer else buffer.copyOf(read)
                        if (!connection.send(makeFileChunk(transferId, base64Encode(piece)))) return@launch
                        sent += read
                    }
                }
                connection.send(makeFileEnd(transferId))
                _state.value = _state.value.reduce(
                    ServerLine.Sys("", "已发送文件：$name（${sent / 1024} KB）"), nowTime(),
                )
            } catch (e: Exception) {
                _state.value = _state.value.reduce(
                    ServerLine.Error("", "发送失败：${e.message}"), nowTime(),
                )
            }
        }
    }

    private fun resolveFile(ctx: Context, uri: Uri): Triple<String, Long, java.io.InputStream>? {
        return try {
            var name = "file"
            var size = -1L
            ctx.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val nameIdx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeIdx = cursor.getColumnIndex(OpenableColumns.SIZE)
                    if (nameIdx >= 0) cursor.getString(nameIdx)?.let { name = it }
                    if (sizeIdx >= 0) size = cursor.getLong(sizeIdx)
                }
            }
            val stream = ctx.contentResolver.openInputStream(uri) ?: return null
            if (size < 0) {
                // 有些 provider 不给大小：先读进内存算出来（上限已由调用方检查）
                val bytes = stream.readBytes()
                Triple(sanitizeFileName(name), bytes.size.toLong(), bytes.inputStream())
            } else {
                Triple(sanitizeFileName(name), size, stream)
            }
        } catch (_: Exception) {
            null
        }
    }

    // ------------------------------------------------------------------

    private fun nowTime(): String = LocalTime.now().format(DateTimeFormatter.ofPattern("HH:mm"))

    companion object {
        /** 收到的文件放这里：`Android/data/<包名>/files/Download/dchat/`，不需要存储权限。 */
        fun receivedDir(ctx: Context): File {
            val base = ctx.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: ctx.filesDir
            val dir = File(base, "dchat")
            if (!dir.exists()) dir.mkdirs()
            return dir
        }
    }
}

/** 改一张文件卡片（ViewModel 侧的小工具，和 reducer 里的同名逻辑分开，避免暴露内部实现）。 */
private fun ChatState.updateFileCard(fileId: String, transform: (ChatItem.FileItem) -> ChatItem.FileItem): ChatState {
    val list = items.map { item ->
        if (item is ChatItem.FileItem && item.fileId == fileId) transform(item) else item
    }
    return copy(items = list)
}
