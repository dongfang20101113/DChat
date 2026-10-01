package com.dongfang20101113.dchat

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.lifecycle.viewmodel.compose.viewModel
import com.dongfang20101113.dchat.ui.ChatViewModel
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
 * 这里刻意不声明 `android:configChanges`：旋转屏幕或改系统字号时让系统重建 Activity，
 * Compose 会用新的宽高重新算一遍 [chatMetrics]，布局自然跟着变——
 * 这比自己拦截配置变更更不容易出错（尤其是字体缩放，拦截了反而容易算错）。
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
    val viewModel: ChatViewModel = viewModel()
    val state by viewModel.state.collectAsState()

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
                onHostChange = viewModel::updateHost,
                onPortChange = viewModel::updatePort,
                onConnect = viewModel::connect,
            )

            Stage.AUTH -> AuthScreen(
                state = state,
                metrics = metrics,
                onLogin = viewModel::login,
                onRegister = viewModel::register,
                onDisconnect = viewModel::disconnect,
            )

            Stage.CHAT -> ChatScreen(
                state = state,
                onSend = viewModel::sendMessage,
                onDownload = viewModel::requestDownload,
                onSendFile = viewModel::sendFile,
                onMarkRead = viewModel::markRead,
                onDisconnect = viewModel::disconnect,
            )
        }
    }
}
