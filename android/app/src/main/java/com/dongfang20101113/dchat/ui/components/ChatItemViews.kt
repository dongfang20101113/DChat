package com.dongfang20101113.dchat.ui.components

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.dongfang20101113.dchat.protocol.nickColorIndex
import com.dongfang20101113.dchat.ui.ChatItem
import com.dongfang20101113.dchat.ui.FileState
import com.dongfang20101113.dchat.ui.layout.ChatMetrics
import com.dongfang20101113.dchat.ui.layout.MessageAlign
import com.dongfang20101113.dchat.ui.layout.announceMaxWidthDp
import com.dongfang20101113.dchat.ui.layout.noticeMaxWidthDp
import com.dongfang20101113.dchat.ui.theme.LocalDchatColors

/**
 * 聊天记录里各种条目的绘制。
 *
 * 三种排布与桌面端一致：**自己靠右、别人靠左、系统提示居中**。
 * 宽度上限、内边距、字号全部来自 [ChatMetrics]（按屏幕尺寸算好的），**没有任何写死的像素**。
 */

/** 一条消息（气泡 + 表头）。 */
@Composable
fun SayRow(item: ChatItem.SayItem, metrics: ChatMetrics) {
    val colors = LocalDchatColors.current
    val say = item.info

    val align = if (say.own) MessageAlign.RIGHT else MessageAlign.LEFT
    val bubbleColor = when {
        say.mention -> colors.mention.copy(alpha = if (colors.isDark) 0.22f else 0.14f)
        say.own -> colors.bubbleOwn
        else -> colors.bubbleOther
    }
    val borderColor = when {
        say.mention -> colors.mention
        say.own -> colors.bubbleOwnBorder
        else -> colors.bubbleOtherBorder
    }
    val textColor = when {
        say.own -> colors.bubbleOwnText
        say.mention -> colors.mention
        else -> colors.text
    }

    Column(
        modifier = Modifier.fillMaxWidth().padding(vertical = metrics.rowGapDp.dp / 2),
        horizontalAlignment = when (align) {
            MessageAlign.RIGHT -> Alignment.End
            MessageAlign.LEFT -> Alignment.Start
            MessageAlign.CENTER -> Alignment.CenterHorizontally
        },
    ) {
        // 表头：别人显示"昵称 + 时间"，自己只显示时间（和桌面端一致）
        Row(
            modifier = Modifier.widthIn(max = metrics.bubbleMaxWidthDp.dp).fillMaxWidth(),
            horizontalArrangement = if (say.own) Arrangement.End else Arrangement.Start,
        ) {
            if (!say.own && say.nick.isNotEmpty()) {
                Text(
                    text = say.nick,
                    color = colors.nickColor(nickColorIndex(say.nick)),
                    fontSize = (metrics.noticeFontSp + 1).sp,
                    fontWeight = FontWeight.Medium,
                )
            }
            if (say.time.isNotEmpty()) {
                if (!say.own && say.nick.isNotEmpty()) Text("  ", fontSize = metrics.noticeFontSp.sp)
                Text(text = say.time, color = colors.time, fontSize = metrics.noticeFontSp.sp)
            }
        }

        Box(
            modifier = Modifier
                .widthIn(max = metrics.bubbleMaxWidthDp.dp)
                .background(bubbleColor, RoundedCornerShape(12.dp))
                .border(1.dp, borderColor, RoundedCornerShape(12.dp))
                .padding(
                    horizontal = metrics.bubblePaddingXDp.dp,
                    vertical = metrics.bubblePaddingYDp.dp,
                ),
        ) {
            Text(
                text = say.text,
                color = textColor,
                fontSize = metrics.messageFontSp.sp,
                // 长文本自动换行；超长单条由外层列表滚动，不做折叠（避免"点开才看得全"的困惑）
                lineHeight = (metrics.messageFontSp + 6).sp,
            )
        }
    }
}

/** 系统提示 / 错误 / 公告：居中的小胶囊，公告更大更醒目。 */
@Composable
fun NoticeRow(item: ChatItem.NoticeItem, metrics: ChatMetrics) {
    val colors = LocalDchatColors.current
    val notice = item.info

    val isAnnounce = notice.isAnnouncement
    val fontSize = when {
        isAnnounce -> metrics.announceFontSp
        else -> metrics.noticeFontSp
    }
    val bg = when {
        isAnnounce -> colors.announceBg
        notice.isError -> colors.error.copy(alpha = if (colors.isDark) 0.16f else 0.10f)
        else -> colors.noticeBg
    }
    val fg = when {
        isAnnounce -> colors.announceText
        notice.isError -> colors.error
        else -> colors.system
    }
    val border = when {
        isAnnounce -> colors.announceBorder
        notice.isError -> colors.error.copy(alpha = 0.5f)
        else -> colors.neutralBorder
    }
    val padX = if (isAnnounce) 18.dp else 10.dp
    val padY = if (isAnnounce) 10.dp else 5.dp

    // 公告占可用宽度 88%、系统提示 70%——直接对齐桌面端的
    // kAnnounceMaxPercent / kNoticeMaxPercent，而不是拿气泡上限乘一个倍数
    // （后者在手机上会算到超过屏幕、被父约束夹成满宽）。
    val maxWidth = (if (isAnnounce) metrics.announceMaxWidthDp() else metrics.noticeMaxWidthDp()).dp

    Column(
        modifier = Modifier.fillMaxWidth().padding(vertical = metrics.rowGapDp.dp / 3),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            modifier = Modifier
                .widthIn(max = maxWidth)
                .background(bg, RoundedCornerShape(if (isAnnounce) 12.dp else 10.dp))
                .border(1.dp, border, RoundedCornerShape(if (isAnnounce) 12.dp else 10.dp))
                .padding(horizontal = padX, vertical = padY),
        ) {
            Text(
                text = notice.text,
                color = fg,
                fontSize = fontSize.sp,
                fontWeight = if (isAnnounce) FontWeight.Medium else FontWeight.Normal,
                textAlign = TextAlign.Center,
                lineHeight = (fontSize + 6).sp,
            )
        }
        if (notice.time.isNotEmpty()) {
            Text(
                text = notice.time,
                color = colors.time,
                fontSize = metrics.noticeFontSp.sp,
                modifier = Modifier.padding(top = 2.dp),
            )
        }
    }
}

/**
 * 贴纸：下载完成后**内联画成大图**，而不是显示成文件卡片。
 *
 * 贴纸和普通文件走的是**完全相同**的传输（同一个文件通道），区别只在这里——
 * 用户看到的应该是"一张表情"，而不是"一个要下载的文件"。
 */
@Composable
fun StickerRow(item: ChatItem.FileItem, metrics: ChatMetrics, onRetry: () -> Unit) {
    val colors = LocalDchatColors.current

    Column(
        modifier = Modifier.fillMaxWidth().padding(vertical = metrics.rowGapDp.dp / 3),
        horizontalAlignment = Alignment.Start,
    ) {
        if (item.fromNick.isNotEmpty()) {
            Text(
                text = item.fromNick,
                color = colors.nickColor(nickColorIndex(item.fromNick)),
                fontSize = (metrics.noticeFontSp + 1).sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.padding(bottom = 4.dp),
            )
        }

        // 从磁盘读图。用 remember 缓存：每次重组都解码一遍会明显卡顿。
        // key 里带上 state，这样"下载完成"之后会重新读一次。
        val bitmap = remember(item.savedPath, item.state) {
            item.savedPath?.let { path ->
                runCatching { android.graphics.BitmapFactory.decodeFile(path) }.getOrNull()
            }
        }

        when {
            bitmap != null -> Image(
                bitmap = bitmap.asImageBitmap(),
                contentDescription = item.fileName,
                // Fit 而不是默认的 FillBounds：贴纸不该被拉变形
                contentScale = ContentScale.Fit,
                modifier = Modifier
                    .widthIn(max = metrics.bubbleMaxWidthDp.dp)
                    .heightIn(max = 180.dp),
            )

            item.state == FileState.FAILED -> Box(
                modifier = Modifier
                    .clickable(onClick = onRetry)
                    .background(colors.error.copy(alpha = 0.15f), RoundedCornerShape(8.dp))
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            ) {
                Text(
                    text = "贴纸没下下来（${item.note ?: "原因不明"}），点一下重试",
                    color = colors.error,
                    fontSize = metrics.noticeFontSp.sp,
                )
            }

            else -> Text(
                text = "贴纸下载中…",
                color = colors.system,
                fontSize = metrics.noticeFontSp.sp,
            )
        }
    }
}

/** 文件卡片：QQ 式，点按钮才下载。 */
@Composable
fun FileCardRow(
    item: ChatItem.FileItem,
    metrics: ChatMetrics,
    onAction: () -> Unit,
) {
    val colors = LocalDchatColors.current
    val own = item.fromNick.isNotEmpty()

    Column(
        modifier = Modifier.fillMaxWidth().padding(vertical = metrics.rowGapDp.dp / 3),
        horizontalAlignment = if (own) Alignment.Start else Alignment.Start,
    ) {
        Row(horizontalArrangement = Arrangement.Start) {
            if (item.fromNick.isNotEmpty()) {
                Text(
                    text = item.fromNick,
                    color = colors.nickColor(nickColorIndex(item.fromNick)),
                    fontSize = (metrics.noticeFontSp + 1).sp,
                    fontWeight = FontWeight.Medium,
                )
                Text("  ", fontSize = metrics.noticeFontSp.sp)
            }
            Text("发来一个文件", color = colors.system, fontSize = metrics.noticeFontSp.sp)
        }

        Box(
            modifier = Modifier
                .padding(top = 4.dp)
                .widthIn(max = metrics.bubbleMaxWidthDp.dp)
                .background(colors.bubbleOther, RoundedCornerShape(12.dp))
                .border(1.dp, colors.bubbleOtherBorder, RoundedCornerShape(12.dp))
                .padding(12.dp),
        ) {
            Column {
                Text(
                    text = item.fileName,
                    color = colors.text,
                    fontSize = metrics.messageFontSp.sp,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    text = item.sizeText + if (item.hasThumbnail) " · 含预览图" else "",
                    color = colors.system,
                    fontSize = metrics.noticeFontSp.sp,
                    modifier = Modifier.padding(top = 2.dp),
                )
                if (item.note != null) {
                    Text(
                        text = item.note,
                        color = colors.error,
                        fontSize = metrics.noticeFontSp.sp,
                        modifier = Modifier.padding(top = 2.dp),
                    )
                }
                // 按钮：整块可点（手机上比桌面端更需要大触控目标）
                Box(
                    modifier = Modifier
                        .padding(top = 8.dp)
                        .background(
                            if (item.state == com.dongfang20101113.dchat.ui.FileState.OFFERED) colors.accent else colors.neutral,
                            RoundedCornerShape(8.dp),
                        )
                        .clickable(onClick = onAction)
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                ) {
                    Text(
                        text = item.buttonLabel,
                        color = if (item.state == com.dongfang20101113.dchat.ui.FileState.OFFERED) colors.accentText else colors.text,
                        fontSize = metrics.noticeFontSp.sp,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }
        }
    }
}
