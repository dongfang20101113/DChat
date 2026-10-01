package com.dongfang20101113.dchat.protocol

/**
 * 显示规则：解析协议行、判定 @提及、昵称配色、未读规则。
 *
 * 这是 C++ 端 `render.cpp` 的等价移植。**不依赖任何 Android API**，可脱离界面做单元测试。
 *
 * 移植时特别注意 [nickColorIndex] 用的是 **UTF-8 字节**做 FNV-1a——
 * 这样同一个昵称在电脑端和手机端会得到**同一个颜色**，两边看起来才一致。
 */

/** 昵称调色板大小（与桌面端一致）。 */
const val NICK_PALETTE_SIZE: Int = 8

/**
 * 同一个昵称永远映射到同一个颜色下标（FNV-1a，按 UTF-8 字节计算）。
 *
 * 必须按字节算而不是按 UTF-16 字符算，否则中文昵称在两端会配色不同。
 */
fun nickColorIndex(nick: String, paletteSize: Int = NICK_PALETTE_SIZE): Int {
    if (paletteSize <= 0) return 0
    var hash = 2166136261u
    for (b in nick.toByteArray(Charsets.UTF_8)) {
        hash = hash xor (b.toInt() and 0xFF).toUInt()
        hash *= 16777619u
    }
    return (hash % paletteSize.toUInt()).toInt()
}

/** 协议里的"词内字符"：只算 ASCII 字母数字下划线（中文不用空格分词，所以中文不算）。 */
internal fun isWordChar(c: Char): Boolean =
    (c in '0'..'9') || (c in 'a'..'z') || (c in 'A'..'Z') || c == '_'

/**
 * 文本里是否提到了 [nick]。
 *
 * 规则：`@` 前面不能紧挨词内字符，昵称后面也不能紧跟词内字符（大小写不敏感）。
 * 于是 `@alicex` **不会**命中 `@alice`，而 `@alice，看这个`、`@alice你好` 都算提及。
 */
fun mentionsNick(text: String, nick: String): Boolean {
    if (nick.isEmpty() || text.isEmpty()) return false
    val length = nick.length
    var i = 0
    while (i + 1 + length <= text.length) {
        if (text[i] != '@') { i++; continue }
        if (i > 0 && isWordChar(text[i - 1])) { i++; continue }

        var same = true
        for (k in 0 until length) {
            if (text[i + 1 + k].lowercaseCharAscii() != nick[k].lowercaseCharAscii()) {
                same = false
                break
            }
        }
        if (!same) { i++; continue }

        val after = i + 1 + length
        if (after < text.length && isWordChar(text[after])) { i++; continue }
        return true
    }
    return false
}

/** 只把 ASCII 大写字母转小写（避免 `lowercaseChar()` 对土耳其语 I 之类的意外行为）。 */
private fun Char.lowercaseCharAscii(): Char = if (this in 'A'..'Z') this + 32 else this

/** `@all`：广播式提及，所有人都算被提到。 */
fun mentionsAll(text: String): Boolean = mentionsNick(text, "all")

/** 一条 SAY 消息解析出来的内容（气泡要用的信息）。 */
data class SayInfo(
    /** `"21:05"`；可能为空（兼容不带时间的旧格式）。 */
    val time: String,
    /** 发言者。 */
    val nick: String,
    /** 正文。 */
    val text: String,
    /** 是不是自己发的（决定气泡靠左还是靠右）。 */
    val own: Boolean,
    /** 是否 @我 或 @all（气泡高亮）。 */
    val mention: Boolean,
)

/** 系统提示（加入/离开/在线名单/错误/公告等）。 */
data class NoticeInfo(
    /** `"21:05"`；可能为空。 */
    val time: String,
    /** 要显示的文本。 */
    val text: String,
    /** 错误信息（红色）。 */
    val isError: Boolean = false,
    /** 全服公告（客户端会大字居中、特殊颜色显示）。 */
    val isAnnouncement: Boolean = false,
    /** 是否是能识别的协议行；false 表示未知命令，按原文显示。 */
    val isNotice: Boolean = true,
)

/** 取出开头可能存在的 `hh:mm` 时间字段，并把它从 rest 里去掉。 */
internal fun takeTimePrefix(rest: String): Pair<String, String> {
    if (rest.isEmpty()) return "" to rest
    val space = rest.indexOf(' ')
    val head = if (space < 0) rest else rest.substring(0, space)
    if (!looksLikeTime(head)) return "" to rest
    val remaining = if (space < 0) "" else rest.substring(space + 1)
    return head to remaining
}

/** 取第一个词，剩下的留下。 */
internal fun takeFirstWord(rest: String): Pair<String, String> {
    if (rest.isEmpty()) return "" to rest
    val space = rest.indexOf(' ')
    if (space < 0) return rest to ""
    return rest.substring(0, space) to rest.substring(space + 1)
}

/**
 * 解析 SAY 行。不是 SAY、或缺少昵称时返回 `null`。
 *
 * 注意：`own` 只比对昵称，所以断线重连后改昵称会影响气泡左右——与桌面端行为一致。
 */
fun parseSay(rawLine: String, selfNick: String): SayInfo? {
    val msg = parseLine(rawLine)
    if (msg.command != "SAY") return null

    val (time, afterTime) = takeTimePrefix(msg.rest)
    val (nick, text) = takeFirstWord(afterTime)
    if (nick.isEmpty()) return null

    val own = selfNick.isNotEmpty() && nick == selfNick
    val mention = !own && (mentionsNick(text, selfNick) || mentionsAll(text))
    return SayInfo(time = time, nick = nick, text = text, own = own, mention = mention)
}

/** 这一行对我而言算不算"有人叫我"（@我 或 @all，自己发的不算）。 */
fun mentionsMe(rawLine: String, selfNick: String): Boolean =
    parseSay(rawLine, selfNick)?.mention ?: false

/**
 * 解析服务器发来的提示行。
 *
 * 覆盖全部已识别的服务器消息，**包括 README 协议表里漏掉的 `ANNOUNCE` 和 `RULES`**
 * （这两个是实际存在但没写进文档的）。
 */
fun parseNotice(rawLine: String): NoticeInfo {
    val msg = parseLine(rawLine)
    if (msg.command.isEmpty()) return NoticeInfo("", "", isNotice = false)

    val (time, rest) = takeTimePrefix(msg.rest)
    return when (msg.command) {
        "WELCOME" -> NoticeInfo(time, "已连接到服务器 $rest")
        "JOINED" -> NoticeInfo(time, "$rest 加入了聊天室")
        "LEFT" -> NoticeInfo(time, "$rest 离开了聊天室")
        "NAMES" -> NoticeInfo(time, "在线成员：$rest")
        "SYS" -> NoticeInfo(time, rest)
        "LOGGEDIN" -> NoticeInfo(time, rest)
        "ERROR" -> NoticeInfo(time, rest, isError = true)
        "PONG" -> NoticeInfo(time, "服务器回应正常（PONG）")
        // 全服公告走独立命令 ANNOUNCE（不是 SAY），客户端的 /say 公告就是它
        "ANNOUNCE" -> NoticeInfo(time, rest, isAnnouncement = true)
        // 服务器下发的运行规则（单文件上限等），属于控制行，不进聊天记录
        "RULES" -> NoticeInfo(time, rest, isNotice = false)
        else -> NoticeInfo("", rawLine, isNotice = false)
    }
}

// ---------------------------------------------------------------------------
// 未读提示规则
// ---------------------------------------------------------------------------

/** 窗口不在前台时，新消息计入未读。 */
fun shouldCountUnread(windowActive: Boolean): Boolean = !windowActive

/** 只有"不在前台 + 有人叫我"才闪任务栏/提示（普通消息不打扰）。 */
fun shouldFlash(windowActive: Boolean, isMention: Boolean): Boolean = !windowActive && isMention

/** 有未读时给标题加 `【N】` 前缀。 */
fun formatUnreadTitle(base: String, unreadCount: Int): String =
    if (unreadCount <= 0) base else "【$unreadCount】$base"
