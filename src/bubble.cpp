#include "bubble.h"

namespace dchat {

namespace {

int ClampInt(int value, int low, int high) {
    if (high < low) high = low;
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

}  // namespace

BubblePlacement PlaceBubble(const RECT& viewport, int textWidth, int textHeight, BubbleAlign align,
                            int paddingX, int paddingY, int margin, int maxBubbleWidth) {
    BubblePlacement placement{};
    const int viewportWidth = viewport.right - viewport.left;
    if (viewportWidth <= 0) return placement;

    // 宽度上限不能超过"视口宽度减去两侧留白"
    const int hardLimit = viewportWidth - margin * 2;
    int limit = maxBubbleWidth < hardLimit ? maxBubbleWidth : hardLimit;
    if (limit < 0) limit = 0;

    const int minWidth = paddingX * 2 + 8;  // 至少能放下几个字
    int bubbleWidth = textWidth + paddingX * 2;
    bubbleWidth = ClampInt(bubbleWidth, minWidth, limit > minWidth ? limit : minWidth);
    if (hardLimit > 0 && bubbleWidth > hardLimit) bubbleWidth = hardLimit;

    const int bubbleHeight = textHeight + paddingY * 2;

    int left = viewport.left + margin;
    switch (align) {
        case BubbleAlign::Right:
            left = viewport.right - margin - bubbleWidth;
            break;
        case BubbleAlign::Center:
            left = viewport.left + (viewportWidth - bubbleWidth) / 2;
            break;
        case BubbleAlign::Left:
        default:
            break;
    }
    left = ClampInt(left, viewport.left + margin, viewport.right - margin - bubbleWidth);
    if (left < viewport.left) left = viewport.left;

    placement.bubble.left = left;
    placement.bubble.top = viewport.top;
    placement.bubble.right = left + bubbleWidth;
    placement.bubble.bottom = viewport.top + bubbleHeight;

    placement.text = placement.bubble;
    placement.text.left += paddingX;
    placement.text.right -= paddingX;
    placement.text.top += paddingY;
    placement.text.bottom -= paddingY;

    placement.totalHeight = bubbleHeight;
    return placement;
}

SIZE MeasureWrappedText(HDC dc, const std::wstring& text, int maxWidth, HFONT font) {
    SIZE size{0, 0};
    if (maxWidth <= 0) return size;

    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const std::wstring content = text.empty() ? L" " : text;  // 空行也要占一行高度
    RECT rect{0, 0, maxWidth, 0};
    DrawTextW(dc, content.c_str(), static_cast<int>(content.size()), &rect,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    size.cx = rect.right - rect.left;
    size.cy = rect.bottom - rect.top;
    if (oldFont) SelectObject(dc, oldFont);
    return size;
}

}  // namespace dchat
