package com.dongfang20101113.dchat.ui.screens

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AttachFile
import androidx.compose.material.icons.filled.EmojiEmotions
import androidx.compose.material.icons.filled.Group
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Send
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.dongfang20101113.dchat.protocol.AttachmentKind
import com.dongfang20101113.dchat.protocol.EmojiPalette
import com.dongfang20101113.dchat.protocol.VoiceMessage
import com.dongfang20101113.dchat.protocol.countTextLines
import com.dongfang20101113.dchat.protocol.escapeText
import com.dongfang20101113.dchat.protocol.nickColorIndex
import com.dongfang20101113.dchat.protocol.unescapeText
import com.dongfang20101113.dchat.protocol.utf8CharCount
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.FileState
import com.dongfang20101113.dchat.ui.components.FileCardRow
import com.dongfang20101113.dchat.ui.components.StickerRow
import com.dongfang20101113.dchat.ui.components.NoticeRow
import com.dongfang20101113.dchat.ui.components.SayRow
import com.dongfang20101113.dchat.ui.components.VoiceBubbleRow
import com.dongfang20101113.dchat.ui.layout.ChatMetrics
import com.dongfang20101113.dchat.ui.layout.chatMetrics
import com.dongfang20101113.dchat.ui.layout.windowSizeClassOf
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors
import com.dongfang20101113.dchat.voice.VoiceRecord
import com.dongfang20101113.dchat.voice.bubbleDurationSeconds

/**
 * 主聊天界面。
 *
 * ## 屏幕适配的几处关键处理
 *
 * 1. **尺寸全部来自 [chatMetrics]**：用 [BoxWithConstraints] 拿到**扣掉状态栏/刘海之后**的真实可用宽高，
 *    再算出气泡上限、内边距、字号。没有任何写死的像素。
 * 2. **分栏随宽度切换**：平板（≥840dp）左右分栏显示成员列表；手机上改为可开合的浮层，
 *    不挤占聊天的宽度。
 * 3. **键盘顶起**：整页 [imePadding]，输入框永远贴在键盘上方，消息列表自动缩短并滚到底；
 *    同时用 [navigationBarsPadding] 避开手势条。
 * 4. **刘海/挖孔**：根节点 [safeDrawingPadding]，内容不会钻到摄像头下面。
 */
@Composable
fun ChatScreen(
    state: ChatState,
    onSend: (String) -> Unit,
    onDownload: (String) -> Unit,
    onSendFile: (android.net.Uri) -> Unit,
    onMarkRead: () -> Unit,
    onDisconnect: () -> Unit,
    /** 点一条语音气泡：播 / 暂停 / 接着播（取舍在会话层）。 */
    onVoiceToggle: (String, String?) -> Unit = { _, _ -> },
    /** 「按住说话」按下。 */
    onStartVoice: () -> Unit = {},
    /** 「按住说话」松手：太短丢掉，够长发出去。 */
    onStopVoice: () -> Unit = {},
    /** 录音中按「取消」：删掉文件、不发。 */
    onCancelVoice: () -> Unit = {},
    /** 输入框的初始内容。**只为截图测试而存在**，正常运行时是空的。 */
    initialDraft: String = "",
    /** 假装"正在录音 N 秒"。同样**只为截图与手势测试而存在**。 */
    initialRecordingSeconds: Int? = null,
    /** 用户核对后接受服务器的新指纹（TOFU 警告条上的按钮）。 */
    onAcceptFingerprint: () -> Unit = {},
) {
    val colors = LocalDchatColors.current

    val filePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri -> uri?.let(onSendFile) }

    // 录音权限：**只在用户第一次按麦克风时申请**，不在启动时弹——
    // 一进 App 就要麦克风权限，用户十有八九直接拒绝。
    val context = LocalContext.current
    val micPermission = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission(),
    ) { granted ->
        // 授权后**接着开始录**：用户已经按住了按钮，还要他再按一次很奇怪
        if (granted) onStartVoice()
    }

    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .background(colors.windowBg)
            // 先避开状态栏/刘海/手势条，这样下面拿到的 maxWidth/maxHeight 就是真实可用空间
            .safeDrawingPadding()
            .imePadding(),
    ) {
        val widthDp = maxWidth.value.toInt()
        val heightDp = maxHeight.value.toInt()
        val fontScale = androidx.compose.ui.platform.LocalDensity.current.fontScale
        val metrics = remember(widthDp, heightDp, fontScale) {
            chatMetrics(widthDp, heightDp, fontScale)
        }

        // 进入聊天即清未读
        LaunchedEffect(Unit) { onMarkRead() }

        val voice = VoiceUi(
            onToggle = onVoiceToggle,
            onPressMic = {
                val granted = ContextCompat.checkSelfPermission(
                    context, Manifest.permission.RECORD_AUDIO,
                ) == PackageManager.PERMISSION_GRANTED
                if (granted) onStartVoice() else micPermission.launch(Manifest.permission.RECORD_AUDIO)
            },
            onReleaseMic = onStopVoice,
            onCancelMic = onCancelVoice,
        )

        if (metrics.useTwoPane) {
            Row(Modifier.fillMaxSize()) {
                ChatPane(
                    modifier = Modifier.weight(1f),
                    state = state, metrics = metrics,
                    onSend = onSend, onDownload = onDownload,
                    onPickFile = { filePicker.launch(arrayOf("*/*")) },
                    onDisconnect = onDisconnect,
                    initialDraft = initialDraft,
                    initialRecordingSeconds = initialRecordingSeconds,
                    voice = voice,
                    onAcceptFingerprint = onAcceptFingerprint,
                )
                MemberPane(
                    modifier = Modifier.width(metrics.memberPaneWidthDp!!.dp).fillMaxHeight(),
                    state = state, metrics = metrics,
                )
            }
        } else {
            Box(Modifier.fillMaxSize()) {
                ChatPane(
                    modifier = Modifier.fillMaxSize(),
                    state = state, metrics = metrics,
                    onSend = onSend, onDownload = onDownload,
                    onPickFile = { filePicker.launch(arrayOf("*/*")) },
                    onDisconnect = onDisconnect,
                    initialDraft = initialDraft,
                    initialRecordingSeconds = initialRecordingSeconds,
                    voice = voice,
                    onAcceptFingerprint = onAcceptFingerprint,
                )
            }
        }
    }
}

/** 语音相关的回调打包在一起，免得 [ChatPane] 的参数列表越拉越长。 */
private data class VoiceUi(
    val onToggle: (String, String?) -> Unit,
    val onPressMic: () -> Unit,
    val onReleaseMic: () -> Unit,
    val onCancelMic: () -> Unit,
)

/** 聊天区：顶栏 + 消息列表 + 输入栏。 */
@Composable
private fun ChatPane(
    modifier: Modifier,
    state: ChatState,
    metrics: ChatMetrics,
    onSend: (String) -> Unit,
    onDownload: (String) -> Unit,
    onPickFile: () -> Unit,
    onDisconnect: () -> Unit,
    voice: VoiceUi,
    initialDraft: String = "",
    initialRecordingSeconds: Int? = null,
    onAcceptFingerprint: () -> Unit = {},
) {
    val colors = LocalDchatColors.current
    var draft by remember { mutableStateOf(initialDraft) }
    var showMembers by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()

    // 新消息到达时自动滚到底（只在底部跟随，不打断往上翻历史）
    LaunchedEffect(state.items.size) {
        if (state.items.isNotEmpty()) {
            listState.animateScrollToItem(state.items.lastIndex)
        }
    }

    Column(modifier = modifier) {
        // ---- 顶栏 ----
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(colors.neutral)
                .padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    text = state.selfNick.ifEmpty { "未登录" },
                    color = colors.text,
                    fontSize = metrics.messageFontSp.sp,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    text = "在线 ${state.onlineNicks.size} 人 · ${state.host}",
                    color = colors.system,
                    fontSize = metrics.noticeFontSp.sp,
                )
            }
            if (!metrics.useTwoPane) {
                IconButton(onClick = { showMembers = !showMembers }) {
                    Icon(Icons.Filled.Group, contentDescription = "成员列表", tint = colors.accent)
                }
            }
            Text(
                text = "断开",
                color = colors.error,
                fontSize = metrics.noticeFontSp.sp,
                modifier = Modifier
                    .clickable(onClick = onDisconnect)
                    .padding(horizontal = 8.dp, vertical = 8.dp),
            )
        }

        // ---- 成员浮层（手机；平板走右侧分栏）----
        if (showMembers && !metrics.useTwoPane) {
            MemberPane(
                modifier = Modifier.fillMaxWidth().height(180.dp),
                state = state, metrics = metrics,
            )
        }

        // ---- 消息列表 ----
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f).fillMaxWidth(),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(
                horizontal = metrics.horizontalMarginDp.dp,
                vertical = metrics.rowGapDp.dp,
            ),
        ) {
            items(state.items, key = { it.key }) { item ->
                when (item) {
                    is ChatItem.SayItem -> SayRow(item, metrics)
                    is ChatItem.NoticeItem -> NoticeRow(item, metrics)
                    is ChatItem.FileItem ->
                        // 三种附件走**同一个传输通道**，区别只在这里：
                        // 语音是能点响的气泡、贴纸是内联大图、普通文件是"点一下才下载"的卡片。
                        when {
                            item.kind == AttachmentKind.VOICE -> {
                                val mine = item.isOwn(state.selfNick)
                                val playingHere = state.playingVoiceId == item.fileId
                                VoiceBubbleRow(
                                    item = item,
                                    metrics = metrics,
                                    own = mine,
                                    playing = playingHere,
                                    playedSeconds = if (playingHere) state.playingSeconds else 0,
                                    durationSeconds = state.playingDurationSeconds,
                                    // 没在播时只显示已知的时长（0:00 表示还不知道）
                                    durationText = bubbleDurationSeconds(
                                        if (playingHere) state.playingSeconds else 0,
                                        if (playingHere) state.playingDurationSeconds else 0,
                                    ),
                                    onToggle = { voice.onToggle(item.fileId, item.savedPath) },
                                )
                            }

                            item.isSticker -> StickerRow(item, metrics) { onDownload(item.fileId) }
                            else -> FileCardRow(item, metrics) { onDownload(item.fileId) }
                        }
                }
            }
        }

        // ---- 限制提示条（服务器设了 maxtextlen / maxtextlines 才显示）----
        // 放在输入框**上方**，这样打字时余光就能看到还差多少，不用等发出去被拒才知道
        if (state.maxTextLength > 0 || state.maxTextLines > 0) {
            val escapedDraft = remember(draft) { escapeText(draft) }
            val blocked = state.whyCannotSend(escapedDraft)
            val usedChars = utf8CharCount(unescapeText(escapedDraft))
            val usedLines = countTextLines(escapedDraft)
            val overLimit = blocked != null && draft.isNotEmpty()

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(if (overLimit) colors.error.copy(alpha = 0.15f) else colors.neutral)
                    .padding(horizontal = 12.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = if (overLimit) blocked!! else "还可以输入…",
                    color = if (overLimit) colors.error else colors.system,
                    fontSize = metrics.noticeFontSp.sp,
                    modifier = Modifier.weight(1f),
                )
                if (state.maxTextLength > 0) {
                    Text(
                        text = "$usedChars/${state.maxTextLength} 字符",
                        color = if (usedChars > state.maxTextLength) colors.error else colors.system,
                        fontSize = metrics.noticeFontSp.sp,
                    )
                }
                if (state.maxTextLines > 0) {
                    Text(
                        text = "  $usedLines/${state.maxTextLines} 行",
                        color = if (usedLines > state.maxTextLines) colors.error else colors.system,
                        fontSize = metrics.noticeFontSp.sp,
                    )
                }
            }
        }

        // emoji 选择器的开关。
        // 刻意声明在**条件块外面**：写在 `if (showEmoji)` 里面的话，
        // 每次切换都会丢掉 remember 的状态，开关会自己弹回去。
        var showEmoji by remember { mutableStateOf(false) }

        // ---- emoji 选择器 ----
        // emoji 就是普通 Unicode 字符，直接插进文本即可——服务端的
        // maxtextlen 按 Unicode 码点计数，一个 emoji 算 1 个（见 EmojiPaletteTest）。
        if (showEmoji) {
            LazyRow(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(colors.bubbleOther)
                    .padding(horizontal = 8.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                items(EmojiPalette.all, key = { it }) { emoji ->
                    Text(
                        text = emoji,
                        fontSize = (metrics.messageFontSp + 8).sp,
                        modifier = Modifier
                            .clickable {
                                // 插到末尾。TextField 的光标位置要改用 TextFieldValue
                                // 才能拿到，那样改动面更大；先按"追加"来，
                                // 对聊天输入这个场景够用，也不会让用户困惑。
                                val (next, _) = EmojiPalette.insert(draft, emoji, draft.length)
                                draft = next
                            }
                            .padding(horizontal = 6.dp, vertical = 4.dp),
                    )
                }
            }
        }

        // ---- 录音中：把整条输入栏换成"松开发送 / 取消" ----
        //
        // 换成整条而不是在输入框旁边加个小提示，是因为**松手这个动作必须有个明确的地方接**：
        // 用户按住麦克风之后，视线和手指都在底部这一条上。
        // 录音中：`state.recording` 是会话给的真状态，[initialRecordingSeconds]
        // 只在截图/手势测试里用来把这一屏造出来（正常运行时它是 null）。
        val recording = state.recording || initialRecordingSeconds != null
        val recordingSeconds = if (state.recording) state.recordingSeconds else initialRecordingSeconds

        if (recording) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(colors.neutral)
                    .navigationBarsPadding()
                    .padding(horizontal = 12.dp, vertical = 10.dp)
                    .pointerInput(Unit) {
                        // 松手就发。这一条**接的是按住麦克风那一次的抬手**：
                        // 手指还按着的时候，录音条才刚出现，所以这里等的是这一轮手势
                        // 结束（抬手或取消），而不是"有人点了这一条"。
                        awaitEachGesture {
                            awaitFirstDown(requireUnconsumed = false)
                            waitForUpOrCancellation()
                            voice.onReleaseMic()
                        }
                    },
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                // 红点 + 计时：一眼看出"正在录"
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .background(colors.error, RoundedCornerShape(5.dp)),
                )
                Text(
                    text = "录音中 ${VoiceRecord.displaySeconds(recordingSeconds ?: 0)}",
                    color = colors.text,
                    fontSize = metrics.messageFontSp.sp,
                )
                Text(
                    text = "最长 ${VoiceRecord.displaySeconds(VoiceMessage.MAX_DURATION_SECONDS)}",
                    color = colors.system,
                    fontSize = metrics.noticeFontSp.sp,
                )
                Box(Modifier.weight(1f))
                Box(
                    modifier = Modifier
                        .background(colors.neutralBorder, RoundedCornerShape(8.dp))
                        .clickable { voice.onCancelMic() }
                        .padding(horizontal = 14.dp, vertical = 10.dp),
                ) {
                    Text("取消", color = colors.text, fontSize = metrics.messageFontSp.sp)
                }
                Box(
                    modifier = Modifier
                        .background(colors.accent, RoundedCornerShape(8.dp))
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                ) {
                    Text(
                        "松开发送",
                        color = colors.accentText,
                        fontSize = metrics.messageFontSp.sp,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }
        } else {
            // ---- 输入栏 ----
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(colors.neutral)
                    .navigationBarsPadding()
                    .padding(horizontal = 8.dp, vertical = 8.dp),
                verticalAlignment = Alignment.Bottom,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                IconButton(onClick = { showEmoji = !showEmoji }) {
                    Icon(
                        Icons.Filled.EmojiEmotions,
                        contentDescription = if (showEmoji) "收起表情" else "表情",
                        tint = if (showEmoji) colors.accent else colors.system,
                    )
                }
                IconButton(onClick = onPickFile) {
                    Icon(Icons.Filled.AttachFile, contentDescription = "发送文件", tint = colors.accent)
                }
                // 麦克风：按住就录、松手就发。
                //
                // 刻意**不用 `clickable`**：这里要的是"按下"和"抬手"两个时刻，
                // 一次点击表达不了"按住 3 秒"。
                //
                // `awaitFirstDown()` 默认走 Initial 阶段并要求事件未被消费，
                // 比自己去读 `awaitPointerEvent()` 再判断 `changedToDown()` 稳：
                // 后者在事件已被上层消费时**永远等不到**（表现为"按了没反应"）。
                //
                // 语义层另外挂一个 `onClick`：TalkBack 用户没法"按住"，
                // 点一下开始 / 再点一下结束是这类按钮的标准兜底。
                Box(
                    modifier = Modifier
                        .size(48.dp)                    // 不小于 48dp 的触控目标
                        .semantics {
                            contentDescription = "按住说话"
                            onClick {
                                if (recording) voice.onReleaseMic() else voice.onPressMic()
                                true
                            }
                        }
                        .pointerInput(state.recording) {
                            if (state.recording) return@pointerInput
                            awaitEachGesture {
                                // ① 按下 → 开始录
                                awaitFirstDown()
                                voice.onPressMic()
                                // ② 抬手（或手势被取消）→ 结束。**中途手指滑出去也算**，
                                //    按住说话时经常一边说一边把手指挪开一点。
                                waitForUpOrCancellation()
                                voice.onReleaseMic()
                            }
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        Icons.Filled.Mic,
                        contentDescription = null,      // 语义已经挂在外层 Box 上，这里不重复朗读
                        tint = colors.accent,
                    )
                }
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = it },
                    // 支持多行：换行会被转义后发出去，服务端和对方都能正确还原
                    placeholder = { Text("说点什么…（可以换行）", fontSize = metrics.messageFontSp.sp) },
                    maxLines = 6,
                    modifier = Modifier.weight(1f).widthIn(min = 120.dp),
                )
                IconButton(
                    onClick = {
                        // 本地先按服务器的限制拦一道，省一次"发出去被拒"的往返
                        if (state.whyCannotSend(escapeText(draft)) == null) {
                            onSend(draft)
                            draft = ""
                        }
                    },
                    enabled = state.whyCannotSend(escapeText(draft)) == null,
                ) {
                    Icon(
                        Icons.Filled.Send,
                        contentDescription = "发送",
                        tint = if (state.whyCannotSend(escapeText(draft)) == null) colors.accent else colors.system,
                    )
                }
            }
        }
    }
}

/** 成员列表（平板是右侧分栏，手机是可展开浮层）。 */
@Composable
private fun MemberPane(modifier: Modifier, state: ChatState, metrics: ChatMetrics) {
    val colors = LocalDchatColors.current
    Column(
        modifier = modifier
            .background(colors.bubbleOther)
            .padding(12.dp),
    ) {
        Text(
            text = "在线成员（${state.onlineNicks.size}）",
            color = colors.text,
            fontSize = metrics.messageFontSp.sp,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        LazyColumn {
            items(state.onlineNicks) { nick ->
                Text(
                    text = nick,
                    color = colors.nickColor(nickColorIndex(nick)),
                    fontSize = metrics.messageFontSp.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 6.dp),
                )
            }
            if (state.onlineNicks.isEmpty()) {
                item {
                    Text("（暂时没人）", color = colors.system, fontSize = metrics.noticeFontSp.sp)
                }
            }
        }
    }
}

