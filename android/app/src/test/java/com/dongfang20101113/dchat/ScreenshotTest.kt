package com.dongfang20101113.dchat

import android.graphics.Bitmap
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
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
