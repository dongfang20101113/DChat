package com.dongfang20101113.dchat.protocol

/**
 * 多会话（私聊 / 群聊）的协议层。
 *
 * 和 [ServerLine] 一样，这一层**零 Android 依赖**：全是纯字符串进、纯数据出，
 * 所以全部行为都能在 JVM 上直接单测，不需要模拟器。这是这个项目一贯的分层——
 * 协议错一个字符不会编译报错，只会表现为"加群没反应"这种最难查的问题。
 *
 * 服务端那边的对应实现是 `src/room.h` / `src/room.cpp`，
 * 线上格式的唯一定义是 `docs/rooms-protocol.md`。三处必须一起改。
 */

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------

/** 房间种类。大厅在数据结构上就是**一个群聊**，只是多了三条硬规则。 */
enum class RoomKind(val wire: String) {
    HALL("hall"),
    GROUP("group"),
    DIRECT("dm");

    /** 这个会话的成员关系是不是不可变更（大厅不可退、不可被踢）。 */
    val fixedMembership: Boolean get() = this == HALL

    companion object {
        fun parse(text: String): RoomKind =
            entries.firstOrNull { it.wire == text.lowercase() } ?: GROUP
    }
}

/** 房间里的角色。 */
enum class RoomRole(val wire: String) {
    OWNER("owner"),
    ADMIN("admin"),
    MEMBER("member");

    val canAdminister: Boolean get() = this == OWNER || this == ADMIN

    companion object {
        fun parse(text: String): RoomRole =
            entries.firstOrNull { it.wire == text.lowercase() } ?: MEMBER
    }
}

/**
 * 加群方式。
 *
 * 三个值**不是同义的**，界面上要分清楚：
 * - [DIRECT] 直接添加：申请即入群，服务端不拦。
 * - [APPROVAL] 管理员审核：进待审队列，必须管理员点同意。
 * - [AUTO] 自动审核：进队列后**立刻自动放行**，但服务端会给管理员一条通知。
 *   如果它和 [DIRECT] 表现完全一样，管理员就永远不知道有谁进来过，
 *   这个选项也就没有存在意义了。
 */
enum class JoinMode(val wire: String, val label: String) {
    DIRECT("direct", "直接添加"),
    APPROVAL("approval", "管理员审核"),
    AUTO("auto", "自动审核");

    companion object {
        fun parse(text: String): JoinMode =
            entries.firstOrNull { it.wire == text.lowercase() } ?: APPROVAL
    }
}

/** 群聊 ID 不合法的原因。和 `room.cpp` 的 RoomIdError 一一对应。 */
enum class RoomIdError {
    NONE,
    EMPTY,
    TOO_SHORT,
    TOO_LONG,
    ILLEGAL_CHAR,
    RESERVED;

    val message: String
        get() = when (this) {
            NONE -> ""
            EMPTY -> "群聊 ID 不能为空"
            TOO_SHORT -> "群聊 ID 太短（至少 3 个字符）"
            TOO_LONG -> "群聊 ID 太长（最多 16 个字符）"
            ILLEGAL_CHAR -> "群聊 ID 只能用英文字母、数字、下划线和减号"
            RESERVED -> "这个 ID 是系统保留的（hall 和 dm- 开头不能用作群聊 ID）"
        }
}

// ---------------------------------------------------------------------------
// 会话数据
// ---------------------------------------------------------------------------

/**
 * 房间设置的**本地镜像**（管理员可设项里那些 `RINFO` 不带的部分）。
 *
 * ⚠️ 字段划分是刻意的：每个值**只有一个家**。
 * 群名 / 最大人数 / 加群方式 / 是否公开 都在 [RoomInfo] 里（`RINFO` 和 `ROOMLIST`
 * 都会给），这里只放它们不带的三项。两边都存一份的话，迟早会出现
 * "列表里显示 200 人、设置页里显示 500 人"这种自相矛盾的界面。
 */
data class RoomSettings(
    val cacheMaxMb: Long = 0L,
    val announcement: String = "",
    val importantNotice: Boolean = true,
)

/**
 * 成员**本地**设置：免打扰 / 置顶 / 重要提醒 / 本地缓存上限。
 *
 * ⚠️ 这一整个结构**永不上传**。服务端代码里没有任何地方会读到它 ——
 * 这就是"管理员设置"和"成员设置"那条分界线在数据类型上的落点。
 */
data class RoomPrefs(
    val muted: Boolean = false,
    val pinned: Boolean = false,
    val alertImportant: Boolean = true,
    val localCacheMb: Long = DEFAULT_LOCAL_CACHE_MB,
) {
    companion object {
        const val DEFAULT_LOCAL_CACHE_MB = 128L
        const val MIN_LOCAL_CACHE_MB = 8L
        const val MAX_LOCAL_CACHE_MB = 64L * 1024L

        /**
         * 还没被用户改过时的默认值。
         *
         * **大厅默认免打扰**：它是全服公共房间，消息最多，默认每条都提示会把人吵死。
         */
        fun default(kind: RoomKind): RoomPrefs = RoomPrefs(muted = kind == RoomKind.HALL)

        /** 落盘格式：`房间ID\t免打扰\t置顶\t重要提醒\t本地缓存MB`。 */
        fun serialize(roomId: String, prefs: RoomPrefs): String = buildString {
            append(roomId).append('\t')
            append(if (prefs.muted) "1" else "0").append('\t')
            append(if (prefs.pinned) "1" else "0").append('\t')
            append(if (prefs.alertImportant) "1" else "0").append('\t')
            append(prefs.localCacheMb)
        }

        /** 解析一行；不是合法行返回 null（调用方跳过）。 */
        fun parseLine(line: String): Pair<String, RoomPrefs>? {
            if (line.isEmpty() || line.startsWith("#")) return null
            val fields = line.split('\t')
            if (fields.size < 5) return null
            val id = fields[0]
            if (!isValidRoomId(id)) return null
            // 缓存上限夹到合法范围内：这个文件是本地的、可能被手工改过或被旧版本写过，
            // 越界值不该让整条设置失效
            val cache = (fields[4].toLongOrNull() ?: DEFAULT_LOCAL_CACHE_MB)
                .coerceIn(MIN_LOCAL_CACHE_MB, MAX_LOCAL_CACHE_MB)
            return id to RoomPrefs(
                muted = fields[1] == "1",
                pinned = fields[2] == "1",
                alertImportant = fields[3] == "1",
                localCacheMb = cache,
            )
        }

        /** 解析整个设置文件；同名以**后出现的为准**（文件是覆盖写的）。 */
        fun parseFile(text: String): Map<String, RoomPrefs> {
            val result = LinkedHashMap<String, RoomPrefs>()
            for (line in text.split('\n')) {
                parseLine(line.trimEnd('\r'))?.let { (id, prefs) -> result[id] = prefs }
            }
            return result
        }
    }
}

/** 一个会话的全部状态（服务端信息 + 本地设置）。 */
data class RoomState(
    val info: RoomInfo,
    val settings: RoomSettings = RoomSettings(),
    val prefs: RoomPrefs = RoomPrefs.default(RoomKind.GROUP),
    val members: List<RoomMember> = emptyList(),
    val pending: List<String> = emptyList(),
    /** 未读条数。免打扰的会话**也照常计数**（界面画成小圆点而不是数字）。 */
    val unread: Int = 0,
    /** 未读里有没有 @我 / @全体成员（免打扰时用强调色提示）。 */
    val unreadImportant: Boolean = false,
) {
    val id: String get() = info.id
    val kind: RoomKind get() = info.kind

    /**
     * 会话列表里显示的名字。
     *
     * 私聊的标题由**服务端**算好（是对方昵称）——不让每个客户端各算一遍，
     * 否则总有一端会算错（比如忘了排除自己，标题显示成自己的名字）。
     */
    val title: String get() = info.displayName.ifEmpty { info.id }

    /** 我的角色能不能管理这个群。 */
    val canAdminister: Boolean get() = info.myRole.canAdminister

    fun withPrefs(update: (RoomPrefs) -> RoomPrefs): RoomState = copy(prefs = update(prefs))

    /**
     * 应用一条 `RSET`。
     *
     * 每一项写到**它唯一的那个家**里（见 [RoomSettings] 的说明）。
     */
    fun applySetting(key: String, value: String): RoomState = when (key) {
        "name" -> copy(info = info.copy(displayName = value.ifEmpty { info.displayName }))
        "maxmembers" -> copy(info = info.copy(maxMembers = value.toIntOrNull() ?: info.maxMembers))
        "joinmode" -> copy(info = info.copy(joinMode = JoinMode.parse(value)))
        "listed" -> copy(info = info.copy(listed = value == "true"))
        "cachemaxmb" -> copy(settings = settings.copy(cacheMaxMb = value.toLongOrNull() ?: 0L))
        "announcement" -> copy(settings = settings.copy(announcement = value))
        "importantnotice" -> copy(settings = settings.copy(importantNotice = value == "true"))
        else -> this
    }

    /**
     * 用一条 `ROOMLIST` 条目刷新基本信息。
     *
     * `ROOMLIST` **只带一部分字段**，所以不能用它整个覆盖 —— 那样会把 `RINFO`
     * 已经告诉我们的群主 / 加群方式 / 是否公开清成默认值，界面上就会突然
     * 从"管理员审核"变成"直接添加"。只更新它真正带来的那几项。
     */
    fun mergeFromListEntry(entry: RoomInfo): RoomState = copy(
        info = info.copy(
            kind = entry.kind,
            displayName = entry.displayName.ifEmpty { info.displayName },
            memberCount = entry.memberCount,
            maxMembers = entry.maxMembers,
            myRole = entry.myRole,
        )
    )
}

/** 会话里的一个成员。 */
data class RoomMember(
    val nick: String,
    val role: RoomRole = RoomRole.MEMBER,
    /** 群昵称；空 = 用账号名。 */
    val display: String = "",
) {
    /** 群里显示的名字。 */
    val shownName: String get() = display.ifEmpty { nick }
}

/** `RINFO` 解出来的会话基本信息。 */
data class RoomInfo(
    val id: String,
    val kind: RoomKind,
    val displayName: String = "",
    val owner: String = "",
    val memberCount: Int = 0,
    val maxMembers: Int = 0,
    val myRole: RoomRole = RoomRole.MEMBER,
    val joinMode: JoinMode = JoinMode.APPROVAL,
    val listed: Boolean = false,
)

/** 群搜索结果（`GHIT`）。 */
data class GroupHit(
    val id: String,
    val name: String,
    val members: Int,
    val maxMembers: Int,
    val joinMode: JoinMode,
)

// ---------------------------------------------------------------------------
// 房间 ID 规则（和服务端 room.cpp 逐条对齐）
// ---------------------------------------------------------------------------

const val MIN_GROUP_ID_CHARS = 3
const val MAX_GROUP_ID_CHARS = 16
const val HALL_ID = "hall"
const val DM_ID_PREFIX = "dm-"

/**
 * 规范化并校验用户自填的群 ID。
 *
 * 强制小写的理由：ID **不可更改**，如果允许大小写，`MyGroup` 和 `mygroup` 就是两个群——
 * 用户搜不到自己刚建的群、多复制一个大写字母就永远进不去，而且没有补救机会。
 * 界面上会先显示规范化后的 ID 让用户确认，不做静默改名。
 *
 * 不许中文的理由：ID 要跨三端、要能出现在命令行里、要能当文件名（每房间的历史
 * 与文件目录按 ID 命名）。想用中文，那是「群名」的活。
 */
fun normalizeGroupId(raw: String): Pair<String, RoomIdError> {
    if (raw.isEmpty()) return "" to RoomIdError.EMPTY
    val id = raw.lowercase()
    if (id.length < MIN_GROUP_ID_CHARS) return "" to RoomIdError.TOO_SHORT
    if (id.length > MAX_GROUP_ID_CHARS) return "" to RoomIdError.TOO_LONG
    for (c in id) {
        val ok = c in 'a'..'z' || c in '0'..'9' || c == '_' || c == '-'
        if (!ok) return "" to RoomIdError.ILLEGAL_CHAR
    }
    if (id == HALL_ID || id.startsWith(DM_ID_PREFIX)) return "" to RoomIdError.RESERVED
    return id to RoomIdError.NONE
}

/** 已经是规范化形式的房间 ID 是否合法（群 ID、私聊 ID、大厅都能过）。 */
fun isValidRoomId(id: String): Boolean {
    if (id == HALL_ID) return true
    if (id.startsWith(DM_ID_PREFIX)) {
        val digits = id.removePrefix(DM_ID_PREFIX)
        return digits.isNotEmpty() && digits.all { it in '0'..'9' }
    }
    val (normalized, error) = normalizeGroupId(id)
    // 规范化结果必须**逐字符相同**：否则说明这个 ID 不是规范形式（比如含大写），
    // 而"不规范但能通过"的 ID 会在查表时找不到自己
    return error == RoomIdError.NONE && normalized == id
}

/**
 * 正文里有没有「@全体成员」。
 *
 * 它是**唯一能越过所有人免打扰**的东西，所以服务端会强制校验（只有群管理员能发）。
 * 客户端也认一遍是为了界面上能提前给出提示，而不是等服务器拒。
 */
fun containsAllMention(text: String): Boolean =
    text.contains("@全体成员") || text.contains("@所有人") ||
        text.lowercase().contains("@all")

/** 群昵称 / 群名的长度上限（和服务端一致，按 Unicode 码点算）。 */
const val MAX_ROOM_NAME_CHARS = 24
const val MAX_ANNOUNCEMENT_CHARS = 500
const val MAX_MEMBER_DISPLAY_CHARS = 12

/** 码点计数（Kotlin 的 String.length 是 UTF-16 单元数，emoji 会被算成 2）。 */
fun codePointCount(text: String): Int = text.codePointCount(0, text.length)

/** 按码点截断。 */
fun truncateByCodePoints(text: String, maxChars: Int): String {
    if (codePointCount(text) <= maxChars) return text
    val end = text.offsetByCodePoints(0, maxChars)
    return text.substring(0, end)
}

// ---------------------------------------------------------------------------
// 服务端 → 客户端的多会话行
// ---------------------------------------------------------------------------

/** 多会话相关的服务端行。解析失败一律返回 null，由调用方当未知命令处理。 */
sealed interface RoomLine {

    /** `FEATURES <能力列表>` —— 服务端确认支持哪些能力。 */
    data class Features(val capabilities: Set<String>) : RoomLine {
        val supportsRooms: Boolean get() = capabilities.contains(ROOMS_CAPABILITY)
    }

    /** `ROOMLIST <房间ID> <kind> <显示名B64> <成员数> <最大人数> <我的角色>` */
    data class RoomListEntry(val info: RoomInfo) : RoomLine

    /** `ROOMLIST_END` */
    data object RoomListEnd : RoomLine

    /** `RINFO <房间ID> <kind> <显示名B64> <群主> <成员数> <上限> <我的角色> <加群方式> <listed>` */
    data class RoomInfoLine(val info: RoomInfo) : RoomLine

    /**
     * `RSET <房间ID> <设置名> <值>`
     *
     * `name` 和 `announcement` 的值是 Base64（可能含空格），其余是明文。
     * 设置名是固定集合，所以这里**直接解出最终值**，调用方不用再判断一次。
     */
    data class RoomSetting(val roomId: String, val key: String, val value: String) : RoomLine

    /** `RMSG <时间> <房间ID> <昵称> <文本>` —— 非大厅消息（群聊 / 私聊）。 */
    data class RoomMessage(
        val roomId: String,
        val time: String,
        val nick: String,
        val text: String,
    ) : RoomLine

    /** `REVENT <时间> <房间ID> <文本>` —— 房间内事件。 */
    data class RoomEvent(val roomId: String, val time: String, val text: String) : RoomLine

    /** `RMEMBERS <房间ID> <昵称> <角色> <群昵称B64>` */
    data class RoomMemberLine(val roomId: String, val member: RoomMember) : RoomLine

    /** `RMEMBERS_END <房间ID>` */
    data class RoomMembersEnd(val roomId: String) : RoomLine

    /** `RHIST <房间ID> <序号> <原始协议行>` —— 历史回放。 */
    data class RoomHistory(val roomId: String, val seq: Long, val raw: String) : RoomLine

    /** `RHIST_END <房间ID>` */
    data class RoomHistoryEnd(val roomId: String) : RoomLine

    /** `RFILE_OFFER <时间> <房间ID> <昵称> <文件ID> <文件名B64> <字节数> <有无缩略图> [<种类>]` */
    data class RoomFileOffer(val roomId: String, val offer: ServerLine.FileOffer) : RoomLine

    /** `GHIT <房间ID> <群名B64> <成员数> <最大人数> <加群方式>` */
    data class GroupHitLine(val hit: GroupHit) : RoomLine

    /** `GHIT_END` */
    data object GroupHitEnd : RoomLine

    /** `GREQ <时间> <房间ID> <昵称>` */
    data class GroupRequest(val roomId: String, val nick: String) : RoomLine

    /** `GREQ_END <房间ID>` */
    data class GroupRequestEnd(val roomId: String) : RoomLine

    /** `GDMLINK <房间ID> <对方昵称>` */
    data class DmLink(val roomId: String, val peer: String) : RoomLine

    /** `GBLOCKS <昵称>` */
    data class BlockListLine(val nick: String) : RoomLine

    /** `GBLOCKS_END` */
    data object BlockListEnd : RoomLine

    companion object {
        /** 客户端声明支持多会话用的能力标签。 */
        const val ROOMS_CAPABILITY = "rooms1"

        /** 参与多会话的命令名。**顺序无关**，只用来快速判断"要不要走这一层"。 */
        val COMMANDS = setOf(
            "FEATURES", "ROOMLIST", "ROOMLIST_END", "RINFO", "RSET",
            "RMSG", "REVENT", "RMEMBERS", "RMEMBERS_END", "RHIST", "RHIST_END",
            "RFILE_OFFER", "GHIT", "GHIT_END", "GREQ", "GREQ_END",
            "GDMLINK", "GBLOCKS", "GBLOCKS_END",
        )

        fun isRoomCommand(command: String): Boolean = COMMANDS.contains(command)

        /** 解析一行；不是多会话行、或字段不全时返回 null。 */
        fun parse(rawLine: String): RoomLine? {
            val msg = parseLine(rawLine)
            if (msg.isEmpty || !COMMANDS.contains(msg.command)) return null
            return when (msg.command) {
                "FEATURES" -> Features(
                    msg.rest.split(',').map { it.trim() }.filter { it.isNotEmpty() }.toSet()
                )
                "ROOMLIST" -> parseRoomListEntry(msg.rest)
                "ROOMLIST_END" -> RoomListEnd
                "RINFO" -> parseRoomInfo(msg.rest)
                "RSET" -> parseSetting(msg.rest)
                "RMSG" -> parseRoomMessage(msg.rest)
                "REVENT" -> {
                    val (time, afterTime) = takeTimePrefix(msg.rest)
                    val words = splitWords(afterTime)
                    if (words.size < 2) null
                    else RoomEvent(words[0], time, words.drop(1).joinToString(" "))
                }
                "RMEMBERS" -> parseMember(msg.rest)
                "RMEMBERS_END" -> RoomMembersEnd(msg.rest.trim())
                "RHIST" -> parseHistory(msg.rest)
                "RHIST_END" -> RoomHistoryEnd(msg.rest.trim())
                "RFILE_OFFER" -> parseRoomFileOffer(msg.rest)
                "GHIT" -> parseGroupHit(msg.rest)
                "GHIT_END" -> GroupHitEnd
                "GREQ" -> parseGroupRequest(msg.rest)
                "GREQ_END" -> GroupRequestEnd(msg.rest.trim())
                "GDMLINK" -> {
                    val words = splitWords(msg.rest)
                    if (words.size < 2) null else DmLink(words[0], words[1])
                }
                "GBLOCKS" -> msg.rest.trim().takeIf { it.isNotEmpty() }?.let { BlockListLine(it) }
                "GBLOCKS_END" -> BlockListEnd
                else -> null
            }
        }

        private fun parseRoomListEntry(rest: String): RoomLine? {
            val words = splitWords(rest)
            if (words.size < 6) return null
            val members = words[3].toIntOrNull() ?: return null
            val max = words[4].toIntOrNull() ?: return null
            return RoomListEntry(
                RoomInfo(
                    id = words[0],
                    kind = RoomKind.parse(words[1]),
                    displayName = base64DecodeToString(words[2]) ?: words[2],
                    memberCount = members,
                    maxMembers = max,
                    myRole = RoomRole.parse(words[5]),
                )
            )
        }

        private fun parseRoomInfo(rest: String): RoomLine? {
            val words = splitWords(rest)
            if (words.size < 9) return null
            val members = words[4].toIntOrNull() ?: return null
            val max = words[5].toIntOrNull() ?: return null
            return RoomInfoLine(
                RoomInfo(
                    id = words[0],
                    kind = RoomKind.parse(words[1]),
                    displayName = base64DecodeToString(words[2]) ?: words[2],
                    owner = words[3],
                    memberCount = members,
                    maxMembers = max,
                    myRole = RoomRole.parse(words[6]),
                    joinMode = JoinMode.parse(words[7]),
                    listed = words[8] == "true",
                )
            )
        }

        /** `name` / `announcement` 的值是 Base64，其余是明文 —— 在这里一次解掉。 */
        private fun parseSetting(rest: String): RoomLine? {
            val words = splitWords(rest)
            if (words.size < 2) return null
            val key = words[1]
            val raw = words.getOrNull(2) ?: ""
            val value = if (key == "name" || key == "announcement") {
                base64DecodeToString(raw) ?: ""
            } else {
                raw
            }
            return RoomSetting(words[0], key, value)
        }

        private fun parseRoomMessage(rest: String): RoomLine? {
            val (time, afterTime) = takeTimePrefix(rest)
            val words = splitWords(afterTime)
            if (words.size < 3) return null
            return RoomMessage(
                roomId = words[0],
                time = time,
                nick = words[1],
                text = words.drop(2).joinToString(" "),
            )
        }

        private fun parseMember(rest: String): RoomLine? {
            val words = splitWords(rest)
            if (words.size < 3) return null
            val display = if (words.size >= 4) base64DecodeToString(words[3]) ?: "" else ""
            return RoomMemberLine(
                words[0],
                RoomMember(nick = words[1], role = RoomRole.parse(words[2]), display = display),
            )
        }

        private fun parseHistory(rest: String): RoomLine? {
            // 格式：<房间ID> <序号> <原始协议行>。原始行里**可能含空格**（本来就有），
            // 所以只能按前两个字段定位，剩下的整段原样保留。
            val firstSpace = rest.indexOf(' ')
            if (firstSpace <= 0) return null
            val roomId = rest.substring(0, firstSpace)
            val afterRoom = rest.substring(firstSpace + 1)
            val secondSpace = afterRoom.indexOf(' ')
            if (secondSpace <= 0) return null
            val seq = afterRoom.substring(0, secondSpace).toLongOrNull() ?: return null
            val raw = afterRoom.substring(secondSpace + 1)
            return RoomHistory(roomId, seq, raw)
        }

        private fun parseRoomFileOffer(rest: String): RoomLine? {
            val (_, afterTime) = takeTimePrefix(rest)
            val words = splitWords(afterTime)
            if (words.size < 6) return null
            val roomId = words[0]
            val size = words[4].toLongOrNull() ?: return null
            val kind = VoiceMessage.parseKind(words.getOrNull(6))
            return RoomFileOffer(
                roomId,
                ServerLine.FileOffer(
                    time = "",
                    nick = words[1],
                    fileId = words[2],
                    fileName = base64DecodeToString(words[3]) ?: words[3],
                    size = size,
                    hasThumbnail = words[5] == "1",
                    isSticker = kind == AttachmentKind.STICKER,
                    kind = kind,
                ),
            )
        }

        private fun parseGroupHit(rest: String): RoomLine? {
            val words = splitWords(rest)
            if (words.size < 5) return null
            val members = words[2].toIntOrNull() ?: return null
            val max = words[3].toIntOrNull() ?: return null
            return GroupHitLine(
                GroupHit(
                    id = words[0],
                    name = base64DecodeToString(words[1]) ?: words[1],
                    members = members,
                    maxMembers = max,
                    joinMode = JoinMode.parse(words[4]),
                )
            )
        }

        private fun parseGroupRequest(rest: String): RoomLine? {
            // GREQ <时间> <房间ID> <昵称>
            val (_, afterTime) = takeTimePrefix(rest)
            val words = splitWords(afterTime)
            if (words.size < 2) return null
            return GroupRequest(words[0], words[1])
        }
    }
}

/**
 * 会话列表的显示顺序：**置顶的排前面**，其余保持服务端给的顺序（大厅、群聊、私聊）。
 *
 * 单独做成纯函数是因为它很容易被"顺手"写进界面代码里，然后三端各排各的、
 * 顺序不一致 —— 而用户会以为"我置顶的群怎么跑到下面去了"。
 */
fun sortRoomsForDisplay(rooms: List<RoomState>): List<RoomState> =
    rooms.filter { it.prefs.pinned } + rooms.filterNot { it.prefs.pinned }

/** Base64 文本解码；失败返回 null（调用方回退用原文，保证至少能显示）。 */
private fun base64DecodeToString(text: String): String? {
    val bytes = base64Decode(text) ?: return null
    return try {
        String(bytes, Charsets.UTF_8)
    } catch (_: Exception) {
        null
    }
}

/** 编码一段文本成 Base64（群名、群公告、搜索关键词都要走它）。 */
fun base64EncodeString(text: String): String = base64Encode(text.toByteArray(Charsets.UTF_8))
