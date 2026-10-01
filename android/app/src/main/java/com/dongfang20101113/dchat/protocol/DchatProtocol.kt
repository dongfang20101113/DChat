package com.dongfang20101113.dchat.protocol

/**
 * dchat 线协议的核心：一行一条消息、UTF-8、`\n` 结尾、单行上限 4096 字节。
 *
 * 这一层**不依赖任何 Android API**，和 C++ 端的 `protocol.cpp` 一样可以脱离界面做单元测试。
 * 移植时严格对齐 C++ 语义（包括 ASCII 大小写、按 UTF-8 码点计数等细节），
 * 避免"看起来一样、边界行为不同"的隐蔽 bug。
 */

/** 服务器默认端口。 */
const val DEFAULT_PORT: Int = 5555

/** 单行字节上限（协议硬约束）。 */
const val MAX_LINE_BYTES: Int = 4096

/** 昵称上限，按 Unicode 码点算（不是字节、也不是 UTF-16 长度）。 */
const val MAX_NICK_CHARS: Int = 12

/** 解析后的一行协议消息。 */
data class Message(
    /** 已转大写；空串表示空行或无效行。 */
    val command: String,
    /** 命令之后的原始文本（首尾空白已去掉）。 */
    val rest: String,
) {
    val isEmpty: Boolean get() = command.isEmpty()

    /** rest 按空白（空格/制表符）切分。 */
    fun words(): List<String> = splitWords(rest)
}

/** 昵称不合法的原因。 */
enum class NickError {
    NONE,
    EMPTY,
    TOO_LONG,
    ILLEGAL_CHAR,
    TAKEN,
}

/** 昵称错误的用户可见文案（与 C++ 端一致）。 */
fun NickError.text(): String = when (this) {
    NickError.NONE -> ""
    NickError.EMPTY -> "昵称不能为空"
    NickError.TOO_LONG -> "昵称太长（最多 12 个字符）"
    NickError.ILLEGAL_CHAR -> "昵称不能包含空格、逗号、冒号、斜杠等字符"
    NickError.TAKEN -> "昵称已被占用"
}

// ---------------------------------------------------------------------------
// 基础字符判定
// ---------------------------------------------------------------------------

/** 协议里的"空白"只有空格和制表符（不含全角空格）。 */
internal fun isProtoSpace(c: Char): Boolean = c == ' ' || c == '\t'

/**
 * 去掉首尾空白。起始只去空格/制表符，结尾额外去掉 `\r` `\n`——和 C++ 的 `Trim` 一致。
 */
internal fun protoTrim(text: String): String {
    var begin = 0
    var end = text.length
    while (begin < end && isProtoSpace(text[begin])) begin++
    while (end > begin) {
        val c = text[end - 1]
        if (!isProtoSpace(c) && c != '\r' && c != '\n') break
        end--
    }
    return text.substring(begin, end)
}

/** 只对 ASCII 字母做大写转换（非 ASCII 原样保留）。 */
internal fun toUpperAscii(text: String): String = buildString(text.length) {
    for (c in text) append(if (c in 'a'..'z') c - 32 else c)
}

/** 只对 ASCII 字母做小写转换。 */
internal fun toLowerAscii(text: String): String = buildString(text.length) {
    for (c in text) append(if (c in 'A'..'Z') c + 32 else c)
}

internal fun isAsciiLetter(c: Char): Boolean = (c in 'A'..'Z') || (c in 'a'..'z')

/**
 * 命令名允许的字符：字母、数字、下划线。
 *
 * 下划线必须允许，否则 `FILE_SEND` / `FILE_DATA` 会被判为非法命令，
 * [buildLine] 返回空串，转发出去就变成一个空行（C++ 端专门注释过这个坑）。
 */
internal fun isCommandChar(c: Char): Boolean = isAsciiLetter(c) || (c in '0'..'9') || c == '_'

/**
 * 昵称里不允许出现的字符：控制字符，以及空格、制表符、逗号、冒号、斜杠、尖括号、引号、竖线、反斜杠。
 *
 * 非 ASCII 字符（中文、emoji）一律允许。
 */
internal fun isNickChar(c: Char): Boolean {
    if (c.code < 0x20 || c.code == 0x7F) return false
    return when (c) {
        ' ', '\t', ',', ':', '/', '<', '>', '"', '\'', '|', '\\' -> false
        else -> true
    }
}

/** 按空白切词（跳过连续空白，不产生空词）。 */
internal fun splitWords(text: String): List<String> {
    val out = mutableListOf<String>()
    var i = 0
    while (i < text.length) {
        while (i < text.length && isProtoSpace(text[i])) i++
        val begin = i
        while (i < text.length && !isProtoSpace(text[i])) i++
        if (i > begin) out.add(text.substring(begin, i))
    }
    return out
}

// ---------------------------------------------------------------------------
// UTF-8 码点工具
// ---------------------------------------------------------------------------

/**
 * Unicode 码点数量。
 *
 * C++ 端按"非 UTF-8 续字节"计数；Kotlin 的 `codePointCount` 结果等价，
 * 且对代理对（emoji）同样算 1 个码点。
 */
fun utf8CharCount(text: String): Int = text.codePointCount(0, text.length)

/**
 * 按码点截断（不会把一个字符切一半）。
 *
 * C++ 端按字节找边界；这里用码点边界，语义一致且更安全。
 * 注意：只保证"不切坏字符"，不保证结果不超过某个字节数。
 */
fun utf8Truncate(text: String, maxChars: Int): String {
    if (maxChars <= 0) return ""
    val total = text.codePointCount(0, text.length)
    if (total <= maxChars) return text
    val end = text.offsetByCodePoints(0, maxChars)
    return text.substring(0, end)
}

/** 字符串的 UTF-8 字节长度（协议按字节限长时要用）。 */
fun utf8ByteLength(text: String): Int = text.toByteArray(Charsets.UTF_8).size

/** 按 UTF-8 字节数截断，且不会把一个字符切一半。 */
fun utf8TruncateBytes(text: String, maxBytes: Int): String {
    val bytes = text.toByteArray(Charsets.UTF_8)
    if (bytes.size <= maxBytes) return text
    var cut = maxBytes
    // UTF-8 续字节形如 10xxxxxx，往前退到字符起点
    while (cut > 0 && (bytes[cut].toInt() and 0xC0) == 0x80) cut--
    return String(bytes, 0, cut, Charsets.UTF_8)
}

// ---------------------------------------------------------------------------
// 时间字段（协议里的 hh:mm，24 小时制）
// ---------------------------------------------------------------------------

/** 把时/分归一化成 `hh:mm`（负数、超过 24 小时都会绕回）。 */
fun formatTime(hour: Int, minute: Int): String {
    var h = hour % 24
    if (h < 0) h += 24
    var m = minute % 60
    if (m < 0) m += 60
    // 固定 Locale.US：阿拉伯语等地区默认会用本地数字，时间戳就会变成非 ASCII
    return String.format(java.util.Locale.US, "%02d:%02d", h, m)
}

/** 是否是严格的 `hh:mm`（必须 5 个字符、第 3 位是冒号、时分在合法范围内）。 */
fun looksLikeTime(text: String): Boolean {
    if (text.length != 5 || text[2] != ':') return false
    for (index in intArrayOf(0, 1, 3, 4)) {
        val c = text[index]
        if (c < '0' || c > '9') return false
    }
    val hour = (text[0] - '0') * 10 + (text[1] - '0')
    val minute = (text[3] - '0') * 10 + (text[4] - '0')
    return hour < 24 && minute < 60
}

// ---------------------------------------------------------------------------
// 行的解析与构造
// ---------------------------------------------------------------------------

/** 解析一行（不含结尾换行符）。空行/纯空白返回 [Message] 的 command 为空串。 */
fun parseLine(line: String): Message {
    val trimmed = protoTrim(line)
    if (trimmed.isEmpty()) return Message("", "")

    var split = 0
    while (split < trimmed.length && !isProtoSpace(trimmed[split])) split++
    return Message(
        command = toUpperAscii(trimmed.substring(0, split)),
        rest = protoTrim(trimmed.substring(split)),
    )
}

/**
 * 组装一行：命令转大写、剥掉 `\r`/`\n`（防协议注入）、超长截断。
 *
 * 命令名非法（空、或含非 [isCommandChar] 字符）时返回**空串**——
 * 调用方必须把空串当作"不要发送"，否则会在协议里插入一个空行。
 */
fun buildLine(command: String, rest: String = ""): String {
    if (command.isEmpty()) return ""
    for (c in command) if (!isCommandChar(c)) return ""

    val safe = buildString(rest.length) {
        for (c in rest) if (c != '\r' && c != '\n') append(c)
    }

    val line = if (safe.isEmpty()) toUpperAscii(command) else "${toUpperAscii(command)} $safe"
    // 按 UTF-8 字节截断到上限（C++ 端按 std::string 字节 resize）
    return utf8TruncateBytes(line, MAX_LINE_BYTES)
}

// ---------------------------------------------------------------------------
// 昵称
// ---------------------------------------------------------------------------

/** 校验并规范化昵称；不合法时返回空串并把原因写入 [error]。 */
fun normalizeNick(raw: String): Pair<String, NickError> {
    val nick = protoTrim(raw)
    if (nick.isEmpty()) return "" to NickError.EMPTY
    if (utf8CharCount(nick) > MAX_NICK_CHARS) return "" to NickError.TOO_LONG
    for (c in nick) if (!isNickChar(c)) return "" to NickError.ILLEGAL_CHAR
    return nick to NickError.NONE
}

// ---------------------------------------------------------------------------
// 常用消息构造
// ---------------------------------------------------------------------------

fun makeRegister(nick: String, password: String): String = buildLine("REGISTER", "$nick $password")
fun makeLogin(nick: String, password: String): String = buildLine("LOGIN", "$nick $password")
fun makeMessage(text: String): String = buildLine("MSG", text)
fun makeList(): String = buildLine("LIST")
fun makePing(): String = buildLine("PING")
fun makeQuit(): String = buildLine("QUIT")
fun makeFileSend(transferId: String, nameBase64: String, size: Long): String =
    buildLine("FILE_SEND", "$transferId $nameBase64 $size")
fun makeFileChunk(transferId: String, dataBase64: String): String =
    buildLine("FILE_CHUNK", "$transferId $dataBase64")
fun makeFileEnd(transferId: String): String = buildLine("FILE_END", transferId)
fun makeFileCancel(transferId: String): String = buildLine("FILE_CANCEL", transferId)
fun makeFileGet(fileId: String): String = buildLine("FILE_GET", fileId)
fun makeFileThumb(transferId: String, dataBase64: String): String =
    buildLine("FILE_THUMB", "$transferId $dataBase64")
fun makeFileThumbGet(fileId: String): String = buildLine("FILE_THUMB_GET", fileId)
