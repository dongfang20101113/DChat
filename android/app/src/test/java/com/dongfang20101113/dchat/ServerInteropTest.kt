package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.net.DchatConnection
import com.dongfang20101113.dchat.protocol.Message
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.makeLogin
import com.dongfang20101113.dchat.protocol.makeMessage
import com.dongfang20101113.dchat.protocol.makeQuit
import com.dongfang20101113.dchat.protocol.makeRegister
import com.dongfang20101113.dchat.protocol.parseLine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
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
 * **真机互操作测试**：让 Android 端的协议/网络代码去连**真正的 C++ 服务端**。
 *
 * 这是最有价值的一条验证——其余 150 多项单测都是自己跟自己对（协议解析、状态机、布局），
 * 只有这个测试能证明「手机端发出去的字节，C++ 服务端听得懂；服务端回的字节，手机端解析得对」。
 *
 * 自动启动 `dchat_server.exe`（临时账号文件 + 随机端口），走完整流程：
 * **连接 → 注册 → 登录 → 收 NAMES/KNOWN/RULES → 发消息 → 收自己的回显 → /help → 断开**。
 *
 * 找不到 exe 时用 [assumeTrue] 跳过，别人 clone 下来构建不会因此失败。
 */
class ServerInteropTest {

    private val serverExe = File("D:/codes/dchat/build/dchat_server.exe")

    /** 收到的所有原始行（持续收集，不能漏——登录后服务端会连发四条）。 */
    private val received: MutableList<String> = Collections.synchronizedList(mutableListOf())

    @Test
    fun `连真实的 C++ 服务端跑通注册登录发消息`() = runBlocking {
        assumeTrue("没找到 dchat_server.exe，跳过互操作测试", serverExe.isFile)

        val port = ServerSocket(0).use { it.localPort }
        // 用 kotlin.io.path 的版本：旧的 File.createTempDir() 已被标记废弃（权限过宽）
        val tmp = kotlin.io.path.createTempDirectory("dchat-interop").toFile()
        val usersFile = File(tmp, "users.txt")
        val rulesFile = File(tmp, "rules.txt")

        val process = ProcessBuilder(
            serverExe.absolutePath,
            "--port", port.toString(),
            "--users", usersFile.absolutePath,
            "--rules", rulesFile.absolutePath,
        )
            .directory(tmp)
            .redirectErrorStream(true)
            .start()

        val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val conn = DchatConnection(scope)
        var collector: kotlinx.coroutines.Job? = null

        try {
            assertTrue("服务端 10 秒内没有开始监听 $port", waitForPort(port, 10_000))

            // ---- 1. 连接 ----
            assertTrue("连不上服务端", conn.connect("127.0.0.1", port))

            // 持续收集所有服务器行（用 flow.first{} 会漏掉先到的那些）
            collector = scope.launch { conn.lines.collect { received.add(it) } }

            assertNotNull("没有收到 WELCOME", awaitRaw(5_000) { it.command == "WELCOME" })

            // ---- 2. 注册 ----
            conn.send(makeRegister("手机用户", "test123456"))
            val regReply = awaitRaw(5_000) { it.command == "SYS" || it.command == "ERROR" }
            assertNotNull("注册没有任何回应", regReply)
            assertTrue(
                "注册被拒绝：${parseLine(regReply!!).rest}",
                parseLine(regReply).command != "ERROR",
            )

            // 注册成功后服务端**不会自动登录**（和桌面端一致），必须再发 LOGIN
            conn.send(makeLogin("手机用户", "test123456"))
            val loggedIn = awaitRaw(5_000) { it.command == "LOGGEDIN" }
            assertNotNull("登录没成功（没收到 LOGGEDIN）", loggedIn)
            // 注意：raw 的 rest 里带着 hh:mm 时间戳，必须走协议层解析才能拿到纯昵称
            val loggedInLine = ServerLine.parse(loggedIn!!, "手机用户")
            assertTrue("LOGGEDIN 应解析成 LoggedIn，实际 $loggedInLine", loggedInLine is ServerLine.LoggedIn)
            assertEquals("手机用户", (loggedInLine as ServerLine.LoggedIn).nick)

            // ---- 3. 登录后服务端会下发三份名单/规则，客户端靠它们工作 ----
            val names = awaitRaw(5_000) { it.command == "NAMES" }
            assertNotNull("没收到 NAMES（在线名单）", names)
            assertTrue("在线名单里应该有自己", parseLine(names!!).rest.contains("手机用户"))

            assertNotNull("没收到 KNOWN（Tab 补全名单）", awaitRaw(5_000) { it.command == "KNOWN" })

            val rules = awaitRaw(5_000) { it.command == "RULES" }
            assertNotNull("没收到 RULES（服务器规则）", rules)
            // 关键：RULES 不带时间戳，验证解析没把第一个数字当成 hh:mm 剥掉
            val ruleLine = ServerLine.parse(rules!!, "手机用户")
            assertTrue("RULES 应解析成 Rules，实际 $ruleLine", ruleLine is ServerLine.Rules)
            assertTrue("单文件上限应为正数", (ruleLine as ServerLine.Rules).documentSizeMb > 0)

            // ---- 4. 发消息，服务端广播回来（含自己）----
            conn.send(makeMessage("来自 Android 客户端的问候"))
            val echo = awaitRaw(5_000) { it.command == "SAY" }
            assertNotNull("没有收到自己消息的回显", echo)

            val say = ServerLine.parse(echo!!, "手机用户")
            assertTrue("SAY 应解析成 Say，实际 $say", say is ServerLine.Say)
            say as ServerLine.Say
            assertEquals("手机用户", say.info.nick)
            assertEquals("来自 Android 客户端的问候", say.info.text)
            assertTrue("自己发的消息应判定为 own（气泡靠右）", say.info.own)
            assertTrue("服务端应带上 hh:mm 时间戳", say.info.time.isNotEmpty())

            // ---- 5. 指令也通（/help 走 SYS 回来）----
            conn.send(makeMessage("/help"))
            assertNotNull("/help 没有回应", awaitRaw(5_000) { it.command == "SYS" })

            // ---- 6. 断开 ----
            conn.send(makeQuit())
        } finally {
            collector?.cancel()
            conn.disconnect()
            scope.cancel()
            process.destroy()
            if (!process.waitFor(3, TimeUnit.SECONDS)) process.destroyForcibly()
            tmp.deleteRecursively()
        }
        // JUnit4 要求测试方法返回 void；runBlocking 的最后一个表达式是 send() 返回的 Boolean，
        // 不显式收口的话方法签名就变成了 Boolean，JUnit 会直接拒绝这个测试类。
        Unit
    }

    /** 在已收集到的行里找一条满足条件的（找不到就等到超时）。 */
    private suspend fun awaitRaw(
        timeoutMs: Long,
        predicate: (Message) -> Boolean,
    ): String? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            val hit = synchronized(received) {
                received.firstOrNull { predicate(parseLine(it)) }
            }
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
