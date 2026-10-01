package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.ui.layout.MIN_TOUCH_TARGET_DP
import com.dongfang20101113.dchat.ui.layout.MessageAlign
import com.dongfang20101113.dchat.ui.layout.WindowSizeClass
import com.dongfang20101113.dchat.ui.layout.announceMaxWidthDp
import com.dongfang20101113.dchat.ui.layout.chatMetrics
import com.dongfang20101113.dchat.ui.layout.noticeMaxWidthDp
import com.dongfang20101113.dchat.ui.layout.placeBubble
import com.dongfang20101113.dchat.ui.layout.shouldCompressListForIme
import com.dongfang20101113.dchat.ui.layout.topBarHeightDp
import com.dongfang20101113.dchat.ui.layout.windowSizeClassOf
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 界面尺寸决策测试 —— **这是"手机屏幕不适配"的正面回答**。
 *
 * 桌面版把窗口写死 940×660、字号写死 -16 像素；搬到手机上必然崩。
 * 这里把尺寸决策做成纯函数，于是**几十种真实屏幕规格全部能在 JVM 上验证**，
 * 不需要真机、不需要模拟器。
 */
class ChatLayoutTest {

    /** 市面主流机型 + 平板 + 折叠屏的可用宽度（dp）。 */
    private val realWorldWidths = listOf(
        320,  // 很老的手机 / 小屏
        360,  // 常见入门安卓
        375,  // iPhone SE 级别
        393,  // Pixel 常见
        411,  // Pixel 大屏
        430,  // iPhone Pro Max 级别
        480,  // 大屏手机横屏前的上限
        600,  // 平板/折叠展开的断点
        673,  // 折叠屏展开
        720,  // 小平板
        800,  // 8 寸平板
        840,  // 大平板断点
        1024, // iPad 竖屏
        1280, // 平板横屏
        1600, // 大屏/桌面模式
    )

    private fun heightFor(width: Int) = if (width <= 480) 800 else 1000

    // ------------------------------------------------------------------
    // 档位判定
    // ------------------------------------------------------------------

    @Test
    fun `档位断点与 Material 3 一致`() {
        assertEquals(WindowSizeClass.COMPACT, windowSizeClassOf(320))
        assertEquals(WindowSizeClass.COMPACT, windowSizeClassOf(599))
        assertEquals(WindowSizeClass.MEDIUM, windowSizeClassOf(600))
        assertEquals(WindowSizeClass.MEDIUM, windowSizeClassOf(839))
        assertEquals(WindowSizeClass.EXPANDED, windowSizeClassOf(840))
        assertEquals(WindowSizeClass.EXPANDED, windowSizeClassOf(1600))
    }

    // ------------------------------------------------------------------
    // 全尺寸不变量（最重要的一组）
    // ------------------------------------------------------------------

    @Test
    fun `任何屏幕宽度下_气泡都必须放得下且不越界`() {
        for (width in realWorldWidths) {
            val m = chatMetrics(width, heightFor(width))
            val needed = m.bubbleMaxWidthDp + m.horizontalMarginDp * 2

            assertTrue("宽 ${width}dp：气泡上限 ${m.bubbleMaxWidthDp} 太小", m.bubbleMaxWidthDp >= 120)
            assertTrue(
                "宽 ${width}dp：气泡 $needed 超出屏幕",
                needed <= width,
            )
        }
    }

    @Test
    fun `任何屏幕宽度下_触控目标都不小于 48dp`() {
        for (width in realWorldWidths) {
            val m = chatMetrics(width, heightFor(width))
            assertTrue(
                "宽 ${width}dp：输入框高度 ${m.inputMinHeightDp}dp 小于无障碍下限",
                m.inputMinHeightDp >= MIN_TOUCH_TARGET_DP,
            )
        }
    }

    @Test
    fun `任何屏幕宽度下_字号和边距都是正数`() {
        for (width in realWorldWidths) {
            val m = chatMetrics(width, heightFor(width))
            assertTrue(m.messageFontSp > 0)
            assertTrue(m.noticeFontSp > 0)
            assertTrue("公告要比正文大", m.announceFontSp > m.messageFontSp)
            assertTrue("系统提示要比正文小", m.noticeFontSp < m.messageFontSp)
            assertTrue(m.horizontalMarginDp > 0)
            assertTrue(m.bubblePaddingXDp > 0)
            assertTrue(m.bubblePaddingYDp > 0)
        }
    }

    @Test
    fun `极窄屏也不崩`() {
        // 折叠屏外屏、分屏模式可能只有 200dp 多
        for (width in listOf(200, 240, 280)) {
            val m = chatMetrics(width, 600)
            assertTrue("宽 $width：${m.bubbleMaxWidthDp}", m.bubbleMaxWidthDp >= 120)
            assertTrue(m.bubbleMaxWidthDp + m.horizontalMarginDp * 2 <= width)
        }
    }

    @Test
    fun `宽度非法时直接报错_而不是算出一个荒唐的结果`() {
        var threw = false
        try {
            chatMetrics(0, 800)
        } catch (_: IllegalArgumentException) {
            threw = true
        }
        assertTrue("宽度 0 应该抛异常", threw)
    }

    // ------------------------------------------------------------------
    // 桌面版的老毛病：写死尺寸 / 平板上气泡过宽
    // ------------------------------------------------------------------

    @Test
    fun `平板上气泡不会横跨整屏_这是桌面端只按百分比算会犯的错`() {
        val phone = chatMetrics(393, 850)
        val tablet = chatMetrics(1600, 1000)

        assertTrue("手机上气泡不该超过屏宽", phone.bubbleMaxWidthDp <= 393)
        assertTrue(
            "1600dp 宽的屏幕上气泡上限 ${tablet.bubbleMaxWidthDp} 太大了，一行会难以阅读",
            tablet.bubbleMaxWidthDp <= 500,
        )
    }

    @Test
    fun `气泡上限随屏幕变宽而增加_但不是线性增长`() {
        val compact = chatMetrics(393, 850).bubbleMaxWidthDp
        val expanded = chatMetrics(1024, 1000).bubbleMaxWidthDp
        assertTrue("大屏上限应更大", expanded > compact)
        // 宽度涨了 2.6 倍，气泡上限不该跟着涨 2.6 倍
        assertTrue("上限增长必须被夹住", expanded < compact * 2)
    }

    // ------------------------------------------------------------------
    // 分栏
    // ------------------------------------------------------------------

    @Test
    fun `只有大屏才做左右分栏`() {
        assertFalse("手机不做分栏", chatMetrics(393, 850).useTwoPane)
        assertFalse("中等宽度不做分栏", chatMetrics(720, 1000).useTwoPane)
        assertTrue("平板做分栏", chatMetrics(1024, 800).useTwoPane)
        assertTrue("大屏分栏", chatMetrics(1600, 1000).useTwoPane)
    }

    @Test
    fun `分栏时气泡上限要按聊天区宽度算_不能按整屏宽度`() {
        val tablet = chatMetrics(1024, 800)
        assertTrue(tablet.useTwoPane)
        val paneWidth = tablet.memberPaneWidthDp!!
        val chatArea = 1024 - paneWidth
        val needed = tablet.bubbleMaxWidthDp + tablet.horizontalMarginDp * 2
        assertTrue(
            "气泡 $needed 超过了聊天区宽度 $chatArea",
            needed <= chatArea,
        )
    }

    @Test
    fun `公告和系统提示的宽度按可用宽度的比例算_对齐桌面端常量`() {
        for (width in realWorldWidths) {
            val m = chatMetrics(width, heightFor(width))

            // 桌面端 kAnnounceMaxPercent = 88 / kNoticeMaxPercent = 70
            assertEquals((width * 88) / 100, m.announceMaxWidthDp())
            assertEquals((width * 70) / 100, m.noticeMaxWidthDp())

            // 关键：不能超过屏幕可用宽度（这是"拿气泡上限乘倍数"那种写法会犯的错）
            assertTrue(
                "宽 ${width}dp：公告宽度 ${m.announceMaxWidthDp()} 越界",
                m.announceMaxWidthDp() <= width,
            )
            assertTrue(
                "宽 ${width}dp：提示宽度 ${m.noticeMaxWidthDp()} 越界",
                m.noticeMaxWidthDp() <= width,
            )
            assertTrue("公告应比系统提示宽", m.announceMaxWidthDp() > m.noticeMaxWidthDp())
        }
    }

    @Test
    fun `availableWidthDp 就是传入的可用宽度`() {
        for (width in realWorldWidths) {
            assertEquals(width, chatMetrics(width, heightFor(width)).availableWidthDp)
        }
    }

    // ------------------------------------------------------------------
    // 字体缩放
    // ------------------------------------------------------------------

    @Test
    fun `系统字号放大时气泡适度收窄`() {
        val normal = chatMetrics(393, 850, fontScale = 1.0f)
        val large = chatMetrics(393, 850, fontScale = 1.5f)
        val huge = chatMetrics(393, 850, fontScale = 2.0f)

        assertTrue("字号放大后气泡仍要放得下", huge.bubbleMaxWidthDp >= 120)
        assertTrue("字号放大后气泡不该变宽", large.bubbleMaxWidthDp <= normal.bubbleMaxWidthDp)
        assertTrue("字号越大气泡越窄（避免一行只有三四个字）", huge.bubbleMaxWidthDp <= large.bubbleMaxWidthDp)
        assertTrue("但不能窄到没法看", huge.bubbleMaxWidthDp >= normal.bubbleMaxWidthDp * 0.6)
    }

    @Test
    fun `字号极端值不会算出越界结果`() {
        for (scale in listOf(0.5f, 0.8f, 1.0f, 1.3f, 2.0f, 3.0f)) {
            val m = chatMetrics(393, 850, scale)
            assertTrue("scale=$scale 上限=${m.bubbleMaxWidthDp}", m.bubbleMaxWidthDp >= 120)
            assertTrue("scale=$scale 越界", m.bubbleMaxWidthDp + m.horizontalMarginDp * 2 <= 393)
        }
    }

    // ------------------------------------------------------------------
    // 横竖屏
    // ------------------------------------------------------------------

    @Test
    fun `手机横屏时竖直间距收紧_让一屏能多显示几条`() {
        val portrait = chatMetrics(360, 780)   // 竖屏
        val landscape = chatMetrics(599, 320)  // 横屏（宽度仍在 COMPACT 区间内）
        assertTrue("横屏间距应更紧凑", landscape.rowGapDp <= portrait.rowGapDp)
        assertTrue("横屏气泡内边距应更紧凑", landscape.bubblePaddingYDp <= portrait.bubblePaddingYDp)
    }

    @Test
    fun `横屏不会把气泡上限算到超出屏幕`() {
        val landscape = chatMetrics(780, 360)
        assertTrue(landscape.bubbleMaxWidthDp + landscape.horizontalMarginDp * 2 <= 780)
    }

    // ------------------------------------------------------------------
    // 气泡摆放
    // ------------------------------------------------------------------

    private val viewport = 400
    private val margin = 8
    private val padX = 12
    private val padY = 8
    private val maxBubble = 300

    @Test
    fun `自己的消息贴右边距`() {
        val box = placeBubble(viewport, 100, 40, MessageAlign.RIGHT, padX, padY, margin, maxBubble)
        assertEquals("右边缘应贴住 margin", viewport - margin, box.right)
        assertEquals("左边不能越过 margin", true, box.left >= margin)
    }

    @Test
    fun `别人的消息贴左边距`() {
        val box = placeBubble(viewport, 100, 40, MessageAlign.LEFT, padX, padY, margin, maxBubble)
        assertEquals(margin, box.left)
    }

    @Test
    fun `系统提示居中`() {
        val box = placeBubble(viewport, 100, 40, MessageAlign.CENTER, padX, padY, margin, maxBubble)
        val expectedLeft = (viewport - box.width) / 2
        assertTrue("居中偏差过大：${box.left} vs $expectedLeft", kotlin.math.abs(box.left - expectedLeft) <= 1)
    }

    @Test
    fun `超长文字被夹到宽度上限`() {
        val box = placeBubble(viewport, 100000, 40, MessageAlign.LEFT, padX, padY, margin, maxBubble)
        assertTrue("气泡宽度 ${box.width} 超过上限 $maxBubble", box.width <= maxBubble)
    }

    @Test
    fun `气泡高度等于文字高度加内边距`() {
        val box = placeBubble(viewport, 100, 40, MessageAlign.LEFT, padX, padY, margin, maxBubble)
        assertEquals(40 + padY * 2, box.height)
        assertEquals(100 + padX * 2, box.width)
    }

    @Test
    fun `极短文字也有最小可见宽度`() {
        val box = placeBubble(viewport, 0, 20, MessageAlign.LEFT, padX, padY, margin, maxBubble)
        assertTrue("空文字也要有个能看见的气泡", box.width > padX)
    }

    @Test
    fun `气泡在极窄视口里不会跑到屏幕外`() {
        val narrow = 150
        for (align in MessageAlign.entries) {
            val box = placeBubble(narrow, 100, 40, align, padX, padY, margin, maxBubble)
            assertTrue("$align: left=${box.left} 越界", box.left >= 0)
            assertTrue("$align: right=${box.right} 越界", box.right <= narrow || box.left == margin)
        }
    }

    // ------------------------------------------------------------------
    // 键盘（IME）避让
    // ------------------------------------------------------------------

    @Test
    fun `键盘占用超过四成高度时才压缩列表`() {
        assertFalse(shouldCompressListForIme(screenHeightDp = 800, imeBottomInsetDp = 300))
        assertTrue(shouldCompressListForIme(screenHeightDp = 800, imeBottomInsetDp = 400))
        assertFalse("没有键盘就不压缩", shouldCompressListForIme(screenHeightDp = 800, imeBottomInsetDp = 0))
    }

    @Test
    fun `屏幕高度非法时不崩`() {
        assertFalse(shouldCompressListForIme(0, 100))
    }

    @Test
    fun `矮屏加键盘时把顶栏收掉_把空间让给内容`() {
        assertEquals(56, topBarHeightDp(WindowSizeClass.COMPACT, 800, imeVisible = false))
        assertEquals("矮屏+键盘应收起顶栏", 0, topBarHeightDp(WindowSizeClass.COMPACT, 600, imeVisible = true))
        assertEquals("高屏不需要收起", 56, topBarHeightDp(WindowSizeClass.COMPACT, 900, imeVisible = true))
        assertEquals(64, topBarHeightDp(WindowSizeClass.EXPANDED, 1000, imeVisible = false))
    }
}
