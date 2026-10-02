package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.net.ConnectionState
import com.dongfang20101113.dchat.net.DchatConnection
import com.dongfang20101113.dchat.protocol.Message
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.escapeText
import com.dongfang20101113.dchat.protocol.makeLogin
import com.dongfang20101113.dchat.protocol.makeMessage
import com.dongfang20101113.dchat.protocol.makeQuit
import com.dongfang20101113.dchat.protocol.makeRegister
import com.dongfang20101113.dchat.protocol.parseLine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.Collections
import java.util.concurrent.TimeUnit

/**
 * 连接生命周期测试——**连真实的 C++ 服务端**。
 *
 * 这里的每一条都对应一个**真机上暴露出来的事故**：
 *
 * 1. [连真实的 C++ 服务端跑通注册登录发消息]：协议能不能互通（基础）
 * 2. [没人收集消息的那段时间服务端发来的消息不会丢]：
 *    **点「📎」打开系统文件选择器 → Activity 被销毁 → 收集协程没了**。
 *    那几秒里服务端发的消息必须留着，回来还能看到，而不是永久丢失。
 * 3. [用户主动断开后状态是 Disconnected 而不是 Lost]：
 *    用户自己点断开不该被当成"掉线"弹出错误提示。
 * 4. [服务端进程消失后状态是 Lost 而且带原因]：
 *    以前接收循环退出时**静默**把状态设成 Disconnected，界面直接弹回连接页，
 *    既不说明原因、也不区分是不是用户自己点的——用户只会觉得"莫名其妙掉线了"。
 *
 * 找不到 `dchat_server.exe` 时全部跳过（[assumeTrue]），别人 clone 下来构建不会失败。
 */
class ServerInteropTest {

    private val serverExe = File("D:/codes/dchat/build/dchat_server.exe")

    private val received: MutableList<String> = Collections.synchronizedList(mutableListOf())

    // ------------------------------------------------------------------
    // 服务端起停脚手架
    // ------------------------------------------------------------------

    private class Server(
        val process: Process,
        val port: Int,
        val dir: File,
        /** 服务端的输出。有些结论（比如"通道确实加密了"）只能从它这里看出来。 */
        val output: StringBuilder,
    ) {
        fun logContains(needle: String): Boolean = synchronized(output) {
            output.contains(needle)
        }
    }

    private fun startServer(rulesText: String? = null): Server {
        val port = ServerSocket(0).use { it.localPort }
        val tmp = kotlin.io.path.createTempDirectory("dchat-interop").toFile()
        val rulesFile = File(tmp, "rules.txt")
        // 传了规则就先写进去：服务端启动时读这个文件
        if (rulesText != null) rulesFile.writeText(rulesText, Charsets.UTF_8)

        val process = ProcessBuilder(
            serverExe.absolutePath,
            "--port", port.toString(),
            "--users", File(tmp, "users.txt").absolutePath,
            "--rules", rulesFile.absolutePath,
        )
            .directory(tmp)
            .redirectErrorStream(true)
            .start()

        // 持续读服务端输出。不读的话管道写满会把服务端卡住，
        // 而且有些断言（"通道确实建立了"）必须看它才能下。
        val output = StringBuilder()
        Thread {
            try {
                process.inputStream.bufferedReader(Charsets.UTF_8).forEachLine { line ->
                    synchronized(output) { output.appendLine(line) }
                }
            } catch (_: Exception) {
                // 进程结束时流会断，正常
            }
        }.apply { isDaemon = true }.start()

        assertTrue("服务端 10 秒内没有开始监听 $port", waitForPort(port, 10_000))
        return Server(process, port, tmp, output)
    }

    private fun stopServer(server: Server) {
        server.process.destroy()
        if (!server.process.waitFor(3, TimeUnit.SECONDS)) server.process.destroyForcibly()
        server.dir.deleteRecursively()
    }

    /** 起服务器 + 连接 + 收集，跑完清理。[rulesText] 非空时先写进规则文件。 */
    private fun withServerAndConnection(
        collectImmediately: Boolean = true,
        rulesText: String? = null,
        encrypt: Boolean = true,
        body: suspend (DchatConnection, Server, Job?) -> Unit,
    ) = runBlocking {
        assumeTrue("没找到 dchat_server.exe，跳过互操作测试", serverExe.isFile)
        val server = startServer(rulesText)
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val conn = DchatConnection(scope)
        var collector: Job? = null
        try {
            assertTrue("连不上服务端", conn.connect("127.0.0.1", server.port, encrypt))
            if (collectImmediately) {
                collector = scope.launch { conn.lines.collect { received.add(it) } }
            }
            body(conn, server, collector)
        } finally {
            collector?.cancel()
            conn.disconnect()
            scope.cancel()
            stopServer(server)
        }
        Unit
    }

    // ------------------------------------------------------------------
    // 1. 基础互通
    // ------------------------------------------------------------------

    @Test
    fun `连真实的 C++ 服务端跑通注册登录发消息`() = withServerAndConnection { conn, _, _ ->
        assertNotNull("没有收到 WELCOME", awaitRaw(5_000) { it.command == "WELCOME" })

        conn.send(makeRegister("手机用户", "test123456"))
        val regReply = awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" }
        assertNotNull("注册没有任何回应", regReply)
        assertTrue("注册被拒绝：${parseLine(regReply!!).rest}", parseLine(regReply).command != "ERROR")

        // 注册成功后服务端**不会自动登录**（和桌面端一致），必须再发 LOGIN
        conn.send(makeLogin("手机用户", "test123456"))
        val loggedIn = awaitRaw(5_000) { it.command == "LOGGEDIN" }
        assertNotNull("登录没成功（没收到 LOGGEDIN）", loggedIn)
        val loggedInLine = ServerLine.parse(loggedIn!!, "手机用户")
        assertTrue("应解析成 LoggedIn，实际 $loggedInLine", loggedInLine is ServerLine.LoggedIn)
        assertEquals("手机用户", (loggedInLine as ServerLine.LoggedIn).nick)

        val names = awaitRaw(5_000) { it.command == "NAMES" }
        assertNotNull("没收到 NAMES（在线名单）", names)
        assertTrue("在线名单里应该有自己", parseLine(names!!).rest.contains("手机用户"))
        assertNotNull("没收到 KNOWN", awaitRaw(5_000) { it.command == "KNOWN" })

        val rules = awaitRaw(5_000) { it.command == "RULES" }
        assertNotNull("没收到 RULES", rules)
        // RULES 不带时间戳，验证解析没把第一个数字误当成 hh:mm 剥掉
        val ruleLine = ServerLine.parse(rules!!, "手机用户")
        assertTrue("RULES 应解析成 Rules，实际 $ruleLine", ruleLine is ServerLine.Rules)
        assertTrue("单文件上限应为正数", (ruleLine as ServerLine.Rules).documentSizeMb > 0)

        conn.send(makeMessage("来自 Android 客户端的问候"))
        val echo = awaitRaw(5_000) { it.command == "SAY" }
        assertNotNull("没有收到自己消息的回显", echo)
        val say = ServerLine.parse(echo!!, "手机用户") as ServerLine.Say
        assertEquals("手机用户", say.info.nick)
        assertEquals("来自 Android 客户端的问候", say.info.text)
        assertTrue("自己的消息应判定为 own", say.info.own)
        assertTrue("服务端应带上 hh:mm 时间戳", say.info.time.isNotEmpty())

        conn.send(makeMessage("/help"))
        assertNotNull("/help 没有回应", awaitRaw(5_000) { it.command == "SYS" })

        conn.send(makeQuit())
    }

    // ------------------------------------------------------------------
    // 2. 事故一：打开文件选择器 / Activity 重建 → 一段时间没人收集消息
    // ------------------------------------------------------------------

    @Test
    fun `没人收集消息的那段时间服务端发来的消息不会丢`() = withServerAndConnection(collectImmediately = false) { conn, _, _ ->
        // 故意不收集：模拟"点📎打开了系统文件选择器 → MainActivity 被销毁 →
        // Compose 的收集协程跟着没了"的那几秒。
        // 服务端在连接成功的那一刻就已经把 WELCOME 发出来了。
        delay(2_000)

        // 现在才恢复收集（相当于用户从文件选择器返回、Activity 重建、重新订阅）
        val collector = CoroutineScope(Dispatchers.IO).launch {
            conn.lines.collect { received.add(it) }
        }

        assertNotNull(
            "WELCOME 丢了！说明消息流没有缓冲——这正是打开文件选择器后掉线/丢消息的根因",
            awaitRaw(5_000) { it.command == "WELCOME" },
        )

        // 连接本身也必须还是活的：登录并发消息，能收到回显
        conn.send(makeRegister("掉线测试", "test123456"))
        assertNotNull("注册没回应（连接可能已经断了）", awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" })
        conn.send(makeLogin("掉线测试", "test123456"))
        assertNotNull("登录失败（连接确实断了）", awaitRaw(5_000) { it.command == "LOGGEDIN" })

        conn.send(makeMessage("切出去再回来还能发消息"))
        val echo = awaitRaw(5_000) { it.command == "SAY" }
        assertNotNull("收不到自己的消息回显", echo)
        assertEquals("切出去再回来还能发消息", (ServerLine.parse(echo!!, "掉线测试") as ServerLine.Say).info.text)

        assertEquals(
            "整个过程中连接状态不该变成断开",
            "Connected",
            conn.state.value::class.simpleName,
        )
        collector.cancel()
    }

    // ------------------------------------------------------------------
    // 3. 事故二：用户主动断开 vs 意外掉线，界面表现必须不同
    // ------------------------------------------------------------------

    @Test
    fun `用户主动断开后状态是 Disconnected 而不是 Lost`() = withServerAndConnection { conn, _, _ ->
        assertNotNull(awaitRaw(5_000) { it.command == "WELCOME" })
        conn.send(makeQuit())
        conn.disconnect()

        // 给接收循环一点时间收尾
        delay(300)
        assertTrue(
            "用户自己点的断开应该得到 Disconnected（界面回连接页是预期行为），实际 ${conn.state.value}",
            conn.state.value is ConnectionState.Disconnected,
        )
    }

    @Test
    fun `服务端进程消失后状态是 Lost 而且带原因`() = runBlocking {
        assumeTrue("没找到 dchat_server.exe，跳过互操作测试", serverExe.isFile)
        val server = startServer()
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val conn = DchatConnection(scope)
        val collector = scope.launch { conn.lines.collect { received.add(it) } }

        try {
            assertTrue("连不上服务端", conn.connect("127.0.0.1", server.port))
            assertNotNull(awaitRaw(5_000) { it.command == "WELCOME" })

            // 直接把服务端进程杀掉（模拟服务器崩了 / 网络断了 / 被踢）
            server.process.destroyForcibly()
            server.process.waitFor(3, TimeUnit.SECONDS)

            // 等状态变化：最多 8 秒
            var state: ConnectionState = conn.state.value
            val deadline = System.currentTimeMillis() + 8_000
            while (System.currentTimeMillis() < deadline) {
                state = conn.state.value
                if (state is ConnectionState.Lost || state is ConnectionState.Disconnected) break
                delay(100)
            }

            assertTrue(
                "服务端没了之后状态应该变成 Lost（带原因、界面能提示），实际 $state",
                state is ConnectionState.Lost,
            )
            state as ConnectionState.Lost
            assertTrue("Lost 必须带上原因，否则界面只能显示一句没用的'已断开'", state.reason.isNotBlank())
        } finally {
            collector.cancel()
            conn.disconnect()
            scope.cancel()
            stopServer(server)
        }
        Unit
    }

    // ------------------------------------------------------------------
    // 5. 跨语言一致性：Kotlin 的转义实现 vs 真实 C++ 服务端的统计
    // ------------------------------------------------------------------

    /**
     * **这是转义规则两端一致性的真正证明**。
     *
     * 前面的 `TextLimitsTest` 只能证明"Kotlin 实现符合 Kotlin 测试的期望"，
     * C++ 那边同理——两边各自的测试都过，不代表它们对同一个输入给出同样的结果。
     *
     * 这条测试让**安卓端的 Kotlin 代码**把多行文本转义后发给**真正的 C++ 服务端**，
     * 由服务端用它的 `CountTextLines` 去统计行数：
     *  - 3 行（正好卡在上限）必须放行，且往返后换行信息一字不差
     *  - 4 行必须被服务端拒绝
     *  - 超过字符上限必须被服务端拒绝
     *
     * 只要两端的转义规则有任何一点不一致，这里就会红。
     */
    @Test
    fun `安卓端发的多行消息_真实 C++ 服务端能正确统计行数`() =
        withServerAndConnection(rulesText = "maxtextlen 30\nmaxtextlines 3\n") { conn, _, _ ->
            assertNotNull(awaitRaw(5_000) { it.command == "WELCOME" })

            conn.send(makeRegister("多行测试", "test123456"))
            assertNotNull(awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" })
            conn.send(makeLogin("多行测试", "test123456"))
            assertNotNull("登录失败", awaitRaw(5_000) { it.command == "LOGGEDIN" })

            // 写进规则文件的两条必须真的生效
            val rulesRaw = awaitRaw(5_000) { it.command == "RULES" }
            assertNotNull("没收到 RULES", rulesRaw)
            val rules = ServerLine.parse(rulesRaw!!, "多行测试")
            assertTrue("RULES 应解析成 Rules，实际 $rules", rules is ServerLine.Rules)
            rules as ServerLine.Rules
            assertEquals("maxtextlen 没生效", 30, rules.maxTextLength)
            assertEquals("maxtextlines 没生效", 3, rules.maxTextLines)

            // ---- 正好 3 行：必须放行，且换行一个都不能少 ----
            val threeLines = "第一行\n第二行\n第三行"
            conn.send(makeMessage(escapeText(threeLines)))
            val echo = awaitRaw(5_000) { it.command == "SAY" }
            assertNotNull(
                "3 行的消息被服务端拒了 —— 说明两端的行数统计不一致",
                echo,
            )
            val say = ServerLine.parse(echo!!, "多行测试") as ServerLine.Say
            assertEquals("换行信息在往返中丢了", threeLines, say.info.text)

            // ---- 4 行：服务端必须拒绝 ----
            conn.send(makeMessage(escapeText("a\nb\nc\nd")))
            assertNotNull(
                "4 行的消息服务端没拦，maxtextlines 没生效",
                awaitRaw(5_000) { it.command == "ERROR" && it.rest.contains("maxtextlines") },
            )

            // ---- 31 字符：服务端必须拒绝 ----
            conn.send(makeMessage(escapeText("x".repeat(31))))
            assertNotNull(
                "超长消息服务端没拦，maxtextlen 没生效",
                awaitRaw(5_000) { it.command == "ERROR" && it.rest.contains("maxtextlen") },
            )

            // ---- 30 字符（边界）：必须放行 ----
            conn.send(makeMessage(escapeText("y".repeat(30))))
            val ok = awaitRaw(5_000) { it.command == "SAY" && it.rest.contains("yyy") }
            assertNotNull("边界值 30 字符被误拦了", ok)
        }

    // ------------------------------------------------------------------
    // 6. ★ 端到端加密
    // ------------------------------------------------------------------

    /**
     * **加密层的第一个完整端到端证据**：安卓端的握手实现 + 真实的 C++ 服务端，
     * 全程密文跑通注册 → 登录 → 发消息 → 收回显。
     *
     * ## 怎么证明"确实是密文"，而不是"握了个手然后继续明文"
     *
     * 客户端这边是透明的（加密解密都在 `DchatConnection` 内部），所以光看
     * "消息能发出去"说明不了任何事——明文也能发出去。真正的判据是**服务端日志**：
     *
     * - 只有真的建立了加密通道，服务端才会写 `encrypted channel established`
     * - 如果客户端在握手后发了明文，服务端会写 `plaintext after handshake` 并**断开**，
     *   那后面的注册登录根本不可能成功
     *
     * 所以下面既断言前者出现，也断言后者没出现。
     */
    @Test
    fun `★ 端到端加密：安卓端与真实 C++ 服务端全程密文`() =
        withServerAndConnection { conn, server, _ ->
            // 握手在 connect() 内部就做完了。WELCOME 是在握手期间读到的，
            // 那时收集器还没起——所以它必须靠 lineChannel 缓冲下来（Channel 不挑订阅时机）。
            assertTrue("握手应当已经完成，连接应当是加密的", conn.encrypted)
            val fingerprint = conn.serverFingerprint
            assertNotNull("握手成功就必须拿到服务器公钥指纹（TOFU 要用）", fingerprint)
            assertTrue(
                "指纹格式应当像 AA:BB:...，实际 $fingerprint",
                fingerprint!!.matches(Regex("^([0-9A-F]{2}:){15}[0-9A-F]{2}$")),
            )

            // 握手期间读到的 WELCOME 必须被保留下来，不能丢掉
            assertNotNull("握手期间读到的 WELCOME 不能丢", awaitRaw(5_000) { it.command == "WELCOME" })

            conn.send(makeRegister("加密测试", "test123456"))
            assertNotNull("注册没回应", awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" })
            conn.send(makeLogin("加密测试", "test123456"))
            assertNotNull("★ 登录失败——说明密文没解开", awaitRaw(5_000) { it.command == "LOGGEDIN" })

            val text = "这条消息是加密传输的，含中文和 emoji 🎉"
            conn.send(makeMessage(escapeText(text)))
            val echo = awaitRaw(5_000) { it.command == "SAY" }
            assertNotNull("★ 没有收到消息回显——密文往返有问题", echo)
            assertEquals(text, (ServerLine.parse(echo!!, "加密测试") as ServerLine.Say).info.text)

            // ★ 决定性证据
            assertTrue(
                "服务端日志里没有 \"encrypted channel established\" —— " +
                    "说明握手根本没成，上面那些成功其实都是明文在跑",
                server.logContains("encrypted channel established"),
            )
            assertFalse(
                "服务端日志出现了降级告警，说明有明文混进了加密通道",
                server.logContains("plaintext after handshake"),
            )
        }

    /**
     * 向后兼容：客户端**主动关掉加密**时，老流程必须照常工作。
     *
     * 这条保证的是"新客户端能连老服务器"——反过来（老客户端连新服务器）
     * 由服务端的协商逻辑保证：老客户端不发 HELLO，服务端就一直用明文。
     */
    @Test
    fun `关闭加密时仍走明文（老服务器兼容路径）`() =
        withServerAndConnection(encrypt = false) { conn, server, _ ->
            assertFalse("明确关掉加密时不该建立加密会话", conn.encrypted)
            assertNull("没有握手就不该有指纹", conn.serverFingerprint)

            assertNotNull("明文模式下也要能收到 WELCOME", awaitRaw(5_000) { it.command == "WELCOME" })

            conn.send(makeRegister("明文测试", "test123456"))
            assertNotNull("注册没回应", awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" })
            conn.send(makeLogin("明文测试", "test123456"))
            assertNotNull("明文模式登录失败", awaitRaw(5_000) { it.command == "LOGGEDIN" })

            conn.send(makeMessage("明文也能用"))
            assertNotNull("明文模式收不到回显", awaitRaw(5_000) { it.command == "SAY" })

            assertFalse(
                "没握手就不该出现加密通道的日志",
                server.logContains("encrypted channel established"),
            )
        }

    // ------------------------------------------------------------------

    /** 在已收集到的行里找一条满足条件的（找不到就等到超时）。 */
    private suspend fun awaitRaw(timeoutMs: Long, predicate: (Message) -> Boolean): String? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            val hit = synchronized(received) { received.firstOrNull { predicate(parseLine(it)) } }
            if (hit != null) return hit
            delay(40)
        }
        return null
    }

    private fun waitForPort(port: Int, timeoutMs: Long): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            try {
                Socket().use { it.connect(InetSocketAddress("127.0.0.1", port), 300) }
                return true
            } catch (_: Exception) {
                Thread.sleep(120)
            }
        }
        return false
    }
}
