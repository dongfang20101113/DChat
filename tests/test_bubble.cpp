// 气泡布局测试：验证"自己的消息靠右、别人的靠左、系统提示居中"，
// 以及气泡宽度上限、内边距换算、换行测量。
#include <cstdio>
#include <windows.h>

#include "bubble.h"

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}
}  // namespace

int main() {
    std::printf("== dchat bubble tests ==\n");
    const RECT viewport{0, 0, 600, 400};
    const int padX = 12, padY = 8, margin = 14, maxBubble = 360;

    {
        std::printf("[1] 左右对齐（微信式布局）\n");
        const auto mine = dchat::PlaceBubble(viewport, 120, 20, dchat::BubbleAlign::Right, padX, padY,
                                            margin, maxBubble);
        check(mine.bubble.right == viewport.right - margin, "自己的消息贴右边距");
        check(mine.bubble.left > viewport.left + (viewport.right - viewport.left) / 2,
              "自己的消息位于右半边");

        const auto other = dchat::PlaceBubble(viewport, 120, 20, dchat::BubbleAlign::Left, padX, padY,
                                             margin, maxBubble);
        check(other.bubble.left == viewport.left + margin, "别人的消息贴左边距");
        check(other.bubble.right < viewport.left + (viewport.right - viewport.left) / 2,
              "别人的消息位于左半边");

        const auto system = dchat::PlaceBubble(viewport, 100, 18, dchat::BubbleAlign::Center, 10, 5,
                                              margin, maxBubble);
        const int leftGap = system.bubble.left - viewport.left;
        const int rightGap = viewport.right - system.bubble.right;
        check(leftGap - rightGap <= 1 && rightGap - leftGap <= 1, "系统提示左右居中");
        check(mine.bubble.top == viewport.top && other.bubble.top == viewport.top,
              "气泡从给定行位置开始（逐条向下排）");
    }

    {
        std::printf("[2] 尺寸与内边距\n");
        const auto shortText = dchat::PlaceBubble(viewport, 30, 20, dchat::BubbleAlign::Left, padX,
                                                 padY, margin, maxBubble);
        check(shortText.bubble.bottom - shortText.bubble.top == 20 + padY * 2,
              "高度 = 文字高 + 上下内边距");
        check(shortText.text.left - shortText.bubble.left == padX &&
                  shortText.bubble.right - shortText.text.right == padX,
              "文字左右各留 paddingX 内边距");
        check(shortText.bubble.right - shortText.bubble.left >= padX * 2, "极短文字也保证最小宽度");

        const auto longText = dchat::PlaceBubble(viewport, 900, 20, dchat::BubbleAlign::Left, padX,
                                                padY, margin, maxBubble);
        check(longText.bubble.right - longText.bubble.left == maxBubble, "超长文字被夹到宽度上限");
        check(longText.bubble.right <= viewport.right - margin, "夹取后仍在视口内");

        const auto huge = dchat::PlaceBubble(viewport, 2000, 20, dchat::BubbleAlign::Right, padX, padY,
                                            margin, 5000);
        check(huge.bubble.left >= viewport.left + margin &&
                  huge.bubble.right <= viewport.right - margin,
              "宽度上限大于视口时也不越界");
    }

    {
        std::printf("[3] 换行测量\n");
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        const SIZE shortSize = dchat::MeasureWrappedText(dc, L"你好", 200, font);
        check(shortSize.cy > 0 && shortSize.cx <= 200, "短文本一行内测出合理尺寸");
        const SIZE longSize = dchat::MeasureWrappedText(
            dc, L"这是一段很长的中文消息，用来验证自动换行的测量是否正确，宽度受限时应当折行显示。",
            160, font);
        check(longSize.cy > shortSize.cy, "长文本换行后高度变大");
        check(longSize.cx <= 160, "换行后宽度不超过限制");
        const SIZE emptySize = dchat::MeasureWrappedText(dc, L"", 200, font);
        check(emptySize.cy > 0, "空文本也占一行高度");
        const SIZE zeroWidth = dchat::MeasureWrappedText(dc, L"abc", 0, font);
        check(zeroWidth.cx == 0 && zeroWidth.cy == 0, "宽度为 0 时返回空尺寸");

        DeleteObject(font);
        DeleteDC(dc);
    }

    {
        std::printf("[4] 系统提示要比正文更紧凑\n");
        check(dchat::kNoticeFontHeight > dchat::kMessageFontHeight,
              "系统提示字号更小（负值越大字越小）");
        check(dchat::kNoticePaddingX < dchat::kMessagePaddingX &&
                  dchat::kNoticePaddingY < dchat::kMessagePaddingY,
              "系统提示的内边距更小");
        const int textHeight = 16;
        const auto message = dchat::PlaceBubble(viewport, 200, textHeight, dchat::BubbleAlign::Left,
                                               dchat::kMessagePaddingX, dchat::kMessagePaddingY,
                                               dchat::kViewMargin, 360);
        const auto notice = dchat::PlaceBubble(viewport, 200, textHeight, dchat::BubbleAlign::Center,
                                              dchat::kNoticePaddingX, dchat::kNoticePaddingY,
                                              dchat::kViewMargin, 360);
        check(notice.bubble.bottom - notice.bubble.top <
                  message.bubble.bottom - message.bubble.top,
              "同样文字下系统提示占的高度更小");
        check(notice.bubble.right - notice.bubble.left <
                  message.bubble.right - message.bubble.left,
              "同样文字下系统提示的宽度更小");
    }

    {
        std::printf("[5] 全服公告要比正文更大\n");
        check(dchat::kAnnounceFontHeight < dchat::kMessageFontHeight, "公告字号更大");
        check(dchat::kAnnouncePaddingX > dchat::kMessagePaddingX &&
                  dchat::kAnnouncePaddingY > dchat::kMessagePaddingY,
              "公告留白更多");
        check(dchat::kAnnounceMaxPercent > dchat::kMaxBubblePercent, "公告可以更宽");
        const int textHeight = 24;
        const auto message = dchat::PlaceBubble(viewport, 200, textHeight, dchat::BubbleAlign::Left,
                                               dchat::kMessagePaddingX, dchat::kMessagePaddingY,
                                               dchat::kViewMargin, 360);
        const auto announce = dchat::PlaceBubble(viewport, 200, textHeight,
                                                dchat::BubbleAlign::Center,
                                                dchat::kAnnouncePaddingX, dchat::kAnnouncePaddingY,
                                                dchat::kViewMargin, 500);
        check(announce.bubble.bottom - announce.bubble.top >
                  message.bubble.bottom - message.bubble.top,
              "同样文字下公告更高（字更大、留白更多）");
        const int leftGap = announce.bubble.left - viewport.left;
        const int rightGap = viewport.right - announce.bubble.right;
        check(leftGap - rightGap <= 1 && rightGap - leftGap <= 1, "公告居中显示");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
