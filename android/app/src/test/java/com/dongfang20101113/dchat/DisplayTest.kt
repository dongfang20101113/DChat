package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.NICK_PALETTE_SIZE
import com.dongfang20101113.dchat.protocol.formatUnreadTitle
import com.dongfang20101113.dchat.protocol.mentionsAll
import com.dongfang20101113.dchat.protocol.mentionsMe
import com.dongfang20101113.dchat.protocol.mentionsNick
import com.dongfang20101113.dchat.protocol.nickColorIndex
import com.dongfang20101113.dchat.protocol.parseNotice
import com.dongfang20101113.dchat.protocol.parseSay
import com.dongfang20101113.dchat.protocol.shouldCountUnread
import com.dongfang20101113.dchat.protocol.shouldFlash
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 显示规则测试（对应桌面端 `test_render`）。
 *
 * @提及的边界是历史 bug 高发区：`@alicex` 不能命中 `@alice`、中文标点紧跟要算提及、
 * 邮箱写法不能误判——这些都在下面钉死。
 */
class DisplayTest {

    // ------------------------------------------------------------------
    // @提及
    // ------------------------------------------------------------------

    @Test
    fun `基本提及`() {
        assertTrue(mentionsNick("@alice 你好", "alice"))
        assertTrue(mentionsNick("你好 @alice", "alice"))
        assertTrue(mentionsNick("@alice", "alice"))
    }

    @Test
    fun `大小写不敏感`() {
        assertTrue(mentionsNick("@ALICE", "alice"))
        assertTrue(mentionsNick("@alice", "ALICE"))
        assertTrue(mentionsNick("@AlIcE 早", "alice"))
    }

    @Test
    fun `@alicex 不能误判成 @alice`() {
        assertFalse(mentionsNick("@alicex 你好", "alice"))
        assertFalse(mentionsNick("@alice_1 你好", "alice"))
        assertFalse(mentionsNick("@alice9", "alice"))
    }

    @Test
    fun `at 前面紧挨词内字符不算提及`() {
        assertFalse("邮箱写法不该误判", mentionsNick("mail@alice.com", "alice"))
        assertFalse(mentionsNick("a@alice", "alice"))
        assertFalse(mentionsNick("_@alice", "alice"))
    }

    @Test
    fun `at 前面是中文或标点算提及`() {
        // 中文不用空格分词，所以"你好@alice"是有效提及
        assertTrue(mentionsNick("你好@alice", "alice"))
        assertTrue(mentionsNick("，@alice", "alice"))
        assertTrue(mentionsNick("(@alice)", "alice"))
    }

    @Test
    fun `昵称后面紧跟中文标点算提及`() {
        assertTrue(mentionsNick("@alice，看这个", "alice"))
        assertTrue(mentionsNick("@alice。", "alice"))
        assertTrue(mentionsNick("@alice你好", "alice"))
    }

    @Test
    fun `@all 对所有人都算提及`() {
        assertTrue(mentionsAll("@all 集合"))
        assertTrue(mentionsAll("@ALL 集合"))
        assertTrue(mentionsAll("大家注意 @all，马上开会"))
        assertFalse(mentionsAll("@allx"))
        assertFalse(mentionsAll("没有提及"))
    }

    @Test
    fun `中文昵称的提及`() {
        assertTrue(mentionsNick("@小明 在吗", "小明"))
        assertTrue(mentionsNick("在吗@小明", "小明"))

        // 这里特意钉住一个**容易被误以为是 bug 的行为**：
        // 只有 ASCII 字母数字下划线算"词内字符"（见 render.h 的注释"中文不用空格分词"），
        // 所以 @小明明 依然会命中"小明"。
        // 曾经我把它写成 assertFalse，测试红了 —— 对照 C++ 源码后确认是**测试写错**，
        // 实现是对的。不能为了让测试变绿而改实现，否则手机端和电脑端的高亮会不一致。
        assertTrue("@小明明 仍然命中 小明（中文不算词内字符）", mentionsNick("@小明明 在吗", "小明"))

        // 而 ASCII 的情况必须挡住
        assertFalse(mentionsNick("@alicex 在吗", "alice"))
    }

    // ------------------------------------------------------------------
    // SAY 解析
    // ------------------------------------------------------------------

    @Test
    fun `解析 SAY_时间可省略`() {
        val withTime = parseSay("SAY 21:05 alice 你好", "bob")!!
        assertEquals("21:05", withTime.time)
        assertEquals("alice", withTime.nick)
        assertEquals("你好", withTime.text)
        assertFalse(withTime.own)

        val withoutTime = parseSay("SAY alice 你好", "bob")!!
        assertEquals("", withoutTime.time)
        assertEquals("alice", withoutTime.nick)
        assertEquals("你好", withoutTime.text)
    }

    @Test
    fun `自己的消息判定为 own 且不再算提及`() {
        val own = parseSay("SAY 21:05 alice @alice 自言自语", "alice")!!
        assertTrue("昵称相同就是自己发的", own.own)
        assertFalse("自己发的不该高亮成提及", own.mention)
    }

    @Test
    fun `别人的消息里提到我才算提及`() {
        val me = parseSay("SAY 21:05 alice @bob 看一下", "bob")!!
        assertTrue(me.mention)
        assertFalse(me.own)

        val all = parseSay("SAY 21:05 alice @all 集合", "bob")!!
        assertTrue(all.mention)
    }

    @Test
    fun `不是 SAY 或者缺昵称时返回 null`() {
        assertNull(parseSay("SYS 21:05 服务器提示", "bob"))
        assertNull(parseSay("SAY 21:05", "bob"))
        assertNull(parseSay("SAY", "bob"))
        assertNull(parseSay("", "bob"))
    }

    @Test
    fun `MentionsMe 只看这一行是不是在叫我`() {
        assertTrue(mentionsMe("SAY 21:05 alice @bob 在吗", "bob"))
        assertFalse(mentionsMe("SAY 21:05 alice 普通消息", "bob"))
        assertFalse(mentionsMe("SYS 21:05 系统提示", "bob"))
    }

    @Test
    fun `正文里有时间样式的文本不会被误剥`() {
        // 昵称后面才是正文，"12:30" 在正文里应该原样保留
        val info = parseSay("SAY 21:05 alice 12:30 开会", "bob")!!
        assertEquals("21:05", info.time)
        assertEquals("alice", info.nick)
        assertEquals("12:30 开会", info.text)
    }

    // ------------------------------------------------------------------
    // 昵称配色
    // ------------------------------------------------------------------

    @Test
    fun `同一个昵称永远同一个颜色`() {
        val a = nickColorIndex("alice")
        repeat(10) { assertEquals(a, nickColorIndex("alice")) }
        assertTrue(a in 0 until NICK_PALETTE_SIZE)
    }

    @Test
    fun `不同昵称会分散到调色板里`() {
        val indices = (1..200).map { nickColorIndex("user$it") }.toSet()
        assertEquals("200 个昵称应该用满 8 色调色板", NICK_PALETTE_SIZE, indices.size)
    }

    @Test
    fun `中文昵称也能取到合法下标`() {
        for (nick in listOf("小明", "张三", "😀", "玩家一号")) {
            val index = nickColorIndex(nick)
            assertTrue("$nick 的下标 $index 越界", index in 0 until NICK_PALETTE_SIZE)
        }
    }

    @Test
    fun `调色板为 0 时不崩`() {
        assertEquals(0, nickColorIndex("alice", 0))
    }

    // ------------------------------------------------------------------
    // 系统提示解析
    // ------------------------------------------------------------------

    @Test
    fun `各类系统提示的中文文案`() {
        assertEquals("已连接到服务器 dchat Server", parseNotice("WELCOME 21:05 dchat Server").text)
        assertEquals("alice 加入了聊天室", parseNotice("JOINED 21:05 alice").text)
        assertEquals("alice 离开了聊天室", parseNotice("LEFT 21:05 alice").text)
        assertEquals("在线成员：alice,bob", parseNotice("NAMES 21:05 alice,bob").text)
        assertEquals("服务器重启通知", parseNotice("SYS 21:05 服务器重启通知").text)
        assertEquals("alice", parseNotice("LOGGEDIN 21:05 alice").text)
        assertEquals("服务器回应正常（PONG）", parseNotice("PONG 21:05").text)
    }

    @Test
    fun `ERROR 被标成错误`() {
        val notice = parseNotice("ERROR 21:05 密码错误")
        assertTrue(notice.isError)
        assertEquals("密码错误", notice.text)
    }

    @Test
    fun `ANNOUNCE 被识别成公告_这就是 say 公告走的那条命令`() {
        val notice = parseNotice("ANNOUNCE 21:05 服务器将在 10 分钟后维护")
        assertTrue("必须是公告", notice.isAnnouncement)
        assertFalse(notice.isError)
        assertEquals("21:05", notice.time)
        assertEquals("服务器将在 10 分钟后维护", notice.text)
    }

    @Test
    fun `未知命令按原文显示_不当作已知提示`() {
        val notice = parseNotice("SOMETHING_NEW 21:05 内容")
        assertFalse("未知命令 isNotice 应为 false", notice.isNotice)
        assertEquals("SOMETHING_NEW 21:05 内容", notice.text)
    }

    // ------------------------------------------------------------------
    // 未读规则
    // ------------------------------------------------------------------

    @Test
    fun `窗口不在前台才计未读`() {
        assertTrue(shouldCountUnread(false))
        assertFalse(shouldCountUnread(true))
    }

    @Test
    fun `只有不在前台且被叫到才闪`() {
        assertTrue(shouldFlash(windowActive = false, isMention = true))
        assertFalse(shouldFlash(windowActive = true, isMention = true))
        assertFalse("普通消息不打扰", shouldFlash(windowActive = false, isMention = false))
    }

    @Test
    fun `未读标题格式`() {
        assertEquals("dchat", formatUnreadTitle("dchat", 0))
        assertEquals("dchat", formatUnreadTitle("dchat", -1))
        assertEquals("【3】dchat", formatUnreadTitle("dchat", 3))
    }
}
