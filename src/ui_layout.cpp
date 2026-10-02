// 主窗口外壳的布局与图标。设计取舍见 ui_layout.h。
#include "ui_layout.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dchat {
namespace {

// 菜单项宽度 = 文字宽度 + 左右内边距
int MenuItemWidth(HDC dc, HFONT font, const std::wstring& label) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
    SelectObject(dc, old);
    return size.cx + kMenuItemPadX * 2;
}

SIZE MeasureText(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size;
}

}  // namespace

std::vector<MenuItem> LayoutMenuItems(HDC dc, HFONT font, int windowWidth, bool connected) {
    std::vector<MenuItem> items;
    // 第一项是"连接/断开"：**它跟着状态变文字**，不再像以前那样两个按钮都摆着
    // （两个按钮永远只有一个能用，占地方还让人犹豫点哪个）
    items.push_back({MenuAction::Connect, connected ? L"断开" : L"连接", {}, false});
    items.push_back({MenuAction::Settings, L"设置", {}, false});
    items.push_back({MenuAction::Help, L"帮助", {}, false});

    int x = 10;
    for (MenuItem& item : items) {
        const int width = MenuItemWidth(dc, font, item.label);
        item.rect = RECT{x, kMenuItemsTop, x + width, kMenuItemsTop + 30};
        x += width;
    }
    (void)windowWidth;
    return items;
}

const MenuItem* HitTestMenu(const std::vector<MenuItem>& items, int x, int y) {
    for (const MenuItem& item : items) {
        if (x >= item.rect.left && x < item.rect.right && y >= item.rect.top &&
            y < item.rect.bottom) {
            return &item;
        }
    }
    return nullptr;
}

RECT GearRect(int windowWidth) {
    const int right = windowWidth - 8;
    return RECT{right - kGearSize, kMenuItemsTop, right, kMenuItemsTop + kGearSize};
}

RECT StatusChipRect(HDC dc, HFONT font, int windowWidth, const std::wstring& text) {
    const SIZE size = MeasureText(dc, font, text);
    const int width = size.cx + 24;
    // 齿轮左边 10 像素处，垂直居中
    const int right = GearRect(windowWidth).left - 10;
    const int top = kMenuItemsTop + (30 - kStatusChipHeight) / 2;
    return RECT{right - width, top, right, top + kStatusChipHeight};
}

BottomLayout LayoutBottom(HDC dc, HFONT font, int windowWidth, int windowHeight, int contentTop) {
    (void)dc;
    (void)font;
    (void)contentTop;
    BottomLayout layout;
    const int statusTop = windowHeight - kStatusBarHeight - kInputRowHeight;
    layout.statusBar = RECT{0, statusTop, windowWidth, statusTop + kStatusBarHeight};

    const int rowTop = statusTop + kStatusBarHeight;
    const int pillTop = rowTop + (kInputRowHeight - kInputPillHeight) / 2;
    const int plusLeft = windowWidth - kInputRowMargin - kSendWidth - 8 - kPlusSize;
    const int pillRight = plusLeft - 8;
    layout.inputPill = RECT{kInputRowMargin, pillTop, pillRight, pillTop + kInputPillHeight};
    layout.plusButton = RECT{plusLeft, rowTop + (kInputRowHeight - kPlusSize) / 2,
                             plusLeft + kPlusSize, rowTop + (kInputRowHeight + kPlusSize) / 2};
    layout.sendButton = RECT{windowWidth - kInputRowMargin - kSendWidth,
                             rowTop + (kInputRowHeight - kInputPillHeight) / 2,
                             windowWidth - kInputRowMargin,
                             rowTop + (kInputRowHeight + kInputPillHeight) / 2};
    return layout;
}

PlusMenuLayout LayoutPlusMenu(const BottomLayout& bottom, int contentTop) {
    PlusMenuLayout menu;
    const int itemHeight = 34;
    const int panelHeight = itemHeight * 2 + 12;
    const int panelWidth = 180;
    const int panelTop = bottom.plusButton.top - 8 - panelHeight;
    // 上方放不下就不弹（否则会盖住聊天记录的最上面几条，还看不出来自己在哪）
    if (panelTop < contentTop + 4) return menu;

    menu.panel = RECT{bottom.plusButton.right - panelWidth, panelTop, bottom.plusButton.right,
                      panelTop + panelHeight};
    menu.fileItem = RECT{menu.panel.left + 6, menu.panel.top + 6, menu.panel.right - 6,
                         menu.panel.top + 6 + itemHeight};
    menu.voiceItem = RECT{menu.fileItem.left, menu.fileItem.bottom + 2, menu.fileItem.right,
                          menu.fileItem.bottom + 2 + itemHeight};
    menu.valid = true;
    return menu;
}

PlusAction HitTestPlusMenu(const PlusMenuLayout& menu, int x, int y) {
    if (!menu.valid) return PlusAction::None;
    auto inside = [&](const RECT& rect) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    };
    if (inside(menu.fileItem)) return PlusAction::SendFile;
    if (inside(menu.voiceItem)) return PlusAction::RecordVoice;
    return PlusAction::None;
}

bool InsidePlusMenu(const PlusMenuLayout& menu, int x, int y) {
    if (!menu.valid) return false;
    return x >= menu.panel.left && x < menu.panel.right && y >= menu.panel.top &&
           y < menu.panel.bottom;
}

SettingsLayout LayoutSettings(int clientWidth, int clientHeight) {
    SettingsLayout layout;
    const int left = kSettingsPadX;
    const int right = clientWidth - kSettingsPadX;
    const int labelLeft = left;
    const int ctrlLeft = left + kSettingsLabelWidth + 16;
    const int ctrlRight = right;

    layout.title = RECT{left, 8, right - 48, 48};
    layout.closeButton = RECT{right - 28, 12, right, 40};
    layout.separator = RECT{left - 12, 52, right + 12, 53};

    int y = 72;
    // 外观
    y += 26;  // 「外观」小节标题
    layout.themeSegment = RECT{ctrlLeft, y, ctrlRight, y + kSettingsRowHeight - 2};
    y += 42;
    layout.colorToggle = RECT{ctrlLeft, y + 3, ctrlLeft + 42, y + 25};
    layout.colorToggleLabel = RECT{ctrlLeft + 54, y - 3, ctrlRight, y + 31};
    y += 46;
    // 服务器
    y += 26;  // 「服务器」小节标题
    layout.hostField = RECT{ctrlLeft, y, ctrlRight, y + kSettingsRowHeight - 2};
    y += 42;
    layout.portField = RECT{ctrlLeft, y, ctrlLeft + 140, y + kSettingsRowHeight - 2};
    y += 52;
    // 语音
    y += 26;  // 「语音」小节标题
    layout.voiceSegment = RECT{ctrlLeft, y, ctrlRight, y + kSettingsRowHeight - 2};
    y += 42;
    layout.voiceHint = RECT{ctrlLeft, y, ctrlRight, y + 24};

    (void)clientHeight;
    layout.okButton = RECT{right - 96, clientHeight - 52, right, clientHeight - 16};
    layout.cancelButton = RECT{layout.okButton.left - 106, layout.okButton.top,
                               layout.okButton.left - 10, layout.okButton.bottom};
    return layout;
}

int SegmentHitTest(const RECT& segment, int segments, int x, int y) {
    if (segments <= 0) return -1;
    if (x < segment.left || x >= segment.right || y < segment.top || y >= segment.bottom) return -1;
    const int width = segment.right - segment.left;
    const int index = (x - segment.left) * segments / (width > 0 ? width : 1);
    return std::min(index, segments - 1);
}

// ---------------------------------------------------------------------------
// 图标
// ---------------------------------------------------------------------------

void DrawGearIcon(HDC dc, int centerX, int centerY, int radius, COLORREF color) {
    const int inner = radius - 4;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    // 八个齿：用小圆糊成一圈（比画多边形简单，缩小到 16px 也还看得出是齿轮）
    for (int i = 0; i < 8; ++i) {
        const double angle = i * 3.14159265358979 / 4.0;
        const int x = centerX + static_cast<int>(std::cos(angle) * radius);
        const int y = centerY + static_cast<int>(std::sin(angle) * radius);
        const int tooth = radius / 3 > 2 ? radius / 3 : 2;
        Ellipse(dc, x - tooth, y - tooth, x + tooth, y + tooth);
    }
    Ellipse(dc, centerX - inner, centerY - inner, centerX + inner, centerY + inner);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawPlusIcon(HDC dc, int centerX, int centerY, int armLength, int thickness,
                  COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    RECT horizontal{centerX - armLength, centerY - thickness / 2,
                    centerX + armLength + 1, centerY - thickness / 2 + thickness};
    RECT vertical{centerX - thickness / 2, centerY - armLength,
                  centerX - thickness / 2 + thickness, centerY + armLength + 1};
    FillRect(dc, &horizontal, brush);
    FillRect(dc, &vertical, brush);
    DeleteObject(brush);
}

void DrawHamburgerIcon(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    const int lineHeight = 2;
    const int gap = 4;
    const int width = box.right - box.left;
    const int total = lineHeight * 3 + gap * 2;
    int y = box.top + ((box.bottom - box.top) - total) / 2;
    for (int i = 0; i < 3; ++i) {
        RECT line{box.left, y, box.left + width, y + lineHeight};
        FillRect(dc, &line, brush);
        y += lineHeight + gap;
    }
    DeleteObject(brush);
}

void DrawPlayIcon(HDC dc, const RECT& box, COLORREF color) {
    const int midY = (box.top + box.bottom) / 2;
    const int height = (box.bottom - box.top) / 2;
    POINT points[3] = {
        {box.left, midY - height},
        {box.right, midY},
        {box.left, midY + height},
    };
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawPauseIcon(HDC dc, const RECT& box, COLORREF color) {
    const int width = box.right - box.left;
    const int barWidth = width / 3 > 1 ? width / 3 : 1;
    HBRUSH brush = CreateSolidBrush(color);
    RECT left{box.left, box.top, box.left + barWidth, box.bottom};
    RECT right{box.right - barWidth, box.top, box.right, box.bottom};
    FillRect(dc, &left, brush);
    FillRect(dc, &right, brush);
    DeleteObject(brush);
}

void DrawThinBar(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

COLORREF AdjustColor(COLORREF color, int delta) {
    auto clamp = [](int value) { return value < 0 ? 0 : (value > 255 ? 255 : value); };
    return RGB(clamp(GetRValue(color) + delta), clamp(GetGValue(color) + delta),
               clamp(GetBValue(color) + delta));
}

}  // namespace dchat
