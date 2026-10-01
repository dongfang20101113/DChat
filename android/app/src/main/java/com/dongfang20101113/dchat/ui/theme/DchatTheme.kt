package com.dongfang20101113.dchat.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color

/**
 * dchat 配色。
 *
 * **这里的取值逐条抄自桌面端 `client.cpp` 的 `kLightPalette` / `kDarkPalette`**
 * （第 260~278 行），所以手机端和电脑端看起来是同一套颜色，而不是"差不多"。
 * 昵称调色板也一样，配合协议层按 **UTF-8 字节**算的 FNV-1a 哈希，
 * 同一个昵称在两端的颜色完全一致。
 */
@Immutable
data class DchatColors(
    val windowBg: Color,
    val bubbleOther: Color,
    val bubbleOtherBorder: Color,
    val bubbleOwn: Color,
    val bubbleOwnBorder: Color,
    val bubbleOwnText: Color,
    val text: Color,
    val system: Color,
    val error: Color,
    val time: Color,
    val mention: Color,
    val noticeBg: Color,
    val announceBg: Color,
    val announceBorder: Color,
    val announceText: Color,
    val neutral: Color,
    val neutralBorder: Color,
    val accent: Color,
    val accentText: Color,
    val border: Color,
    /** 8 色昵称调色板，顺序与桌面端一致。 */
    val nick: List<Color>,
    val isDark: Boolean,
) {
    /** 按协议层的下标取昵称颜色（下标由 [com.dongfang20101113.dchat.protocol.nickColorIndex] 给出）。 */
    fun nickColor(index: Int): Color = nick[index.mod(nick.size)]
}

/** 浅色主题（对应桌面端 `kLightPalette`）。 */
val LightDchatColors = DchatColors(
    windowBg = Color(0xFFF2F3F5),
    bubbleOther = Color(0xFFFFFFFF),
    bubbleOtherBorder = Color(0xFFDEE1E6),
    bubbleOwn = Color(0xFF0078D7),
    bubbleOwnBorder = Color(0xFF0064BE),
    bubbleOwnText = Color(0xFFFFFFFF),
    text = Color(0xFF202020),
    system = Color(0xFF6E6E6E),
    error = Color(0xFFC80000),
    time = Color(0xFF828282),
    mention = Color(0xFFB06000),
    noticeBg = Color(0xFFE8EAEE),
    announceBg = Color(0xFFFFF6DE),
    announceBorder = Color(0xFFD6960A),
    announceText = Color(0xFF965C00),
    neutral = Color(0xFFE9ECEF),
    neutralBorder = Color(0xFFCDD2D8),
    accent = Color(0xFF0078D7),
    accentText = Color(0xFFFFFFFF),
    border = Color(0xFFD6D9DE),
    nick = listOf(
        Color(0xFF0066CC), Color(0xFF008C3C), Color(0xFFCC6600), Color(0xFF960096),
        Color(0xFF008C8C), Color(0xFFC80064), Color(0xFF6E6E00), Color(0xFF5A3CA0),
    ),
    isDark = false,
)

/** 深色主题（对应桌面端 `kDarkPalette`，也是桌面端的默认值）。 */
val DarkDchatColors = DchatColors(
    windowBg = Color(0xFF202020),
    bubbleOther = Color(0xFF303236),
    bubbleOtherBorder = Color(0xFF46494E),
    bubbleOwn = Color(0xFF0078D7),
    bubbleOwnBorder = Color(0xFF1E5AA0),
    bubbleOwnText = Color(0xFFFFFFFF),
    text = Color(0xFFE8E8E8),
    system = Color(0xFF969696),
    error = Color(0xFFFF6E6E),
    time = Color(0xFF8C8C8C),
    mention = Color(0xFFFFBE50),
    noticeBg = Color(0xFF2C2E32),
    announceBg = Color(0xFF3A3018),
    announceBorder = Color(0xFFFFBE50),
    announceText = Color(0xFFFFD68C),
    neutral = Color(0xFF34363A),
    neutralBorder = Color(0xFF4A4D52),
    accent = Color(0xFF0078D7),
    accentText = Color(0xFFFFFFFF),
    border = Color(0xFF404246),
    nick = listOf(
        Color(0xFF569CD6), Color(0xFF78C882), Color(0xFFEBAF5F), Color(0xFFCD8CEB),
        Color(0xFF5FCDCD), Color(0xFFF587AF), Color(0xFFCDCD73), Color(0xFF9B9BF5),
    ),
    isDark = true,
)

/** 通过这个取当前主题配色：`LocalDchatColors.current`。 */
val LocalDchatColors = staticCompositionLocalOf { DarkDchatColors }

/**
 * 应用主题。
 *
 * 默认跟随系统深浅色——手机上没有桌面端那种"深色/浅色/跟随系统"三态按钮的必要，
 * 系统设置已经是用户表达偏好的地方了。
 */
@Composable
fun DchatTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val colors = if (darkTheme) DarkDchatColors else LightDchatColors

    val scheme = if (darkTheme) {
        darkColorScheme(
            primary = colors.accent,
            onPrimary = colors.accentText,
            background = colors.windowBg,
            onBackground = colors.text,
            surface = colors.bubbleOther,
            onSurface = colors.text,
            surfaceVariant = colors.neutral,
            onSurfaceVariant = colors.system,
            outline = colors.border,
            error = colors.error,
        )
    } else {
        lightColorScheme(
            primary = colors.accent,
            onPrimary = colors.accentText,
            background = colors.windowBg,
            onBackground = colors.text,
            surface = colors.bubbleOther,
            onSurface = colors.text,
            surfaceVariant = colors.neutral,
            onSurfaceVariant = colors.system,
            outline = colors.border,
            error = colors.error,
        )
    }

    CompositionLocalProvider(LocalDchatColors provides colors) {
        MaterialTheme(colorScheme = scheme, content = content)
    }
}
