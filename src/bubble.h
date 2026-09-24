// 聊天气泡的几何与文字测量：决定"自己的消息靠右、别人的靠左、系统提示居中"。
// 纯计算 + GDI 测量，不涉及界面状态，便于单元测试。
#pragma once

#include <windows.h>

#include <string>

namespace dchat {

// 记录区统一的字号与间距（界面和测试共用同一套数值）
inline constexpr int kMessageFontHeight = -16;  // 正文字号（负值 = 字符高度，像素）
inline constexpr int kNoticeFontHeight = -12;   // 系统提示：比正文小一号
inline constexpr int kAnnounceFontHeight = -21; // 全服公告：比正文大一号
inline constexpr int kMessagePaddingX = 12;
inline constexpr int kMessagePaddingY = 8;
inline constexpr int kNoticePaddingX = 8;       // 系统提示的气泡更紧凑
inline constexpr int kNoticePaddingY = 3;
inline constexpr int kAnnouncePaddingX = 18;    // 公告留白更多
inline constexpr int kAnnouncePaddingY = 10;
inline constexpr int kAnnounceMaxPercent = 88;  // 公告可以占视口宽度的比例
inline constexpr int kMessageGap = 12;          // 两条消息之间的竖直间距
inline constexpr int kViewMargin = 14;          // 气泡与记录区左右边缘的距离
inline constexpr int kMaxBubblePercent = 62;    // 气泡最大宽度占视口的百分比
inline constexpr int kNoticeMaxPercent = 70;    // 系统提示最大宽度占视口的百分比

enum class BubbleAlign {
    Left,    // 别人发的：靠左
    Right,   // 自己发的：靠右
    Center,  // 系统提示：居中
};

struct BubblePlacement {
    RECT bubble;      // 气泡矩形（含内边距）
    RECT text;        // 文字矩形（气泡内部）
    int totalHeight;  // 这一条占用的总高度（= 气泡高度）
};

// 在给定视口里摆放一个气泡。
// maxBubbleWidth：气泡宽度上限（微信式布局通常是视口宽度的一部分）
// paddingX/paddingY：气泡内边距；margin：气泡与视口左右边缘的最小距离
BubblePlacement PlaceBubble(const RECT& viewport, int textWidth, int textHeight, BubbleAlign align,
                            int paddingX, int paddingY, int margin, int maxBubbleWidth);

// 用给定字体测量"按 maxWidth 换行后"的文字尺寸。
// 绘制时必须用同一套参数，否则气泡宽度会和换行对不上。
SIZE MeasureWrappedText(HDC dc, const std::wstring& text, int maxWidth, HFONT font);

}  // namespace dchat
