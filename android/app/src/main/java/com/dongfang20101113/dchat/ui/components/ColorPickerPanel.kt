package com.dongfang20101113.dchat.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.dongfang20101113.dchat.protocol.ChatColor
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors
import kotlin.math.atan2
import kotlin.math.hypot

/**
 * 取色盘：外圈色相环 + 中间饱和度/明度方块 + 常用色 + 十六进制输入。
 *
 * 用的是**触摸事件**（`awaitEachGesture` + `awaitFirstDown`），不是 `detectDragGestures`——
 * 后者在 Robolectric 里根本收不到手势（这个项目已知：原始 MotionEvent 送不到 Compose）。
 * 逻辑尽量放在纯函数里（[ChatColor.hsvToColor] 等），手势只负责把坐标换算成颜色。
 */
@Composable
fun ColorPickerPanel(
    initial: Color,
    onPicked: (Color) -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = LocalDchatColors.current
    var hue by remember { mutableStateOf(ChatColor.colorToHsv(initial).first) }
    var sat by remember { mutableStateOf(ChatColor.colorToHsv(initial).second) }
    var value by remember { mutableStateOf(ChatColor.colorToHsv(initial).third) }
    var current by remember { mutableStateOf(initial) }
    var hexText by remember { mutableStateOf(ChatColor.toHex(initial)) }

    fun syncFromColor(color: Color) {
        val (h, s, v) = ChatColor.colorToHsv(color)
        hue = h
        sat = s
        value = v
        current = color
        hexText = ChatColor.toHex(color)
    }

    val wheelSize: Dp = 240.dp
    val ringWidth: Dp = 30.dp
    val squareRatio = 0.68f

    Column(modifier = modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
        Text("选择颜色", color = colors.text, fontSize = 18.sp, fontWeight = FontWeight.Medium)

        // ---- 色相环 + 明度/饱和度方块 ----
        Box(
            modifier = Modifier
                .size(wheelSize)
                .align(Alignment.CenterHorizontally)
                .pointerInput(Unit) {
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        val center = Offset(size.width / 2f, size.height / 2f)
                        val outer = size.width / 2f
                        val inner = outer - ringWidth.toPx()
                        val squareSide = size.width * squareRatio
                        val squareLeft = center.x - squareSide / 2f
                        val squareTop = center.y - squareSide / 2f

                        fun apply(point: Offset) {
                            val dx = point.x - center.x
                            val dy = point.y - center.y
                            val distance = hypot(dx, dy)
                            if (distance in inner..outer) {
                                // 环上：色相由角度定，饱和度和明度取满
                                val degrees = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble()))
                                hue = ((degrees % 360.0) + 360.0) % 360.0
                                sat = 1.0
                                value = 1.0
                            } else if (point.x in squareLeft..(squareLeft + squareSide) &&
                                point.y in squareTop..(squareTop + squareSide)
                            ) {
                                sat = ((point.x - squareLeft) / squareSide).toDouble().coerceIn(0.0, 1.0)
                                // 画布 y 向下，越往上越亮
                                value = (1.0 - (point.y - squareTop) / squareSide).toDouble()
                                    .coerceIn(0.0, 1.0)
                            } else {
                                return
                            }
                            current = ChatColor.hsvToColor(hue, sat, value)
                            hexText = ChatColor.toHex(current)
                        }

                        apply(down.position)
                        // 按住拖动连续取色
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull { it.id == down.id } ?: break
                            apply(change.position)
                            change.consume()
                        }
                    }
                },
        ) {
            Canvas(modifier = Modifier.fillMaxWidth().height(wheelSize)) {
                val center = Offset(size.width / 2f, size.height / 2f)
                val outer = size.width / 2f
                val inner = outer - ringWidth.toPx()

                // 色相环：切成 360 个小扇形（每片 1 度多一点点，互相重叠避免缝）
                val steps = 360
                for (i in 0 until steps) {
                    val from = i.toFloat()
                    val sweep = 360f / steps * 1.6f
                    val color = ChatColor.hsvToColor(from.toDouble(), 1.0, 1.0)
                    drawArc(
                        color = color,
                        startAngle = from,
                        sweepAngle = sweep,
                        useCenter = false,
                        topLeft = Offset(center.x - outer, center.y - outer),
                        size = Size(outer * 2, outer * 2),
                        style = Stroke(width = ringWidth.toPx()),
                    )
                }

                // 明度/饱和度方块：横向 白->纯色，纵向 亮->暗
                val side = size.width * squareRatio
                val left = center.x - side / 2f
                val top = center.y - side / 2f
                val pure = ChatColor.hsvToColor(hue, 1.0, 1.0)
                drawRect(
                    brush = Brush.horizontalGradient(
                        listOf(Color.White, pure),
                        startX = left,
                        endX = left + side,
                    ),
                    topLeft = Offset(left, top),
                    size = Size(side, side),
                )
                drawRect(
                    brush = Brush.verticalGradient(
                        listOf(Color.Transparent, Color.Black),
                        startY = top,
                        endY = top + side,
                    ),
                    topLeft = Offset(left, top),
                    size = Size(side, side),
                )

                // 环上的标记
                val marker = ChatColor.hsvToColor(hue, 1.0, 1.0)
                val radians = Math.toRadians(hue)
                val ringRadius = (outer + inner) / 2f
                val markCenter = Offset(
                    center.x + (kotlin.math.cos(radians) * ringRadius).toFloat(),
                    center.y + (kotlin.math.sin(radians) * ringRadius).toFloat(),
                )
                drawCircle(Color.White, radius = 8f, center = markCenter, style = Stroke(3f))
                drawCircle(Color.Black.copy(alpha = 0.5f), radius = 10f, center = markCenter, style = Stroke(1.5f))
                // 方块里的标记
                val squareMark = Offset(
                    left + (sat.toFloat() * side),
                    top + ((1f - value.toFloat()) * side),
                )
                drawCircle(Color.White, radius = 7f, center = squareMark, style = Stroke(3f))
                drawCircle(Color.Black.copy(alpha = 0.5f), radius = 9f, center = squareMark, style = Stroke(1.5f))
            }
        }

        // ---- 当前色 / 原色对比 ----
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            ColorChip("原颜色", initial, Modifier.weight(1f))
            ColorChip("新颜色", current, Modifier.weight(1f))
        }

        // ---- 常用色 ----
        Text("常用色", color = colors.system, fontSize = 12.sp)
        LazyVerticalGrid(
            columns = GridCells.Fixed(8),
            modifier = Modifier.fillMaxWidth().height(72.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            items(ChatColor.palette) { swatch ->
                Box(
                    modifier = Modifier
                        .size(28.dp)
                        .clip(RoundedCornerShape(6.dp))
                        .background(swatch.color)
                        .border(1.dp, colors.border, RoundedCornerShape(6.dp))
                        .clickable { syncFromColor(swatch.color) },
                )
            }
        }

        // ---- 确定 ----
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(44.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(colors.accent)
                .clickable { onPicked(current) },
            contentAlignment = Alignment.Center,
        ) {
            Text("确定", color = colors.accentText, fontSize = 16.sp)
        }
        Text(
            text = "色码 ${ChatColor.toHex(current)}（确定后放进输入框，按发送才发出去）",
            color = colors.system,
            fontSize = 12.sp,
        )
    }
}

@Composable
private fun ColorChip(label: String, color: Color, modifier: Modifier = Modifier) {
    val colors = LocalDchatColors.current
    Column(modifier = modifier) {
        Text(label, color = colors.system, fontSize = 12.sp)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(36.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(color)
                .border(1.dp, colors.border, RoundedCornerShape(8.dp)),
        )
    }
}
