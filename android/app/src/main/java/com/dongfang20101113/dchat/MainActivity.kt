package com.dongfang20101113.dchat

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
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

            Stage.CHAT -> ChatScreen(
                state = state,
                onSend = DchatSession::sendMessage,
                onDownload = DchatSession::requestDownload,
                onSendFile = DchatSession::sendFile,
                onMarkRead = DchatSession::markRead,
                onDisconnect = DchatSession::disconnect,
            )
        }
    }
}
