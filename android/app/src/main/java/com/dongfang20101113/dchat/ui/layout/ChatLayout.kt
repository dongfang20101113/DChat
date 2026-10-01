package com.dongfang20101113.dchat.ui.layout

/**
 * 聊天界面的**尺寸决策**，全部是纯函数 —— 不引用任何 Android API。
 *
 * ## 为什么要单独抽出来
 *
 * 桌面版 dchat 的界面是**写死像素**的：窗口固定 940×660，字号 `-16` 像素，气泡最大宽度是视口的
 * 固定百分比。那套东西搬到手机上必然崩：手机宽度只有 360~430dp，平板能到 1280dp，
 * 横竖屏还会翻转，系统字号还能被用户调大。
 *
 * 所以这里把"**给多大的屏幕、用多大的尺寸**"变成可以**脱离设备做单元测试**的纯函数：
 * 传进宽高（dp）和字体缩放，算出全部尺寸。这样几十种屏幕规格都能在 JVM 上验证，
 * 而不是"装到某台手机上看着还行"。
 *
 * ## 三条硬规则（防止重犯桌面端的问题）
 *
 * 1. **一律用 dp / sp，不用像素**。dp 会随屏幕密度自动换算，sp 还会跟随系统字号设置。
 * 2. **宽度用"百分比 + 绝对上限"双约束**：手机上取百分比（窄屏也要留边），
 *    平板上必须加上限，否则一行 40 个字的宽屏气泡极难读。
 * 3. **触控目标不小于 48dp**：手机用手指点，桌面端那些 20 像素高的按钮直接照搬会点不中。
 */

/** 屏幕宽度档位（对齐 Material 3 的 window size class 断点）。 */
enum class WindowSizeClass {
    /** 手机竖屏：< 600dp */
    COMPACT,

    /** 手机横屏 / 小折叠展开 / 小平板：600 ~ 839dp */
    MEDIUM,

    /** 平板 / 桌面模式：>= 840dp */
    EXPANDED,
}

/** 气泡对齐方式。 */
enum class MessageAlign { LEFT, RIGHT, CENTER }

/** Android 无障碍指南要求的最小触控目标。 */
const val MIN_TOUCH_TARGET_DP: Int = 48

/** 按宽度判定档位。 */
fun windowSizeClassOf(widthDp: Int): WindowSizeClass = when {
    widthDp < 600 -> WindowSizeClass.COMPACT
    widthDp < 840 -> WindowSizeClass.MEDIUM
    else -> WindowSizeClass.EXPANDED
}

/**
 * 一次算好的全部界面尺寸。
 *
 * 每个字段单位都在名字里标了：`Dp` 是密度无关像素，`Sp` 是可缩放字号。
 */
data class ChatMetrics(
    val sizeClass: WindowSizeClass,

    /**
     * 真实可用宽度（dp，已扣掉刘海/状态栏/手势条）。
     *
     * 单独留这个值是为了让"公告占 88%、系统提示占 70%"这类**按比例的宽度**
     * 能照着实际可用宽度算，而不是拿气泡上限去乘一个倍数——
     * 后者在手机上会算出超过屏幕的值（被父约束夹住后变成满宽，与桌面端的 88% 不符）。
     */
    val availableWidthDp: Int,

    /** 气泡与屏幕左右边缘的最小距离。 */
    val horizontalMarginDp: Int,

    /** 气泡宽度**上限**（已经按视口宽度和绝对上限双重夹取过）。 */
    val bubbleMaxWidthDp: Int,

    /** 气泡内边距。 */
    val bubblePaddingXDp: Int,
    val bubblePaddingYDp: Int,

    /** 字号（sp）。Compose 会自动再乘系统字号缩放，这里不要重复乘。 */
    val messageFontSp: Int,
    val noticeFontSp: Int,
    val announceFontSp: Int,

    /** 两条消息之间的竖直间距。 */
    val rowGapDp: Int,

    /** 气泡上方"昵称 + 时间"表头的高度。 */
    val headerHeightDp: Int,

    /** 底部输入框的最小高度（不小于 [MIN_TOUCH_TARGET_DP]）。 */
    val inputMinHeightDp: Int,

    /** 头像大小；0 表示这个档位不显示头像（窄屏省地方）。 */
    val avatarSizeDp: Int,

    /** 成员列表宽度（dp）；null 表示不做左右分栏（手机上是抽屉/二级页）。 */
    val memberPaneWidthDp: Int?,

    /** 是否走"聊天 + 成员列表"左右分栏。 */
    val useTwoPane: Boolean,

    /** 单条消息最多显示几行正文后折叠（窄屏避免一条消息占满整屏）。 */
    val maxMessageLines: Int,
)

/** 计算气泡宽度上限时使用的绝对上限（防止平板上一行太长）。 */
private const val BUBBLE_ABS_MAX_COMPACT_DP = 300
private const val BUBBLE_ABS_MAX_MEDIUM_DP = 380
private const val BUBBLE_ABS_MAX_EXPANDED_DP = 420

/**
 * 算出当前屏幕该用的全部尺寸。
 *
 * @param widthDp 可用宽度（dp）。**要传"扣掉系统栏/刘海之后"的宽度**，
 *                调用方在 Compose 里用 `WindowInsets` 处理后再传进来。
 * @param heightDp 可用高度（dp），用于判断横屏（宽 > 高）。
 * @param fontScale 系统字体缩放（1.0 = 默认）。字大时收窄气泡上限，避免一行放不下几个字。
 */
fun chatMetrics(widthDp: Int, heightDp: Int, fontScale: Float = 1.0f): ChatMetrics {
    require(widthDp > 0) { "widthDp 必须为正数，实际传入 $widthDp" }

    val sizeClass = windowSizeClassOf(widthDp)
    val landscape = widthDp > heightDp

    val base = when (sizeClass) {
        WindowSizeClass.COMPACT -> ChatMetrics(
            sizeClass = sizeClass,
            availableWidthDp = widthDp,
            horizontalMarginDp = 8,
            bubbleMaxWidthDp = 0, // 下面统一算
            bubblePaddingXDp = 12,
            bubblePaddingYDp = 8,
            messageFontSp = 16,
            noticeFontSp = 12,
            announceFontSp = 20,
            rowGapDp = 12,
            headerHeightDp = 18,
            inputMinHeightDp = 48,
            avatarSizeDp = 0,             // 窄屏不显示头像，把宽度让给正文
            memberPaneWidthDp = null,
            useTwoPane = false,
            maxMessageLines = 40,
        )

        WindowSizeClass.MEDIUM -> ChatMetrics(
            sizeClass = sizeClass,
            availableWidthDp = widthDp,
            horizontalMarginDp = 16,
            bubbleMaxWidthDp = 0,
            bubblePaddingXDp = 14,
            bubblePaddingYDp = 10,
            messageFontSp = 16,
            noticeFontSp = 13,
            announceFontSp = 21,
            rowGapDp = 14,
            headerHeightDp = 20,
            inputMinHeightDp = 48,
            avatarSizeDp = 32,
            memberPaneWidthDp = null,
            useTwoPane = false,
            maxMessageLines = 60,
        )

        WindowSizeClass.EXPANDED -> ChatMetrics(
            sizeClass = sizeClass,
            availableWidthDp = widthDp,
            horizontalMarginDp = 24,
            bubbleMaxWidthDp = 0,
            bubblePaddingXDp = 16,
            bubblePaddingYDp = 10,
            messageFontSp = 16,
            noticeFontSp = 13,
            announceFontSp = 22,
            rowGapDp = 14,
            headerHeightDp = 20,
            inputMinHeightDp = 48,
            avatarSizeDp = 36,
            memberPaneWidthDp = 280,
            useTwoPane = true,
            maxMessageLines = 80,
        )
    }

    // —— 气泡宽度上限：百分比 + 绝对上限 双重夹取 ——
    // 桌面端只有百分比（kMaxBubblePercent = 62），在 1280dp 平板上会算出 793dp 的一行，
    // 人眼横跨这么长很难读；所以这里必须再压一个绝对上限。
    val percent = when (sizeClass) {
        WindowSizeClass.COMPACT -> 0.82
        WindowSizeClass.MEDIUM -> 0.72
        WindowSizeClass.EXPANDED -> 0.62
    }
    val absMax = when (sizeClass) {
        WindowSizeClass.COMPACT -> BUBBLE_ABS_MAX_COMPACT_DP
        WindowSizeClass.MEDIUM -> BUBBLE_ABS_MAX_MEDIUM_DP
        WindowSizeClass.EXPANDED -> BUBBLE_ABS_MAX_EXPANDED_DP
    }

    // 系统字号放大时，同样宽度能放的字变少 → 按缩放比例适度收窄上限，避免"一行只有三四个字"
    val fontCompensation = fontScale.coerceIn(0.85f, 2.0f).let { scale ->
        // 1.0 → 1.0；2.0 → 约 1.25（不是线性，避免字大时气泡窄得没法看）
        1f + (scale - 1f) * 0.25f
    }

    val usableWidth = widthDp - base.horizontalMarginDp * 2
    val byPercent = (usableWidth * percent / fontCompensation).toInt()
    val bubbleMax = byPercent.coerceAtMost((absMax / fontCompensation).toInt())
        .coerceAtLeast(120) // 兜底：再窄也要能放下几个字
        .coerceAtMost(usableWidth) // 不能超过可用宽度（超窄屏保护）

    // 分栏模式下，聊天区实际宽度要扣掉成员列表，气泡上限按聊天区的宽度重算
    val chatAreaWidth = if (base.useTwoPane && base.memberPaneWidthDp != null) {
        widthDp - base.memberPaneWidthDp
    } else {
        widthDp
    }
    val bubbleMaxFinal = if (chatAreaWidth != widthDp) {
        val chatUsable = chatAreaWidth - base.horizontalMarginDp * 2
        val chatByPercent = (chatUsable * percent / fontCompensation).toInt()
        minOf(bubbleMax, chatByPercent.coerceAtMost((absMax / fontCompensation).toInt()))
            .coerceAtLeast(120)
            .coerceAtMost(chatUsable)
    } else {
        bubbleMax
    }

    // 横屏手机：高度很矮，竖直间距收紧，让一屏能看到更多消息
    val tightened = if (landscape && sizeClass == WindowSizeClass.COMPACT) {
        base.copy(rowGapDp = 8, bubblePaddingYDp = 6, headerHeightDp = 16)
    } else {
        base
    }

    return tightened.copy(bubbleMaxWidthDp = bubbleMaxFinal)
}

// ---------------------------------------------------------------------------
// 按比例的宽度（与桌面端的常量对齐）
// ---------------------------------------------------------------------------

/** 全服公告最多占可用宽度的比例（桌面端 `kAnnounceMaxPercent = 88`）。 */
const val ANNOUNCE_MAX_PERCENT: Int = 88

/** 系统提示最多占可用宽度的比例（桌面端 `kNoticeMaxPercent = 70`）。 */
const val NOTICE_MAX_PERCENT: Int = 70

/** 公告的宽度上限（dp）。 */
fun ChatMetrics.announceMaxWidthDp(): Int =
    (availableWidthDp * ANNOUNCE_MAX_PERCENT) / 100

/** 系统提示的宽度上限（dp）。 */
fun ChatMetrics.noticeMaxWidthDp(): Int =
    (availableWidthDp * NOTICE_MAX_PERCENT) / 100

// ---------------------------------------------------------------------------
// 气泡摆放（对应桌面端 bubble.cpp 的 PlaceBubble）
// ---------------------------------------------------------------------------

/** 一个气泡的摆放结果，坐标相对消息行左上角。 */
data class BubbleBox(
    /** 气泡外框（含内边距）。 */
    val left: Int, val top: Int, val right: Int, val bottom: Int,
    /** 文字区域（气泡内部）。 */
    val textLeft: Int, val textTop: Int, val textRight: Int, val textBottom: Int,
) {
    val width: Int get() = right - left
    val height: Int get() = bottom - top
}

/**
 * 在给定视口宽度里摆放一个气泡。
 *
 * 自己的消息贴右、别人的贴左、系统提示居中——和桌面端一致。
 *
 * @param viewportWidth 可用宽度（dp/px 同一单位即可，本函数不做单位换算）
 * @param contentWidth 文字测量后的宽度
 * @param contentHeight 文字测量后的高度
 * @param maxBubbleWidth 气泡宽度上限（来自 [ChatMetrics.bubbleMaxWidthDp]）
 * @param margin 与视口左右边缘的最小距离
 */
fun placeBubble(
    viewportWidth: Int,
    contentWidth: Int,
    contentHeight: Int,
    align: MessageAlign,
    paddingX: Int,
    paddingY: Int,
    margin: Int,
    maxBubbleWidth: Int,
): BubbleBox {
    val innerMax = (maxBubbleWidth - paddingX * 2).coerceAtLeast(1)
    val innerWidth = contentWidth.coerceAtMost(innerMax).coerceAtLeast(1)
    val bubbleWidth = innerWidth + paddingX * 2
    val bubbleHeight = contentHeight + paddingY * 2

    val left = when (align) {
        MessageAlign.LEFT -> margin
        MessageAlign.RIGHT -> (viewportWidth - margin - bubbleWidth).coerceAtLeast(margin)
        MessageAlign.CENTER -> ((viewportWidth - bubbleWidth) / 2).coerceAtLeast(margin)
    }

    return BubbleBox(
        left = left,
        top = 0,
        right = left + bubbleWidth,
        bottom = bubbleHeight,
        textLeft = left + paddingX,
        textTop = paddingY,
        textRight = left + paddingX + innerWidth,
        textBottom = paddingY + contentHeight,
    )
}

/**
 * 底部输入区应该避让的高度（键盘弹出时）。
 *
 * `imeBottomInsetPx` 由 Compose 的 `WindowInsets.ime` 提供；这里只做"要不要把列表也抬起来"的判断，
 * 纯逻辑便于测试。
 *
 * 规则：键盘占用超过屏幕高度的 40% 时，认为用户是在"打字"，
 * 此时消息列表只保留顶部少量内容并自动滚到底，避免列表被压成一条缝还继续加载历史。
 */
fun shouldCompressListForIme(screenHeightDp: Int, imeBottomInsetDp: Int): Boolean =
    screenHeightDp > 0 && imeBottomInsetDp > (screenHeightDp * 0.4f)

/**
 * 计算"列表顶部要留出多少"——键盘弹出且屏幕很矮时，隐藏顶部工具栏把空间让给内容。
 */
fun topBarHeightDp(sizeClass: WindowSizeClass, screenHeightDp: Int, imeVisible: Boolean): Int {
    val base = if (sizeClass == WindowSizeClass.COMPACT) 56 else 64
    return if (imeVisible && screenHeightDp < 640) 0 else base
}
