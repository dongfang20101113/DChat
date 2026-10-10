package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.JoinMode
import com.dongfang20101113.dchat.protocol.RoomKind
import com.dongfang20101113.dchat.protocol.RoomLine
import com.dongfang20101113.dchat.protocol.RoomPrefs
import com.dongfang20101113.dchat.protocol.RoomRole
import com.dongfang20101113.dchat.protocol.base64EncodeString
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.HALL
import com.dongfang20101113.dchat.ui.appendToRoom
import com.dongfang20101113.dchat.ui.clearRoomHistory
import com.dongfang20101113.dchat.ui.currentRoom
import com.dongfang20101113.dchat.ui.orderedRooms
import com.dongfang20101113.dchat.ui.reduceRoom
import com.dongfang20101113.dchat.ui.switchRoom
import com.dongfang20101113.dchat.ui.totalUnread
import com.dongfang20101113.dchat.ui.updatePrefs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 多会话的状态迁移测试。
 *
 * 多会话最容易出的错**不是崩溃**，而是这两类：
 *   ① 消息落进了错的会话（私聊内容出现在群里 —— 串会话等于泄露）；
 *   ② 未读 / 免打扰算不对（用户要么被吵死，要么以为群死了）。
 * 这两类只有对着状态断言才查得出来，所以下面大量用例是围绕它们写的。
 */
class RoomStateReducerTest {

    private fun state(): ChatState = ChatState(selfNick = "me")

    /** 建立两个会话：大厅 + 一个群。 */
    private fun ChatState.withTwoRooms(): ChatState =
        this.reduceRoom(RoomLine.Features(setOf("rooms1")), "me")
            .reduceRoom(
                RoomLine.RoomListEntry(
                    com.dongfang20101113.dchat.protocol.RoomInfo(
                        id = HALL, kind = RoomKind.HALL, displayName = "大厅", memberCount = 2,
                    )
                ),
                "me",
            )
            .reduceRoom(
                RoomLine.RoomListEntry(
                    com.dongfang20101113.dchat.protocol.RoomInfo(
                        id = "devgroup", kind = RoomKind.GROUP, displayName = "开发组",
                        memberCount = 5, maxMembers = 200, myRole = RoomRole.OWNER,
                    )
                ),
                "me",
            )
            .reduceRoom(RoomLine.RoomListEnd, "me")

    // -----------------------------------------------------------------------

    @Test
    fun `服务端确认才进多会话模式`() {
        // 老服务端不发 FEATURES；发了但不含 rooms1 也当不支持 ——
        // 界面必须据此退回"只有大厅"，而不是发一堆对方看不懂的命令
        assertFalse(state().reduceRoom(RoomLine.Features(emptySet()), "me").roomsEnabled)
        assertFalse(state().reduceRoom(RoomLine.Features(setOf("别的")), "me").roomsEnabled)
        assertTrue(state().reduceRoom(RoomLine.Features(setOf("rooms1")), "me").roomsEnabled)
    }

    @Test
    fun `会话列表建立后默认停在大厅`() {
        val s = state().withTwoRooms()
        assertEquals(2, s.rooms.size)
        assertEquals(HALL, s.currentRoomId)
        // 大厅默认免打扰：它是全服公共房间，默认每条都提示会把人吵死
        assertTrue(s.rooms.first { it.id == HALL }.prefs.muted)
        assertFalse(s.rooms.first { it.id == "devgroup" }.prefs.muted)
    }

    @Test
    fun `ROOMLIST 不会把 RINFO 已经给的字段清成默认值`() {
        // 真实顺序是 ROOMLIST 先到、RINFO 后到；但列表也可能因为重连再发一次。
        // 如果那时用 ROOMLIST 整个覆盖，界面会突然从"仅群主能发"变回"直接添加"——
        // 用户会以为设置丢了。
        val s = state()
            .reduceRoom(
                RoomLine.RoomInfoLine(
                    com.dongfang20101113.dchat.protocol.RoomInfo(
                        id = "g", kind = RoomKind.GROUP, displayName = "群", owner = "alice",
                        memberCount = 3, maxMembers = 500, myRole = RoomRole.MEMBER,
                        joinMode = JoinMode.AUTO, listed = true,
                    )
                ),
                "me",
            )
            .reduceRoom(
                RoomLine.RoomListEntry(
                    com.dongfang20101113.dchat.protocol.RoomInfo(
                        id = "g", kind = RoomKind.GROUP, displayName = "群", memberCount = 4,
                        maxMembers = 500, myRole = RoomRole.MEMBER,
                    )
                ),
                "me",
            )
        val room = s.rooms.single()
        assertEquals("群主不能被清掉", "alice", room.info.owner)
        assertEquals(JoinMode.AUTO, room.info.joinMode)
        assertTrue(room.info.listed)
        // 但列表真正带来的字段要更新
        assertEquals(4, room.info.memberCount)
    }

    @Test
    fun `RSET 每一项写进它唯一的那个家`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomSetting("devgroup", "name", "新群名"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "maxmembers", "500"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "joinmode", "auto"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "listed", "true"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "cachemaxmb", "512"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "announcement", "今晚八点"), "me")
            .reduceRoom(RoomLine.RoomSetting("devgroup", "importantnotice", "false"), "me")
        val room = s.rooms.first { it.id == "devgroup" }
        assertEquals("新群名", room.title)
        assertEquals(500, room.info.maxMembers)
        assertEquals(JoinMode.AUTO, room.info.joinMode)
        assertTrue(room.info.listed)
        assertEquals(512L, room.settings.cacheMaxMb)
        assertEquals("今晚八点", room.settings.announcement)
        assertFalse(room.settings.importantNotice)
    }

    @Test
    fun `当前会话的消息直接进记录区`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage(HALL, "21:00", "bob", "大家好"), "me")
        assertEquals(1, s.items.size)
        assertEquals(0, s.totalUnread)
    }

    @Test
    fun `别的会话的消息不会串到当前记录区`() {
        // 这是多会话里最严重的一类 bug：私聊内容出现在群里 = 泄露
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:00", "bob", "群里的话"), "me")
        // 当前在大厅，记录区不该有它
        assertTrue(s.items.isEmpty())
        assertEquals(1, s.roomItems.getValue("devgroup").size)
        assertEquals(1, s.totalUnread)
    }

    @Test
    fun `切过去之后能看到那个会话的记录`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:00", "bob", "群里的话"), "me")
            .switchRoom("devgroup")
        assertEquals(1, s.items.size)
        assertEquals(0, s.totalUnread)  // 看了就清未读
        // 切回去，大厅原来的内容还在（没丢）
        val back = s.switchRoom(HALL)
        assertEquals(HALL, back.currentRoomId)
        assertEquals(1, back.roomItems.getValue("devgroup").size)
    }

    @Test
    fun `切换会话不丢任何一边的记录`() {
        var s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage(HALL, "21:00", "a", "大厅一句"), "me")
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:01", "b", "群里一句"), "me")
        // 大厅 -> 群 -> 大厅 -> 群，来回切
        repeat(3) {
            s = s.switchRoom("devgroup").switchRoom(HALL)
        }
        assertEquals(1, s.items.size)
        assertEquals(1, s.roomItems.getValue("devgroup").size)
    }

    @Test
    fun `免打扰的会话照常计未读`() {
        // 免打扰的意思是"别打扰我"（不闪任务栏、不出提示音），
        // 不是"有新消息也别让我知道"。完全不计数的话用户会以为这个群死了。
        val s = state().withTwoRooms()
            .updatePrefs("devgroup") { it.copy(muted = true) }
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:00", "bob", "静音群里的消息"), "me")
        assertEquals(1, s.rooms.first { it.id == "devgroup" }.unread)
        assertEquals(1, s.totalUnread)
    }

    @Test
    fun `历史回放不计未读`() {
        // 否则一进群就是满屏红点，而用户根本没离开过
        val raw = "RMSG 21:00 devgroup bob 以前说过的话"
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomHistory("devgroup", 1L, raw), "me")
            .reduceRoom(RoomLine.RoomHistory("devgroup", 2L, raw), "me")
            .reduceRoom(RoomLine.RoomHistoryEnd("devgroup"), "me")
        assertEquals(2, s.roomItems.getValue("devgroup").size)
        assertEquals(0, s.totalUnread)
    }

    @Test
    fun `历史里裹着的行和实时消息解析出同样的内容`() {
        // 两条路径共用同一套解析，所以"历史里显示不对、实时是对的"不会发生
        val raw = "RMSG 21:00 devgroup bob @me 在吗"
        val live = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:00", "bob", "@me 在吗"), "me")
        val fromHistory = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomHistory("devgroup", 1L, raw), "me")
        val liveItem = live.roomItems.getValue("devgroup").single() as ChatItem.SayItem
        val histItem = fromHistory.roomItems.getValue("devgroup").single() as ChatItem.SayItem
        assertEquals(liveItem.info.text, histItem.info.text)
        assertEquals(liveItem.info.nick, histItem.info.nick)
        // 两边都要认出"这是在 @我"
        assertTrue(liveItem.info.mention)
        assertTrue(histItem.info.mention)
    }

    @Test
    fun `自己发的消息不会被当成 @我`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage("devgroup", "21:00", "me", "@me 自言自语"), "me")
        val item = s.roomItems.getValue("devgroup").single() as ChatItem.SayItem
        assertTrue(item.info.own)
        assertFalse(item.info.mention)
    }

    @Test
    fun `记录条目的 key 全局唯一`() {
        // Compose 的列表按 key 复用节点：重复的 key 会画出错位的界面，
        // 症状是"两条消息看起来是同一条"
        var s = state().withTwoRooms()
        repeat(5) { i ->
            s = s.reduceRoom(RoomLine.RoomMessage(HALL, "21:0$i", "bob", "第 $i 条"), "me")
            s = s.reduceRoom(RoomLine.RoomMessage("devgroup", "21:0$i", "bob", "群 $i"), "me")
        }
        val keys = s.items.map { it.key } + s.roomItems.getValue("devgroup").map { it.key }
        assertEquals(keys.size, keys.toSet().size)
    }

    @Test
    fun `置顶的会话排在前面`() {
        val s = state().withTwoRooms()
            .updatePrefs("devgroup") { it.copy(pinned = true) }
        assertEquals(listOf("devgroup", HALL), s.orderedRooms.map { it.id })
    }

    @Test
    fun `群搜索攒齐一轮才显示`() {
        var s = state().copy(groupSearching = true)
        s = s.reduceRoom(
            RoomLine.GroupHitLine(
                com.dongfang20101113.dchat.protocol.GroupHit("a", "群一", 3, 50, JoinMode.APPROVAL)
            ),
            "me",
        )
        assertEquals(1, s.groupHits.size)
        s = s.reduceRoom(RoomLine.GroupHitEnd, "me")
        assertFalse(s.groupSearching)
        assertEquals(1, s.groupHits.size)
    }

    @Test
    fun `私聊会话用对方昵称当标题`() {
        // 服务端算好标题：让每个客户端各算一遍的话，总有一端会算错
        // （比如忘了排除自己，标题显示成自己的名字）
        val s = state().reduceRoom(RoomLine.DmLink("dm-000001", "bob"), "me")
        assertEquals("bob", s.rooms.single().title)
        assertEquals(RoomKind.DIRECT, s.rooms.single().kind)
    }

    @Test
    fun `删除本地记录只影响本机状态`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.RoomMessage(HALL, "21:00", "bob", "一句"), "me")
            .clearRoomHistory(HALL)
        assertTrue(s.items.isEmpty())
    }

    @Test
    fun `群文件和群事件落进对应的会话`() {
        val nameB64 = base64EncodeString("报告.pdf")
        val s = state().withTwoRooms()
            .reduceRoom(
                RoomLine.RoomFileOffer(
                    "devgroup",
                    com.dongfang20101113.dchat.protocol.ServerLine.FileOffer(
                        time = "21:00", nick = "bob", fileId = "F1", fileName = "报告.pdf",
                        size = 1024, hasThumbnail = false,
                    ),
                ),
                "me",
            )
            .reduceRoom(RoomLine.RoomEvent("devgroup", "21:01", "carol 加入了群聊"), "me")
        val items = s.roomItems.getValue("devgroup")
        assertEquals(2, items.size)
        assertTrue(items[0] is ChatItem.FileItem)
        assertTrue(items[1] is ChatItem.NoticeItem)
        // 群事件不计未读（"谁加入了"不该把人从睡梦里叫醒），文件计
        assertEquals(1, s.rooms.first { it.id == "devgroup" }.unread)
    }

    @Test
    fun `入群申请进对应会话的待审队列`() {
        val s = state().withTwoRooms()
            .reduceRoom(RoomLine.GroupRequest("devgroup", "bob"), "me")
            .reduceRoom(RoomLine.GroupRequest("devgroup", "bob"), "me")
        // 同一个人重复申请不该在列表里出现两次
        assertEquals(listOf("bob"), s.rooms.first { it.id == "devgroup" }.pending)
    }

    @Test
    fun `黑名单收得进来`() {
        val s = state()
            .reduceRoom(RoomLine.BlockListLine("bob"), "me")
            .reduceRoom(RoomLine.BlockListLine("carol"), "me")
            .reduceRoom(RoomLine.BlockListEnd, "me")
        assertEquals(listOf("bob", "carol"), s.blockList)
    }

    @Test
    fun `ROOMLIST_END 时一个会话都没有就补上大厅`() {
        // 兜底：万一服务端没发 ROOMLIST（或者中途断了），界面不该是一片空白
        val s = state().reduceRoom(RoomLine.RoomListEnd, "me")
        assertEquals(1, s.rooms.size)
        assertEquals(HALL, s.rooms.single().id)
        assertTrue(s.rooms.single().prefs.muted)
    }

    @Test
    fun `老服务端下一切照旧`() {
        // 不发 FEATURES 就是老服务端：roomsEnabled 保持 false，界面退回只有大厅，
        // 大厅消息走原来的 items 路径，一行都没变
        val s = state()
            .reduceRoom(RoomLine.RoomMessage(HALL, "21:00", "bob", "大厅消息"), "me")
        assertFalse(s.roomsEnabled)
        assertEquals(1, s.items.size)
    }

    @Test
    fun `本地设置默认值来自会话种类`() {
        val s = state().withTwoRooms()
        assertEquals(RoomPrefs.default(RoomKind.HALL), s.rooms.first { it.id == HALL }.prefs)
        assertEquals(
            RoomPrefs.default(RoomKind.GROUP),
            s.rooms.first { it.id == "devgroup" }.prefs,
        )
    }

    @Test
    fun `currentRoom 取得到当前会话`() {
        val s = state().withTwoRooms().switchRoom("devgroup")
        assertNotNull(s.currentRoom)
        assertEquals("devgroup", s.currentRoom?.id)
        assertNull(state().currentRoom)
    }
}
