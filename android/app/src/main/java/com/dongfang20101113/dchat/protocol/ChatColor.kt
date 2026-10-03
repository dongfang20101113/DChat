package com.dongfang20101113.dchat.protocol

import androidx.compose.ui.graphics.Color

/**
 * 聊天文字里的彩色代码（和桌面端 src/chat_color.cpp 是同一套规则，两边必须一致）。
 *
 * 两套写法，可以混着用：
 *  - `#RRGGBB` 十六进制色码，例：`#ff0000红色文字`
 *  - `&a` ~ `&f` 单色种快捷码，例：`&c红色`
 *  - `&&` 表示一个字面的 `&`；写成 `&z` 这种认不出来的**原样显示**（不要悄悄吞掉用户的字）
 *
 * 全部是纯函数，方便单测。
 */
object ChatColor {

    /** 快捷色码一共 16 个：&0 ~ &9 与 &a ~ &f（&a 起是亮色）。 */
    const val QUICK_COLOR_COUNT = 16

    /** 一条消息里最多解析多少个"上色片段"，防止有人发几千个色码把排版拖慢。 */
    const val MAX_SPANS = 256

    /**
     * 16 个快捷色，顺序和常见聊天软件的习惯一致（0-7 暗色，8-15 亮色）。
     * 数值必须和桌面端 `src/chat_color.cpp` 里的 `kQuickColors` **一模一样**，
     * 否则同一句话在两端颜色不同。
     */
    private val QUICK_COLORS = intArrayOf(
        0xFF000000.toInt(), // &0 黑
        0xFF0000AA.toInt(), // &1 暗蓝
        0xFF00AA00.toInt(), // &2 暗绿
        0xFF00AAAA.toInt(), // &3 暗青
        0xFFAA0000.toInt(), // &4 暗红
        0xFFAA00AA.toInt(), // &5 暗紫
        0xFFAA5500.toInt(), // &6 暗黄（棕）
        0xFFAAAAAA.toInt(), // &7 浅灰
        0xFF555555.toInt(), // &8 深灰
        0xFF5555FF.toInt(), // &9 亮蓝
        0xFF55FF55.toInt(), // &a 亮绿
        0xFF55FFFF.toInt(), // &b 亮青
        0xFFFF5555.toInt(), // &c 亮红
        0xFFFF55FF.toInt(), // &d 亮紫
        0xFFFFFF55.toInt(), // &e 亮黄
        0xFFFFFFFF.toInt(), // &f 白
    )

    /** 快捷色对应的 Compose 颜色。 */
    fun quickColor(index: Int): Color =
        Color(if (index in 0 until QUICK_COLOR_COUNT) QUICK_COLORS[index] else 0xFFFFFFFF.toInt())

    /** 快捷色码的字符（`0`-`9` `a`-`f`），越界返回 null。 */
    fun quickColorDigit(index: Int): Char? = when {
        index in 0..9 -> '0' + index
        index in 10..15 -> 'a' + (index - 10)
        else -> null
    }

    /** 一段要画的文字，以及它自己的颜色。 */
    data class Span(val text: String, val color: Color, val hasColor: Boolean)

    /**
     * 解析一段文字里的色码。
     *
     * [enabled] 为 false 时**不做任何解析**：色码原样留在文字里被完整显示。
     * 这是刻意的——服务器关掉彩色聊天后，用户打进去的色码不该凭空消失，
     * 否则他会以为自己的字被吃掉了。
     */
    fun parse(
        text: String,
        defaultColor: Color,
        enabled: Boolean,
        maxSpans: Int = MAX_SPANS,
    ): List<Span> {
        if (!enabled) {
            // 服务器关了彩色聊天：一个字符都不动
            return listOf(Span(text, defaultColor, false))
        }
        val spans = ArrayList<Span>()
        var current = StringBuilder()
        var currentColor = defaultColor
        var currentHasColor = false

        fun flush() {
            if (current.isNotEmpty()) {
                spans.add(Span(current.toString(), currentColor, currentHasColor))
                current = StringBuilder()
            }
        }

        var i = 0
        while (i < text.length) {
            val matched = matchCode(text, i)
            if (matched != null) {
                if (matched.length == 2 && text[i] == '&' && text[i + 1] == '&') {
                    current.append('&') // 转义：显示一个 &
                    i += matched.length
                    continue
                }
                // 色码本身不显示，只换色。
                // 到上限之后**不再新建片段**，但解析继续——剩下的文字仍要按色码切掉，
                // 并进最后一段里。上限是防排版被拖慢的，不该改变显示出来的内容。
                if (spans.size < maxSpans) {
                    flush()
                    currentColor = matched.color
                    currentHasColor = true
                }
                i += matched.length
                continue
            }
            current.append(text[i])
            i++
        }
        flush()
        if (spans.isEmpty()) spans.add(Span("", defaultColor, false))
        return spans
    }

    private class Match(val length: Int, val color: Color)

    /** 从 [pos] 开始认一个色码；认不出来返回 null（那就原样显示）。 */
    private fun matchCode(text: String, pos: Int): Match? {
        if (pos >= text.length) return null
        when (text[pos]) {
            '&' -> {
                if (pos + 1 >= text.length) return null
                val next = text[pos + 1]
                if (next == '&') return Match(2, Color.Unspecified) // 转义，颜色不变
                val index = when (next) {
                    in '0'..'9' -> next - '0'
                    in 'a'..'f' -> next - 'a' + 10
                    in 'A'..'F' -> next - 'A' + 10
                    else -> return null // 认不出来就原样显示
                }
                return Match(2, quickColor(index))
            }
            '#' -> {
                if (pos + 6 >= text.length) return null
                var value = 0
                for (k in 1..6) {
                    val digit = hexValue(text[pos + k]) ?: return null
                    value = value * 16 + digit
                }
                return Match(7, Color(0xFF000000.toInt() or value))
            }
            else -> return null
        }
    }

    private fun hexValue(c: Char): Int? = when (c) {
        in '0'..'9' -> c - '0'
        in 'a'..'f' -> c - 'a' + 10
        in 'A'..'F' -> c - 'A' + 10
        else -> null
    }

    /** 把文字里所有的色码**去掉**，只留可见文字（用于未读提示、通知这类纯文本场合）。 */
    fun stripCodes(text: String): String {
        val out = StringBuilder()
        var i = 0
        while (i < text.length) {
            val matched = matchCode(text, i)
            if (matched != null) {
                if (matched.length == 2 && text[i] == '&' && text[i + 1] == '&') {
                    out.append('&')
                }
                i += matched.length
                continue
            }
            out.append(text[i])
            i++
        }
        return out.toString()
    }

    /** 把颜色转成 `#rrggbb`（小写，和用户手打的写法一致）。 */
    fun toHex(color: Color): String {
        val r = (color.red * 255f + 0.5f).toInt().coerceIn(0, 255)
        val g = (color.green * 255f + 0.5f).toInt().coerceIn(0, 255)
        val b = (color.blue * 255f + 0.5f).toInt().coerceIn(0, 255)
        return String.format("#%02x%02x%02x", r, g, b)
    }

    /** 解析用户输入的色码（`#rrggbb` / `rrggbb` / `#rgb`），失败返回 null。 */
    fun parseHex(text: String): Color? {
        var body = text.trim()
        if (body.startsWith("#")) body = body.substring(1)
        if (body.length == 3) {
            // #abc -> #aabbcc
            body = buildString { for (c in body) { append(c); append(c) } }
        }
        if (body.length != 6) return null
        var value = 0
        for (c in body) {
            val digit = hexValue(c) ?: return null
            value = value * 16 + digit
        }
        return Color(0xFF000000.toInt() or value)
    }

    /** 色板里的一格。 */
    data class Swatch(val color: Color, val name: String)

    /**
     * 取色盘用的色板：和桌面端一致（HSV 色相环 + 明度饱和度方块），
     * 这里只给"常用色"那一排和帮助文本用。
     */
    val palette: List<Swatch> = listOf(
        Swatch(Color(0xFF000000), "黑"), Swatch(Color(0xFF404040), "深灰"),
        Swatch(Color(0xFF808080), "灰"), Swatch(Color(0xFFC0C0C0), "浅灰"),
        Swatch(Color(0xFFFFFFFF), "白"), Swatch(Color(0xFF800000), "暗红"),
        Swatch(Color(0xFFFF0000), "红"), Swatch(Color(0xFFFF8080), "粉红"),
        Swatch(Color(0xFFFF8000), "橙"), Swatch(Color(0xFFFFC000), "琥珀"),
        Swatch(Color(0xFFFFFF00), "黄"), Swatch(Color(0xFF808000), "橄榄"),
        Swatch(Color(0xFF008000), "深绿"), Swatch(Color(0xFF00FF00), "绿"),
        Swatch(Color(0xFF80FF80), "浅绿"), Swatch(Color(0xFF008080), "青"),
        Swatch(Color(0xFF00FFFF), "亮青"), Swatch(Color(0xFF80FFFF), "淡青"),
        Swatch(Color(0xFF000080), "深蓝"), Swatch(Color(0xFF0000FF), "蓝"),
        Swatch(Color(0xFF8080FF), "浅蓝"), Swatch(Color(0xFF800080), "紫"),
        Swatch(Color(0xFFFF00FF), "品红"), Swatch(Color(0xFFFF80FF), "浅紫"),
    )

    /** `/chatcolor help` 要显示的内容。 */
    fun helpText(): String {
        val out = StringBuilder()
        out.append("彩色文字用法：\n")
        out.append("  #rrggbb   十六进制色码，例：#ff0000这是红字\n")
        out.append("  &0~&f     快捷色码（16 色），例：&c这是红字\n")
        out.append("  色码之后一直到下一个色码为止，都是那个颜色\n")
        out.append("  &&        想显示一个 & 就写两个\n")
        out.append("快捷色码对照：")
        for (i in 0 until QUICK_COLOR_COUNT) {
            out.append("\n  &").append(quickColorDigit(i)).append("  ").append(toHex(quickColor(i)))
        }
        out.append("\n更多颜色：/chatcolor choose（弹出取色盘）")
        return out.toString()
    }

    // ---- HSV <-> RGB：取色盘要用（和桌面端 color_picker.cpp 同一套换算）----

    /** 色相 h ∈ [0,360)，饱和度 s / 明度 v ∈ [0,1]。 */
    fun hsvToColor(h: Double, s: Double, v: Double): Color {
        val hue = ((h % 360.0) + 360.0) % 360.0
        val sat = s.coerceIn(0.0, 1.0)
        val value = v.coerceIn(0.0, 1.0)
        val c = value * sat
        val hp = hue / 60.0
        val x = c * (1.0 - kotlin.math.abs((hp % 2.0) - 1.0))
        val (r, g, b) = when {
            hp < 1 -> Triple(c, x, 0.0)
            hp < 2 -> Triple(x, c, 0.0)
            hp < 3 -> Triple(0.0, c, x)
            hp < 4 -> Triple(0.0, x, c)
            hp < 5 -> Triple(x, 0.0, c)
            else -> Triple(c, 0.0, x)
        }
        val m = value - c
        return Color(
            red = (r + m).toFloat().coerceIn(0f, 1f),
            green = (g + m).toFloat().coerceIn(0f, 1f),
            blue = (b + m).toFloat().coerceIn(0f, 1f),
        )
    }

    /** 返回 Triple(色相, 饱和度, 明度)。 */
    fun colorToHsv(color: Color): Triple<Double, Double, Double> {
        val r = color.red.toDouble()
        val g = color.green.toDouble()
        val b = color.blue.toDouble()
        val maxValue = maxOf(r, g, b)
        val minValue = minOf(r, g, b)
        val delta = maxValue - minValue
        var hue = 0.0
        if (delta > 1e-9) {
            hue = when (maxValue) {
                r -> 60.0 * (((g - b) / delta) % 6.0)
                g -> 60.0 * (((b - r) / delta) + 2.0)
                else -> 60.0 * (((r - g) / delta) + 4.0)
            }
            if (hue < 0) hue += 360.0
        }
        val sat = if (maxValue > 1e-9) delta / maxValue else 0.0
        return Triple(hue, sat, maxValue)
    }

    /** ANSI 那 16 个快捷色在深色底上的可读性可能不够，这里只用于取色盘预览。 */
    fun readableOn(color: Color): Color =
        if (color.luminance() > 0.55f) Color(0xFF141414) else Color(0xFFF0F0F0)

    private fun Color.luminance(): Float = 0.299f * red + 0.587f * green + 0.114f * blue
}
