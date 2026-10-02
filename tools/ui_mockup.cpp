// 界面改版效果图：把"新布局长什么样"离屏画成 PNG，供人先看再改真代码。
//
// **刻意复用真界面的同一套东西**：配色（client.cpp 的 kDarkPalette / kLightPalette 数值）、
// 字号（bubble.h 的 kMessageFontHeight / kNoticeFontHeight）、字体（Microsoft YaHei UI）、
// 圆角绘制（rounded.h 的 ui::DrawRoundedControl / FillRoundedRect / OutlineRoundedRect）。
// 这样效果图不会"看着好看、真做出来两回事"。
//
// 产物：build\ui-proposal-*.png
// 用法：build\ui_mockup.exe [输出目录]
#include <objidl.h>
#include <windows.h>

#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bubble.h"
#include "rounded.h"

namespace {

// ---------------------------------------------------------------------------
// 主题配色：抄自 client.cpp 的调色板（这里只抄这次用得到的几个）
// ---------------------------------------------------------------------------
struct Theme {
    COLORREF windowBg;
    COLORREF titleBar;
    COLORREF bubbleOther;
    COLORREF bubbleOtherBorder;
    COLORREF bubbleOwn;
    COLORREF bubbleOwnBorder;
    COLORREF bubbleOwnText;
    COLORREF text;
    COLORREF system;
    COLORREF time;
    COLORREF noticeBg;
    COLORREF border;
    COLORREF neutral;
    COLORREF neutralBorder;
    COLORREF accent;
    COLORREF accentText;
    COLORREF panel;  // 菜单栏 / 状态条 / 设置窗口的背景
    COLORREF hover;  // 悬停高亮
};

const Theme kDark = {
    /*windowBg*/ RGB(0x20, 0x20, 0x20),
    /*titleBar*/ RGB(0x2B, 0x2B, 0x2B),
    /*bubbleOther*/ RGB(0x30, 0x32, 0x36),
    /*bubbleOtherBorder*/ RGB(0x46, 0x49, 0x4E),
    /*bubbleOwn*/ RGB(0x00, 0x78, 0xD7),
    /*bubbleOwnBorder*/ RGB(0x1E, 0x5A, 0xA0),
    /*bubbleOwnText*/ RGB(0xFF, 0xFF, 0xFF),
    /*text*/ RGB(0xE8, 0xE8, 0xE8),
    /*system*/ RGB(0x96, 0x96, 0x96),
    /*time*/ RGB(0x8C, 0x8C, 0x8C),
    /*noticeBg*/ RGB(0x2C, 0x2E, 0x32),
    /*border*/ RGB(0x40, 0x42, 0x46),
    /*neutral*/ RGB(0x34, 0x36, 0x3A),
    /*neutralBorder*/ RGB(0x4A, 0x4D, 0x52),
    /*accent*/ RGB(0x00, 0x78, 0xD7),
    /*accentText*/ RGB(0xFF, 0xFF, 0xFF),
    /*panel*/ RGB(0x2A, 0x2C, 0x30),
    /*hover*/ RGB(0x3A, 0x3D, 0x42),
};

// ---------------------------------------------------------------------------
// 离屏画布 + 存 PNG
// ---------------------------------------------------------------------------
class Canvas {
public:
    Canvas(int width, int height) : width_(width), height_(height) {
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 负数 = 自上而下
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        previous_ = SelectObject(dc_, bitmap_);
    }

    ~Canvas() {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    HDC dc() const { return dc_; }
    int width() const { return width_; }
    int height() const { return height_; }

    void Fill(const RECT& rect, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc_, &rect, brush);
        DeleteObject(brush);
    }

    bool SavePng(const std::wstring& path) {
        GdiFlush();
        Gdiplus::Bitmap bitmap(bitmap_, nullptr);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;

        CLSID encoder{};
        if (!EncoderClsid(L"image/png", &encoder)) return false;
        return bitmap.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
    }

private:
    static bool EncoderClsid(const wchar_t* mime, CLSID* out) {
        UINT count = 0;
        UINT bytes = 0;
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        if (bytes == 0) return false;
        std::vector<unsigned char> buffer(bytes);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        Gdiplus::GetImageEncoders(count, bytes, codecs);
        for (UINT i = 0; i < count; ++i) {
            if (std::wstring(codecs[i].MimeType) == mime) {
                *out = codecs[i].Clsid;
                return true;
            }
        }
        return false;
    }

    int width_ = 0;
    int height_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* pixels_ = nullptr;
};

// ---------------------------------------------------------------------------
// 文字
// ---------------------------------------------------------------------------
HFONT MakeFont(int height, int weight = FW_NORMAL) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, old);
}

SIZE Measure(HDC dc, const std::wstring& text, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size;
}

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

// 菜单栏上的一项（文字按钮，没有边框）
void MenuItem(HDC dc, int left, int top, const wchar_t* label, HFONT font, const Theme& theme,
              bool active = false) {
    const SIZE size = Measure(dc, label, font);
    RECT box{left, top, left + size.cx + 24, top + 30};
    if (active) ui::FillRoundedRect(dc, box, 6, theme.hover, theme.hover, 0.0f);
    RECT textRect = box;
    Text(dc, label, textRect, font, theme.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

int MenuItemWidth(HDC dc, const wchar_t* label, HFONT font) {
    const SIZE size = Measure(dc, label, font);
    return size.cx + 24;
}

// 把颜色调亮/调暗一点（画按下态、按钮边框时用）
COLORREF Adjust(COLORREF color, int delta) {
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return RGB(clamp(GetRValue(color) + delta), clamp(GetGValue(color) + delta),
               clamp(GetBValue(color) + delta));
}

// 齿轮图标：一个圆 + 八个齿（不依赖任何字体，免得缺字形变成方块）
void GearIcon(HDC dc, int cx, int cy, int radius, COLORREF color) {
    const int outer = radius;
    const int inner = radius - 4;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    for (int i = 0; i < 8; ++i) {
        const double angle = i * 3.14159265358979 / 4.0;
        const int x = cx + static_cast<int>(std::cos(angle) * outer);
        const int y = cy + static_cast<int>(std::sin(angle) * outer);
        RECT tooth{x - 3, y - 3, x + 3, y + 3};
        Ellipse(dc, tooth.left, tooth.top, tooth.right, tooth.bottom);
    }
    Ellipse(dc, cx - inner, cy - inner, cx + inner, cy + inner);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// 标题栏右上角的三个窗口按钮（最小化 / 最大化 / 关闭）
void WindowButtons(HDC dc, int right, const Theme& theme) {
    const int w = 46;
    const int h = 32;
    const int top = 0;
    for (int i = 0; i < 3; ++i) {
        const int left = right - (3 - i) * w;
        RECT box{left, top, left + w, top + h};
        Text(dc, i == 0 ? L"–" : (i == 1 ? L"□" : L"✕"), box,
             CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol"),
             theme.system, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 胶囊按钮
void PillButton(HDC dc, const RECT& rect, const wchar_t* label, HFONT font, const Theme& theme,
                bool primary) {
    const COLORREF fill = primary ? theme.accent : theme.neutral;
    const COLORREF border = primary ? theme.accent : theme.neutralBorder;
    const COLORREF text = primary ? theme.accentText : theme.text;
    ui::DrawRoundedControl(dc, rect, (rect.bottom - rect.top) / 2, theme.panel, fill, border);
    Text(dc, label, rect, font, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// 聊天内容（所有效果图共用同一份，方便对比）
// ---------------------------------------------------------------------------
struct ChatMetrics {
    HFONT message = nullptr;
    HFONT notice = nullptr;
    HFONT announce = nullptr;
    HFONT small = nullptr;
};

void DrawNoticeCapsule(HDC dc, int viewLeft, int viewWidth, int* y, const wchar_t* text,
                       const ChatMetrics& fonts, const Theme& theme, bool withTime = true) {
    const int maxWidth = viewWidth * dchat::kNoticeMaxPercent / 100;
    RECT measure{0, 0, maxWidth - dchat::kNoticePaddingX * 2, 0};
    HGDIOBJ old = SelectObject(dc, fonts.notice);
    DrawTextW(dc, text, -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    const int textW = measure.right - measure.left;
    const int textH = measure.bottom - measure.top;
    const int boxW = textW + dchat::kNoticePaddingX * 2;
    const int boxH = textH + dchat::kNoticePaddingY * 2;
    const int left = viewLeft + (viewWidth - boxW) / 2;
    RECT box{left, *y, left + boxW, *y + boxH};
    ui::FillRoundedRect(dc, box, 10, theme.noticeBg, theme.border, 1.0f);
    Text(dc, text, box, fonts.notice, theme.system, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    *y = box.bottom;
    if (withTime) {
        RECT timeRect{viewLeft, *y + 2, viewLeft + viewWidth, *y + 2 + 16};
        Text(dc, L"21:05", timeRect, fonts.small, theme.time,
             DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        *y += 18;
    }
    *y += dchat::kMessageGap / 2;
}

// 一条聊天气泡（自己发的靠右、别人的靠左）
void DrawBubble(HDC dc, int viewLeft, int viewWidth, int* y, const wchar_t* nick,
                const wchar_t* time, const wchar_t* body, bool own, const ChatMetrics& fonts,
                const Theme& theme) {
    const int margin = dchat::kViewMargin;
    const int maxBubble = viewWidth * dchat::kMaxBubblePercent / 100;
    const int padX = dchat::kMessagePaddingX;
    const int padY = dchat::kMessagePaddingY;

    // 表头
    RECT header{viewLeft, *y, viewLeft + viewWidth, *y + 16};
    if (own) {
        Text(dc, time, header, fonts.small, theme.time,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        RECT nickRect{header.left + 16, header.top, header.left + 320, header.bottom};
        Text(dc, nick, nickRect, fonts.small, RGB(0x56, 0x9C, 0xD6),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const SIZE nickSize = Measure(dc, nick, fonts.small);
        RECT timeRect{nickRect.left + nickSize.cx + 8, header.top, nickRect.left + nickSize.cx + 100,
                      header.bottom};
        Text(dc, time, timeRect, fonts.small, theme.time,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    *y = header.bottom;

    const SIZE size =
        dchat::MeasureWrappedText(dc, body, maxBubble - padX * 2, fonts.message);
    RECT area{viewLeft, *y, viewLeft + viewWidth, *y};
    const dchat::BubblePlacement place =
        dchat::PlaceBubble(area, size.cx, size.cy,
                           own ? dchat::BubbleAlign::Right : dchat::BubbleAlign::Left, padX, padY,
                           margin, maxBubble);
    const COLORREF fill = own ? theme.bubbleOwn : theme.bubbleOther;
    const COLORREF border = own ? theme.bubbleOwnBorder : theme.bubbleOtherBorder;
    ui::FillRoundedRect(dc, place.bubble, 12, fill, border, 1.0f);
    Text(dc, body, place.text, fonts.message, own ? theme.bubbleOwnText : theme.text,
         DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    *y = place.bubble.bottom + dchat::kMessageGap;
}

// 文件卡片（QQ 式：点了才下载）
void DrawFileCard(HDC dc, int viewLeft, int viewWidth, int* y, const wchar_t* nick,
                  const wchar_t* fileName, const wchar_t* detail, const wchar_t* button,
                  const ChatMetrics& fonts, const Theme& theme) {
    const int margin = dchat::kViewMargin;
    RECT header{viewLeft, *y, viewLeft + viewWidth, *y + 16};
    Text(dc, nick, header, fonts.small, RGB(0x78, 0xC8, 0x82),
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const SIZE nickSize = Measure(dc, nick, fonts.small);
    RECT hint{header.left + nickSize.cx + 8, header.top, header.left + viewWidth, header.bottom};
    Text(dc, L"发来一个文件", hint, fonts.small, theme.system,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    *y = header.bottom + 4;

    const int cardW = 400;
    const int cardH = 68;
    RECT card{viewLeft + margin, *y, viewLeft + margin + cardW, *y + cardH};
    ui::FillRoundedRect(dc, card, 10, theme.bubbleOther, theme.border, 1.0f);
    RECT nameRect{card.left + 14, card.top + 10, card.right - 130, card.top + 32};
    Text(dc, fileName, nameRect, fonts.message, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    RECT detailRect{card.left + 14, card.top + 36, card.right - 130, card.top + 54};
    Text(dc, detail, detailRect, fonts.small, theme.system,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    RECT buttonRect{card.right - 14 - 104, card.top + (cardH - 30) / 2, card.right - 14,
                    card.top + (cardH - 30) / 2 + 30};
    ui::DrawRoundedControl(dc, buttonRect, 15, theme.bubbleOther, theme.accent,
                           Adjust(theme.accent, -20));
    Text(dc, button, buttonRect, fonts.message, theme.accentText,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    *y = card.bottom + dchat::kMessageGap;
}

// 语音气泡（点一下播/暂停）
void DrawVoiceBubble(HDC dc, int viewLeft, int viewWidth, int* y, const wchar_t* nick,
                     const wchar_t* duration, bool own, bool playing, const ChatMetrics& fonts,
                     const Theme& theme) {
    const int margin = dchat::kViewMargin;
    if (!own) {
        RECT header{viewLeft, *y, viewLeft + viewWidth, *y + 16};
        Text(dc, nick, header, fonts.small, RGB(0x56, 0x9C, 0xD6),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        *y = header.bottom + 4;
    } else {
        *y += 4;
    }

    const int bubbleW = 176;
    const int bubbleH = 40;
    const int left = own ? (viewLeft + viewWidth - margin - bubbleW) : (viewLeft + margin);
    RECT bubble{left, *y, left + bubbleW, *y + bubbleH};
    const COLORREF fill = own ? theme.bubbleOwn : theme.bubbleOther;
    const COLORREF border = own ? theme.bubbleOwnBorder : theme.bubbleOtherBorder;
    const COLORREF fg = own ? theme.bubbleOwnText : theme.text;
    ui::FillRoundedRect(dc, bubble, 12, fill, border, 1.0f);

    const int midY = (bubble.top + bubble.bottom) / 2;
    const int iconLeft = bubble.left + 12;
    if (playing) {  // 两条竖线
        HBRUSH brush = CreateSolidBrush(fg);
        RECT a{iconLeft, midY - 8, iconLeft + 4, midY + 8};
        RECT b{iconLeft + 8, midY - 8, iconLeft + 12, midY + 8};
        FillRect(dc, &a, brush);
        FillRect(dc, &b, brush);
        DeleteObject(brush);
    } else {  // 三角形
        POINT points[3] = {{iconLeft, midY - 8}, {iconLeft + 16, midY}, {iconLeft, midY + 8}};
        HBRUSH brush = CreateSolidBrush(fg);
        HPEN pen = CreatePen(PS_SOLID, 1, fg);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Polygon(dc, points, 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
    // 几根短竖条
    int barX = iconLeft + 28;
    for (int i = 0; i < 4; ++i) {
        const int height = (i % 2 == 0) ? 10 : 16;
        RECT bar{barX, midY - height / 2, barX + 3, midY + height / 2};
        HBRUSH brush = CreateSolidBrush((playing && (i == 1 || i == 2)) ? fg : theme.system);
        FillRect(dc, &bar, brush);
        DeleteObject(brush);
        barX += 6;
    }
    RECT timeRect{barX + 8, bubble.top, bubble.right - 12, bubble.bottom};
    Text(dc, duration, timeRect, fonts.small, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (playing) {  // 进度线
        RECT track{bubble.left + 12, bubble.bottom - 6, bubble.right - 12, bubble.bottom - 3};
        HBRUSH trackBrush = CreateSolidBrush(theme.border);
        FillRect(dc, &track, trackBrush);
        DeleteObject(trackBrush);
        RECT done = track;
        done.right = track.left + (track.right - track.left) * 35 / 100;
        HBRUSH doneBrush = CreateSolidBrush(fg);
        FillRect(dc, &done, doneBrush);
        DeleteObject(doneBrush);
    }
    *y = bubble.bottom + dchat::kMessageGap;
}

// 共用的一段聊天记录
void DrawChatLog(HDC dc, int viewLeft, int viewTop, int viewWidth, int viewBottom,
                 const ChatMetrics& fonts, const Theme& theme) {
    RECT clip{viewLeft, viewTop, viewLeft + viewWidth, viewBottom};
    HRGN region = CreateRectRgn(clip.left, clip.top, clip.right, clip.bottom);
    SelectClipRgn(dc, region);

    int y = viewTop + 6;
    DrawNoticeCapsule(dc, viewLeft, viewWidth, &y, L"欢迎来到 dchat 聊天室", fonts, theme);
    DrawBubble(dc, viewLeft, viewWidth, &y, L"小明", L"21:05", L"在吗？我把上次说的季度报告传上来了，你看下第二页的表格。",
               false, fonts, theme);
    DrawBubble(dc, viewLeft, viewWidth, &y, L"", L"21:06", L"收到，我现在看。", true, fonts, theme);
    DrawFileCard(dc, viewLeft, viewWidth, &y, L"小明", L"季度报告.pdf", L"1.0 MB　来自 小明", L"下载",
                 fonts, theme);
    DrawVoiceBubble(dc, viewLeft, viewWidth, &y, L"小明", L"0:07", false, true, fonts, theme);
    DrawBubble(dc, viewLeft, viewWidth, &y, L"", L"21:08", L"这段听完了，第 3 点的口径要改。", true,
               fonts, theme);
    DrawNoticeCapsule(dc, viewLeft, viewWidth, &y, L"Alice 加入了聊天室", fonts, theme);

    SelectClipRgn(dc, nullptr);
    DeleteObject(region);
}

// ---------------------------------------------------------------------------
// 三种布局
// ---------------------------------------------------------------------------

constexpr int kWinW = 940;
constexpr int kWinH = 660;
constexpr int kTitleH = 34;

// 现在这一版：顶部一条挤了 6 个按钮（用来对比）
void DrawCurrent(Canvas& canvas, const ChatMetrics& fonts, const Theme& theme) {
    HDC dc = canvas.dc();
    canvas.Fill(RECT{0, 0, kWinW, kWinH}, theme.windowBg);
    canvas.Fill(RECT{0, 0, kWinW, kTitleH}, theme.titleBar);
    Text(dc, L"dchat 客户端", RECT{14, 0, 400, kTitleH}, fonts.message, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    WindowButtons(dc, kWinW, theme);

    // 顶部按钮条（现状：6 个按钮 + 状态文字）
    const int margin = 12;
    const int stripTop = kTitleH + margin;
    const int stripH = 32;
    canvas.Fill(RECT{0, kTitleH, kWinW, stripTop + stripH + margin}, theme.windowBg);
    RECT statusRect{margin, stripTop + 6, 300, stripTop + stripH};
    Text(dc, L"已连接 192.168.1.100:5555　用户：我", statusRect, fonts.small, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    struct Item {
        const wchar_t* label;
        int width;
        bool accent;  // 主操作/开关打开：用强调色
    };
    // 现状里「彩色聊天」是打开状态（强调色），其余都是中性色——
    // 这正是问题之一：一眼看不出哪个是"当前状态"、哪个是"点了会做事"
    const Item items[] = {{L"录音", 76, false}, {L"发送文件", 96, false}, {L"彩色聊天", 96, true},
                          {L"浅色", 128, false}, {L"连接", 78, false}, {L"断开", 78, false}};
    int total = 0;
    for (const Item& item : items) total += item.width + 8;
    int x = kWinW - margin - total + 8;
    for (const Item& item : items) {
        RECT box{x, stripTop, x + item.width, stripTop + stripH};
        PillButton(dc, box, item.label, fonts.message, theme, item.accent);
        x += item.width + 8;
    }

    const int viewTop = stripTop + stripH + 8;
    const int viewBottom = kWinH - kTitleH - 32 - 44;
    DrawChatLog(dc, 0, viewTop, kWinW, viewBottom, fonts, theme);

    // 状态栏 + 输入行
    const int inputTop = kWinH - 44 - 12;
    RECT pill{12, inputTop, kWinW - 12 - 78 - 8, inputTop + 44};
    ui::DrawRoundedControl(dc, pill, 22, theme.windowBg, theme.bubbleOther, theme.border);
    Text(dc, L"说点什么…（回车发送）", RECT{pill.left + 16, pill.top, pill.right - 16, pill.bottom},
         fonts.message, theme.system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT send{kWinW - 12 - 78, inputTop, kWinW - 12, inputTop + 44};
    PillButton(dc, send, L"发送", fonts.message, theme, true);
}

// 方案 A：菜单栏 + 状态条 + 输入行（三行，各司其职）
void DrawProposalA(Canvas& canvas, const ChatMetrics& fonts, const Theme& theme) {
    HDC dc = canvas.dc();
    canvas.Fill(RECT{0, 0, kWinW, kWinH}, theme.windowBg);
    canvas.Fill(RECT{0, 0, kWinW, kTitleH}, theme.titleBar);
    Text(dc, L"dchat 客户端", RECT{14, 0, 400, kTitleH}, fonts.message, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    WindowButtons(dc, kWinW, theme);

    // 菜单栏
    const int menuH = 34;
    canvas.Fill(RECT{0, kTitleH, kWinW, kTitleH + menuH}, theme.panel);
    canvas.Fill(RECT{0, kTitleH + menuH - 1, kWinW, kTitleH + menuH}, theme.border);
    int x = 10;
    for (const wchar_t* label : {L"连接", L"设置", L"帮助"}) {
        MenuItem(dc, x, kTitleH + 2, label, fonts.message, theme, false);
        x += MenuItemWidth(dc, label, fonts.message);
    }
    // 右侧：连接状态小胶囊 + 齿轮
    const std::wstring who = L"● 192.168.1.100　我";
    const SIZE whoSize = Measure(dc, who, fonts.small);
    RECT whoRect{kWinW - 60 - whoSize.cx - 20, kTitleH + 7, kWinW - 60, kTitleH + menuH - 5};
    ui::FillRoundedRect(dc, whoRect, 10, theme.noticeBg, theme.border, 1.0f);
    Text(dc, who, whoRect, fonts.small, theme.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GearIcon(dc, kWinW - 32, kTitleH + menuH / 2, 8, theme.text);

    // 记录区
    const int viewTop = kTitleH + menuH;
    const int statusH = 24;
    const int inputH = 56;
    const int viewBottom = kWinH - statusH - inputH;
    DrawChatLog(dc, 0, viewTop, kWinW, viewBottom, fonts, theme);

    // 状态条：只读信息
    canvas.Fill(RECT{0, viewBottom, kWinW, viewBottom + statusH}, theme.panel);
    canvas.Fill(RECT{0, viewBottom, kWinW, viewBottom + 1}, theme.border);
    RECT statusText{14, viewBottom, kWinW - 14, viewBottom + statusH};
    Text(dc, L"已连接 192.168.1.100:5555　·　在线 3 人　·　语音上限 1:00", statusText, fonts.small,
         theme.system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 输入行
    const int inputTop = viewBottom + statusH;
    RECT pill{12, inputTop + 8, kWinW - 12 - 78 - 8 - 44, inputTop + 48};
    ui::DrawRoundedControl(dc, pill, 20, theme.windowBg, theme.bubbleOther, theme.border);
    Text(dc, L"说点什么…（回车发送）", RECT{pill.left + 16, pill.top, pill.right - 16, pill.bottom},
         fonts.message, theme.system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // 「＋」：文件 / 语音都收在这里
    RECT plus{pill.right + 6, inputTop + 8, pill.right + 6 + 40, inputTop + 48};
    ui::DrawRoundedControl(dc, plus, 20, theme.windowBg, theme.neutral, theme.neutralBorder);
    Text(dc, L"＋", RECT{plus.left, plus.top - 1, plus.right, plus.bottom}, fonts.message, theme.text,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT send{kWinW - 12 - 78, inputTop + 8, kWinW - 12, inputTop + 48};
    PillButton(dc, send, L"发送", fonts.message, theme, true);

    // 展开的「＋」菜单（示意）：往上弹，贴在输入行上方
    const int menuW = 180;
    const int itemH = 34;
    RECT menu{plus.right - menuW, pill.top - 10 - itemH * 2 - 12, plus.right,
              pill.top - 10};
    ui::DrawRoundedControl(dc, menu, 10, theme.windowBg, theme.neutral, theme.border);
    RECT item1{menu.left + 6, menu.top + 6, menu.right - 6, menu.top + 6 + itemH};
    ui::FillRoundedRect(dc, item1, 8, theme.hover, theme.hover, 0.0f);
    Text(dc, L"📎  发送文件", RECT{item1.left + 14, item1.top, item1.right, item1.bottom},
         fonts.message, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT item2{item1.left, item1.bottom + 2, item1.right, item1.bottom + 2 + itemH};
    Text(dc, L"🎤  录一段语音", RECT{item2.left + 14, item2.top, item2.right, item2.bottom},
         fonts.message, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// 方案 B：完全无菜单栏，左上角一个汉堡按钮 + 右上角齿轮；发送按钮悬浮在输入框右下
void DrawProposalB(Canvas& canvas, const ChatMetrics& fonts, const Theme& theme) {
    HDC dc = canvas.dc();
    canvas.Fill(RECT{0, 0, kWinW, kWinH}, theme.windowBg);
    canvas.Fill(RECT{0, 0, kWinW, kTitleH}, theme.titleBar);
    Text(dc, L"dchat 客户端", RECT{14, 0, 400, kTitleH}, fonts.message, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    WindowButtons(dc, kWinW, theme);

    const int topH = 40;
    canvas.Fill(RECT{0, kTitleH, kWinW, kTitleH + topH}, theme.panel);
    canvas.Fill(RECT{0, kTitleH + topH - 1, kWinW, kTitleH + topH}, theme.border);
    // 汉堡
    HBRUSH bar = CreateSolidBrush(theme.text);
    for (int i = 0; i < 3; ++i) {
        RECT line{18, kTitleH + 12 + i * 6, 18 + 20, kTitleH + 14 + i * 6};
        FillRect(dc, &line, bar);
    }
    DeleteObject(bar);
    RECT roomRect{52, kTitleH + 8, 400, kTitleH + topH - 8};
    Text(dc, L"192.168.1.100 · 我", roomRect, fonts.message, theme.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT onlineRect{kWinW - 260, kTitleH + 8, kWinW - 56, kTitleH + topH - 8};
    Text(dc, L"● 在线 3 人", onlineRect, fonts.small, theme.system,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    GearIcon(dc, kWinW - 32, kTitleH + topH / 2, 8, theme.text);

    const int viewTop = kTitleH + topH;
    const int inputH = 60;
    const int viewBottom = kWinH - inputH;
    DrawChatLog(dc, 0, viewTop, kWinW, viewBottom, fonts, theme);

    // 输入区：一整条，没有分隔状态条（状态并到顶栏里）
    canvas.Fill(RECT{0, viewBottom, kWinW, kWinH}, theme.windowBg);
    canvas.Fill(RECT{0, viewBottom, kWinW, viewBottom + 1}, theme.border);
    RECT pill{12, viewBottom + 10, kWinW - 12 - 44, viewBottom + 50};
    ui::DrawRoundedControl(dc, pill, 20, theme.windowBg, theme.bubbleOther, theme.border);
    Text(dc, L"说点什么…（回车发送）", RECT{pill.left + 16, pill.top, pill.right - 16, pill.bottom},
         fonts.message, theme.system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT plus{pill.right + 6, viewBottom + 10, pill.right + 6 + 38, viewBottom + 50};
    ui::DrawRoundedControl(dc, plus, 19, theme.windowBg, theme.neutral, theme.neutralBorder);
    Text(dc, L"＋", RECT{plus.left, plus.top - 1, plus.right, plus.bottom}, fonts.message, theme.text,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// 设置窗口（盖在主界面上，画成模态对话框的样子）
void DrawSettings(Canvas& canvas, const ChatMetrics& fonts, const Theme& theme) {
    DrawProposalA(canvas, fonts, theme);  // 背景用方案 A 的主界面
    HDC dc = canvas.dc();

    // 主界面压暗
    RECT all{0, 0, kWinW, kWinH};
    HBRUSH dim = CreateSolidBrush(RGB(0x14, 0x14, 0x14));
    // 没法直接半透明，这里用"只覆盖一次、颜色接近背景"的做法示意
    (void)dim;
    DeleteObject(dim);

    const int w = 620;
    const int h = 470;
    const int left = (kWinW - w) / 2;
    const int top = 46;
    RECT box{left, top, left + w, top + h};
    ui::DrawRoundedControl(dc, box, 12, theme.windowBg, theme.panel, theme.border);

    RECT title{left + 20, top + 8, left + w - 60, top + 48};
    Text(dc, L"设置", title, fonts.announce, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // 关闭按钮
    RECT close{left + w - 40, top + 12, left + w - 12, top + 40};
    ui::FillRoundedRect(dc, close, 6, theme.hover, theme.hover, 0.0f);
    Text(dc, L"✕", close, fonts.message, theme.system, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // 分隔线
    canvas.Fill(RECT{left + 12, top + 52, left + w - 12, top + 53}, theme.border);

    // 左标签列宽 150，右控件区从 left+190 开始（留足中文标签的位置，
    // 免得"彩色聊天（给昵称上色）"这种长标签被挤到控件底下）
    const int labelLeft = left + 24;
    const int ctrlLeft = left + 190;
    const int ctrlRight = left + w - 24;
    int y = top + 72;
    auto label = [&](const wchar_t* text) {
        RECT rect{labelLeft, y, ctrlLeft - 12, y + 30};
        Text(dc, text, rect, fonts.message, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto section = [&](const wchar_t* text) {
        RECT rect{labelLeft, y, ctrlRight, y + 22};
        Text(dc, text, rect, fonts.small, theme.system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 26;
    };
    auto toggle = [&](bool on) {
        RECT track{ctrlLeft, y + 3, ctrlLeft + 42, y + 25};
        ui::DrawRoundedControl(dc, track, 11, theme.panel, on ? theme.accent : theme.neutral,
                               on ? theme.accent : theme.neutralBorder);
        RECT knob{on ? track.right - 20 : track.left + 2, track.top + 2,
                  on ? track.right - 2 : track.left + 20, track.bottom - 2};
        ui::FillRoundedRect(dc, knob, 9, on ? theme.accentText : theme.text,
                            on ? theme.accentText : theme.text, 0.0f);
    };
    auto segment = [&](const wchar_t* const* options, int count, int selected) {
        RECT seg{ctrlLeft, y, ctrlRight, y + 32};
        ui::DrawRoundedControl(dc, seg, 8, theme.panel, theme.neutral, theme.border);
        const int segW = (seg.right - seg.left) / count;
        for (int i = 0; i < count; ++i) {
            RECT part{seg.left + i * segW + 2, seg.top + 2, seg.left + (i + 1) * segW - 2,
                      seg.bottom - 2};
            if (i == selected) ui::FillRoundedRect(dc, part, 6, theme.accent, theme.accent, 0.0f);
            Text(dc, options[i], part, fonts.message,
                 i == selected ? theme.accentText : theme.system,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    };

    section(L"外观");
    label(L"主题");
    {
        const wchar_t* options[] = {L"深色", L"浅色", L"跟随系统"};
        segment(options, 3, 0);
    }
    y += 42;
    label(L"彩色聊天");
    toggle(true);
    {
        RECT hint{ctrlLeft + 54, y, ctrlRight, y + 28};
        Text(dc, L"给昵称和气泡上色", hint, fonts.small, theme.system,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += 46;

    section(L"服务器");
    label(L"地址");
    {
        RECT field{ctrlLeft, y, ctrlRight, y + 32};
        ui::DrawRoundedControl(dc, field, 8, theme.panel, theme.bubbleOther, theme.border);
        Text(dc, L"192.168.1.100", RECT{field.left + 12, field.top, field.right, field.bottom},
             fonts.message, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += 42;
    label(L"端口");
    {
        RECT field{ctrlLeft, y, ctrlLeft + 140, y + 32};
        ui::DrawRoundedControl(dc, field, 8, theme.panel, theme.bubbleOther, theme.border);
        Text(dc, L"5555", RECT{field.left + 12, field.top, field.right, field.bottom}, fonts.message,
             theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += 52;

    section(L"语音");
    label(L"最长时长");
    {
        const wchar_t* options[] = {L"30 秒", L"1 分钟", L"2 分钟"};
        segment(options, 3, 1);
    }
    y += 42;
    {
        RECT hint{ctrlLeft, y, ctrlRight, y + 24};
        Text(dc, L"当前格式是未压缩 PCM，2 MB 上限只够 1 分钟", hint, fonts.small, theme.system,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 底部按钮
    RECT ok{ctrlRight - 96, top + h - 52, ctrlRight, top + h - 16};
    PillButton(dc, ok, L"完成", fonts.message, theme, true);
    RECT cancel{ok.left - 96 - 10, ok.top, ok.left - 10, ok.bottom};
    PillButton(dc, cancel, L"取消", fonts.message, theme, false);
}

struct Output {
    std::wstring name;
    void (*draw)(Canvas&, const ChatMetrics&, const Theme&);
};

}  // namespace

int main(int argc, char** argv) {
    std::string outDir = (argc > 1) ? argv[1] : "build";
    const std::wstring wideDir(outDir.begin(), outDir.end());
    if (!ui::Startup()) {
        std::printf("GDI+ 初始化失败\n");
        return 1;
    }

    ChatMetrics fonts;
    fonts.message = MakeFont(dchat::kMessageFontHeight);
    fonts.notice = MakeFont(dchat::kNoticeFontHeight);
    fonts.announce = MakeFont(dchat::kAnnounceFontHeight, FW_SEMIBOLD);
    fonts.small = MakeFont(dchat::kNoticeFontHeight);

    const Output outputs[] = {
        {L"ui-proposal-current.png", DrawCurrent},
        {L"ui-proposal-a.png", DrawProposalA},
        {L"ui-proposal-b.png", DrawProposalB},
        {L"ui-proposal-settings.png", DrawSettings},
    };

    int failures = 0;
    for (const Output& output : outputs) {
        Canvas canvas(kWinW, kWinH);
        output.draw(canvas, fonts, kDark);
        const std::wstring path = wideDir + L"\\" + output.name;
        if (canvas.SavePng(path)) {
            std::wprintf(L"  ok   %ls\n", path.c_str());
        } else {
            ++failures;
            std::wprintf(L"  FAIL %ls\n", path.c_str());
        }
    }

    for (HFONT font : {fonts.message, fonts.notice, fonts.announce, fonts.small}) DeleteObject(font);
    ui::Shutdown();
    std::printf("%d 张效果图，%d 个失败\n", static_cast<int>(sizeof(outputs) / sizeof(outputs[0])),
                failures);
    return failures == 0 ? 0 : 1;
}
