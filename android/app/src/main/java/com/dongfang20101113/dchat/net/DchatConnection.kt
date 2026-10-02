package com.dongfang20101113.dchat.net

import com.dongfang20101113.dchat.protocol.CryptoSession
import com.dongfang20101113.dchat.protocol.DchatCrypto
import com.dongfang20101113.dchat.protocol.LineBuffer
import com.dongfang20101113.dchat.protocol.MAX_LINE_BYTES
import com.dongfang20101113.dchat.protocol.Message
import com.dongfang20101113.dchat.protocol.base64Decode
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.protocol.buildLine
import com.dongfang20101113.dchat.protocol.makePing
import com.dongfang20101113.dchat.protocol.parseLine
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

    /**
     * 加密会话。握手上成功后才非空；在此之前收发都是明文。
     *
     * 老服务器不认识 `HELLO`，握手会自然失败，这里就一直是 null——
     * 也就是保持明文，不会因为客户端多问了一句就连不上。
     */
    @Volatile private var crypto: CryptoSession? = null

    /**
     * 服务器公钥指纹。
     *
     * 给 TOFU 用：客户端应当把这个值记住，下次连同一台服务器时比对，
     * 变了就说明可能有人在中间（也可能是服务器换了密钥，两者都该让用户知道）。
     */
    @Volatile var serverFingerprint: String? = null
        private set

    /** 当前这条连接是否已加密。 */
    val encrypted: Boolean get() = crypto != null

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
    suspend fun connect(host: String, port: Int, encrypt: Boolean = true): Boolean = withContext(Dispatchers.IO) {
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

            // ---- 加密握手必须在这里、**启动接收循环之前**同步做完 ----
            // 握手期间要同步读几行（WELCOME / HELLO_OK）。如果接收协程已经在跑，
            // 两边会抢同一个输入流，读到的行会随机分给其中一边——极难排查。
            if (encrypt) performHandshake(sock)

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

    /**
     * 同步完成加密握手。
     *
     * - 服务器支持：派生会话密钥并激活 [crypto]
     * - 服务器不支持（回 `ERROR`）：**保持明文**——这是协商的自然降级，
     *   老服务器不会因为客户端多问了一句就连不上
     * - 服务器不回话：超时后按明文继续
     *
     * 握手期间读到的其它行（比如 `WELCOME`）会被送回 [lineChannel]，
     * 不会被丢掉。
     */
    private fun performHandshake(sock: Socket) {
        val pair = DchatCrypto.generateKeyPair()
        val clientNonce = DchatCrypto.randomBytes(DchatCrypto.HANDSHAKE_NONCE_BYTES)
        val hello = buildLine(
            "HELLO",
            "${DchatCrypto.CRYPTO_VERSION} ${base64Encode(pair.publicKey)} ${base64Encode(clientNonce)}",
        )
        if (!writeRawLine(sock, hello)) return

        val previousTimeout = sock.soTimeout
        sock.soTimeout = HANDSHAKE_TIMEOUT_MS
        try {
            val buffer = LineBuffer()
            val chunk = ByteArray(512)
            val deadline = System.currentTimeMillis() + HANDSHAKE_TIMEOUT_MS

            while (System.currentTimeMillis() < deadline) {
                val line = buffer.popLine()
                if (line == null) {
                    val read = try {
                        (input ?: return).read(chunk)
                    } catch (_: SocketTimeoutException) {
                        continue
                    }
                    if (read <= 0) return
                    buffer.append(chunk, 0, read)
                    if (buffer.bad) return
                    continue
                }

                when (val command = parseLine(line).command) {
                    "HELLO_OK" -> {
                        startCrypto(parseLine(line), pair, clientNonce)
                        return
                    }
                    // 服务器不认识 HELLO：说明是老版本，保持明文继续
                    "ERROR" -> {
                        lineChannel.trySend(line)
                        return
                    }
                    // WELCOME 之类：不能让它在握手期间丢掉
                    else -> {
                        if (command.isNotEmpty()) lineChannel.trySend(line)
                    }
                }
            }
        } finally {
            sock.soTimeout = previousTimeout
        }
    }

    /** 用 HELLO_OK 里的服务器公钥和随机数把会话密钥算出来并激活加密。 */
    private fun startCrypto(helloOk: Message, pair: DchatCrypto.EcdhKeyPair, clientNonce: ByteArray) {
        val words = helloOk.rest.split(' ').filter { it.isNotEmpty() }
        if (words.size < 2) return

        val serverPublic = runCatching { base64Decode(words[0]) }.getOrNull() ?: return
        val serverNonce = runCatching { base64Decode(words[1]) }.getOrNull() ?: return
        if (serverPublic.size != DchatCrypto.P256_PUBLIC_KEY_BYTES) return
        if (serverNonce.size != DchatCrypto.HANDSHAKE_NONCE_BYTES) return

        val shared = runCatching {
            DchatCrypto.computeSharedSecret(pair.privateKey, serverPublic)
        }.getOrNull() ?: return

        val keys = DchatCrypto.deriveSessionKeys(shared, clientNonce, serverNonce)
        if (!keys.valid) return

        // 客户端发用 c2s、收用 s2c（服务端正好相反）
        crypto = CryptoSession(keys.clientToServer, keys.serverToClient)
        serverFingerprint = DchatCrypto.publicKeyFingerprint(serverPublic)
    }

    private fun writeRawLine(sock: Socket, line: String): Boolean = try {
        val stream = sock.getOutputStream()
        stream.write(line.toByteArray(Charsets.UTF_8))
        stream.write('\n'.code)
        stream.flush()
        true
    } catch (_: Exception) {
        false
    }

    /** 发一行（自动补 `\n`）。返回是否发送成功。加密已启用时自动包成 `ENC <base64>`。 */
    suspend fun send(line: String): Boolean {
        if (line.isEmpty()) return false
        val out = output ?: return false
        return withContext(Dispatchers.IO) {
            sendMutex.withLock {
                try {
                    // 加密必须在锁内做：CryptoSession 的 nonce 计数器不是线程安全的，
                    // 两个协程同时加密就可能撞上同一个 nonce —— GCM 下这是致命的。
                    val payload = crypto?.let { session ->
                        buildLine("ENC", base64Encode(session.encrypt(line.toByteArray(Charsets.UTF_8))))
                    } ?: line
                    out.write(payload.toByteArray(Charsets.UTF_8))
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
        crypto = null
        serverFingerprint = null
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
                    val session = crypto
                    if (session == null) {
                        lineChannel.send(line)
                        continue
                    }

                    // 加密已启用：每一行都必须是 `ENC <base64>`。
                    // ⚠️ 收到明文就必须断开，不能"宽容地当明文处理"——
                    // 否则攻击者在加密通道里发明文指令就能绕过加密（降级攻击）。
                    val outer = parseLine(line)
                    if (outer.command != "ENC") {
                        reason = "加密已启用，服务器却发来了明文指令（${outer.command}）"
                        break
                    }
                    val sealed = runCatching { base64Decode(outer.rest) }.getOrNull()
                    if (sealed == null) {
                        reason = "收到无法解析的 ENC 行"
                        break
                    }
                    val plain = session.decrypt(sealed)
                    if (plain == null) {
                        // 认证失败 = 被篡改 / 密钥不对 / 顺序错乱。绝不能继续用这条连接。
                        reason = "解密失败（数据可能被篡改）"
                        break
                    }
                    lineChannel.send(String(plain, Charsets.UTF_8))
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
                reportUnexpectedLoss(reason ?: "连接已断开")
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
                    reportUnexpectedLoss("心跳发送失败，连接已中断")
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

    /**
     * 报告"意外掉线"，但**绝不覆盖用户主动断开**。
     *
     * 这里有个真实踩到的竞态：用户点断开时我们会先发 `QUIT`，服务端收到后立刻关连接，
     * 接收循环于是读到 -1 并准备报 `Lost`；与此同时 `disconnect()` 已经把状态设成了
     * `Disconnected`。接收循环随后直接赋值的话，就会把 `Disconnected` **覆盖**成 `Lost`，
     * 界面上表现为「用户明明是自己点的断开，却弹出一条错误」。
     *
     * 用 `compareAndSet` 解决：只有当状态**仍然是**连接中/已连接时才改；
     * 中途被 `disconnect()` 改过的话 CAS 会失败，我们就不改。
     */
    private fun reportUnexpectedLoss(reason: String) {
        if (userRequestedDisconnect) return
        val current = _state.value
        if (current !is ConnectionState.Connected && current !is ConnectionState.Connecting) return
        _state.compareAndSet(current, ConnectionState.Lost(host, port, reason))
    }

    companion object {
        /** 与桌面端一致的连接超时。 */
        const val CONNECT_TIMEOUT_MS: Int = 4000

        /** 心跳间隔：桌面端是 45 秒，这里保持一致。 */
        const val HEARTBEAT_INTERVAL_MS: Long = 45_000L

        /**
         * 加密握手的超时（毫秒）。
         *
         * 超时后按**明文**继续——老服务器不认识 `HELLO`，可能既不回 `HELLO_OK`
         * 也不回 `ERROR`（比如它把这行当成了普通聊天内容）。宁可退回明文可用，
         * 也不要让用户卡在连不上。
         */
        const val HANDSHAKE_TIMEOUT_MS: Int = 3_000
    }
}
