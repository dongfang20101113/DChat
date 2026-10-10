package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.JoinMode
import com.dongfang20101113.dchat.protocol.RoomIdError
import com.dongfang20101113.dchat.protocol.RoomInfo
import com.dongfang20101113.dchat.protocol.RoomKind
import com.dongfang20101113.dchat.protocol.RoomLine
import com.dongfang20101113.dchat.protocol.RoomMember
import com.dongfang20101113.dchat.protocol.RoomPrefs
import com.dongfang20101113.dchat.protocol.RoomRole
import com.dongfang20101113.dchat.protocol.RoomState
import com.dongfang20101113.dchat.protocol.base64EncodeString
import com.dongfang20101113.dchat.protocol.codePointCount
import com.dongfang20101113.dchat.protocol.containsAllMention
import com.dongfang20101113.dchat.protocol.isValidRoomId
import com.dongfang20101113.dchat.protocol.normalizeGroupId
import com.dongfang20101113.dchat.protocol.sortRoomsForDisplay
import com.dongfang20101113.dchat.protocol.truncateByCodePoints
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 多会话协议层的单元测试。
 *
 * 这一层零 Android 依赖，所以全部行为都能在 JVM 上直接跑。测的重点是
 * **只有跨端联调才会暴露的那类错误**：字段顺序、Base64、时间戳位置、ID 规则。
 * 这些错了不会编译报错，只会表现为"加群没反应"或"名字显示成乱码"。
 */
class RoomProtocolTest {

    // -----------------------------------------------------------------------
    // 群聊 ID 规则
    // -----------------------------------------------------------------------

    @Test
    fun `群聊 ID 合法时原样通过`() {
        assertEquals("abc" to RoomIdError.NONE, normalizeGroupId("abc"))
        assertEquals("abc-123_x" to RoomIdError.NONE, normalizeGroupId("abc-123_x"))
        assertEquals("abcdefghijklmnop" to RoomIdError.NONE, normalizeGroupId("abcdefghijklmnop"))
    }

    @Test
    fun `群聊 ID 统一转小写`() {
        // ID 不可更改：允许大小写的话 MyGroup 和 mygroup 就是两个群，
        // 用户搜不到自己刚建的群，而且没有补救机会
        assertEquals("mygroup" to RoomIdError.NONE, normalizeGroupId("MyGroup"))
        assertEquals("abc" to RoomIdError.NONE, normalizeGroupId("ABC"))
    }

    @Test
    fun `群聊 ID 长度与字符集被守住`() {
        assertEquals(RoomIdError.EMPTY, normalizeGroupId("").second)
        assertEquals(RoomIdError.TOO_SHORT, normalizeGroupId("ab").second)
        assertEquals(RoomIdError.TOO_LONG, normalizeGroupId("abcdefghijklmnopq").second)
        assertEquals(RoomIdError.ILLEGAL_CHAR, normalizeGroupId("ab c").second)
        assertEquals(RoomIdError.ILLEGAL_CHAR, normalizeGroupId("中文群").second)
        assertEquals(RoomIdError.ILLEGAL_CHAR, normalizeGroupId("ab.c").second)
    }

    @Test
    fun `hall 与 dm- 是保留名`() {
        assertEquals(RoomIdError.RESERVED, normalizeGroupId("hall").second)
        // 大写转小写之后同样是保留字
        assertEquals(RoomIdError.RESERVED, normalizeGroupId("HALL").second)
        assertEquals(RoomIdError.RESERVED, normalizeGroupId("dm-001").second)
        // 只保留带减号的前缀：dmgroup 是个正常名字
        assertEquals("dmgroup" to RoomIdError.NONE, normalizeGroupId("dmgroup"))
    }

    @Test
    fun `isValidRoomId 只认规范形式`() {
        assertTrue(isValidRoomId("hall"))
        assertTrue(isValidRoomId("mygroup"))
        assertTrue(isValidRoomId("dm-000001"))
        // 含大写 = 不是规范形式。放行的话查表时找不到自己
        assertFalse(isValidRoomId("MyGroup"))
        assertFalse(isValidRoomId("ab"))
        assertFalse(isValidRoomId("dm-abc"))
    }

    // -----------------------------------------------------------------------
    // @全体成员
    // -----------------------------------------------------------------------

    @Test
    fun `@全体成员 认三种写法`() {
        // 唯一能越过所有人免打扰的东西，认漏一种就会变成"我明明 @ 了全体却没人收到"
        assertTrue(containsAllMention("@全体成员 开会了"))
        assertTrue(containsAllMention("@所有人 开会了"))
        assertTrue(containsAllMention("@ALL 开会了"))
        assertTrue(containsAllMention("大家 @All 注意"))
        assertFalse(containsAllMention("@小明 你看看"))
        assertFalse(containsAllMention("普通的消息"))
    }

    // -----------------------------------------------------------------------
    // 本地设置
    // -----------------------------------------------------------------------

    @Test
    fun `大厅默认免打扰`() {
        assertTrue(RoomPrefs.default(RoomKind.HALL).muted)
        assertFalse(RoomPrefs.default(RoomKind.GROUP).muted)
        assertFalse(RoomPrefs.default(RoomKind.DIRECT).muted)
    }

    @Test
    fun `本地设置落盘往返一致`() {
        val prefs = RoomPrefs(muted = true, pinned = true, alertImportant = false, localCacheMb = 512)
        val line = RoomPrefs.serialize("mygroup", prefs)
        assertEquals("mygroup\t1\t1\t0\t512", line)
        val parsed = RoomPrefs.parseLine(line)
        assertEquals("mygroup", parsed?.first)
        assertEquals(prefs, parsed?.second)
    }

    @Test
    fun `本地设置文件越界值被夹住而不是整条失效`() {
        // 这个文件是本地的，可能被手工改过或被旧版本写过。
        // 越界值让整条设置失效的话，用户会觉得"我设的免打扰又没了"
        val tooBig = RoomPrefs.parseLine("mygroup\t1\t0\t1\t999999999")
        assertEquals(RoomPrefs.MAX_LOCAL_CACHE_MB, tooBig?.second?.localCacheMb)
        val tooSmall = RoomPrefs.parseLine("mygroup\t1\t0\t1\t0")
        assertEquals(RoomPrefs.MIN_LOCAL_CACHE_MB, tooSmall?.second?.localCacheMb)
    }

    @Test
    fun `本地设置文件里的坏行被跳过`() {
        assertNull(RoomPrefs.parseLine("# 注释"))
        assertNull(RoomPrefs.parseLine("mygroup\t1\t1"))
        assertNull(RoomPrefs.parseLine("非法ID!\t1\t1\t1\t128"))

        val file = """
            # 注释
            mygroup	1	1	0	512

            hall	1	0	1	128
        """.trimIndent()
        val parsed = RoomPrefs.parseFile(file)
        assertEquals(2, parsed.size)
        assertTrue(parsed.getValue("mygroup").pinned)
        assertTrue(parsed.getValue("hall").muted)
    }

    // -----------------------------------------------------------------------
    // 会话列表顺序
    // -----------------------------------------------------------------------

    @Test
    fun `置顶的会话排在最前面`() {
        fun room(id: String, pinned: Boolean) = RoomState(
            info = RoomInfo(id = id, kind = RoomKind.GROUP),
            prefs = RoomPrefs(pinned = pinned),
        )
        val ordered = sortRoomsForDisplay(
            listOf(room("a", false), room("b", true), room("c", false), room("d", true))
        )
        // 置顶的保持原有相对顺序，没置顶的也是
        assertEquals(listOf("b", "d", "a", "c"), ordered.map { it.id })
    }

    // -----------------------------------------------------------------------
    // 服务端行的解析
    // -----------------------------------------------------------------------

    @Test
    fun `FEATURES 判定服务端是否支持多会话`() {
        val yes = RoomLine.parse("FEATURES rooms1") as RoomLine.Features
        assertTrue(yes.supportsRooms)
        // 老服务端根本不会发这一行；发了但不含 rooms1 也当不支持 ——
        // 客户端必须据此退回"只有大厅"的老界面，而不是发一堆对方看不懂的命令
        val no = RoomLine.parse("FEATURES somethingelse") as RoomLine.Features
        assertFalse(no.supportsRooms)
    }

    @Test
    fun `ROOMLIST 解出会话条目`() {
        val nameB64 = base64EncodeString("dchat 开发组")
        val line = RoomLine.parse("ROOMLIST mygroup group $nameB64 5 200 owner") as RoomLine.RoomListEntry
        assertEquals("mygroup", line.info.id)
        assertEquals(RoomKind.GROUP, line.info.kind)
        assertEquals("dchat 开发组", line.info.displayName)
        assertEquals(5, line.info.memberCount)
        assertEquals(200, line.info.maxMembers)
        assertEquals(RoomRole.OWNER, line.info.myRole)
        assertTrue(RoomLine.parse("ROOMLIST_END") is RoomLine.RoomListEnd)
    }

    @Test
    fun `RINFO 解出加群方式与是否公开`() {
        val name = base64EncodeString("我的群")
        val line = RoomLine.parse("RINFO mygroup group $name alice 3 200 member auto true")
            as RoomLine.RoomInfoLine
        assertEquals("我的群", line.info.displayName)
        assertEquals("alice", line.info.owner)
        assertEquals(RoomRole.MEMBER, line.info.myRole)
        assertEquals(JoinMode.AUTO, line.info.joinMode)
        assertTrue(line.info.listed)
    }

    @Test
    fun `RSET 把群名和公告的 Base64 直接解掉`() {
        val name = RoomLine.parse("RSET mygroup name ${base64EncodeString("我的群聊")}")
            as RoomLine.RoomSetting
        assertEquals("name", name.key)
        assertEquals("我的群聊", name.value)

        // 其余项是明文，不能被当成 Base64 去解（解出来会是乱码或空）
        val max = RoomLine.parse("RSET mygroup maxmembers 500") as RoomLine.RoomSetting
        assertEquals("500", max.value)
        val listed = RoomLine.parse("RSET mygroup listed true") as RoomLine.RoomSetting
        assertEquals("true", listed.value)
        // 空公告是合法的（"撤销公告"是个正常操作）
        val emptyAnn = RoomLine.parse("RSET mygroup announcement ")
            as RoomLine.RoomSetting
        assertEquals("", emptyAnn.value)
    }

    @Test
    fun `RMSG 的时间戳在前、房间号紧随其后`() {
        // ⚠️ 这条是踩过的坑：协议文档一度写成"房间号在前"，而服务端所有带时间的行
        // 都走同一个 Timed() 辅助函数，时间永远在最前面。写反了消息会整体错位。
        val line = RoomLine.parse("RMSG 21:05 mygroup alice 大家好 这是一句话")
            as RoomLine.RoomMessage
        assertEquals("21:05", line.time)
        assertEquals("mygroup", line.roomId)
        assertEquals("alice", line.nick)
        // 正文里的空格要原样保留（不能按空白切完就丢掉）
        assertEquals("大家好 这是一句话", line.text)
    }

    @Test
    fun `REVENT 的事件文本保留下划线之外的空格`() {
        val line = RoomLine.parse("REVENT 21:06 mygroup bob 被移出了群聊") as RoomLine.RoomEvent
        assertEquals("mygroup", line.roomId)
        assertEquals("21:06", line.time)
        assertEquals("bob 被移出了群聊", line.text)
    }

    @Test
    fun `RMEMBERS 解出角色与群昵称`() {
        val displayB64 = base64EncodeString("小鲍勃")
        val line = RoomLine.parse("RMEMBERS mygroup bob admin $displayB64")
            as RoomLine.RoomMemberLine
        assertEquals("mygroup", line.roomId)
        assertEquals("bob", line.member.nick)
        assertEquals(RoomRole.ADMIN, line.member.role)
        assertEquals("小鲍勃", line.member.shownName)
        // 没设群昵称就显示账号名
        val plain = RoomLine.parse("RMEMBERS mygroup carol member ") as RoomLine.RoomMemberLine
        assertEquals("carol", plain.member.shownName)
    }

    @Test
    fun `RHIST 原样保留裹在里面的协议行`() {
        // 历史里裹着的是一条**完整的协议行**，它本身含有空格。
        // 按空白切完再拼回去会把多个连续空格压成一个 —— 所以必须按前两个字段定位、
        // 剩下的整段保留。
        val inner = "RMSG 21:05 mygroup alice 大家好 这是一句话"
        val line = RoomLine.parse("RHIST mygroup 42 $inner") as RoomLine.RoomHistory
        assertEquals("mygroup", line.roomId)
        assertEquals(42L, line.seq)
        assertEquals(inner, line.raw)
        // 裹着的那行自己还要能再解析一次（同一套函数，历史和实时共用）
        assertTrue(RoomLine.parse(line.raw) is RoomLine.RoomMessage)
    }

    @Test
    fun `RFILE_OFFER 比 FILE_OFFER 多一格房间号`() {
        val nameB64 = base64EncodeString("报告.pdf")
        val line = RoomLine.parse("RFILE_OFFER 21:07 mygroup alice F1 $nameB64 1024 1 voice")
            as RoomLine.RoomFileOffer
        assertEquals("mygroup", line.roomId)
        assertEquals("alice", line.offer.nick)
        assertEquals("F1", line.offer.fileId)
        assertEquals("报告.pdf", line.offer.fileName)
        assertEquals(1024L, line.offer.size)
        assertTrue(line.offer.hasThumbnail)
        assertTrue(line.offer.isVoiceOrStickerKind())
    }

    private fun com.dongfang20101113.dchat.protocol.ServerLine.FileOffer.isVoiceOrStickerKind(): Boolean =
        kind != com.dongfang20101113.dchat.protocol.AttachmentKind.FILE

    @Test
    fun `群搜索与入群申请`() {
        val name = base64EncodeString("摸鱼群")
        val hit = RoomLine.parse("GHIT moyu $name 28 50 approval") as RoomLine.GroupHitLine
        assertEquals("moyu", hit.hit.id)
        assertEquals("摸鱼群", hit.hit.name)
        assertEquals(28, hit.hit.members)
        assertEquals(50, hit.hit.maxMembers)
        assertEquals(JoinMode.APPROVAL, hit.hit.joinMode)
        assertTrue(RoomLine.parse("GHIT_END") is RoomLine.GroupHitEnd)

        val request = RoomLine.parse("GREQ 21:08 mygroup bob") as RoomLine.GroupRequest
        assertEquals("mygroup", request.roomId)
        assertEquals("bob", request.nick)
    }

    @Test
    fun `私聊与黑名单`() {
        val link = RoomLine.parse("GDMLINK dm-000001 bob") as RoomLine.DmLink
        assertEquals("dm-000001", link.roomId)
        assertEquals("bob", link.peer)

        val blocked = RoomLine.parse("GBLOCKS bob") as RoomLine.BlockListLine
        assertEquals("bob", blocked.nick)
        assertTrue(RoomLine.parse("GBLOCKS_END") is RoomLine.BlockListEnd)
    }

    @Test
    fun `字段不全的行一律返回 null 而不是崩`() {
        // 网络数据不可信：坏行不该让客户端崩溃，也不该被当成"这条命令没发生"
        assertNull(RoomLine.parse("ROOMLIST mygroup group"))
        assertNull(RoomLine.parse("RINFO mygroup group name"))
        assertNull(RoomLine.parse("RMSG 21:05 mygroup"))
        assertNull(RoomLine.parse("RHIST mygroup"))
        assertNull(RoomLine.parse("GHIT moyu"))
        assertNull(RoomLine.parse("RMEMBERS mygroup"))
    }

    @Test
    fun `不是多会话的行不会被这一层接走`() {
        // 老命令必须原样落到原来的解析路径上，否则大厅会整个失效
        assertFalse(RoomLine.isRoomCommand("SAY"))
        assertFalse(RoomLine.isRoomCommand("FILE_OFFER"))
        assertNull(RoomLine.parse("SAY 21:05 bob 大家好"))
        assertNull(RoomLine.parse("FILE_OFFER 21:05 bob F1 eA== 10 0"))
        assertTrue(RoomLine.isRoomCommand("RMSG"))
        assertTrue(RoomLine.isRoomCommand("RFILE_OFFER"))
    }

    // -----------------------------------------------------------------------
    // 显示
    // -----------------------------------------------------------------------

    @Test
    fun `私聊标题由服务端算好`() {
        // 服务端直接给"对方昵称"。让每个客户端各算一遍的话，总有一端会算错
        // （比如忘了排除自己，标题显示成自己的名字）
        val dm = RoomState(
            info = RoomInfo(id = "dm-000001", kind = RoomKind.DIRECT, displayName = "bob"),
        )
        assertEquals("bob", dm.title)
        // 还没收到 RINFO 时退回房间 ID，至少不是空白
        val pending = RoomState(info = RoomInfo(id = "dm-000002", kind = RoomKind.DIRECT))
        assertEquals("dm-000002", pending.title)
    }

    @Test
    fun `长度按码点算而不是 UTF-16 单元`() {
        // emoji 在 Kotlin 里占 2 个 char，按 length 算的话用户只能起一半长度的名字。
        // "群名😀测试" = 5 个码点，但 length 是 6。
        val text = "群名😀测试"
        assertEquals(5, codePointCount(text))
        assertEquals(6, text.length)
        assertEquals("群名😀", truncateByCodePoints(text, 3))
        // 恰好等于上限时不截断
        assertEquals(text, truncateByCodePoints(text, 5))
        assertEquals("群", truncateByCodePoints(text, 1))
    }

    @Test
    fun `大厅的规则与群聊不同`() {
        assertTrue(RoomKind.HALL.fixedMembership)
        assertFalse(RoomKind.GROUP.fixedMembership)
        assertTrue(RoomRole.OWNER.canAdminister)
        assertTrue(RoomRole.ADMIN.canAdminister)
        assertFalse(RoomRole.MEMBER.canAdminister)
    }
}
