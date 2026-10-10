package com.dongfang20101113.dchat

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import com.dongfang20101113.dchat.net.DchatSession
import com.dongfang20101113.dchat.ui.Stage
import com.dongfang20101113.dchat.ui.layout.chatMetrics
import com.dongfang20101113.dchat.ui.screens.AuthScreen
import com.dongfang20101113.dchat.ui.screens.ChatScreen
import com.dongfang20101113.dchat.ui.screens.ConnectScreen
import com.dongfang20101113.dchat.ui.screens.GroupWindowSheet
import com.dongfang20101113.dchat.ui.screens.RoomListPanel
import com.dongfang20101113.dchat.ui.screens.RoomSettingsSheet
import com.dongfang20101113.dchat.ui.theme.DchatTheme
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors

/**
 * 唯一 Activity，按阶段切换三屏：**连接 → 登录/注册 → 聊天**。
 *
 * 状态来自进程级的 [DchatSession]，**不经 ViewModel**：
 * 这样即使 Activity 因为打开文件选择器、旋转屏幕、切深色模式被销毁重建，
 * 连接和聊天记录都还在，重建后重新订阅一下即可。
 *
 * 刻意不声明 `android:configChanges`：让系统正常重建，Compose 会用新的宽高
 * 重新算一遍布局（见 [chatMetrics]），比自己拦截配置变更更不容易出错。
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 全面屏：内容延伸到状态栏/手势条下面，再由 WindowInsets 把内容让开
        enableEdgeToEdge()
        setContent {
            DchatTheme {
                DchatRoot()
            }
        }
    }
}

/** 根节点：按阶段切换三屏。命名为 Root 以免和 Application 类 `DchatApp` 撞名。 */
@Composable
private fun DchatRoot() {
    val state by DchatSession.state.collectAsState()

    // 入口屏（连接 / 登录）也需要尺寸，用系统给的可用宽高算一次即可
    val configuration = LocalConfiguration.current
    val fontScale = LocalDensity.current.fontScale
    val metrics = remember(configuration.screenWidthDp, configuration.screenHeightDp, fontScale) {
        chatMetrics(configuration.screenWidthDp, configuration.screenHeightDp, fontScale)
    }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = LocalDchatColors.current.windowBg,
    ) {
        when (state.stage) {
            Stage.CONNECT -> ConnectScreen(
                state = state,
                metrics = metrics,
                onHostChange = DchatSession::updateHost,
                onPortChange = DchatSession::updatePort,
                onConnect = DchatSession::connect,
            )

            Stage.AUTH -> AuthScreen(
                state = state,
                metrics = metrics,
                onLogin = DchatSession::login,
                onRegister = DchatSession::register,
                onDisconnect = DchatSession::disconnect,
            )

            Stage.CHAT -> {
                // 多会话的浮层。
                //
                // 为什么盖在聊天界面之上、而不是另开一个 Activity 或导航目的地：
                // 会话是**同一条连接**的一部分，分屏就要把连接和状态在两边同步，
                // 而那种同步迟早会出现"这边退群了、那边列表还在"。浮层共用同一份
                // state，不存在同步问题。
                //
                // 入口暂时是右下角一个悬浮按钮（而不是改 ChatScreen 的顶栏）：
                // ChatScreen 有 650 行，顶栏那块改动面大、风险高，而这一个按钮
                // 就能让整条链路先跑起来、能被真机验证。位置后面再挪。
                var overlay by remember { mutableStateOf(RoomOverlay.NONE) }
                var settingsRoomId by remember { mutableStateOf<String?>(null) }

                Box(Modifier.fillMaxSize()) {
                    ChatScreen(
                        state = state,
                        onSend = DchatSession::sendMessage,
                        onDownload = DchatSession::requestDownload,
                        onSendFile = DchatSession::sendFile,
                        onMarkRead = DchatSession::markRead,
                        onDisconnect = DchatSession::disconnect,
                        onVoiceToggle = DchatSession::toggleVoice,
                        onStartVoice = DchatSession::beginVoiceRecording,
                        onStopVoice = DchatSession::finishVoiceRecording,
                        onCancelVoice = DchatSession::cancelVoiceRecording,
                        // 取色盘：选好的色码交给会话层，界面负责放进输入框（不直接发出去）
                        onPickColor = DchatSession::rememberPickedColor,
                        onDismissColorPicker = DchatSession::dismissColorPicker,
                        onColorCodeConsumed = DchatSession::clearPendingColorCode,
                    )

                    // 只有服务端确认支持多会话才出现 —— 连旧版服务端时界面
                    // 和升级前一模一样，一个像素都不多
                    if (state.roomsEnabled && overlay == RoomOverlay.NONE &&
                        settingsRoomId == null
                    ) {
                        Text(
                            "会话",
                            color = LocalDchatColors.current.accentText,
                            modifier = Modifier
                                .align(Alignment.TopStart)
                                .padding(12.dp)
                                .clip(RoundedCornerShape(16.dp))
                                .background(LocalDchatColors.current.accent)
                                .clickable { overlay = RoomOverlay.LIST }
                                .padding(horizontal = 14.dp, vertical = 8.dp),
                        )
                    }

                    when {
                        settingsRoomId != null -> {
                            val room = state.rooms.firstOrNull { it.id == settingsRoomId }
                            if (room == null) {
                                settingsRoomId = null
                            } else {
                                Surface(
                                    modifier = Modifier.fillMaxSize(),
                                    color = LocalDchatColors.current.windowBg,
                                ) {
                                    RoomSettingsSheet(
                                        room = room,
                                        selfNick = state.selfNick,
                                        onLocal = { key, on ->
                                            DchatSession.updateRoomPrefs(room.id) { prefs ->
                                                when (key) {
                                                    "mute" -> prefs.copy(muted = on)
                                                    "pin" -> prefs.copy(pinned = on)
                                                    else -> prefs.copy(alertImportant = on)
                                                }
                                            }
                                        },
                                        onLocalCache = { mb ->
                                            DchatSession.updateRoomPrefs(room.id) {
                                                it.copy(localCacheMb = mb)
                                            }
                                        },
                                        onSetting = { key, value ->
                                            DchatSession.sendRoomSetting(room.id, key, value)
                                        },
                                        onAction = { arg, target ->
                                            DchatSession.roomAction(room.id, arg, target)
                                        },
                                    )
                                }
                                BackButton { settingsRoomId = null }
                            }
                        }

                        overlay == RoomOverlay.LIST -> {
                            Surface(
                                modifier = Modifier.fillMaxSize(),
                                color = LocalDchatColors.current.windowBg,
                            ) {
                                RoomListPanel(
                                    rooms = state.rooms,
                                    currentRoomId = state.currentRoomId,
                                    selfNick = state.selfNick,
                                    onSelect = {
                                        DchatSession.selectRoom(it)
                                        overlay = RoomOverlay.NONE
                                    },
                                    onOpenSettings = { settingsRoomId = it },
                                    onNewGroup = { overlay = RoomOverlay.GROUP },
                                )
                            }
                            BackButton { overlay = RoomOverlay.NONE }
                        }

                        overlay == RoomOverlay.GROUP -> {
                            Surface(
                                modifier = Modifier.fillMaxSize(),
                                color = LocalDchatColors.current.windowBg,
                            ) {
                                GroupWindowSheet(
                                    hits = state.groupHits,
                                    searching = state.groupSearching,
                                    hint = "",
                                    onCreate = { id, name ->
                                        DchatSession.createGroup(id, name)?.let { reason ->
                                            DchatSession.postNotice(reason)
                                        }
                                    },
                                    onSearch = DchatSession::searchGroups,
                                    onJoin = { DchatSession.joinGroup(it) },
                                    onDismiss = { overlay = RoomOverlay.NONE },
                                )
                            }
                            BackButton { overlay = RoomOverlay.NONE }
                        }
                    }
                }
            }
        }
    }
}

/** 多会话的浮层状态。 */
private enum class RoomOverlay { NONE, LIST, GROUP }

/** 浮层左上角的「返回」。 */
@Composable
private fun BackButton(onClick: () -> Unit) {
    Box(Modifier.fillMaxSize()) {
        Text(
            "← 返回",
            color = LocalDchatColors.current.accentText,
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(16.dp)
                .clip(RoundedCornerShape(16.dp))
                .background(LocalDchatColors.current.neutral)
                .clickable { onClick() }
                .padding(horizontal = 16.dp, vertical = 10.dp),
        )
    }
}
