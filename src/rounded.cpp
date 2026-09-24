#include "rounded.h"

#include <objidl.h>  // MinGW 下 gdiplus.h 依赖这里的 PROPID
#include <gdiplus.h>

namespace ui {

namespace {
ULONG_PTR g_token = 0;
bool g_started = false;
}  // namespace

bool Startup() {
    if (g_started) return true;
    Gdiplus::GdiplusStartupInput input;
    g_started = Gdiplus::GdiplusStartup(&g_token, &input, nullptr) == Gdiplus::Ok;
    return g_started;
}

void Shutdown() {
    if (!g_started) return;
    Gdiplus::GdiplusShutdown(g_token);
    g_started = false;
}

namespace {

Gdiplus::Color ToGdiColor(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void BuildPath(Gdiplus::GraphicsPath& path, const RECT& rect, int radius) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    path.Reset();
    // 半径夹到可用上限：允许正好等于 min(宽,高)/2（这时形状是胶囊/圆形）
    const int limit = (width < height ? width : height) / 2;
    int r = radius < 0 ? 0 : radius;
    if (r > limit) r = limit;
    const int diameter = r * 2;
    if (diameter <= 0) {
        path.AddRectangle(Gdiplus::Rect(rect.left, rect.top, width, height));
        return;
    }
    path.AddArc(rect.left, rect.top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.left, rect.bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

}  // namespace

void DrawRoundedControl(HDC dc, const RECT& rect, int radius, COLORREF background, COLORREF fill,
                        COLORREF border) {
    // 先铺底色，把圆角外面那部分也一起覆盖掉
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(dc, &rect, backgroundBrush);
    DeleteObject(backgroundBrush);

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    BuildPath(path, rect, radius);
    Gdiplus::SolidBrush fillBrush(ToGdiColor(fill));
    graphics.FillPath(&fillBrush, &path);
    Gdiplus::Pen borderPen(ToGdiColor(border), 1.0f);
    graphics.DrawPath(&borderPen, &path);
}

void OutlineRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF border) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    BuildPath(path, rect, radius);
    Gdiplus::Pen pen(ToGdiColor(border), 1.0f);
    graphics.DrawPath(&pen, &path);
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border,
                     float borderWidth) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    BuildPath(path, rect, radius);
    Gdiplus::SolidBrush fillBrush(ToGdiColor(fill));
    graphics.FillPath(&fillBrush, &path);
    if (borderWidth > 0.0f) {
        Gdiplus::Pen pen(ToGdiColor(border), borderWidth);
        graphics.DrawPath(&pen, &path);
    }
}

}  // namespace ui
