package com.dongfang20101113.dchat.ui

import com.dongfang20101113.dchat.protocol.AttachmentKind
import com.dongfang20101113.dchat.protocol.NoticeInfo
import com.dongfang20101113.dchat.protocol.SayInfo
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.TrustDecision
import com.dongfang20101113.dchat.protocol.countTextLines
import com.dongfang20101113.dchat.protocol.formatBytes
import com.dongfang20101113.dchat.protocol.unescapeText
import com.dongfang20101113.dchat.protocol.utf8CharCount

/** 聊天记录里的一条。 */
sealed interface ChatItem {
    val key: Long

    /** 一条聊天气泡。 */
    data class SayItem(override val key: Long, val info: SayInfo) : ChatItem

    /** 一条系统提示 / 错误 / 公告。 */
    data class NoticeItem(override val key: Long, val info: NoticeInfo) : ChatItem

    /** 一张文件卡片（QQ 式：点了才下载）。 */
    data class FileItem(
        override val key: Long,
        val fileId: String,
        val fromNick: String,
        val fileName: String,
        val size: Long,
        val hasThumbnail: Boolean,
        val state: FileState = FileState.OFFERED,
        val progress: Int = 0,
        val savedPath: String? = null,
        val note: String? = null,
        /**
         * 这是不是一张**贴纸**。
         *
         * 保留它是为了兼容已有代码；真相应以 [kind] 为准。
         */
        val isSticker: Boolean = false,
        /**
         * 附件种类：普通文件 / 贴纸 / 语音消息。
         *
         * 三种附件走**完全相同**的传输通道，区别只在渲染方式和要不要自动下载。
         */
        val kind: AttachmentKind = AttachmentKind.FILE,
    ) : ChatItem {
        /** 卡片上的按钮文案，和桌面端一致：下载 → 下载中 N% → 打开文件夹 / 重试。 */
        val buttonLabel: String
            get() = when (state) {
                FileState.OFFERED -> "下载"
                FileState.DOWNLOADING -> "下载中 $progress%"
                FileState.DONE -> "打开"
                FileState.FAILED -> "重试"
            }

        val sizeText: String get() = formatBytes(size)

        /**
         * 这条附件是不是**自己刚发出去的**（界面据此决定靠右还是靠左）。
         *
         * 判断依据是"上传成功那一刻的本地回显"：发出文件时服务端登记的昵称
         * 就是发送者昵称，所以收到的 `FILE_OFFER` 里昵称等于自己即为自己发的。
         * 这条规则必须和 [SayItem] 用同一套（见 `ChatStateReducerTest`），
         * 否则会出现"消息靠右、自己发的语音靠左"这种自相矛盾的界面。
         */
        fun isOwn(selfNick: String): Boolean = selfNick.isNotEmpty() && fromNick == selfNick

        /**
         * 该不该自动开始下载？
         *
         * **贴纸和语音才自动下。普通文件（可能是几百 MB 的视频）绝不能自动下**——
         * 那会偷偷吃掉用户的流量，而且他可能根本不想要。
         * 这条边界的后果不是"功能不好用"，而是用户的流量账单。
         */
        val shouldAutoDownload: Boolean
            get() = kind != AttachmentKind.FILE && state == FileState.OFFERED
    }
}

/** 文件卡片状态。 */
enum class FileState { OFFERED, DOWNLOADING, DONE, FAILED }

/** 连接与登录阶段。 */
enum class Stage { CONNECT, AUTH, CHAT }

/**
 * 界面状态。
 *
 * 这是一个**不可变数据类**，所有的状态迁移都由 [reduce] 这个纯函数完成——
 * 于是"收到某条协议消息后界面应该变成什么样"可以脱离 Android 做单元测试。
 */
data class ChatState(
    val stage: Stage = Stage.CONNECT,

    // ---- 连接 ----
    val host: String = "127.0.0.1",
    val port: Int = 5555,
    val connected: Boolean = false,
    val connecting: Boolean = false,
    val connectionError: String? = null,

    /**
     * 服务器身份的可信状态（TOFU）。
     *
     * 默认是 [TrustDecision.NotEncrypted]：还没连上、或者连上了但没加密。
     * 握手成功后会被替换成 首次连接 / 一致 / **变了**。
     * 界面必须把 [TrustDecision.Changed] 显眼地展示出来——悄悄接受新指纹
     * 等于 TOFU 完全没做。
     */
    val trust: TrustDecision = TrustDecision.NotEncrypted,

    // ---- 登录 ----
    val selfNick: String = "",
    val loggingIn: Boolean = false,
    val authError: String? = null,

    // ---- 聊天 ----
    val items: List<ChatItem> = emptyList(),
    val onlineNicks: List<String> = emptyList(),
    val knownNicks: List<String> = emptyList(),
    val unread: Int = 0,
    val maxFileMb: Int = 64,

    // ---- 服务器下发的限制（全部 0 = 不限制）----
    // 老服务器只发前 3 个 RULES 字段，这 4 个会保持 0 也就是"不限制"，行为完全不变。
    /** uploadrate：本机上传限速 KB/s（真正的限速在服务端做，这里只用于界面提示）。 */
    val uploadRateKbps: Int = 0,
    /** downloadrate：本机下载限速 KB/s。 */
    val downloadRateKbps: Int = 0,
    /** maxtextlen：单条消息最大字符数（Unicode 码点）。 */
    val maxTextLength: Int = 0,
    /** maxtextlines：单条消息最大行数。 */
    val maxTextLines: Int = 0,
    /**
     * chatcolor：服务器允不允许彩色文字代码。关掉后聊天气泡里的色码**原样显示**
     * （和桌面端同一条约定：用户打进去的字不该凭空消失）。
     */
    val chatColorEnabled: Boolean = true,

    /** 取色盘是否打开（由 `/chatcolor choose` 翻起来，界面负责显示）。 */
    val showColorPicker: Boolean = false,
    /**
     * 取色盘刚选好的色码：界面把它放进输入框，放完就清掉。
     * **不直接发出去**——用户还要接着打字（和桌面端一致）。
     */
    val pendingColorCode: String? = null,

    /** 每条提示/消息产生时的本地时间（`hh:mm`），与桌面端"时间在产生那一刻固定"的做法一致。 */
    val nextKey: Long = 1L,

    // ---- 语音消息（阶段 5）----
    /**
     * 正在录音吗？
     *
     * 录音状态放在**进程级状态**里而不是界面里：录音是「按住说话」触发的，
     * 松手之前 Activity 可能已经被重建（转屏），状态丢了就会录出一个没人收尾的文件。
     */
    val recording: Boolean = false,

    /** 这次录音已经录了多久（秒）。仅用于界面上的计时。 */
    val recordingSeconds: Int = 0,

    /**
     * 正在播放的语音（`fileId`）；null 表示没有在播。
     *
     * **同一时刻只有一条语音在播**（和微信一致）：点第二条会把第一条停掉，
     * 否则两条声音叠在一起谁也听不清。
     */
    val playingVoiceId: String? = null,

    /** 正在播放的这条已经播到第几秒，用于气泡上的进度。 */
    val playingSeconds: Int = 0,

    /**
     * 正在播放的这条**总共**多少秒（播放器读出来的真实时长）。
     *
     * 协议里 `FILE_OFFER` 没有时长字段，所以没播过的语音显示 `0:00`——
     * 宁可先显示 0:00，也不拿字节数按码率估一个数出来骗人。
     */
    val playingDurationSeconds: Int = 0,
) {
    /** 聊天记录上限：和桌面端的 `kMaxItems = 400` 一致。 */
    val isFull: Boolean get() = items.size >= MAX_ITEMS

    /**
     * 这条输入能不能发出去？返回 `null` 表示可以，否则是**给用户看的原因**。
     *
     * 放在这里而不是散进界面代码有两个好处：
     * ① 界面直接用它决定「发送」按钮灰不灰、提示写什么；
     * ② 它是纯函数，能脱离界面单测。服务端也会做同样的检查，
     *    但本地先拦一道能省一次往返，体验更好。
     *
     * @param escaped 已经过 `escapeText` 的文本（发送时用的就是它）
     */
    fun whyCannotSend(escaped: String): String? {
        if (escaped.isEmpty()) return "说点什么再发吧"

        if (maxTextLength > 0) {
            // 字符数按**还原后**的文本算，而且按 Unicode 码点算
            // —— 一个 emoji 可能是两个 UTF-16 char，直接用 String.length 会算多。
            val chars = utf8CharCount(unescapeText(escaped))
            if (chars > maxTextLength) {
                return "太长了：服务器限制 $maxTextLength 字符，你这条有 $chars 字符"
            }
        }
        if (maxTextLines > 0) {
            val lines = countTextLines(escaped)
            if (lines > maxTextLines) {
                return "行数太多：服务器限制 $maxTextLines 行，你这条有 $lines 行"
            }
        }
        return null
    }

    companion object {
        const val MAX_ITEMS: Int = 400
    }
}

/**
 * 收到一条服务器消息后，状态怎么变。
 *
 * **纯函数**：相同输入必得相同输出，不碰 Android、不碰网络、不碰时间（时间由 [nowTime] 参数注入）。
 * 这样协议层的所有分支都能在 JVM 单测里跑，不用真机。
 *
 * @param nowTime 产生本地提示时用的 `hh:mm`（注入而不是内部取当前时间，便于测试）
 */
fun ChatState.reduce(line: ServerLine, nowTime: String): ChatState = when (line) {

    is ServerLine.Welcome -> addNotice(NoticeInfo(line.time, "已连接到服务器 ${line.serverName}"))

    is ServerLine.LoggedIn -> copy(
        stage = Stage.CHAT,
        selfNick = line.nick,
        loggingIn = false,
        authError = null,
    ).addNotice(NoticeInfo(line.time, "已登录：${line.nick}"))

    is ServerLine.Say -> addItem(ChatItem.SayItem(nextKey, line.info))

    is ServerLine.Announce -> addNotice(
        NoticeInfo(line.time, line.text, isAnnouncement = true),
    )

    is ServerLine.Joined -> copy(onlineNicks = (onlineNicks + line.nick).distinct())
        .addNotice(NoticeInfo(line.time, "${line.nick} 加入了聊天室"))

    is ServerLine.Left -> copy(onlineNicks = onlineNicks - line.nick)
        .addNotice(NoticeInfo(line.time, "${line.nick} 离开了聊天室"))

    // NAMES 既更新名单、也显示一条"在线成员"
    is ServerLine.Names -> copy(onlineNicks = line.nicks)
        .addNotice(NoticeInfo(line.time, "在线成员：${line.nicks.joinToString(",")}"))

    // KNOWN 只用于 Tab 补全的名单，**不进聊天记录**（和桌面端一致）
    is ServerLine.Known -> copy(knownNicks = line.nicks)

    is ServerLine.Sys -> addNotice(NoticeInfo(line.time, line.text))

    is ServerLine.Error -> copy(
        loggingIn = false,
        // 登录阶段的错误同时写到 authError，让账号窗口能显示红字
        authError = if (stage == Stage.AUTH || loggingIn) line.text else authError,
    ).addNotice(NoticeInfo(line.time, line.text, isError = true))

    // PONG 是心跳回包，不显示
    is ServerLine.Pong -> this

    // RULES 是控制行：只更新本地限制，不显示
    is ServerLine.Rules -> copy(
        maxFileMb = line.documentSizeMb,
        uploadRateKbps = line.uploadRateKbps,
        downloadRateKbps = line.downloadRateKbps,
        maxTextLength = line.maxTextLength,
        maxTextLines = line.maxTextLines,
        chatColorEnabled = line.chatColor,
    )

    is ServerLine.FileOffer -> if (items.any { it is ChatItem.FileItem && it.fileId == line.fileId }) {
        this
    } else {
        addItem(
            ChatItem.FileItem(
                key = nextKey,
                fileId = line.fileId,
                fromNick = line.nick,
                fileName = line.fileName,
                size = line.size,
                hasThumbnail = line.hasThumbnail,
                // 种类由服务端透传；老服务器不发这一格，默认 FILE，
                // 那时按普通文件卡片显示才是对的（用户点一下还能下载）。
                isSticker = line.isSticker,
                kind = line.kind,
            ),
        )
    }

    is ServerLine.FileBegin -> updateFile(line.fileId) {
        it.copy(state = FileState.DOWNLOADING, progress = 0)
    }

    is ServerLine.FileData -> updateFile(line.fileId) { card ->
        val received = (card.progress.coerceAtLeast(0) * card.size / 100) + line.data.size
        val percent = if (card.size > 0) ((received * 100) / card.size).toInt().coerceIn(0, 100) else 0
        card.copy(state = FileState.DOWNLOADING, progress = percent)
    }

    is ServerLine.FileEnd -> updateFile(line.fileId) { it.copy(state = FileState.DONE, progress = 100) }

    is ServerLine.FileFail -> updateFile(line.fileId) {
        it.copy(state = FileState.FAILED, note = line.reason.ifEmpty { "下载失败" })
    }

    // 缩略图数据/结束只影响卡片外观，这里先不做图像处理，保持状态机简单
    is ServerLine.FileThumbData, is ServerLine.FileThumbEnd -> this

    // 未知行：原样显示成提示，便于排查（桌面端也是这个取向）
    is ServerLine.Unknown -> if (line.raw.isBlank()) this
    else addNotice(NoticeInfo("", line.raw, isNotice = false))
}

/** 追加一条记录；超过上限时丢掉最旧的。 */
private fun ChatState.addItem(item: ChatItem): ChatState {
    val list = items + item
    return copy(
        items = if (list.size > ChatState.MAX_ITEMS) list.takeLast(ChatState.MAX_ITEMS) else list,
        nextKey = nextKey + 1,
    )
}

private fun ChatState.addNotice(info: NoticeInfo): ChatState = addItem(ChatItem.NoticeItem(nextKey, info))

/** 改一张文件卡片的状态。 */
private fun ChatState.updateFile(fileId: String, transform: (ChatItem.FileItem) -> ChatItem.FileItem): ChatState {
    var changed = false
    val list = items.map { item ->
        if (item is ChatItem.FileItem && item.fileId == fileId) {
            changed = true
            transform(item)
        } else {
            item
        }
    }
    return if (changed) copy(items = list) else this
}
