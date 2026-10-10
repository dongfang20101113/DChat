package com.dongfang20101113.dchat.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.dongfang20101113.dchat.protocol.GroupHit
import com.dongfang20101113.dchat.protocol.JoinMode
import com.dongfang20101113.dchat.protocol.RoomKind
import com.dongfang20101113.dchat.protocol.RoomState
import com.dongfang20101113.dchat.protocol.sortRoomsForDisplay
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors

/**
 * 会话列表（私聊 / 群聊）。
 *
 * **纯展示 + 回调**：它不碰状态、也不发协议，所有动作交回给调用方。
 * 这样它能被截图测试直接喂假数据渲染，也能在不知道"会话层怎么连服务器"的情况下单测。
 *
 * 和桌面端一样，免打扰的会话**照常显示未读**，只是画成一个小圆点而不是数字：
 * 免打扰的意思是"别打扰我"，不是"有新消息也别让我知道"—— 完全不显示的话，
 * 用户会以为这个群死了，而他其实只是把它静音了。
 */
@Composable
fun RoomListPanel(
    rooms: List<RoomState>,
    currentRoomId: String,
    selfNick: String,
    modifier: Modifier = Modifier,
    onSelect: (String) -> Unit = {},
    onOpenSettings: (String) -> Unit = {},
    onNewGroup: () -> Unit = {},
) {
    val colors = LocalDchatColors.current
    val ordered = remember(rooms) { sortRoomsForDisplay(rooms) }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(colors.windowBg),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp)
                .padding(horizontal = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("会话", color = colors.text, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.weight(1f))
            Text(
                "＋ 新建 / 搜索",
                color = colors.accent,
                fontSize = 14.sp,
                modifier = Modifier
                    // 触控目标至少 48dp：这是 Material 的硬要求，
                    // 小于这个尺寸的按钮在真机上会频繁点不中
                    .heightIn(min = 48.dp)
                    .clickable { onNewGroup() }
                    .padding(horizontal = 8.dp, vertical = 14.dp),
            )
        }
        HorizontalDivider(color = colors.border)
        if (ordered.isEmpty()) {
            Text(
                "还没有会话",
                color = colors.system,
                fontSize = 14.sp,
                modifier = Modifier.padding(16.dp),
            )
            return@Column
        }
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(ordered, key = { it.id }) { room ->
                RoomListRow(
                    room = room,
                    selected = room.id == currentRoomId,
                    onClick = { onSelect(room.id) },
                    onLongClick = { onOpenSettings(room.id) },
                )
                HorizontalDivider(color = colors.border.copy(alpha = 0.4f))
            }
        }
    }
}

@Composable
private fun RoomListRow(
    room: RoomState,
    selected: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    val colors = LocalDchatColors.current
    val background = if (selected) colors.noticeBg else Color.Transparent
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .background(background)
            // 长按开设置：和桌面端的右键是同一件事。
            // 手机上没有一个"次要点击"键，长按是用户唯一能想到的入口。
            // **必须用 combinedClickable 一个修饰符**：叠两个 clickable 的话
            // 内层会抢走单击，用户点一下会直接弹出设置而不是切到那个会话。
            .combinedClickable(onClick = onClick, onLongClick = onLongClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                // 置顶用一个文字标记，不用图标：图标字体在这个位置很难保证每台机器都有
                if (room.prefs.pinned) {
                    Text("置顶 ", color = colors.accent, fontSize = 11.sp)
                }
                Text(
                    room.title,
                    color = colors.text,
                    fontSize = 16.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
            }
            Spacer(Modifier.height(2.dp))
            Text(
                roomSubtitle(room),
                color = colors.system,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (room.prefs.muted) {
            Text("静音", color = colors.system, fontSize = 11.sp)
            Spacer(Modifier.width(6.dp))
        }
        if (room.unread > 0) {
            if (room.prefs.muted) {
                // 小圆点，不显示数字（和桌面端一致）
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .clip(RoundedCornerShape(4.dp))
                        .background(if (room.unreadImportant) colors.accent else colors.system),
                )
            } else {
                Box(
                    modifier = Modifier
                        .heightIn(min = 18.dp)
                        .width(18.dp)
                        .clip(RoundedCornerShape(9.dp))
                        .background(colors.accent),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        if (room.unread > 99) "99" else room.unread.toString(),
                        color = colors.accentText,
                        fontSize = 10.sp,
                    )
                }
            }
        }
    }
}

/** 会话行的第二行小字。 */
private fun roomSubtitle(room: RoomState): String = when (room.kind) {
    RoomKind.DIRECT -> "私聊"
    RoomKind.HALL -> "公共大厅"
    RoomKind.GROUP -> buildString {
        append(room.info.memberCount)
        if (room.info.maxMembers > 0) append(" / ").append(room.info.maxMembers)
        append(" 人")
        when {
            room.info.myRole.wire == "owner" -> append("・群主")
            room.info.myRole.wire == "admin" -> append("・管理员")
        }
    }
}


// ---------------------------------------------------------------------------
// 会话设置
// ---------------------------------------------------------------------------

/**
 * 会话设置里的一行。
 *
 * 和桌面端同一个设计：**行是按会话类型现算出来的**，界面本身不懂业务。
 * 加一个设置项 = 加一行，群聊 / 私聊 / 大厅三种界面同时生效 —— 做成三套界面
 * 就是三份各自维护、迟早对不上的代码。
 */
sealed interface RoomSettingRow {
    /** 分组标题。 */
    data class Section(val title: String) : RoomSettingRow

    /** 只读的一行（会话 ID 这种不可改的）。 */
    data class ReadOnly(val label: String, val value: String, val note: String = "") :
        RoomSettingRow

    /** 一行灰字说明。 */
    data class Hint(val text: String) : RoomSettingRow

    /** 开关。`key` 非空 = 管理员设置项（走服务端）；`local` 非空 = 只改本机。 */
    data class Toggle(
        val label: String,
        val detail: String,
        val on: Boolean,
        val enabled: Boolean = true,
        val key: String = "",
        val local: String = "",
    ) : RoomSettingRow

    /** 点一下循环切换的选项。 */
    data class Choice(
        val label: String,
        val value: String,
        val options: List<String>,
        val enabled: Boolean = true,
        val key: String = "",
    ) : RoomSettingRow

    /** 点一下就地编辑的文本 / 数字。 */
    data class Editable(
        val label: String,
        val value: String,
        val enabled: Boolean = true,
        val key: String = "",
        val local: String = "",
        val numeric: Boolean = false,
    ) : RoomSettingRow

    /** 一个动作按钮。 */
    data class Action(val label: String, val arg: String, val danger: Boolean = false) :
        RoomSettingRow

    /** 成员一行：右边两个按钮。 */
    data class MemberRow(
        val nick: String,
        val roleText: String,
        val display: String,
        val canPromote: Boolean,
        val canKick: Boolean,
    ) : RoomSettingRow

    /** 入群申请一行。 */
    data class RequestRow(val nick: String) : RoomSettingRow
}

/**
 * 按会话类型算出要显示哪些行。
 *
 * 权限判断也在这里：没有管理权限的设置项 `enabled = false`，界面把它们画灰、
 * 点了给一句明确的提示 —— 比"点下去没反应"好得多。
 */
fun buildRoomSettingRows(room: RoomState, selfNick: String): List<RoomSettingRow> {
    val rows = mutableListOf<RoomSettingRow>()
    val canAdmin = room.canAdminister
    val isGroup = room.kind == RoomKind.GROUP
    val isHall = room.kind == RoomKind.HALL
    val isDm = room.kind == RoomKind.DIRECT

    rows += RoomSettingRow.Section(if (isDm) "私聊" else if (isHall) "大厅" else "群聊")
    rows += RoomSettingRow.ReadOnly(if (isDm) "对方" else "名称", room.title)
    rows += RoomSettingRow.ReadOnly(
        "会话 ID",
        room.id,
        // 群 ID 不可更改是需求里明确要求的，界面上也要说清楚，
        // 否则用户会一直找"哪里能改名"
        if (isGroup) "建好后不可更改" else "",
    )
    if (!isDm) {
        rows += RoomSettingRow.ReadOnly(
            "我的身份",
            when (room.info.myRole.wire) {
                "owner" -> "群主"
                "admin" -> "管理员"
                else -> "成员"
            },
        )
        rows += RoomSettingRow.ReadOnly("成员", "${room.info.memberCount} / ${room.info.maxMembers}")
    }

    if (isGroup || isHall) {
        rows += RoomSettingRow.Section("群设置（管理员修改，全群可见）")
        rows += RoomSettingRow.Editable("群聊名称", room.title, canAdmin, key = "name")
        rows += RoomSettingRow.Editable(
            "最大人数", room.info.maxMembers.toString(), canAdmin,
            key = "maxmembers", numeric = true,
        )
        rows += RoomSettingRow.Choice(
            "加群方式",
            room.info.joinMode.label,
            JoinMode.entries.map { it.label },
            canAdmin,
            key = "joinmode",
        )
        rows += RoomSettingRow.Toggle(
            "向公众展示",
            if (room.info.listed) "其他人可以在群搜索里找到" else "不在群搜索里出现",
            room.info.listed,
            canAdmin,
            key = "listed",
        )
        rows += RoomSettingRow.Editable(
            "总缓存最大大小",
            if (room.settings.cacheMaxMb > 0) "${room.settings.cacheMaxMb} MB" else "",
            canAdmin,
            key = "cachemaxmb",
            numeric = true,
        )
        rows += RoomSettingRow.Toggle(
            "消息重要提醒",
            if (room.settings.importantNotice) "管理员可以发 @全体成员" else "谁都发不了 @全体成员",
            room.settings.importantNotice,
            canAdmin,
            key = "importantnotice",
        )
        rows += RoomSettingRow.Editable(
            "群公告",
            room.settings.announcement.ifEmpty { "（没有公告）" },
            canAdmin,
            key = "announcement",
        )
        if (!canAdmin) rows += RoomSettingRow.Hint("你不是群主或管理员，上面这些只能看不能改")
    }

    // ---- 本地设置：永不上传 ----
    rows += RoomSettingRow.Section("我的设置（只在这台设备上，不上传）")
    rows += RoomSettingRow.Toggle(
        "免打扰",
        if (room.prefs.muted) "不提示新消息" else "新消息会提示",
        room.prefs.muted,
        local = "mute",
    )
    rows += RoomSettingRow.Toggle(
        "置顶",
        if (room.prefs.pinned) "排在最上面" else "按默认顺序排列",
        room.prefs.pinned,
        local = "pin",
    )
    rows += RoomSettingRow.Toggle(
        "重要提醒仍然提醒",
        if (room.prefs.alertImportant) "@我 / @全体成员 照常提示" else "免打扰时连 @我 也不提示",
        room.prefs.alertImportant,
        local = "alert",
    )
    rows += RoomSettingRow.Editable(
        "本地缓存上限",
        "${room.prefs.localCacheMb} MB",
        local = "cache",
        numeric = true,
    )
    if (isHall) {
        rows += RoomSettingRow.Hint("大厅是所有人的公共房间：默认免打扰，不能退出、不能删除")
    }

    if ((isGroup || isHall) && canAdmin) {
        rows += RoomSettingRow.Section("成员")
        for (member in room.members) {
            rows += RoomSettingRow.MemberRow(
                nick = member.nick,
                roleText = when (member.role.wire) {
                    "owner" -> "群主"
                    "admin" -> "管理员"
                    else -> "成员"
                },
                display = member.display,
                // 群主只能有一个、管理员之间不能互相踢 —— 和服务端同一套规则。
                // 界面先把不能做的按钮禁掉，比让用户点了再被服务器拒绝好。
                canPromote = member.nick != selfNick &&
                    (room.info.myRole.wire == "owner" || member.role.wire == "member"),
                canKick = member.nick != selfNick && member.role.wire != "owner",
            )
        }
        if (room.members.isEmpty()) rows += RoomSettingRow.Hint("成员表还没加载出来")

        rows += RoomSettingRow.Section("入群申请")
        for (nick in room.pending) rows += RoomSettingRow.RequestRow(nick)
        if (room.pending.isEmpty()) rows += RoomSettingRow.Hint("现在没有待审核的申请")
    }

    rows += RoomSettingRow.Section("其它")
    if (isGroup) rows += RoomSettingRow.Action("退出群聊", "leave")
    if (isGroup && room.info.myRole.wire == "owner") {
        rows += RoomSettingRow.Action("解散群聊", "delete", danger = true)
    }
    rows += RoomSettingRow.Action("删除本地聊天记录", "clearlocal")
    if (isDm) rows += RoomSettingRow.Action("拉黑对方", "block", danger = true)

    return rows
}

/**
 * 会话设置界面。
 *
 * @param onLocal 本地设置（免打扰/置顶/重要提醒/本地缓存）—— **只改本机**，永不上传
 * @param onSetting 管理员设置项 —— 会发给服务器、全群可见
 * @param onAction 危险操作 / 成员操作 / 审核
 */
@Composable
fun RoomSettingsSheet(
    room: RoomState,
    selfNick: String,
    modifier: Modifier = Modifier,
    onLocal: (String, Boolean) -> Unit = { _, _ -> },
    onLocalCache: (Long) -> Unit = {},
    onSetting: (String, String) -> Unit = { _, _ -> },
    onAction: (String, String) -> Unit = { _, _ -> },
) {
    val colors = LocalDchatColors.current
    val rows = remember(room) { buildRoomSettingRows(room, selfNick) }
    var editing by remember { mutableStateOf<Pair<Int, String>?>(null) }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(colors.windowBg),
    ) {
        Text(
            "会话设置",
            color = colors.text,
            fontSize = 20.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
        )
        HorizontalDivider(color = colors.border)
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(bottom = 24.dp),
        ) {
            for ((index, row) in rows.withIndex()) {
                when (row) {
                    is RoomSettingRow.Section -> Text(
                        row.title,
                        color = colors.accent,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(start = 16.dp, top = 18.dp, bottom = 6.dp),
                    )

                    is RoomSettingRow.Hint -> Text(
                        row.text,
                        color = colors.system,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                    )

                    is RoomSettingRow.ReadOnly -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 48.dp)
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(row.label, color = colors.system, fontSize = 14.sp)
                        Spacer(Modifier.width(12.dp))
                        Column(modifier = Modifier.weight(1f)) {
                            Text(row.value, color = colors.text, fontSize = 15.sp)
                            if (row.note.isNotEmpty()) {
                                Text(row.note, color = colors.system, fontSize = 11.sp)
                            }
                        }
                    }

                    is RoomSettingRow.Toggle -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 56.dp)
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                row.label,
                                color = if (row.enabled) colors.text else colors.system,
                                fontSize = 15.sp,
                            )
                            Text(row.detail, color = colors.system, fontSize = 12.sp)
                        }
                        Switch(
                            checked = row.on,
                            enabled = row.enabled,
                            onCheckedChange = { next ->
                                when {
                                    row.local.isNotEmpty() -> onLocal(row.local, next)
                                    // 管理员设置项没权限时明确说一句，而不是默默不动
                                    !row.enabled -> Unit
                                    else -> onSetting(row.key, if (next) "true" else "false")
                                }
                            },
                        )
                    }

                    is RoomSettingRow.Choice -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 56.dp)
                            .clickable(enabled = row.enabled) {
                                // 三个值循环。顺序固定，点几下就能到自己要的那个
                                val i = row.options.indexOf(row.value)
                                val next = row.options[(i + 1).mod(row.options.size)]
                                val wire = when (next) {
                                    "直接添加" -> "direct"
                                    "自动审核" -> "auto"
                                    else -> "approval"
                                }
                                onSetting(row.key, wire)
                            }
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            row.label,
                            color = if (row.enabled) colors.text else colors.system,
                            fontSize = 15.sp,
                        )
                        Spacer(Modifier.weight(1f))
                        Text(
                            row.value,
                            color = if (row.enabled) colors.accent else colors.system,
                            fontSize = 15.sp,
                        )
                    }

                    is RoomSettingRow.Editable -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 56.dp)
                            .clickable(enabled = row.enabled) {
                                editing = index to row.value
                            }
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            row.label,
                            color = if (row.enabled) colors.text else colors.system,
                            fontSize = 15.sp,
                        )
                        Spacer(Modifier.weight(1f))
                        Text(
                            row.value,
                            color = if (row.enabled) colors.text else colors.system,
                            fontSize = 15.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }

                    is RoomSettingRow.Action -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 52.dp)
                            .clickable { onAction(row.arg, room.id) }
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            row.label,
                            color = if (row.danger) colors.error else colors.accent,
                            fontSize = 15.sp,
                        )
                    }

                    is RoomSettingRow.MemberRow -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 56.dp)
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(row.nick, color = colors.text, fontSize = 15.sp)
                            Text(
                                row.roleText + if (row.display.isNotEmpty()) "・${row.display}" else "",
                                color = colors.system,
                                fontSize = 12.sp,
                            )
                        }
                        Text(
                            if (row.roleText == "成员") "设为管理员" else "撤销",
                            color = if (row.canPromote) colors.accent else colors.system,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .clickable(enabled = row.canPromote) {
                                    onAction(
                                        if (row.roleText == "成员") "promote" else "demote",
                                        row.nick,
                                    )
                                }
                                .padding(8.dp),
                        )
                        Text(
                            "踢出",
                            color = if (row.canKick) colors.error else colors.system,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .clickable(enabled = row.canKick) { onAction("kick", row.nick) }
                                .padding(8.dp),
                        )
                    }

                    is RoomSettingRow.RequestRow -> Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 56.dp)
                            .padding(horizontal = 16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(row.nick, color = colors.text, fontSize = 15.sp)
                            Text("申请入群", color = colors.system, fontSize = 12.sp)
                        }
                        Text(
                            "同意",
                            color = colors.accent,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .clickable { onAction("approve", row.nick) }
                                .padding(8.dp),
                        )
                        Text(
                            "拒绝",
                            color = colors.error,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .clickable { onAction("reject", row.nick) }
                                .padding(8.dp),
                        )
                    }
                }
            }
        }
    }

    // 就地编辑：点一行弹一个输入框，确认后按 key/local 分头发出去。
    // 用弹出对话框而不是行内编辑，是因为手机屏幕窄、行内输入会把下面顶得很乱。
    editing?.let { (index, initial) ->
        val row = rows.getOrNull(index)
        if (row is RoomSettingRow.Editable) {
            RoomValueDialog(
                title = row.label,
                initial = if (row.value.startsWith("（")) "" else initial,
                onDismiss = { editing = null },
                onConfirm = { value ->
                    editing = null
                    when {
                        row.local == "cache" -> onLocalCache(value.toLongOrNull() ?: 0L)
                        row.local.isNotEmpty() -> Unit
                        row.key == "name" || row.key == "announcement" -> onSetting(row.key, value)
                        else -> onSetting(row.key, value.trim())
                    }
                },
            )
        }
    }
}

/** 一个最简单的"输入一个值"对话框。 */
@Composable
private fun RoomValueDialog(
    title: String,
    initial: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    var text by remember { mutableStateOf(initial) }
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title, color = LocalDchatColors.current.text) },
        text = {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                singleLine = true,
            )
        },
        confirmButton = {
            Text(
                "确定",
                color = LocalDchatColors.current.accent,
                modifier = Modifier
                    .clickable { onConfirm(text) }
                    .padding(12.dp),
            )
        },
        dismissButton = {
            Text(
                "取消",
                color = LocalDchatColors.current.system,
                modifier = Modifier
                    .clickable { onDismiss() }
                    .padding(12.dp),
            )
        },
    )
}

// ---------------------------------------------------------------------------
// 群聊：新建 + 搜索
// ---------------------------------------------------------------------------

/**
 * 群聊窗口：**新建群聊和搜索群聊放在一起**。
 *
 * 想加群的人先搜，搜不到就自己建 —— 分成两个入口的话，用户搜完还得回头找另一个。
 */
@Composable
fun GroupWindowSheet(
    hits: List<GroupHit>,
    searching: Boolean,
    hint: String,
    modifier: Modifier = Modifier,
    onCreate: (String, String) -> Unit = { _, _ -> },
    onSearch: (String) -> Unit = {},
    onJoin: (String) -> Unit = {},
    onDismiss: () -> Unit = {},
) {
    val colors = LocalDchatColors.current
    var groupId by remember { mutableStateOf("") }
    var groupName by remember { mutableStateOf("") }
    var keyword by remember { mutableStateOf("") }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(colors.windowBg),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp)
                .padding(horizontal = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("群聊", color = colors.text, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.weight(1f))
            Text(
                "关闭",
                color = colors.system,
                fontSize = 14.sp,
                modifier = Modifier
                    .clickable { onDismiss() }
                    .padding(12.dp),
            )
        }
        HorizontalDivider(color = colors.border)
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            OutlinedTextField(
                value = groupId,
                onValueChange = { groupId = it },
                label = { Text("群聊 ID") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                "只能用 3-16 位小写字母 / 数字 / 下划线 / 减号，建好后不可更改",
                color = colors.system,
                fontSize = 11.sp,
                modifier = Modifier.padding(top = 4.dp, bottom = 10.dp),
            )
            OutlinedTextField(
                value = groupName,
                onValueChange = { groupName = it },
                label = { Text("群聊名称") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(12.dp))
            Text(
                "创建群聊",
                color = colors.accentText,
                fontSize = 15.sp,
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .background(colors.accent)
                    .clickable { onCreate(groupId, groupName) }
                    .padding(vertical = 12.dp),
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )

            Spacer(Modifier.height(28.dp))
            HorizontalDivider(color = colors.border)
            Spacer(Modifier.height(16.dp))
            OutlinedTextField(
                value = keyword,
                onValueChange = { keyword = it },
                label = { Text("搜索群聊") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(10.dp))
            Text(
                "搜索",
                color = colors.text,
                fontSize = 15.sp,
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .background(colors.neutral)
                    .clickable { onSearch(keyword) }
                    .padding(vertical = 12.dp),
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
            if (hint.isNotEmpty()) {
                Text(
                    hint,
                    color = colors.system,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(top = 10.dp),
                )
            }
            if (searching) {
                Text("正在搜索…", color = colors.system, fontSize = 12.sp)
            }
            for (hit in hits) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 60.dp)
                        .padding(top = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(hit.name, color = colors.text, fontSize = 15.sp)
                        Text(
                            "${hit.id}　${hit.members} / ${hit.maxMembers} 人　${hit.joinMode.label}",
                            color = colors.system,
                            fontSize = 12.sp,
                        )
                    }
                    Text(
                        "加入",
                        color = colors.accentText,
                        fontSize = 13.sp,
                        modifier = Modifier
                            .clip(RoundedCornerShape(6.dp))
                            .background(colors.accent)
                            .clickable { onJoin(hit.id) }
                            .padding(horizontal = 14.dp, vertical = 8.dp),
                    )
                }
            }
        }
    }
}


