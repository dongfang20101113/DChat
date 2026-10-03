// 取色盘对话框：色相环 + 饱和度/明度方块 + 当前色/原色对比 + 十六进制输入 + 常用色。
//
// 从 client.cpp 搬出来单独成模块，因为**几何和绘制必须共用一套**——各写一遍就会
// 出现"点到的颜色和眼睛看到的位置不一样"。搬出来还有个好处：工具能离屏出图
// （tools/picker_mockup.cpp），改颜色相关的东西时先看图再动手。
//
// 颜色数学与几何在 color_picker.cpp（纯计算 + 单测）。
#include "picker_dialog.h"

#include "chat_color.h"    // ColorToHex
#include "color_picker.h"
#include "rounded.h"

#include <objidl.h>
#include <gdiplus.h>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace dchat {
namespace {

// 和主界面深色主题同一套颜色（单独出图时看起来才和真界面一致）
constexpr COLORREF kPickerBg = RGB(32, 32, 32);
constexpr COLORREF kPickerBorder = RGB(64, 66, 70);
constexpr COLORREF kPickerField = RGB(48, 50, 54);
constexpr COLORREF kPickerText = RGB(232, 232, 232);
constexpr COLORREF kPickerDim = RGB(150, 150, 150);

constexpr DWORD kPickerStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kPickerExStyle = WS_EX_DLGMODALFRAME;

constexpr int kPickerOkId = 301;
constexpr int kPickerCancelId = 302;

// 字体由客户端设进来；单独出图时用系统默认字体
HFONT g_pickFont = nullptr;
HFONT g_pickFontSmall = nullptr;

HFONT PickFont() { return g_pickFont; }
HFONT PickFontSmall() { return g_pickFontSmall ? g_pickFontSmall : g_pickFont; }

// 自绘按钮：**按窗口记原来的过程**。主界面用过一个全局 WNDPROC，被后来的控件覆盖掉，
// 结果所有按钮都不画（踩过这个坑，这里不再犯）。
std::map<HWND, WNDPROC> g_pickerButtons;
std::map<HWND, bool> g_pickerHover;

void DrawTextAt(HDC dc, const std::wstring& text, const RECT& rect, HFONT font, COLORREF color,
                UINT flags) {
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    RECT target = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (oldFont) SelectObject(dc, oldFont);
}

void DrawPickerButton(const DRAWITEMSTRUCT* item) {
    if (!item) return;
    const RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool hover = g_pickerHover[item->hwndItem];
    const bool accent = item->CtlID == static_cast<UINT>(kPickerOkId);
    COLORREF fill = accent ? RGB(0, 120, 215) : RGB(52, 54, 58);
    if (pressed) {
        fill = RGB(GetRValue(fill) * 8 / 10, GetGValue(fill) * 8 / 10, GetBValue(fill) * 8 / 10);
    } else if (hover) {
        fill = RGB((std::min)(255, GetRValue(fill) + 18), (std::min)(255, GetGValue(fill) + 18),
                   (std::min)(255, GetBValue(fill) + 18));
    }
    ui::DrawRoundedControl(item->hDC, rect, 6, kPickerBg, fill,
                           accent ? RGB(0, 100, 180) : kPickerBorder, kPickerBg);
    wchar_t label[64] = {0};
    GetWindowTextW(item->hwndItem, label, 64);
    DrawTextAt(item->hDC, label, rect, PickFont(), accent ? RGB(255, 255, 255) : kPickerText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK PickerButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!g_pickerHover[hwnd]) {
                g_pickerHover[hwnd] = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        case WM_MOUSELEAVE:
            g_pickerHover[hwnd] = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WM_NCDESTROY: {
            const auto it = g_pickerButtons.find(hwnd);
            const WNDPROC previous = it == g_pickerButtons.end() ? nullptr : it->second;
            g_pickerButtons.erase(hwnd);
            g_pickerHover.erase(hwnd);
            return previous ? CallWindowProcW(previous, hwnd, msg, wp, lp)
                            : DefWindowProcW(hwnd, msg, wp, lp);
        }
        default:
            break;
    }
    const auto it = g_pickerButtons.find(hwnd);
    if (it == g_pickerButtons.end() || !it->second) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(it->second, hwnd, msg, wp, lp);
}


struct ColorPickerState {
    COLORREF chosen = RGB(255, 255, 255);   // 当前选中的颜色
    COLORREF original = RGB(255, 255, 255); // 打开时的颜色（对比用）
    COLORREF previewOriginal = RGB(255, 255, 255);
    double hue = 0.0;
    double sat = 1.0;
    double val = 1.0;
    HWND hexEdit = nullptr;
    bool accepted = false;
    bool dragging = false;   // 按住拖动能连续取色
    bool onRing = false;     // 拖动时锁在环上还是方块里
    POINT cursor{0, 0};
};

void SyncPickerFromColor(ColorPickerState* state) {
    double h = 0, s = 0, v = 0;
    RgbToHsv(state->chosen, &h, &s, &v);
    state->hue = h;
    state->sat = s;
    state->val = v;
    if (state->hexEdit) {
        const std::wstring text = ColorToHex(state->chosen);
        SetWindowTextW(state->hexEdit, text.c_str());
    }
}

// 画色相环：切成很多小扇形（每片 0.5 度，互相重叠一点，避免出现缝）
void DrawHueRing(HDC dc, const PickerGeometry& geometry) {
    constexpr int kWedges = 720;
    constexpr double kStep = 360.0 / kWedges;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (int i = 0; i < kWedges; ++i) {
        const double from = i * kStep;
        const double to = from + kStep * 1.6;  // 稍微重叠，避免细缝
        const COLORREF color = HsvToRgb(from, 1.0, 1.0);
        Gdiplus::GraphicsPath path;
        const int outer = geometry.outerRadius;
        const int inner = geometry.innerRadius;
        const int left = geometry.centerX - outer;
        const int top = geometry.centerY - outer;
        path.AddArc(left, top, outer * 2, outer * 2, static_cast<Gdiplus::REAL>(from),
                    static_cast<Gdiplus::REAL>(to - from));
        const int innerLeft = geometry.centerX - inner;
        const int innerTop = geometry.centerY - inner;
        path.AddArc(innerLeft, innerTop, inner * 2, inner * 2, static_cast<Gdiplus::REAL>(to),
                    static_cast<Gdiplus::REAL>(from - to));
        path.CloseFigure();
        Gdiplus::SolidBrush brush(
            Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &path);
    }
}

// 画中间的饱和度/明度方块：**逐行用 GradientFill 式的线性渐变**太麻烦，
// 这里按行画一条水平渐变（左白 -> 右纯色），行与行之间用明度控制。
void DrawSaturationSquare(HDC dc, const PickerGeometry& geometry, double hue) {
    const RECT square = geometry.square;
    const int height = square.bottom - square.top;
    if (height <= 0) return;
    // 每行画成一条水平渐变：用 GDI+ 的 LinearGradientBrush
    const COLORREF pure = HsvToRgb(hue, 1.0, 1.0);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    for (int y = square.top; y < square.bottom; ++y) {
        const double v = static_cast<double>(square.bottom - 1 - y) / (height - 1);
        Gdiplus::Point from(square.left, y);
        Gdiplus::Point to(square.right, y);
        Gdiplus::LinearGradientBrush brush(
            from, to,
            Gdiplus::Color(255, static_cast<BYTE>(255 * v), static_cast<BYTE>(255 * v),
                           static_cast<BYTE>(255 * v)),
            Gdiplus::Color(255, static_cast<BYTE>(GetRValue(pure) * v),
                           static_cast<BYTE>(GetGValue(pure) * v),
                           static_cast<BYTE>(GetBValue(pure) * v)));
        graphics.FillRectangle(&brush, square.left, y, square.right - square.left, 1);
    }
}

void DrawPickerPanel(HDC dc, const PickerGeometry& geometry, const ColorPickerState& state) {
    // 色相环 + 方块
    DrawHueRing(dc, geometry);
    DrawSaturationSquare(dc, geometry, state.hue);

    // 环上的选中标记
    const POINT ringMark = RingMarkerPoint(geometry, state.chosen);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    {
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), 2.0f);
        graphics.DrawEllipse(&pen, ringMark.x - 6, ringMark.y - 6, 12, 12);
        Gdiplus::Pen dark(Gdiplus::Color(200, 0, 0, 0), 1.0f);
        graphics.DrawEllipse(&dark, ringMark.x - 8, ringMark.y - 8, 16, 16);
    }
    // 方块里的选中标记
    const POINT squareMark = SquareMarkerPoint(geometry, state.chosen);
    {
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), 2.0f);
        graphics.DrawEllipse(&pen, squareMark.x - 5, squareMark.y - 5, 10, 10);
        Gdiplus::Pen dark(Gdiplus::Color(200, 0, 0, 0), 1.0f);
        graphics.DrawEllipse(&dark, squareMark.x - 6, squareMark.y - 6, 12, 12);
    }

    // 右侧：新 / 原色对比（各自上面一行小标签）
    const int half = (geometry.preview.right - geometry.preview.left - 8) / 2;
    RECT currentRect = geometry.preview;
    currentRect.right = currentRect.left + half;
    RECT originalRect = geometry.preview;
    originalRect.left = originalRect.right - half;
    RECT currentLabel{currentRect.left, currentRect.top - 18, currentRect.right,
                      currentRect.top - 2};
    RECT originalLabel{originalRect.left, originalRect.top - 18, originalRect.right,
                       originalRect.top - 2};
    DrawTextAt(dc, L"新颜色", currentLabel, PickFontSmall(), kPickerDim,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextAt(dc, L"原颜色", originalLabel, PickFontSmall(), kPickerDim,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ui::FillRoundedRect(dc, currentRect, 6, state.chosen, state.chosen, 0.0f);
    ui::FillRoundedRect(dc, originalRect, 6, state.original, state.original, 0.0f);
    ui::OutlineRoundedRect(dc, currentRect, 6, kPickerBorder);
    ui::OutlineRoundedRect(dc, originalRect, 6, kPickerBorder);
    DrawTextAt(dc, L"新", currentRect, PickFontSmall(), ContrastText(state.chosen),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextAt(dc, L"原", originalRect, PickFontSmall(), ContrastText(state.original),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 十六进制输入框（底板 + 上面的小标题）
    RECT hexLabel{geometry.hexField.left, geometry.hexField.top - 18, geometry.hexField.right,
                  geometry.hexField.top - 2};
    DrawTextAt(dc, L"十六进制色码", hexLabel, PickFontSmall(), kPickerDim,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ui::DrawRoundedControl(dc, geometry.hexField, 6, kPickerBg, kPickerField, kPickerBorder,
                           kPickerBg);

    // 常用色：网格（8 列 x 2 行），格与格之间留缝，不再挤成一条
    int count = 0;
    const COLORREF* quick = QuickPickColors(&count);
    RECT swatchLabel{geometry.swatches.left, geometry.swatches.top - 18, geometry.swatches.right,
                     geometry.swatches.top - 2};
    DrawTextAt(dc, L"常用色", swatchLabel, PickFontSmall(), kPickerDim,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (int i = 0; i < count; ++i) {
        const int col = i % kPickerSwatchCols;
        const int row = i / kPickerSwatchCols;
        RECT cellRect{geometry.swatches.left + col * kPickerSwatchCell + 2,
                      geometry.swatches.top + row * kPickerSwatchCell + 2,
                      geometry.swatches.left + (col + 1) * kPickerSwatchCell - 2,
                      geometry.swatches.top + (row + 1) * kPickerSwatchCell - 2};
        ui::FillRoundedRect(dc, cellRect, 4, quick[i], quick[i], 0.0f);
        ui::OutlineRoundedRect(dc, cellRect, 4, kPickerBorder);
    }
}

// 在深色/浅色底上都能看清标签
void PickerApplyPoint(ColorPickerState* state, const PickerGeometry& geometry, int x, int y) {
    if (HitRing(geometry, x, y)) {
        state->hue = HueAtPoint(geometry, x, y);
        state->sat = 1.0;
        state->val = 1.0;
    } else if (HitSquare(geometry, x, y)) {
        SquareValueAtPoint(geometry, x, y, &state->sat, &state->val);
    }
    state->chosen = HsvToRgb(state->hue, state->sat, state->val);
    SyncPickerFromColor(state);
}

LRESULT CALLBACK ColorPickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<ColorPickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<ColorPickerState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            RECT client{};
            GetClientRect(hwnd, &client);
            state->hexEdit = CreateWindowExW(
                0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPickerHexEditId)), nullptr, nullptr);
            SendMessageW(state->hexEdit, WM_SETFONT, reinterpret_cast<WPARAM>(PickFont()), TRUE);
            SendMessageW(state->hexEdit, EM_SETLIMITTEXT, 7, 0);
            // 确定 / 取消：确定把当前颜色带回去，取消原样丢弃
            const HWND ok = CreateWindowW(
                L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPickerOkId)), nullptr, nullptr);
            const HWND cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPickerCancelId)), nullptr, nullptr);
            SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(PickFont()), TRUE);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(PickFont()), TRUE);
            // 自绘按钮的过程**按窗口记**（一个全局变量会被后面的控件覆盖，踩过）
            g_pickerButtons[ok] = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(ok, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PickerButtonProc)));
            g_pickerButtons[cancel] = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(cancel, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(PickerButtonProc)));
            SyncPickerFromColor(state);
            return 0;
        }
        case WM_SIZE: {
            if (!state) return 0;
            RECT client{};
            GetClientRect(hwnd, &client);
            const PickerGeometry geometry = PickerLayout(client.right, client.bottom);
            // 输入框和两个按钮按布局矩形摆（布局常量都在 color_picker.h）
            MoveWindow(state->hexEdit, geometry.hexField.left, geometry.hexField.top + 3,
                       geometry.hexField.right - geometry.hexField.left,
                       geometry.hexField.bottom - geometry.hexField.top - 6, TRUE);
            MoveWindow(GetDlgItem(hwnd, kPickerOkId), geometry.okButton.left,
                       geometry.okButton.top, geometry.okButton.right - geometry.okButton.left,
                       geometry.okButton.bottom - geometry.okButton.top, TRUE);
            MoveWindow(GetDlgItem(hwnd, kPickerCancelId), geometry.cancelButton.left,
                       geometry.cancelButton.top,
                       geometry.cancelButton.right - geometry.cancelButton.left,
                       geometry.cancelButton.bottom - geometry.cancelButton.top, TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(kPickerBg);
            FillRect(dc, &client, bg);
            DeleteObject(bg);
            if (state) {
                const PickerGeometry geometry =
                    PickerLayout(client.right, client.bottom);
                DrawPickerPanel(dc, geometry, *state);
                DrawTextAt(dc, L"回车确定 · Esc 取消", geometry.hint, PickFontSmall(), kPickerDim,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kPickerText);
            SetBkColor(dc, kPickerField);
            SetBkMode(dc, TRANSPARENT);
            static HBRUSH brush = CreateSolidBrush(kPickerField);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_MOUSEMOVE: {
            if (!state) return 0;
            RECT client{};
            GetClientRect(hwnd, &client);
            const PickerGeometry geometry =
                PickerLayout(client.right, client.bottom);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (state->dragging) {
                if (state->onRing) {
                    if (HitRing(geometry, x, y)) {
                        state->hue = HueAtPoint(geometry, x, y);
                        state->chosen = HsvToRgb(state->hue, state->sat, state->val);
                        SyncPickerFromColor(state);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    SquareValueAtPoint(geometry, x, y, &state->sat, &state->val);
                    state->chosen = HsvToRgb(state->hue, state->sat, state->val);
                    SyncPickerFromColor(state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!state) return 0;
            SetFocus(hwnd);
            RECT client{};
            GetClientRect(hwnd, &client);
            const PickerGeometry geometry =
                PickerLayout(client.right, client.bottom);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);

            // 常用色网格
            int count = 0;
            const COLORREF* quick = QuickPickColors(&count);
            if (count > 0 && HitSwatches(geometry, x, y)) {
                const int col = (x - geometry.swatches.left) / kPickerSwatchCell;
                const int row = (y - geometry.swatches.top) / kPickerSwatchCell;
                const int index = row * kPickerSwatchCols + col;
                if (col >= 0 && col < kPickerSwatchCols && row >= 0 && index < count) {
                    state->chosen = quick[index];
                    SyncPickerFromColor(state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (HitRing(geometry, x, y)) {
                state->dragging = true;
                state->onRing = true;
                SetCapture(hwnd);
                PickerApplyPoint(state, geometry, x, y);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (HitSquare(geometry, x, y)) {
                state->dragging = true;
                state->onRing = false;
                SetCapture(hwnd);
                PickerApplyPoint(state, geometry, x, y);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (state && state->dragging) {
                state->dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wp) == kPickerHexEditId && HIWORD(wp) == EN_CHANGE) {
                // 直接在输入框里打色码：解析成功就立刻同步到色盘
                wchar_t buffer[16] = {0};
                GetWindowTextW(state->hexEdit, buffer, 16);
                COLORREF parsed = 0;
                if (ParseHexColor(buffer, &parsed) && parsed != state->chosen) {
                    state->chosen = parsed;
                    double h = 0, s = 0, v = 0;
                    RgbToHsv(parsed, &h, &s, &v);
                    state->hue = h;
                    state->sat = s;
                    state->val = v;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (state && LOWORD(wp) == kPickerOkId) {
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 弹取色盘；用户确定了返回 true，把颜色写进 *out。
// **取消（Esc / 关掉）不写回颜色**，相当于"这次不选"。
bool ShowColorPickerImpl(HWND parent, COLORREF initial, COLORREF* out) {
    constexpr const wchar_t* kPickerClass = L"DchatColorPicker";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wc.lpfnWndProc = ColorPickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPickerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    ColorPickerState state;
    state.chosen = initial;
    state.original = initial;
    state.previewOriginal = initial;
    SyncPickerFromColor(&state);

    RECT frame{0, 0, kPickerWindowWidth, kPickerWindowHeight};
    AdjustWindowRectEx(&frame, kPickerStyle, FALSE, kPickerExStyle);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    const int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    const int y = parentRect.top + 60;

    HWND picker = CreateWindowExW(kPickerExStyle, kPickerClass, L"选择颜色", kPickerStyle, x, y, width,
                                  height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!picker) return false;
    EnableWindow(parent, FALSE);
    ShowWindow(picker, SW_SHOW);
    (void)0;

    MSG msg;
    while (IsWindow(picker) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
            (msg.hwnd == picker || IsChild(picker, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(picker);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                state.accepted = true;
                DestroyWindow(picker);
                continue;
            }
        }
        if (!IsDialogMessageW(picker, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(picker)) DestroyWindow(picker);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    // 焦点交回主窗口的输入框由调用方负责（这里只把主窗口重新激活）
    if (!state.accepted) return false;
    *out = state.chosen;
    return true;
}


}  // namespace

void SetPickerFonts(HFONT normal, HFONT small) {
    g_pickFont = normal;
    g_pickFontSmall = small;
}

void DrawColorPicker(HDC dc, int width, int height, COLORREF current, COLORREF original) {
    const PickerGeometry geometry = PickerLayout(width, height);
    ColorPickerState state;
    state.chosen = current;
    state.original = original;
    RgbToHsv(current, &state.hue, &state.sat, &state.val);
    DrawPickerPanel(dc, geometry, state);
}

bool ShowColorPicker(HWND parent, COLORREF initial, COLORREF* out) {
    return ShowColorPickerImpl(parent, initial, out);
}

COLORREF ContrastText(COLORREF background) {
    const int luma = (GetRValue(background) * 299 + GetGValue(background) * 587 +
                      GetBValue(background) * 114) /
                     1000;
    return luma > 140 ? RGB(20, 20, 20) : RGB(240, 240, 240);
}

}  // namespace dchat