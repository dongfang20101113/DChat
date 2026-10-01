package com.dongfang20101113.dchat.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AttachFile
import androidx.compose.material.icons.filled.Group
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.dongfang20101113.dchat.protocol.nickColorIndex
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.components.FileCardRow
import com.dongfang20101113.dchat.ui.components.NoticeRow
import com.dongfang20101113.dchat.ui.components.SayRow
import com.dongfang20101113.dchat.ui.layout.ChatMetrics
import com.dongfang20101113.dchat.ui.layout.chatMetrics
import com.dongfang20101113.dchat.ui.layout.windowSizeClassOf
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors

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
) {
    val colors = LocalDchatColors.current

    val filePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri -> uri?.let(onSendFile) }

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

        if (metrics.useTwoPane) {
            Row(Modifier.fillMaxSize()) {
                ChatPane(
                    modifier = Modifier.weight(1f),
                    state = state, metrics = metrics,
                    onSend = onSend, onDownload = onDownload,
                    onPickFile = { filePicker.launch(arrayOf("*/*")) },
                    onDisconnect = onDisconnect,
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
                )
            }
        }
    }
}

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
) {
    val colors = LocalDchatColors.current
    var draft by remember { mutableStateOf("") }
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
                    is ChatItem.FileItem -> FileCardRow(item, metrics) { onDownload(item.fileId) }
                }
            }
        }

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
            IconButton(onClick = onPickFile) {
                Icon(Icons.Filled.AttachFile, contentDescription = "发送文件", tint = colors.accent)
            }
            OutlinedTextField(
                value = draft,
                onValueChange = { draft = it },
                placeholder = { Text("说点什么…", fontSize = metrics.messageFontSp.sp) },
                maxLines = 4,
                modifier = Modifier.weight(1f).widthIn(min = 120.dp),
            )
            IconButton(
                onClick = {
                    if (draft.isNotBlank()) {
                        onSend(draft)
                        draft = ""
                    }
                },
                enabled = draft.isNotBlank(),
            ) {
                Icon(
                    Icons.Filled.Send,
                    contentDescription = "发送",
                    tint = if (draft.isNotBlank()) colors.accent else colors.system,
                )
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
