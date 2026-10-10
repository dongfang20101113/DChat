package com.dongfang20101113.dchat

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import android.graphics.Bitmap
import com.dongfang20101113.dchat.protocol.GroupHit
import com.dongfang20101113.dchat.protocol.JoinMode
import com.dongfang20101113.dchat.protocol.RoomInfo
import com.dongfang20101113.dchat.protocol.RoomKind
import com.dongfang20101113.dchat.protocol.RoomMember
import com.dongfang20101113.dchat.protocol.RoomPrefs
import com.dongfang20101113.dchat.protocol.RoomRole
import com.dongfang20101113.dchat.protocol.RoomSettings
import com.dongfang20101113.dchat.protocol.RoomState
import com.dongfang20101113.dchat.ui.screens.GroupWindowSheet
import com.dongfang20101113.dchat.ui.screens.RoomListPanel
import com.dongfang20101113.dchat.ui.screens.RoomSettingsSheet
import com.dongfang20101113.dchat.ui.theme.DchatTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * 多会话界面的离屏截图测试。
 *
 * 和 [ScreenshotTest] 一样：**真的渲染出像素**，不是"看着差不多"。
 * 会话列表和设置页的排版问题（文字被挤没、未读气泡压住标题、按钮点不到）
 * 在真机上要一张张看，这里一次跑出来。
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35])
class RoomScreenshotTest {

    @get:Rule
    val composeRule = createComposeRule()

    private val outDir = File("build/screenshots").apply { mkdirs() }

    private fun sampleRooms(): List<RoomState> = listOf(
        RoomState(
            info = RoomInfo(id = "hall", kind = RoomKind.HALL, displayName = "大厅", memberCount = 12),
            // 大厅默认免打扰 —— 这是用户明确要求的（公共房间消息最多）
            prefs = RoomPrefs(muted = true, pinned = true),
            unread = 7,
        ),
        RoomState(
            info = RoomInfo(
                id = "devgroup", kind = RoomKind.GROUP, displayName = "dchat 开发组",
                owner = "我", memberCount = 5, maxMembers = 200, myRole = RoomRole.OWNER,
            ),
            settings = RoomSettings(cacheMaxMb = 256, announcement = "今晚八点联调"),
            prefs = RoomPrefs(pinned = false),
            unread = 3,
            members = listOf(
                RoomMember("我", RoomRole.OWNER, "群主大人"),
                RoomMember("小明", RoomRole.ADMIN),
                RoomMember("Alice", RoomRole.MEMBER, "爱丽丝"),
            ),
            pending = listOf("bob", "carol"),
        ),
        RoomState(
            info = RoomInfo(id = "dm-000001", kind = RoomKind.DIRECT, displayName = "小明"),
            prefs = RoomPrefs(),
        ),
        RoomState(
            info = RoomInfo(
                id = "moyu", kind = RoomKind.GROUP, displayName = "摸鱼群",
                memberCount = 28, maxMembers = 50, myRole = RoomRole.MEMBER,
            ),
            prefs = RoomPrefs(muted = true),
            // 免打扰的会话：只画小圆点，不显示数字（未读仍然照常计数）
            unread = 128,
            unreadImportant = true,
        ),
    )

    private fun shoot(fileName: String, content: @Composable () -> Unit) {
        composeRule.setContent {
            DchatTheme(darkTheme = true) {
                Surface(Modifier.fillMaxSize()) { content() }
            }
        }
        composeRule.waitForIdle()
        val bitmap = composeRule.onRoot().captureToImage().asAndroidBitmap()
        File(outDir, fileName).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `会话列表 手机竖屏`() = shoot("rooms-list-phone.png") {
        RoomListPanel(rooms = sampleRooms(), currentRoomId = "devgroup", selfNick = "我")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `会话设置 群主视角`() = shoot("rooms-settings-owner.png") {
        RoomSettingsSheet(room = sampleRooms()[1], selfNick = "我")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `会话设置 普通成员视角`() = shoot("rooms-settings-member.png") {
        // 普通成员：群设置那一栏全部变灰、并给出"只能看不能改"的提示，
        // 而本地设置（免打扰/置顶）永远可点 —— 权限边界在界面上一眼可见
        RoomSettingsSheet(room = sampleRooms()[3], selfNick = "我")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `私聊设置`() = shoot("rooms-settings-dm.png") {
        RoomSettingsSheet(room = sampleRooms()[2], selfNick = "我")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w393dp-h851dp-xxhdpi")
    fun `群聊 新建与搜索`() = shoot("rooms-group-window.png") {
        GroupWindowSheet(
            hits = listOf(
                GroupHit("devgroup", "dchat 开发组", 5, 200, JoinMode.APPROVAL),
                GroupHit("moyu", "摸鱼群", 28, 50, JoinMode.DIRECT),
                GroupHit("test", "测试群", 2, 20, JoinMode.AUTO),
            ),
            searching = false,
            hint = "搜到 3 个群",
        )
    }
}
