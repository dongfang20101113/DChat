package com.dongfang20101113.dchat

import android.graphics.Bitmap
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import com.dongfang20101113.dchat.protocol.ServerLine
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.ui.ChatState
import com.dongfang20101113.dchat.ui.Stage
import com.dongfang20101113.dchat.ui.layout.chatMetrics
import com.dongfang20101113.dchat.ui.reduce
import com.dongfang20101113.dchat.ui.screens.AuthScreen
import com.dongfang20101113.dchat.ui.screens.ChatScreen
import com.dongfang20101113.dchat.ui.screens.ConnectScreen
import com.dongfang20101113.dchat.ui.theme.DchatTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * **多屏幕尺寸的离屏渲染截图**。
 *
 * 这台机器跑不了模拟器（`HypervisorPresent = False`，没有 WHPX 加速），
 * 所以用 Robolectric 的原生图形模式在 JVM 里渲染出真实像素，产出 PNG 供人工复核。
 *
 * 覆盖四类最容易出问题的屏幕：
 *  - 手机竖屏（最常见）
 *  - 极窄屏 320dp（最挤）
 *  - 手机横屏（高度只有 393dp，最容易把输入框挤没）
 *  - 平板（≥840dp 会切到左右分栏）
 *
 * ## 一个踩过的坑
 *
 * 屏幕尺寸**必须用方法级 `@Config(qualifiers = ...)` 声明，而且限定符顺序有严格要求**：
 * Android 规定是 `语言-区域` 在前、`宽-高` 居中、`密度` 在后。
 * 写成 `w393dp-h851dp-xxhdpi-zh-rCN` 会直接抛
 * `IllegalArgumentException: Found unrecognized trailing qualifier segments`。
 * 另外运行时 `RuntimeEnvironment.setQualifiers()` 不可靠——Activity 可能已经建好了。
 *
 * 产物在 `app/build/screenshots/`。
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35])
class ScreenshotTest {

    @get:Rule
    val composeRule = createComposeRule()

    private val outDir = File("build/screenshots").apply { mkdirs() }

    /** 造一份有代表性的聊天记录，覆盖所有条目类型。 */
    private fun sampleState(selfNick: String = "我"): ChatState {
        var s = ChatState(
            stage = Stage.CHAT, connected = true, selfNick = selfNick,
            host = "192.168.1.100", port = 5555,
        )
        fun feed(raw: String) { s = s.reduce(ServerLine.parse(raw, selfNick), "21:05") }

        feed("WELCOME 21:05 dchat Server")
        feed("NAMES 21:05 我,小明,Alice")
        feed("KNOWN 21:05 我,小明,Alice,bob")
        feed("RULES 128 0 0")
        feed("JOINED 21:05 小明")
        feed("SAY 21:05 小明 大家好，我是小明")
        feed("SAY 21:05 我 你好！这是一条自己发的消息，应该靠右显示")
        feed("SAY 21:05 Alice @我 这条消息提到了我，应该高亮显示")
        feed("ANNOUNCE 21:05 服务器将在 10 分钟后维护，请提前保存进度")
        feed("SYS 21:05 欢迎来到 dchat 聊天室")
        feed("SAY 21:05 小明 这是一条比较长的消息，用来验证在窄屏上气泡会正确换行而不会超出屏幕边界。")
        val name = base64Encode("季度报告.pdf".toByteArray(Charsets.UTF_8))
        feed("FILE_OFFER 21:05 小明 F1 $name 1048576 1")
        feed("ERROR 21:05 发言太快了，请稍后再试")
        return s
    }

    private fun shoot(fileName: String, dark: Boolean, widthDp: Int, heightDp: Int, content: @Composable () -> Unit) {
        composeRule.setContent {
            DchatTheme(darkTheme = dark) {
                Surface(Modifier.fillMaxSize()) { content() }
            }
        }
        composeRule.waitForIdle()
        val bitmap = composeRule.onRoot().captureToImage().asAndroidBitmap()
        File(outDir, fileName).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun chat(dark: Boolean, name: String, w: Int, h: Int) {
        val state = sampleState()
        shoot(name, dark, w, h) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
            )
        }
    }

    // 限定符顺序：语言-区域 → 宽-高 → 密度
    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `手机竖屏 393x851 深色`() = chat(true, "chat-phone-393x851-dark.png", 393, 851)

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `手机竖屏 393x851 浅色`() = chat(false, "chat-phone-393x851-light.png", 393, 851)

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h640dp-xxhdpi")
    fun `小屏手机 360x640`() = chat(true, "chat-phone-360x640-dark.png", 360, 640)

    @Test
    @Config(qualifiers = "zh-rCN-w320dp-h568dp-xhdpi")
    fun `极窄屏 320x568`() = chat(true, "chat-phone-320x568-dark.png", 320, 568)

    @Test
    @Config(qualifiers = "zh-rCN-w851dp-h393dp-xxhdpi")
    fun `手机横屏 851x393`() = chat(true, "chat-phone-land-851x393-dark.png", 851, 393)

    @Test
    @Config(qualifiers = "zh-rCN-w1024dp-h768dp-xhdpi")
    fun `平板 1024x768 应该分栏`() = chat(true, "chat-tablet-1024x768-dark.png", 1024, 768)

    @Test
    @Config(qualifiers = "zh-rCN-w1280dp-h800dp-xhdpi")
    fun `大屏 1280x800 应该分栏`() = chat(true, "chat-tablet-1280x800-dark.png", 1280, 800)

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `连接界面`() {
        val state = ChatState(stage = Stage.CONNECT, host = "192.168.1.100", port = 5555)
        shoot("entry-connect.png", true, 393, 851) {
            ConnectScreen(
                state = state, metrics = chatMetrics(393, 851, 1f),
                onHostChange = {}, onPortChange = {}, onConnect = {},
            )
        }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `登录界面 含错误提示`() {
        val state = ChatState(
            stage = Stage.AUTH, connected = true, host = "192.168.1.100", port = 5555,
            authError = "用户名不存在",
        )
        shoot("entry-auth-error.png", true, 393, 851) {
            AuthScreen(
                state = state, metrics = chatMetrics(393, 851, 1f),
                onLogin = { _, _ -> }, onRegister = { _, _ -> }, onDisconnect = {},
            )
        }
    }

    // ------------------------------------------------------------------
    // 阶段 1 新增：服务器文本限制
    // ------------------------------------------------------------------

    private fun limitedState(maxChars: Int, maxLines: Int): ChatState =
        sampleState().copy(maxTextLength = maxChars, maxTextLines = maxLines)

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `输入未超限_显示还能输多少`() {
        val state = limitedState(maxChars = 200, maxLines = 8)
        shoot("limits-under.png", true, 393, 851) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
                // 3 行，远没到 200 字符
                initialDraft = "第一行\n第二行\n第三行",
            )
        }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `输入超限_变红并说明原因`() {
        val state = limitedState(maxChars = 20, maxLines = 2)
        shoot("limits-over.png", true, 393, 851) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
                // 3 行 > 2 行，字符数也超了
                initialDraft = "第一行内容比较长\n第二行内容也比较长\n第三行",
            )
        }
    }

    // ------------------------------------------------------------------
    // 阶段 3：emoji 选择器
    // ------------------------------------------------------------------

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `emoji 选择器展开后的样子`() {
        val state = sampleState()
        composeRule.setContent {
            DchatTheme(darkTheme = true) {
                Surface(Modifier.fillMaxSize()) {
                    ChatScreen(
                        state = state,
                        onSend = {}, onDownload = {}, onSendFile = {},
                        onMarkRead = {}, onDisconnect = {},
                    )
                }
            }
        }
        composeRule.waitForIdle()
        // 真的去点那个按钮，而不是靠测试钩子——这样连"按钮在不在、点得动吗"一起验了
        composeRule.onNodeWithContentDescription("表情").performClick()
        composeRule.waitForIdle()

        val bitmap = composeRule.onRoot().captureToImage().asAndroidBitmap()
        File(outDir, "emoji-picker.png").outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
    }

    // ------------------------------------------------------------------
    // 阶段 3：贴纸内联渲染
    // ------------------------------------------------------------------

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `贴纸内联渲染`() {
        // 必须造一张**真实的 PNG 落到磁盘**：savedPath 指向不存在的文件时，
        // BitmapFactory 解不出东西，渲染出来的只会是"下载中"的占位——
        // 那样截出来的图看着正常，其实什么都没验证到。
        val dir = File(System.getProperty("java.io.tmpdir"), "dchat-shot").apply { mkdirs() }
        val stickerFile = File(dir, "sticker.png")
        val sticker = Bitmap.createBitmap(160, 160, Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(sticker)
        canvas.drawColor(0xFFFFD54F.toInt())                                   // 暖黄底
        canvas.drawCircle(80f, 80f, 52f, android.graphics.Paint().apply {
            color = 0xFF202020.toInt(); isAntiAlias = true
        })
        canvas.drawCircle(62f, 68f, 9f, android.graphics.Paint().apply { color = 0xFFFFD54F.toInt() })
        canvas.drawCircle(98f, 68f, 9f, android.graphics.Paint().apply { color = 0xFFFFD54F.toInt() })
        stickerFile.outputStream().use { sticker.compress(Bitmap.CompressFormat.PNG, 100, it) }
        sticker.recycle()

        // 一条贴纸 + 一条**普通文件**，对照着看：两者走同一个传输通道，
        // 只有渲染分支不同，截图里必须一眼能看出区别。
        var state = sampleState()
        val stickerName = base64Encode("开心.png".toByteArray(Charsets.UTF_8))
        state = state.reduce(
            ServerLine.parse("FILE_OFFER 21:06 小明 S1 $stickerName 4096 0 1", "我"),
            "21:06",
        )
        state = state.copy(
            items = state.items.map { item ->
                if (item is com.dongfang20101113.dchat.ui.ChatItem.FileItem &&
                    item.fileId == "S1"
                ) {
                    // 模拟"已经自动下载完了"
                    item.copy(
                        state = com.dongfang20101113.dchat.ui.FileState.DONE,
                        savedPath = stickerFile.absolutePath,
                    )
                } else {
                    item
                }
            },
        )

        shoot("sticker-inline.png", true, 393, 851) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
            )
        }
    }

    // ------------------------------------------------------------------
    // 阶段 5：语音消息
    // ------------------------------------------------------------------

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `语音气泡与正在播放的样子`() {
        // 和贴纸那张一样的道理：`savedPath` 必须指向**真实存在**的文件，
        // 否则气泡会画成"没下载完"的灰按钮——截图看着没崩，其实什么都没验到。
        val dir = File(System.getProperty("java.io.tmpdir"), "dchat-shot").apply { mkdirs() }
        val voice = File(dir, "voice-1.m4a")
        voice.writeBytes(ByteArray(48_000))          // 内容无所谓，这里只验渲染

        var state = sampleState()
        val name = base64Encode("voice-1.m4a".toByteArray(Charsets.UTF_8))
        // 别人发的一条 + 自己发的一条：要靠左/靠右对照着看
        state = state.reduce(ServerLine.parse("FILE_OFFER 21:06 小明 V1 $name 48000 0 voice", "我"), "21:06")
        state = state.reduce(ServerLine.parse("FILE_OFFER 21:07 我 V2 $name 48000 0 voice", "我"), "21:07")

        state = state.copy(
            items = state.items.map { item ->
                if (item is com.dongfang20101113.dchat.ui.ChatItem.FileItem &&
                    item.fileId.startsWith("V")
                ) {
                    // 模拟"已经自动下载完了"
                    item.copy(
                        state = com.dongfang20101113.dchat.ui.FileState.DONE,
                        savedPath = voice.absolutePath,
                    )
                } else {
                    item
                }
            },
            // 别人的那条正在播：气泡上应当出现暂停图标和进度线
            playingVoiceId = "V1",
            playingSeconds = 3,
            playingDurationSeconds = 8,
        )

        shoot("voice-bubbles.png", true, 393, 851) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
            )
        }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `按住说话时的录音状态`() {
        val state = sampleState()
        shoot("voice-recording.png", true, 393, 851) {
            ChatScreen(
                state = state,
                onSend = {}, onDownload = {}, onSendFile = {}, onMarkRead = {}, onDisconnect = {},
                // 正常运行时这几秒来自会话状态；截图里直接给一个值，
                // 免得为了截一张图去等三秒
                initialRecordingSeconds = 3,
            )
        }
    }

    // ------------------------------------------------------------------
    // 应用图标
    // ------------------------------------------------------------------

    /**
     * 把图标渲染出来看。
     *
     * **必须出两种**：
     *  - 未裁切的方形（看整体造型）
     *  - **圆形遮罩**（启动器普遍用圆形裁切，用来验证图案有没有超出安全区被切掉）
     */
    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `渲染应用图标_方形与圆形遮罩`() {
        val ctx = androidx.test.core.app.ApplicationProvider.getApplicationContext<android.content.Context>()
        val size = 432   // 相当于 108dp @ 4x，够看清细节

        val foreground = androidx.core.content.ContextCompat.getDrawable(ctx, R.drawable.ic_launcher_foreground)!!
        val background = androidx.core.content.ContextCompat.getDrawable(ctx, R.drawable.ic_launcher_monochrome)!!

        // --- 自适应图标合成：底色 + 前景 ---
        val icon = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(icon)
        canvas.drawColor(0xFF0078D7.toInt())
        foreground.setBounds(0, 0, size, size)
        foreground.draw(canvas)
        File(outDir, "icon-square.png").outputStream().use { icon.compress(Bitmap.CompressFormat.PNG, 100, it) }

        // --- 圆形遮罩：模拟启动器的裁切 ---
        val mask = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        android.graphics.Canvas(mask).drawCircle(
            size / 2f, size / 2f, size / 2f,
            android.graphics.Paint().apply { color = android.graphics.Color.BLACK },
        )
        val round = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val rc = android.graphics.Canvas(round)
        rc.drawBitmap(icon, 0f, 0f, null)
        rc.drawBitmap(
            mask, 0f, 0f,
            android.graphics.Paint().apply {
                xfermode = android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.DST_IN)
            },
        )
        File(outDir, "icon-round.png").outputStream().use { round.compress(Bitmap.CompressFormat.PNG, 100, it) }

        // --- 单色层（Android 13+ 主题化图标会拿它上色）---
        val mono = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val mc = android.graphics.Canvas(mono)
        mc.drawColor(0xFF202020.toInt())
        background.setBounds(0, 0, size, size)
        background.draw(mc)
        File(outDir, "icon-monochrome.png").outputStream().use { mono.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
