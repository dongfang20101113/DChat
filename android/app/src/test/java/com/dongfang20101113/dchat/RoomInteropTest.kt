package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.net.DchatConnection
import com.dongfang20101113.dchat.protocol.RoomKind
import com.dongfang20101113.dchat.protocol.RoomLine
import com.dongfang20101113.dchat.protocol.buildLine
import com.dongfang20101113.dchat.protocol.makeLogin
import com.dongfang20101113.dchat.protocol.makeMessage
import com.dongfang20101113.dchat.protocol.makeRegister
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
import java.net.ServerSocket
import java.util.Base64
import java.util.Collections
import java.util.concurrent.TimeUnit

/**
 * **安卓端连真实 C++ 服务端**跑通多会话（群聊 / 私聊）全流程。
 *
 * ## 为什么要有它
 *
 * 多会话这块的单元测试（`RoomProtocolTest` / `RoomStateReducerTest`）验的是**纯函数**：
 * 给一行文本，解析对不对、状态怎么变。但它们证明不了"安卓端真的能和真实服务端
 * 把群建起来、把人加进去、把消息发对房间"—— 这一层以前只能靠真机点。
 *
 * 本机没有安卓设备，但 `ServerInteropTest` 已经趟出一条路：**起真实的 dchat_server.exe，
 * 用安卓客户端自己的 `DchatConnection` 连上去**。这里把同样的办法用到多会话上，
 * 于是"安卓端端到端"这件事在没有设备的情况下也能自动验。
 *
 * 覆盖：能力协商 → 房间列表 → 建群 → 新群默认不公开（搜不到）→ 勾公开 →
 * 搜得到 → 申请入群（管理员收到 GREQ）→ 同意 → 群消息只进群不进大厅 → `/dm` 发起私聊。
 *
 * 找不到 `dchat_server.exe` 时整体跳过（[assumeTrue]），别人 clone 下来构建不会失败。
 */
class RoomInteropTest {

    private val serverExe = File("D:/codes/dchat/build/dchat_server.exe")

    private fun b64(text: String): String =
        Base64.getEncoder().encodeToString(text.toByteArray(Charsets.UTF_8))

    // ------------------------------------------------------------------
    // 脚手架
    // ------------------------------------------------------------------

    private class Server(val process: Process, val port: Int, val dir: File, val output: StringBuilder)

    /** 一端连接 + 它收到的所有行。 */
    private class Peer(val conn: DchatConnection, val lines: MutableList<String>)

    private fun startServer(): Server {
        val port = ServerSocket(0).use { it.localPort }
        val tmp = kotlin.io.path.createTempDirectory("dchat-room-interop").toFile()
        File(tmp, "rules.txt").writeText("# 联调规则\nkeepchathistory true\n", Charsets.UTF_8)
        val process = ProcessBuilder(
            serverExe.absolutePath,
            "--port", port.toString(),
            "--users", File(tmp, "users.txt").absolutePath,
            "--rules", File(tmp, "rules.txt").absolutePath,
        )
            .directory(tmp)
            .redirectErrorStream(true)
            .start()

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

    private fun waitForPort(port: Int, timeoutMs: Long): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            try {
                java.net.Socket("127.0.0.1", port).use { return true }
            } catch (_: Exception) {
                Thread.sleep(80)
            }
        }
        return false
    }

    /** 等某一行出现（原始文本）。超时返回 null。 */
    private suspend fun await(peer: Peer, timeoutMs: Long = 6_000, pred: (String) -> Boolean): String? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            synchronized(peer.lines) { peer.lines.firstOrNull(pred) }?.let { return it }
            delay(40)
        }
        return null
    }

    /** 等某一行**能被安卓自己的解析器认出来**并且满足条件。返回解析结果，超时返回 null。 */
    private suspend fun awaitRoom(peer: Peer, timeoutMs: Long = 6_000, pred: (RoomLine) -> Boolean): RoomLine? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            synchronized(peer.lines) {
                for (raw in peer.lines) {
                    val parsed = RoomLine.parse(raw)
                    if (parsed != null && pred(parsed)) return parsed
                }
            }
            delay(40)
        }
        return null
    }

    /** RINFO 的第 5 个字段是成员数（`RINFO <id> <kind> <nameB64> <owner> <members> ...`）。 */
    private fun membersInRinfo(raw: String): Int =
        raw.trim().split(Regex("\\s+")).getOrNull(5)?.toIntOrNull() ?: -1

    private suspend fun registerAndLogin(peer: Peer, nick: String, pass: String) {
        // 服务端一连上就先发 WELCOME
        assertNotNull("没有收到 WELCOME", await(peer) { it.startsWith("WELCOME") })
        peer.conn.send(makeRegister(nick, pass))
        delay(400)
        peer.conn.send(makeLogin(nick, pass))
        assertNotNull("$nick 没登录成功（没收到 LOGGEDIN）", await(peer) { it.startsWith("LOGGEDIN") })
    }

    // ------------------------------------------------------------------
    // 主流程
    // ------------------------------------------------------------------

    @Test
    fun `安卓端连真服务端跑通群聊全流程`() = runBlocking {
        assumeTrue("没找到 dchat_server.exe，跳过互操作测试", serverExe.isFile)
        val server = startServer()
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        var collectorA: Job? = null
        var collectorB: Job? = null
        val connA = DchatConnection(scope)
        val connB = DchatConnection(scope)
        val admin = Peer(connA, Collections.synchronizedList(mutableListOf()))
        val member = Peer(connB, Collections.synchronizedList(mutableListOf()))
        try {
            assertTrue("管理员连不上服务端", connA.connect("127.0.0.1", server.port, true))
            assertTrue("成员连不上服务端", connB.connect("127.0.0.1", server.port, true))
            collectorA = scope.launch { connA.lines.collect { admin.lines.add(it) } }
            collectorB = scope.launch { connB.lines.collect { member.lines.add(it) } }

            registerAndLogin(admin, "安卓管理员", "pass123456")
            registerAndLogin(member, "安卓成员", "pass123456")

            // ---- 能力协商：没有这一步，客户端会退回"只有大厅"的老界面 ----
            admin.conn.send(buildLine("FEATURES", RoomLine.ROOMS_CAPABILITY))
            member.conn.send(buildLine("FEATURES", RoomLine.ROOMS_CAPABILITY))
            val feat = awaitRoom(admin) { it is RoomLine.Features }
            assertNotNull("服务端没有确认 FEATURES（安卓端会一直停在只有大厅的老界面）", feat)
            assertTrue(
                "确认里没有 ${RoomLine.ROOMS_CAPABILITY}",
                (feat as RoomLine.Features).capabilities.contains(RoomLine.ROOMS_CAPABILITY),
            )

            // ---- 房间列表：至少要有大厅 ----
            val hall = awaitRoom(admin) { it is RoomLine.RoomListEntry && it.info.id == "hall" }
            assertNotNull("ROOMLIST 里没有大厅", hall)
            assertEquals("大厅的 kind 应该是 hall", RoomKind.HALL, (hall as RoomLine.RoomListEntry).info.kind)

            // ---- 建群 ----
            admin.conn.send(buildLine("GCREATE", "androidroom ${b64("安卓测试群")}"))
            val info = awaitRoom(admin) { it is RoomLine.RoomInfoLine && it.info.id == "androidroom" }
            assertNotNull("建群之后没有收到 RINFO", info)

            // ---- 新群默认**不公开** ----
            val listedOff = awaitRoom(admin) {
                it is RoomLine.RoomSetting && it.roomId == "androidroom" && it.key == "listed"
            }
            assertNotNull("没有收到 listed 的设置项", listedOff)
            assertEquals("新群默认应该是未公开的", "false", (listedOff as RoomLine.RoomSetting).value)

            // ---- 没公开：搜不到（这一条要等超时，是"确认没有"而不是"等它来"）----
            member.conn.send(buildLine("GSEARCH", b64("androidroom")))
            val noHit = awaitRoom(member, 2_500) { it is RoomLine.GroupHitLine }
            assertNull("还没公开就不该被搜到", noHit)

            // ---- 勾选公开 ----
            admin.conn.send(buildLine("GSET", "androidroom listed true"))
            val listedOn = awaitRoom(admin) {
                it is RoomLine.RoomSetting && it.roomId == "androidroom" && it.key == "listed" &&
                    it.value == "true"
            }
            assertNotNull("服务端没有确认 listed=true", listedOn)

            // ---- 这次搜得到 ----
            member.conn.send(buildLine("GSEARCH", b64("androidroom")))
            val hit = awaitRoom(member) { it is RoomLine.GroupHitLine && it.hit.id == "androidroom" }
            assertNotNull("公开之后安卓端还是搜不到 room1", hit)

            // ---- 申请入群 → 管理员收到 GREQ ----
            member.conn.send(buildLine("GJOIN", "androidroom"))
            val req = awaitRoom(admin) {
                it is RoomLine.GroupRequest && it.roomId == "androidroom" && it.nick == "安卓成员"
            }
            assertNotNull("管理员没有收到入群申请（GREQ）", req)

            // ---- 同意 → 成员那边要拿到新的房间信息（成员数变 2）----
            admin.conn.send(buildLine("GREQ", "androidroom 安卓成员 ok"))
            val rinfoForAdmin = await(admin) {
                it.startsWith("RINFO androidroom ") && membersInRinfo(it) >= 2
            }
            assertNotNull("同意之后管理员那边的成员数没变成 2", rinfoForAdmin)
            val rinfoForMember = await(member) {
                it.startsWith("RINFO androidroom ") && membersInRinfo(it) >= 2
            }
            assertNotNull("同意之后成员那边没拿到新的房间信息", rinfoForMember)

            // ---- 群消息只进群，不进大厅 ----
            member.conn.send(buildLine("ROOM", "androidroom"))
            delay(600)
            member.conn.send(makeMessage("安卓端群消息"))
            val roomMsg = await(admin) {
                it.startsWith("RMSG ") && it.contains("androidroom") && it.contains("安卓端群消息")
            }
            assertNotNull("管理员没有收到群消息（RMSG）", roomMsg)
            assertTrue(
                "RMSG 的时间戳应该在房间号前面（协议约定）",
                Regex("^RMSG \\d{2}:\\d{2} androidroom ").containsMatchIn(roomMsg!!),
            )
            val leakedToHall = synchronized(admin.lines) {
                admin.lines.any { it.startsWith("SAY ") && it.contains("安卓端群消息") }
            }
            assertFalse("群消息跑到大厅去了（老客户端会看到不属于它的消息）", leakedToHall)

            // ---- 私聊：安卓端用 /dm 发起 ----
            member.conn.send(makeMessage("/dm 安卓管理员"))
            val dm = awaitRoom(member) { it is RoomLine.DmLink }
            assertNotNull("安卓端发 /dm 之后没有拿到 GDMLINK", dm)
            assertEquals("私聊的对方应该是管理员", "安卓管理员", (dm as RoomLine.DmLink).peer)

            // ---- 私聊消息不该掉进大厅 ----
            val dmRoom = dm.roomId
            member.conn.send(buildLine("ROOM", dmRoom))
            delay(500)
            member.conn.send(makeMessage("安卓端私聊一句"))
            assertNotNull(
                "管理员没有收到私聊消息",
                await(admin) { it.startsWith("RMSG ") && it.contains(dmRoom) && it.contains("安卓端私聊一句") },
            )
            val dmLeaked = synchronized(admin.lines) {
                admin.lines.any { it.startsWith("SAY ") && it.contains("安卓端私聊一句") }
            }
            assertFalse("私聊消息掉进大厅了", dmLeaked)
        } finally {
            collectorA?.cancel()
            collectorB?.cancel()
            connA.disconnect()
            connB.disconnect()
            scope.cancel()
            server.process.destroy()
            if (!server.process.waitFor(3, TimeUnit.SECONDS)) server.process.destroyForcibly()
            server.dir.deleteRecursively()
        }
        Unit
    }
}
