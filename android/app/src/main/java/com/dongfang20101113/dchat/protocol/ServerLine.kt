package com.dongfang20101113.dchat.protocol

/**
 * 服务器 → 客户端的**全部**命令，做成强类型。
 *
 * 为什么要有这一层：dchat 的协议表里实际有 **6 个命令没有写进文档**——
 * `ANNOUNCE`、`RULES`、`FILE_THUMB`、`FILE_THUMB_GET`、`FILE_THUMB_DATA`、`FILE_THUMB_END`。
 * 如果按 README 猜协议，公告会显示成未知命令、缩略图功能整个接不上。
 * 这里按**服务端源码的实际行为**建模（server.cpp / server_rules.cpp 里逐个核对过）。
 *
 * 另一个坑：`FILE_END` **双向同名**——客户端上传结束时发它，服务器下载结束时也发它，
 * 语义完全不同。所以这里分成 [ServerLine.FileEnd]（下载结束）和构造端 `makeFileEnd()`（上传结束）。
 */
sealed interface ServerLine {

    /** `WELCOME <时间> <服务器名>` */
    data class Welcome(val time: String, val serverName: String) : ServerLine

    /** `LOGGEDIN <时间> <用户名>` —— 客户端**只认这个**才认为登录成功。 */
    data class LoggedIn(val time: String, val nick: String) : ServerLine

    /** `SAY <时间> <昵称> <内容>` */
    data class Say(val info: SayInfo) : ServerLine

    /** `ANNOUNCE <时间> <内容>` —— `/say` 公告（**不是** SAY）。 */
    data class Announce(val time: String, val text: String) : ServerLine

    /** `JOINED <时间> <昵称>` / `LEFT <时间> <昵称>` */
    data class Joined(val time: String, val nick: String) : ServerLine
    data class Left(val time: String, val nick: String) : ServerLine

    /** `NAMES <时间> <昵称列表>` —— 当前在线成员。 */
    data class Names(val time: String, val nicks: List<String>) : ServerLine

    /** `KNOWN <时间> <昵称列表>` —— 服务器认识的昵称（已注册 + 管理员 + 黑名单），不进聊天记录。 */
    data class Known(val time: String, val nicks: List<String>) : ServerLine

    /** `SYS <时间> <文本>` */
    data class Sys(val time: String, val text: String) : ServerLine

    /** `ERROR <时间> <文本>` */
    data class Error(val time: String, val text: String) : ServerLine

    /** `PONG <时间>` */
    data class Pong(val time: String) : ServerLine

    /**
     * `RULES <单文件MB> <聊天间隔ms> <是否保留历史>` —— 服务器下发的运行规则。
     *
     * **不带时间戳**（和 `FILE_*` 一样属于控制行），所以解析时不能按时间字段剥前缀。
     */
    data class Rules(
        val documentSizeMb: Int,
        val chatIntervalMs: Int,
        val keepChatHistory: Boolean,
    ) : ServerLine

    /** `FILE_OFFER <时间> <昵称> <文件ID> <文件名B64> <字节数> [1]` —— 第 5 个字段为 `1` 表示带缩略图。 */
    data class FileOffer(
        val time: String,
        val nick: String,
        val fileId: String,
        val fileName: String,
        val size: Long,
        val hasThumbnail: Boolean,
    ) : ServerLine

    /** `FILE_BEGIN <文件ID> <文件名B64> <字节数>` —— 下载开始。 */
    data class FileBegin(val fileId: String, val fileName: String, val size: Long) : ServerLine

    /** `FILE_DATA <文件ID> <Base64数据>` */
    data class FileData(val fileId: String, val data: ByteArray) : ServerLine {
        // data 是数组，data class 的 equals/hashCode 需要手写
        override fun equals(other: Any?): Boolean =
            this === other || (other is FileData && fileId == other.fileId && data.contentEquals(other.data))

        override fun hashCode(): Int = 31 * fileId.hashCode() + data.contentHashCode()
    }

    /** `FILE_END <文件ID>` —— **下载**结束（上传结束是客户端发出去的同一个命令名）。 */
    data class FileEnd(val fileId: String) : ServerLine

    /** `FILE_FAIL <文件ID> <原因>` */
    data class FileFail(val fileId: String, val reason: String) : ServerLine

    /** `FILE_THUMB_DATA <文件ID> <Base64数据>` —— 缩略图分片。 */
    data class FileThumbData(val fileId: String, val data: ByteArray) : ServerLine {
        override fun equals(other: Any?): Boolean =
            this === other || (other is FileThumbData && fileId == other.fileId && data.contentEquals(other.data))

        override fun hashCode(): Int = 31 * fileId.hashCode() + data.contentHashCode()
    }

    /** `FILE_THUMB_END <文件ID>` —— 缩略图下发结束。 */
    data class FileThumbEnd(val fileId: String) : ServerLine

    /** 无法识别的行，保留原文。 */
    data class Unknown(val raw: String) : ServerLine

    companion object {
        /**
         * 把一行原始协议文本解析成强类型。
         *
         * 解析失败（字段不全、Base64 坏了）时退回 [Unknown] 并保留原文——
         * 网络数据不可信，坏行不应该让客户端崩，也不该被静默丢掉（便于排查）。
         */
        fun parse(rawLine: String, selfNick: String = ""): ServerLine {
            val msg = parseLine(rawLine)
            if (msg.isEmpty) return Unknown(rawLine)

            return when (msg.command) {
                "WELCOME" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Welcome(time, rest)
                }
                "LOGGEDIN" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    LoggedIn(time, rest)
                }
                "SAY" -> parseSay(rawLine, selfNick)?.let { Say(it) } ?: Unknown(rawLine)
                "ANNOUNCE" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Announce(time, rest)
                }
                "JOINED" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Joined(time, rest)
                }
                "LEFT" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Left(time, rest)
                }
                "NAMES" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Names(time, parseNickList(rest))
                }
                "KNOWN" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Known(time, parseNickList(rest))
                }
                "SYS" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Sys(time, rest)
                }
                "ERROR" -> {
                    val (time, rest) = takeTimePrefix(msg.rest)
                    Error(time, rest)
                }
                "PONG" -> Pong(takeTimePrefix(msg.rest).first)
                "RULES" -> parseRules(msg.rest)
                "FILE_OFFER" -> parseFileOffer(msg.rest)
                "FILE_BEGIN" -> parseFileBegin(msg.rest)
                "FILE_DATA" -> parseFileData(msg.rest)
                "FILE_END" -> FileEnd(msg.rest.trim())
                "FILE_FAIL" -> parseFileFail(msg.rest)
                "FILE_THUMB_DATA" -> parseFileThumbData(msg.rest)
                "FILE_THUMB_END" -> FileThumbEnd(msg.rest.trim())
                else -> Unknown(rawLine)
            }
        }

        /** `RULES` 行**不带时间戳**，所以不能先剥时间前缀。 */
        private fun parseRules(rest: String): ServerLine {
            val words = splitWords(rest)
            if (words.size < 3) return Unknown("RULES $rest")
            val mb = words[0].toIntOrNull() ?: return Unknown("RULES $rest")
            val interval = words[1].toIntOrNull() ?: return Unknown("RULES $rest")
            val keep = words[2] == "1" || words[2].equals("true", ignoreCase = true)
            return Rules(mb, interval, keep)
        }

        private fun parseFileOffer(rest: String): ServerLine {
            val (time, afterTime) = takeTimePrefix(rest)
            val words = splitWords(afterTime)
            if (words.size < 4) return Unknown("FILE_OFFER $rest")
            val size = words[3].toLongOrNull() ?: return Unknown("FILE_OFFER $rest")
            return FileOffer(
                time = time,
                nick = words[0],
                fileId = words[1],
                fileName = base64DecodeToString(words[2]) ?: words[2],
                size = size,
                hasThumbnail = words.size >= 5 && words[4] == "1",
            )
        }

        private fun parseFileBegin(rest: String): ServerLine {
            val words = splitWords(rest)
            if (words.size < 3) return Unknown("FILE_BEGIN $rest")
            val size = words[2].toLongOrNull() ?: return Unknown("FILE_BEGIN $rest")
            return FileBegin(words[0], base64DecodeToString(words[1]) ?: words[1], size)
        }

        private fun parseFileData(rest: String): ServerLine {
            val words = splitWords(rest)
            if (words.size < 2) return Unknown("FILE_DATA $rest")
            val data = base64Decode(words[1]) ?: return Unknown("FILE_DATA $rest")
            return FileData(words[0], data)
        }

        private fun parseFileFail(rest: String): ServerLine {
            val words = splitWords(rest)
            if (words.isEmpty()) return Unknown("FILE_FAIL $rest")
            val id = words[0]
            val reason = rest.substringAfter(id, "").trim()
            return FileFail(id, reason)
        }

        private fun parseFileThumbData(rest: String): ServerLine {
            val words = splitWords(rest)
            if (words.size < 2) return Unknown("FILE_THUMB_DATA $rest")
            val data = base64Decode(words[1]) ?: return Unknown("FILE_THUMB_DATA $rest")
            return FileThumbData(words[0], data)
        }

        /** 昵称列表：逗号分隔，过滤空项和桌面端的占位文案。 */
        private fun parseNickList(text: String): List<String> =
            text.split(',')
                .map { it.trim() }
                .filter { it.isNotEmpty() && it != "(暂时没人设置昵称)" && it != "(没有管理员)" }
    }
}

/** Base64 文件名解码；失败时返回 `null`（调用方回退用原文，保证至少能显示）。 */
private fun base64DecodeToString(text: String): String? {
    val bytes = base64Decode(text) ?: return null
    return try {
        String(bytes, Charsets.UTF_8)
    } catch (_: Exception) {
        null
    }
}
