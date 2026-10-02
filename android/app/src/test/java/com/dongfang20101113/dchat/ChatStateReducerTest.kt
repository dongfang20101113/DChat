package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.AttachmentKind
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.FileState
import com.dongfang20101113.dchat.ui.Stage
import com.dongfang20101113.dchat.ui.reduce
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 界面状态机测试。
 *
 * `reduce` 是纯函数（时间由参数注入），所以"收到某条服务器消息后界面应该变成什么样"
 * 全部能在 JVM 上验证，不需要真机也不需要模拟器。
 *
 * 这里覆盖的是**最容易写错的几条**：哪些消息该进聊天记录、哪些只是控制行、
 * 名单怎么维护、文件卡片的状态流转。
 */
class ChatStateReducerTest {

    private val t = "21:05"
    private fun ChatState.feed(raw: String, nick: String = selfNick) =
        reduce(ServerLine.parse(raw, nick), t)

    // ------------------------------------------------------------------
    // 进不进聊天记录（桌面端踩过的坑）
    // ------------------------------------------------------------------

    @Test
    fun `KNOWN 只更新名单_不进聊天记录`() {
        // 桌面端专门注释过：KNOWN 是给 Tab 补全用的，不能显示
        val s = ChatState().feed("KNOWN 21:05 alice,bob")
        assertEquals(listOf("alice", "bob"), s.knownNicks)
        assertTrue("KNOWN 不该产生任何聊天条目", s.items.isEmpty())
    }

    @Test
    fun `RULES 是控制行_只更新上限不进记录`() {
        val s = ChatState().feed("RULES 128 500 1")
        assertEquals(128, s.maxFileMb)
        assertTrue("RULES 不该产生聊天条目", s.items.isEmpty())
    }

    @Test
    fun `PONG 是心跳回包_完全不显示`() {
        val s = ChatState().feed("PONG 21:05")
        assertTrue(s.items.isEmpty())
    }

    @Test
    fun `NAMES 既更新名单也显示一条提示`() {
        // 与桌面端一致：NAMES 会被显示成「在线成员：…」
        val s = ChatState().feed("NAMES 21:05 alice,bob")
        assertEquals(listOf("alice", "bob"), s.onlineNicks)
        assertEquals(1, s.items.size)
        val notice = s.items.first() as ChatItem.NoticeItem
        assertEquals("在线成员：alice,bob", notice.info.text)
    }

    @Test
    fun `未知命令原样显示_便于排查`() {
        val s = ChatState().feed("BRAND_NEW 21:05 内容")
        val notice = s.items.first() as ChatItem.NoticeItem
        assertEquals("BRAND_NEW 21:05 内容", notice.info.text)
    }

    @Test
    fun `空行不产生任何条目`() {
        assertTrue(ChatState().feed("").items.isEmpty())
    }

    // ------------------------------------------------------------------
    // 登录流程
    // ------------------------------------------------------------------

    @Test
    fun `LOGGEDIN 进入聊天阶段并记住昵称`() {
        val s = ChatState(stage = Stage.AUTH, loggingIn = true).feed("LOGGEDIN 21:05 小明")
        assertEquals(Stage.CHAT, s.stage)
        assertEquals("小明", s.selfNick)
        assertTrue("登录成功后不该还在 loading", !s.loggingIn)
        assertNull(s.authError)
    }

    @Test
    fun `登录阶段的 ERROR 会写进 authError_窗口里显示红字`() {
        val s = ChatState(stage = Stage.AUTH, loggingIn = true).feed("ERROR 21:05 密码错误")
        assertEquals("密码错误", s.authError)
        assertTrue("登录失败不能进入聊天阶段", s.stage != Stage.CHAT)
        val notice = s.items.first() as ChatItem.NoticeItem
        assertTrue(notice.info.isError)
    }

    @Test
    fun `聊天阶段的 ERROR 不该污染 authError`() {
        val s = ChatState(stage = Stage.CHAT, selfNick = "小明", loggingIn = false)
            .feed("ERROR 21:05 发言太快了")
        assertNull("已经在聊天了，这个错误属于聊天不是登录", s.authError)
        assertTrue(s.items.isNotEmpty())
    }

    @Test
    fun `登录失败后连接不断开_阶段仍是 AUTH`() {
        val s = ChatState(stage = Stage.AUTH, connected = true, loggingIn = true)
            .feed("ERROR 21:05 用户名不存在")
        assertEquals(Stage.AUTH, s.stage)
        assertTrue("连接必须保持", s.connected)
        assertTrue("必须能再次提交", !s.loggingIn)
    }

    // ------------------------------------------------------------------
    // 名单维护
    // ------------------------------------------------------------------

    @Test
    fun `JOINED 和 LEFT 维护在线名单`() {
        var s = ChatState().feed("NAMES 21:05 alice,bob")
        s = s.feed("JOINED 21:05 carol")
        assertEquals(listOf("alice", "bob", "carol"), s.onlineNicks)
        s = s.feed("LEFT 21:05 bob")
        assertEquals(listOf("alice", "carol"), s.onlineNicks)
    }

    @Test
    fun `同一个人重复 JOIN 不会在名单里出现两次`() {
        val s = ChatState().feed("JOINED 21:05 alice").feed("JOINED 21:05 alice")
        assertEquals(listOf("alice"), s.onlineNicks)
    }

    // ------------------------------------------------------------------
    // 消息
    // ------------------------------------------------------------------

    @Test
    fun `SAY 变成气泡_自己发的标记为 own`() {
        val s = ChatState(selfNick = "我").feed("SAY 21:05 我 大家好")
        val item = s.items.first() as ChatItem.SayItem
        assertTrue(item.info.own)
        assertEquals("大家好", item.info.text)
    }

    @Test
    fun `ANNOUNCE 是公告_不是普通提示`() {
        val s = ChatState().feed("ANNOUNCE 21:05 10 分钟后维护")
        val item = s.items.first() as ChatItem.NoticeItem
        assertTrue(item.info.isAnnouncement)
        assertEquals("10 分钟后维护", item.info.text)
    }

    @Test
    fun `每条记录的时间在产生那一刻固定`() {
        // 时间戳不随重绘变化——桌面端专门注释过这点
        val s = ChatState().feed("SAY 21:05 alice 你好")
        val item = s.items.first() as ChatItem.SayItem
        assertEquals("21:05", item.info.time)
    }

    // ------------------------------------------------------------------
    // 记录条数上限
    // ------------------------------------------------------------------

    @Test
    fun `记录超过 400 条时丢掉最旧的`() {
        var s = ChatState()
        repeat(ChatState.MAX_ITEMS + 50) { index ->
            s = s.feed("SAY 21:05 alice 消息$index")
        }
        assertEquals(ChatState.MAX_ITEMS, s.items.size)
        // 最旧的应该已经被丢掉，最新的还在
        val last = s.items.last() as ChatItem.SayItem
        assertEquals("消息${ChatState.MAX_ITEMS + 49}", last.info.text)
    }

    // ------------------------------------------------------------------
    // 文件卡片状态流转
    // ------------------------------------------------------------------

    private fun offerLine(id: String = "F1", name: String = "报告.pdf", thumb: Boolean = false): String {
        val b64 = base64Encode(name.toByteArray(Charsets.UTF_8))
        return "FILE_OFFER 21:05 alice $id $b64 1024" + if (thumb) " 1" else ""
    }

    @Test
    fun `FILE_OFFER 生成一张卡片`() {
        val s = ChatState().feed(offerLine())
        val card = s.items.first() as ChatItem.FileItem
        assertEquals("报告.pdf", card.fileName)
        assertEquals("alice", card.fromNick)
        assertEquals(1024L, card.size)
        assertEquals(FileState.OFFERED, card.state)
        assertEquals("下载", card.buttonLabel)
    }

    @Test
    fun `同一个文件重复 OFFER 不会出现两张卡片`() {
        val s = ChatState().feed(offerLine()).feed(offerLine())
        assertEquals(1, s.items.filterIsInstance<ChatItem.FileItem>().size)
    }

    @Test
    fun `下载过程 开始 到 完成`() {
        var s = ChatState().feed(offerLine())
        s = s.feed("FILE_BEGIN F1 ${base64Encode("报告.pdf".toByteArray())} 1024")
        var card = s.items.filterIsInstance<ChatItem.FileItem>().first()
        assertEquals(FileState.DOWNLOADING, card.state)
        assertTrue(card.buttonLabel.startsWith("下载中"))

        s = s.feed("FILE_DATA F1 ${base64Encode(ByteArray(512))}")
        card = s.items.filterIsInstance<ChatItem.FileItem>().first()
        assertEquals(FileState.DOWNLOADING, card.state)
        assertEquals("进度应该按字节算", 50, card.progress)

        s = s.feed("FILE_END F1")
        card = s.items.filterIsInstance<ChatItem.FileItem>().first()
        assertEquals(FileState.DONE, card.state)
        assertEquals(100, card.progress)
    }

    @Test
    fun `下载失败显示原因并且可以重试`() {
        var s = ChatState().feed(offerLine())
        s = s.feed("FILE_FAIL F1 文件不存在或已经过期")
        val card = s.items.filterIsInstance<ChatItem.FileItem>().first()
        assertEquals(FileState.FAILED, card.state)
        assertEquals("文件不存在或已经过期", card.note)
        assertEquals("重试", card.buttonLabel)
    }

    @Test
    fun `文件大小显示成人话`() {
        val b64 = base64Encode("a.bin".toByteArray())
        val s = ChatState().feed("FILE_OFFER 21:05 alice F9 $b64 1048576")
        val card = s.items.first() as ChatItem.FileItem
        assertEquals("1.0 MB", card.sizeText)
    }

    @Test
    fun `无关文件的失败不会影响别的卡片`() {
        var s = ChatState().feed(offerLine("F1", "a.txt")).feed(offerLine("F2", "b.txt"))
        s = s.feed("FILE_FAIL F1 过期")
        val cards = s.items.filterIsInstance<ChatItem.FileItem>()
        assertEquals(FileState.FAILED, cards.first { it.fileId == "F1" }.state)
        assertEquals(FileState.OFFERED, cards.first { it.fileId == "F2" }.state)
    }

    // ------------------------------------------------------------------
    // 语音消息：从协议到界面的最后一段
    // ------------------------------------------------------------------

    @Test
    fun `语音消息进来时会被认成语音_并且自动下载`() {
        // 这一条把"协议 → 状态机 → 要不要自动下载"串起来验：
        // 前面 StickerProtocolTest 验的是 FileItem 上的规则，这里验的是
        // **真的收到一条 voice 消息时，状态机算出来的确实满足那条规则**。
        val b64 = base64Encode("voice-3.m4a".toByteArray())
        val s = ChatState().feed("FILE_OFFER 21:06 小明 V1 $b64 48000 0 voice")

        val card = s.items.filterIsInstance<ChatItem.FileItem>().first()
        assertEquals(AttachmentKind.VOICE, card.kind)
        assertFalse("语音不该被当成贴纸去画大图", card.isSticker)
        assertTrue("语音收到就该开始下载，不然点它没反应", card.shouldAutoDownload)
    }

    @Test
    fun `老服务器发的贴纸写法仍然按贴纸处理`() {
        // 已经发出去的贴纸消息写的是 1。换成"种类"字段之后**不能**把它们弄丢，
        // 否则历史消息里的贴纸会变成"要下载的文件卡片"。
        val b64 = base64Encode("开心.png".toByteArray())
        for (kindWord in listOf("1", "sticker")) {
            val s = ChatState().feed("FILE_OFFER 21:06 小明 S1 $b64 4096 0 $kindWord")
            val card = s.items.filterIsInstance<ChatItem.FileItem>().first()
            assertEquals("写 $kindWord 必须仍然认成贴纸", AttachmentKind.STICKER, card.kind)
            assertTrue(card.isSticker)
        }
    }

    @Test
    fun `自己发的语音靠右_别人发的靠左`() {
        val b64 = base64Encode("voice-1.m4a".toByteArray())
        var s = ChatState().copy(selfNick = "我")
        s = s.feed("FILE_OFFER 21:06 我 V1 $b64 48000 0 voice")
        s = s.feed("FILE_OFFER 21:06 小明 V2 $b64 48000 0 voice")

        val cards = s.items.filterIsInstance<ChatItem.FileItem>()
        assertTrue("自己发的应该判成自己的", cards.first { it.fileId == "V1" }.isOwn("我"))
        assertFalse("别人发的不能判成自己的", cards.first { it.fileId == "V2" }.isOwn("我"))
        // 没登录（昵称为空）时谁都不是"自己"，否则界面会把别人的语音画到自己这边
        assertFalse(cards.first().isOwn(""))
    }

    // ------------------------------------------------------------------

    @Test
    fun `WELCOME 显示服务器名`() {
        val s = ChatState().feed("WELCOME 21:05 dchat Server")
        val notice = s.items.first() as ChatItem.NoticeItem
        assertEquals("已连接到服务器 dchat Server", notice.info.text)
        assertNotNull(notice.info.time)
    }
}
