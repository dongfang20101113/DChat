package com.dongfang20101113.dchat.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.layout.ChatMetrics
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors

/**
 * 第一步：连服务器（只有地址 + 端口）。
 *
 * 和桌面端一样是**两步式**——账号不在这一步填。
 * 手机上额外注意：整页可滚动 + [imePadding]，否则键盘弹出会盖住「连接」按钮。
 */
@Composable
fun ConnectScreen(
    state: ChatState,
    metrics: ChatMetrics,
    onHostChange: (String) -> Unit,
    onPortChange: (Int) -> Unit,
    onConnect: () -> Unit,
) {
    val colors = LocalDchatColors.current
    var portText by remember(state.port) { mutableStateOf(state.port.toString()) }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(colors.windowBg)
            .safeDrawingPadding()
            .imePadding(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .verticalScroll(rememberScrollState())
                .widthIn(max = 420.dp)
                .padding(metrics.horizontalMarginDp.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = "dchat",
                color = colors.accent,
                fontSize = (metrics.announceFontSp + 10).sp,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = "局域网聊天室",
                color = colors.system,
                fontSize = metrics.noticeFontSp.sp,
                modifier = Modifier.padding(top = 4.dp, bottom = 24.dp),
            )

            OutlinedTextField(
                value = state.host,
                onValueChange = onHostChange,
                label = { Text("服务器地址") },
                singleLine = true,
                enabled = !state.connecting,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(12.dp))
            OutlinedTextField(
                value = portText,
                onValueChange = { text ->
                    // 手机键盘上只允许数字，避免出现非法字符
                    val digits = text.filter { it.isDigit() }.take(5)
                    portText = digits
                    digits.toIntOrNull()?.takeIf { it in 1..65535 }?.let(onPortChange)
                },
                label = { Text("端口") },
                singleLine = true,
                enabled = !state.connecting,
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Number,
                    imeAction = ImeAction.Done,
                ),
                modifier = Modifier.fillMaxWidth(),
            )

            if (state.connectionError != null) {
                Text(
                    text = state.connectionError,
                    color = colors.error,
                    fontSize = metrics.noticeFontSp.sp,
                    modifier = Modifier.padding(top = 12.dp).fillMaxWidth(),
                )
            }

            Spacer(Modifier.height(20.dp))
            Button(
                onClick = onConnect,
                enabled = !state.connecting && state.host.isNotBlank(),
                modifier = Modifier.fillMaxWidth().height(metrics.inputMinHeightDp.dp),
            ) {
                if (state.connecting) {
                    CircularProgressIndicator(
                        modifier = Modifier.height(20.dp).widthIn(max = 20.dp),
                        strokeWidth = 2.dp,
                    )
                    Spacer(Modifier.height(0.dp))
                    Text("  连接中…")
                } else {
                    Text("连接", fontSize = metrics.messageFontSp.sp, fontWeight = FontWeight.Medium)
                }
            }
        }
    }
}

/**
 * 第二步：登录 / 注册。
 *
 * 两个模式共用一屏（手机上没必要像桌面端那样开两个独立窗口），
 * 底部按钮切换，已填的用户名密码会带过去——和桌面端的行为一致。
 *
 * 登录失败**不断开连接**：错误红字显示在下面，改完直接再点一次。
 */
@Composable
fun AuthScreen(
    state: ChatState,
    metrics: ChatMetrics,
    onLogin: (String, String) -> Unit,
    onRegister: (String, String) -> Unit,
    onDisconnect: () -> Unit,
) {
    val colors = LocalDchatColors.current
    var isRegister by remember { mutableStateOf(false) }
    var user by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }

    val canSubmit = user.isNotBlank() && password.isNotEmpty() &&
        (!isRegister || confirm == password) && !state.loggingIn

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(colors.windowBg)
            .safeDrawingPadding()
            .imePadding(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .verticalScroll(rememberScrollState())
                .widthIn(max = 420.dp)
                .padding(metrics.horizontalMarginDp.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = if (isRegister) "注册新账号" else "登录",
                color = colors.text,
                fontSize = metrics.announceFontSp.sp,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = "已连接 ${state.host}:${state.port}",
                color = colors.system,
                fontSize = metrics.noticeFontSp.sp,
                modifier = Modifier.padding(top = 4.dp, bottom = 20.dp),
            )

            OutlinedTextField(
                value = user,
                onValueChange = { user = it },
                label = { Text("用户名") },
                singleLine = true,
                enabled = !state.loggingIn,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(12.dp))
            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text("密码") },
                singleLine = true,
                enabled = !state.loggingIn,
                visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Password,
                    imeAction = if (isRegister) ImeAction.Next else ImeAction.Done,
                ),
                modifier = Modifier.fillMaxWidth(),
            )

            if (isRegister) {
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = confirm,
                    onValueChange = { confirm = it },
                    label = { Text("确认密码") },
                    singleLine = true,
                    enabled = !state.loggingIn,
                    visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(
                        keyboardType = KeyboardType.Password,
                        imeAction = ImeAction.Done,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
                if (confirm.isNotEmpty() && confirm != password) {
                    Text(
                        text = "两次输入的密码不一致",
                        color = colors.error,
                        fontSize = metrics.noticeFontSp.sp,
                        modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                    )
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                Checkbox(checked = showPassword, onCheckedChange = { showPassword = it })
                Text(
                    text = "显示密码",
                    color = colors.system,
                    fontSize = metrics.noticeFontSp.sp,
                    modifier = Modifier.clickable { showPassword = !showPassword },
                )
            }

            if (state.authError != null) {
                Text(
                    text = state.authError,
                    color = colors.error,
                    fontSize = metrics.noticeFontSp.sp,
                    modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
                )
            }

            Button(
                onClick = {
                    if (isRegister) onRegister(user.trim(), password) else onLogin(user.trim(), password)
                },
                enabled = canSubmit,
                modifier = Modifier.fillMaxWidth().height(metrics.inputMinHeightDp.dp),
            ) {
                Text(
                    text = if (isRegister) "注册并登录" else "登录",
                    fontSize = metrics.messageFontSp.sp,
                    fontWeight = FontWeight.Medium,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                TextButton(onClick = {
                    // 切换时保留已填的用户名和密码（手机打字慢，别让用户重敲）
                    isRegister = !isRegister
                    confirm = ""
                }) {
                    Text(
                        text = if (isRegister) "已有账号？去登录" else "没有账号？注册新账号",
                        fontSize = metrics.noticeFontSp.sp,
                    )
                }
                TextButton(onClick = onDisconnect) {
                    Text("断开", fontSize = metrics.noticeFontSp.sp, color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
}
