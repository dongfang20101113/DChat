// 圆角控件的像素级回归测试。
// 起因：按钮只画了圆角形状、没铺底色时，圆角以外的四个角会残留控件 DC 的旧内容，
// 表现为"方形底色 + 圆角块"。这个测试用"哨兵色"把整块画布先涂满，画完控件后
// 只要还有哨兵色残留，就说明底色没铺满，问题会立刻暴露。
#include <cstdio>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "rounded.h"

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

void checkColor(COLORREF actual, COLORREF expected, const char* what) {
    ++g_checks;
    if (actual == expected) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s（实际 #%02X%02X%02X，期望 #%02X%02X%02X）\n", what, GetRValue(actual),
                    GetGValue(actual), GetBValue(actual), GetRValue(expected), GetGValue(expected),
                    GetBValue(expected));
    }
}

constexpr int kWidth = 96;
constexpr int kHeight = 32;
const COLORREF kSentinel = RGB(255, 0, 255);  // 画布底色：如果它还在，说明没被覆盖
const COLORREF kBackground = RGB(32, 32, 32);
const COLORREF kFill = RGB(52, 54, 58);
const COLORREF kBorder = RGB(74, 77, 82);

// 离屏渲染一小块画布，读取像素做断言
class Canvas {
public:
    Canvas(int width, int height) : width_(width), height_(height) {
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 负数：自上而下，方便按坐标读像素
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        previous_ = SelectObject(dc_, bitmap_);
        clear(kSentinel);
    }

    ~Canvas() {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    HDC dc() const { return dc_; }
    int width() const { return width_; }
    int height() const { return height_; }

    void clear(COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        RECT rect{0, 0, width_, height_};
        FillRect(dc_, &rect, brush);
        DeleteObject(brush);
        GdiFlush();
    }

    COLORREF At(int x, int y) const {
        const auto* bytes = static_cast<const unsigned char*>(pixels_);
        const std::size_t offset = (static_cast<std::size_t>(y) * width_ + x) * 4;
        return RGB(bytes[offset + 2], bytes[offset + 1], bytes[offset + 0]);
    }

    int CountColor(COLORREF color) const {
        int count = 0;
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x)
                if (At(x, y) == color) ++count;
        return count;
    }

private:
    int width_ = 0;
    int height_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* pixels_ = nullptr;
};

}  // namespace

int main() {
    std::printf("== rounded control tests ==\n");

    if (!ui::Startup()) {
        std::printf("GDI+ 初始化失败\n");
        return 1;
    }

    RECT rect{0, 0, kWidth, kHeight};
    const int radius = kHeight / 2;  // 胶囊形

    {
        std::printf("[1] 胶囊按钮：圆角外面不能残留旧内容\n");
        Canvas canvas(kWidth, kHeight);
        canvas.clear(kSentinel);
        ui::DrawRoundedControl(canvas.dc(), rect, radius, kBackground, kFill, kBorder);
        GdiFlush();

        check(canvas.CountColor(kSentinel) == 0, "画布上不再有任何哨兵色像素（底色铺满）");
        checkColor(canvas.At(0, 0), kBackground, "左上角是背景色（不是方块残留）");
        checkColor(canvas.At(kWidth - 1, 0), kBackground, "右上角是背景色");
        checkColor(canvas.At(0, kHeight - 1), kBackground, "左下角是背景色");
        checkColor(canvas.At(kWidth - 1, kHeight - 1), kBackground, "右下角是背景色");
        checkColor(canvas.At(kWidth / 2, kHeight / 2), kFill, "中心是填充色");
        check(canvas.At(2, kHeight / 2) != kBackground || canvas.At(3, kHeight / 2) == kFill,
              "左端中点附近已是填充色（胶囊形状画到了边缘）");
    }

    {
        std::printf("[2] 圆角半径自适应\n");
        Canvas canvas(kWidth, kHeight);
        canvas.clear(kSentinel);
        ui::DrawRoundedControl(canvas.dc(), rect, 8, kBackground, kFill, kBorder);
        GdiFlush();
        check(canvas.CountColor(kSentinel) == 0, "半径 8 时底色依然铺满");
        checkColor(canvas.At(0, 0), kBackground, "半径 8 时左上角仍是背景色");
        checkColor(canvas.At(kWidth / 2, kHeight / 2), kFill, "半径 8 时中心仍是填充色");

        Canvas tiny(kHeight, kHeight);  // 边界情形：半径超过可容纳范围
        tiny.clear(kSentinel);
        RECT square{0, 0, kHeight, kHeight};
        ui::DrawRoundedControl(tiny.dc(), square, kHeight, kBackground, kFill, kBorder);
        GdiFlush();
        check(tiny.CountColor(kSentinel) == 0, "半径超出可用范围时自动夹到上限，也不会漏底");
        checkColor(tiny.At(0, 0), kBackground, "半径夹到上限后左上角仍是背景色");
    }

    {
        std::printf("[3] 圆角外框（记录区/输入框用）\n");
        Canvas canvas(kWidth, kHeight);
        canvas.clear(kSentinel);
        ui::OutlineRoundedRect(canvas.dc(), rect, 10, kBorder);
        GdiFlush();
        check(canvas.At(kWidth / 2, kHeight / 2) == kSentinel, "只画边框：内部保持原样");
        check(canvas.At(0, 0) == kSentinel, "只画边框：圆角外不改动画布");
        int borderPixels = 0;
        for (int x = 0; x < kWidth; ++x)
            for (int y = 0; y < kHeight; ++y)
                if (canvas.At(x, y) != kSentinel) ++borderPixels;
        check(borderPixels > 100, "边框确实画出来了（有足够多的非背景像素）");
    }

    ui::Shutdown();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
