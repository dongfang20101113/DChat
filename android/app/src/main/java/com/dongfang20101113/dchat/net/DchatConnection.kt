package com.dongfang20101113.dchat.net

import com.dongfang20101113.dchat.protocol.LineBuffer
import com.dongfang20101113.dchat.protocol.makePing
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket

/** 连接状态。 */
sealed interface ConnectionState {
    data object Disconnected : ConnectionState

    data class Connecting(val host: String, val port: Int) : ConnectionState

    /** 已连上服务器（**还没登录**——dchat 是先连接再登录的两步式）。 */
    data class Connected(val host: String, val port: Int) : ConnectionState

    data class Failed(val host: String, val port: Int, val reason: String) : ConnectionState
}

/**
 * dchat 的 TCP 连接。
 *
 * 对应桌面端的"接收线程 + 心跳线程 + 加锁发送"三件套，这里用协程表达：
 *  - [receiveLoop] 一个协程负责 recv → [LineBuffer] 切行 → 发到 [lines] 流；
 *  - [heartbeatLoop] 每 45 秒发一次 `PING`（和桌面端同一个间隔，防 NAT/路由器回收空闲连接）；
 *  - 发送用 [sendMutex] 串行化，保证多协程并发写不会把一行拆散。
 *
 * 连接超时用 4 秒，也与桌面端一致。
 */
class DchatConnection(private val scope: CoroutineScope) {

    private var socket: Socket? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null

    private var receiveJob: Job? = null
    private var heartbeatJob: Job? = null

    private val sendMutex = Mutex()

    private val _state = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    // 用 Channel 而不是 MutableSharedFlow：
    // SharedFlow **在没有订阅者时会直接丢弃发射**，而服务端连上就立刻发 WELCOME——
    // 只要它比收集协程先到，这条消息就永久丢了（实测在全量测试里偶发复现，
    // 单独跑却正常，正是典型的竞态）。Channel 无论有没有人收都会缓冲，不会丢。
    // 注意：receiveAsFlow() 的消费端只能有一个（应用里就是 ViewModel 那一个收集器）。
    private val lineChannel = Channel<String>(capacity = Channel.UNLIMITED)
    val lines: Flow<String> = lineChannel.receiveAsFlow()

    val isConnected: Boolean get() = socket?.isConnected == true && socket?.isClosed == false

    /**
     * 连接服务器。成功返回 true；失败把原因写进 [ConnectionState.Failed]。
     */
    suspend fun connect(host: String, port: Int): Boolean = withContext(Dispatchers.IO) {
        disconnect()
        _state.value = ConnectionState.Connecting(host, port)

        try {
            val sock = Socket()
            sock.tcpNoDelay = true          // 聊天是小包，禁用 Nagle 让消息更跟手
            sock.keepAlive = true
            sock.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)

            socket = sock
            input = sock.getInputStream()
            output = sock.getOutputStream()
            _state.value = ConnectionState.Connected(host, port)

            receiveJob = scope.launch(Dispatchers.IO) { receiveLoop(sock) }
            heartbeatJob = scope.launch(Dispatchers.IO) { heartbeatLoop() }
            true
        } catch (e: Exception) {
            val reason = when (e) {
                is java.net.SocketTimeoutException -> "连接超时（服务器没响应）"
                is java.net.UnknownHostException -> "找不到这个地址：$host"
                is java.net.ConnectException -> "对方拒绝连接（服务器没开？端口不对？）"
                else -> e.message ?: e::class.simpleName ?: "未知错误"
            }
            _state.value = ConnectionState.Failed(host, port, reason)
            cleanup()
            false
        }
    }

    /** 发一行（自动补 `\n`）。返回是否发送成功。 */
    suspend fun send(line: String): Boolean {
        if (line.isEmpty()) return false
        val out = output ?: return false
        return withContext(Dispatchers.IO) {
            sendMutex.withLock {
                try {
                    out.write(line.toByteArray(Charsets.UTF_8))
                    out.write('\n'.code)
                    out.flush()
                    true
                } catch (_: Exception) {
                    false
                }
            }
        }
    }

    /** 主动断开。会结束接收与心跳协程。 */
    fun disconnect() {
        receiveJob?.cancel()
        heartbeatJob?.cancel()
        receiveJob = null
        heartbeatJob = null
        cleanup()
        if (_state.value !is ConnectionState.Failed) {
            _state.value = ConnectionState.Disconnected
        }
    }

    private suspend fun receiveLoop(sock: Socket) {
        val buffer = LineBuffer()
        val chunk = ByteArray(2048)   // 与桌面端相同的读取块大小
        try {
            val stream = input ?: return
            while (scope.isActive && !sock.isClosed) {
                val read = stream.read(chunk)
                if (read <= 0) break
                buffer.append(chunk, 0, read)

                if (buffer.bad) {
                    // 单行超长：服务端也会这么判，直接断开
                    lineChannel.send("ERROR ${com.dongfang20101113.dchat.protocol.formatTime(0, 0)} 单行数据过长，连接已关闭")
                    break
                }
                while (true) {
                    val line = buffer.popLine() ?: break
                    lineChannel.send(line)
                }
            }
        } catch (_: Exception) {
            // 连接被关闭 / 网络断开：正常结束路径
        } finally {
            cleanup()
            _state.value = ConnectionState.Disconnected
        }
    }

    private suspend fun heartbeatLoop() {
        while (scope.isActive) {
            delay(HEARTBEAT_INTERVAL_MS)
            if (!send(makePing())) break
        }
    }

    private fun cleanup() {
        try { socket?.close() } catch (_: Exception) { }
        socket = null
        input = null
        output = null
    }

    companion object {
        /** 与桌面端一致的连接超时。 */
        const val CONNECT_TIMEOUT_MS: Int = 4000

        /** 心跳间隔：桌面端是 45 秒，这里保持一致。 */
        const val HEARTBEAT_INTERVAL_MS: Long = 45_000L
    }
}
