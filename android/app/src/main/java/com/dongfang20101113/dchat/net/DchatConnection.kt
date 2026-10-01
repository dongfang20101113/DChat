package com.dongfang20101113.dchat.net

import com.dongfang20101113.dchat.protocol.LineBuffer
import com.dongfang20101113.dchat.protocol.MAX_LINE_BYTES
import com.dongfang20101113.dchat.protocol.makePing
import kotlinx.coroutines.CancellationException
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/** 连接状态。 */
sealed interface ConnectionState {
    data object Disconnected : ConnectionState

    data class Connecting(val host: String, val port: Int) : ConnectionState

    /** 已连上服务器（**还没登录**——dchat 是先连接再登录的两步式）。 */
    data class Connected(val host: String, val port: Int) : ConnectionState

    /** 压根没连上（地址错、服务器没开、超时）。 */
    data class Failed(val host: String, val port: Int, val reason: String) : ConnectionState

    /**
     * 连上了，后来又断了。
     *
     * 和 [Failed] 分开是有意为之：[Failed] 是"没连上"，重试通常没意义；
     * [Lost] 是"本来好好的，断了"，界面应该显示**原因**并给一个重连入口，
     * 而不是像以前那样悄悄把用户弹回连接页、让人以为是自己点错了。
     */
    data class Lost(val host: String, val port: Int, val reason: String) : ConnectionState
}

/**
 * dchat 的 TCP 连接。
 *
 * 对应桌面端的「接收线程 + 心跳线程 + 加锁发送」三件套，这里用协程表达。
 *
 * ## 生命周期很重要
 *
 * [scope] **必须是进程级的**（见 [DchatSession]），不能传 `viewModelScope`：
 * 系统文件选择器、旋转屏幕、Activity 重建都会让 ViewModel 被清除，
 * 一旦用 `viewModelScope`，socket 就会被连带取消——表现就是"打开文件上传几秒后自动掉线"。
 */
class DchatConnection(private val scope: CoroutineScope) {

    private var socket: Socket? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null

    private var receiveJob: Job? = null
    private var heartbeatJob: Job? = null

    private val sendMutex = Mutex()

    /** 连接成功后记住地址，掉线时好写进 [ConnectionState.Lost]。 */
    @Volatile private var host: String = ""
    @Volatile private var port: Int = 0

    /** 是"用户自己点的断开"还是"意外掉线"——两者在界面上的表现必须不同。 */
    @Volatile private var userRequestedDisconnect = false

    private val _state = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    // 用 Channel 而不是 MutableSharedFlow：
    // SharedFlow **在没有订阅者时会直接丢弃发射**，而服务端连上就立刻发 WELCOME——
    // 只要它比收集协程先到，这条消息就永久丢了（实测在全量测试里偶发复现，单独跑却正常，正是竞态）。
    // Channel 无论有没有人收都会缓冲，不会丢。
    // 注意：receiveAsFlow() 的消费端只能有一个（应用里由 DchatSession 独占）。
    private val lineChannel = Channel<String>(capacity = Channel.UNLIMITED)
    val lines: Flow<String> = lineChannel.receiveAsFlow()

    /**
     * 连接服务器。成功返回 true；失败把原因写进 [ConnectionState.Failed]。
     */
    suspend fun connect(host: String, port: Int): Boolean = withContext(Dispatchers.IO) {
        // 先清掉旧连接；disconnect() 会把 userRequestedDisconnect 置位，所以下面要重置
        disconnect()
        userRequestedDisconnect = false
        this@DchatConnection.host = host
        this@DchatConnection.port = port
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
            _state.value = ConnectionState.Failed(host, port, describe(e, host))
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

    /**
     * **用户主动断开**。会结束接收与心跳协程。
     *
     * 和意外掉线的区别在于状态被设成 [ConnectionState.Disconnected]（界面回到连接页是预期行为，
     * 因为用户自己点的），而不是 [ConnectionState.Lost]。
     */
    fun disconnect() {
        userRequestedDisconnect = true
        receiveJob?.cancel()
        heartbeatJob?.cancel()
        receiveJob = null
        heartbeatJob = null
        cleanup()
        _state.value = ConnectionState.Disconnected
    }

    private suspend fun receiveLoop(sock: Socket) {
        val buffer = LineBuffer()
        val chunk = ByteArray(2048)   // 与桌面端相同的读取块大小
        var reason: String? = null

        try {
            val stream = input ?: return
            while (scope.isActive && !sock.isClosed) {
                val read = try {
                    stream.read(chunk)
                } catch (e: SocketTimeoutException) {
                    reason = "读取超时（网络可能已中断）"
                    break
                }

                if (read < 0) {
                    reason = "服务器关闭了连接"
                    break
                }
                if (read == 0) continue

                buffer.append(chunk, 0, read)

                if (buffer.bad) {
                    // 单行超过协议上限：服务端也会这么判，属于协议层错误，必须断开
                    reason = "收到超长数据行（协议上限 $MAX_LINE_BYTES 字节），连接已关闭"
                    lineChannel.trySend("ERROR 00:00 $reason")
                    break
                }
                while (true) {
                    val line = buffer.popLine() ?: break
                    lineChannel.send(line)
                }
            }
        } catch (e: CancellationException) {
            // 协程被取消 = 正常收尾（用户断开或进程退出），不要当成错误
            throw e
        } catch (e: IOException) {
            reason = "网络中断：${e.message ?: e::class.simpleName}"
        } catch (e: Exception) {
            reason = e.message ?: e::class.simpleName ?: "未知错误"
        } finally {
            // 只有"不是用户主动断开"时才报掉线——否则用户点断开后还会看到一条错误提示
            if (!userRequestedDisconnect) {
                cleanup()
                _state.value = ConnectionState.Lost(host, port, reason ?: "连接已断开")
            }
        }
    }

    private suspend fun heartbeatLoop() {
        while (scope.isActive) {
            delay(HEARTBEAT_INTERVAL_MS)
            if (!send(makePing())) {
                // 发不出去了。这里**必须改状态**：以前只是 break，
                // 界面会一直显示"已连接"而实际早断了（僵尸状态），用户完全察觉不到。
                if (!userRequestedDisconnect) {
                    cleanup()
                    _state.value = ConnectionState.Lost(host, port, "心跳发送失败，连接已中断")
                }
                return
            }
        }
    }

    private fun cleanup() {
        try { socket?.close() } catch (_: Exception) { }
        socket = null
        input = null
        output = null
    }

    private fun describe(e: Exception, host: String): String = when (e) {
        is SocketTimeoutException -> "连接超时（服务器没响应）"
        is java.net.UnknownHostException -> "找不到这个地址：$host"
        is java.net.ConnectException -> "对方拒绝连接（服务器没开？端口不对？）"
        else -> e.message ?: e::class.simpleName ?: "未知错误"
    }

    companion object {
        /** 与桌面端一致的连接超时。 */
        const val CONNECT_TIMEOUT_MS: Int = 4000

        /** 心跳间隔：桌面端是 45 秒，这里保持一致。 */
        const val HEARTBEAT_INTERVAL_MS: Long = 45_000L
    }
}
