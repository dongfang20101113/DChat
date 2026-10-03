#include "color_picker.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace dchat {
namespace {

constexpr double kPi = 3.14159265358979323846;

int ClampInt(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

double Clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

int Channel(double value) { return ClampInt(static_cast<int>(value * 255.0 + 0.5), 0, 255); }

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

}  // namespace

COLORREF HsvToRgb(double h, double s, double v) {
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    s = Clamp01(s);
    v = Clamp01(v);
    const double c = v * s;
    const double hp = h / 60.0;
    const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if (hp < 1.0) {
        r = c; g = x;
    } else if (hp < 2.0) {
        r = x; g = c;
    } else if (hp < 3.0) {
        g = c; b = x;
    } else if (hp < 4.0) {
        g = x; b = c;
    } else if (hp < 5.0) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }
    const double m = v - c;
    return RGB(Channel(r + m), Channel(g + m), Channel(b + m));
}

void RgbToHsv(COLORREF color, double* h, double* s, double* v) {
    const double r = GetRValue(color) / 255.0;
    const double g = GetGValue(color) / 255.0;
    const double b = GetBValue(color) / 255.0;
    const double maxValue = (std::max)(r, (std::max)(g, b));
    const double minValue = (std::min)(r, (std::min)(g, b));
    const double delta = maxValue - minValue;
    double hue = 0.0;
    if (delta > 1e-9) {
        if (maxValue == r) {
            hue = 60.0 * std::fmod((g - b) / delta, 6.0);
        } else if (maxValue == g) {
            hue = 60.0 * ((b - r) / delta + 2.0);
        } else {
            hue = 60.0 * ((r - g) / delta + 4.0);
        }
        if (hue < 0.0) hue += 360.0;
    }
    if (h) *h = hue;
    if (s) *s = maxValue > 1e-9 ? delta / maxValue : 0.0;
    if (v) *v = maxValue;
}

double RingAngleToHue(double degrees) {
    double hue = degrees;  // 0 度 = 正右方，顺时针（屏幕 y 向下，所以顺时针就是角度增大）
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) hue += 360.0;
    return hue;
}

double HueToRingAngle(double hue) { return RingAngleToHue(hue); }

PickerGeometry PickerLayout(int clientWidth, int clientHeight) {
    PickerGeometry geometry;
    geometry.centerX = clientWidth / 2;
    geometry.centerY = kPickerMargin + kPickerOuterRadius;
    geometry.outerRadius = kPickerOuterRadius;
    geometry.innerRadius = kPickerOuterRadius - kPickerRingWidth;

    // 方块必须**整个落在环的内圈里**：方块的角到中心的距离是 side*sqrt(2)/2，
    // 只按边长比较的话四角会伸进环里——那些点同时命中环和方块，
    // 取到的颜色就和眼睛看到的位置对不上了（实测就是这个 bug）。
    // 所以边长还要受内圈直径 / sqrt(2) 约束。
    const int preferred = (kPickerOuterRadius * 2 * kPickerSquareRatio) / 100;
    const int byDiagonal = static_cast<int>(geometry.innerRadius * 2 / 1.41421356237) - 2;
    const int side = std::min(preferred, byDiagonal);
    geometry.square = RECT{geometry.centerX - side / 2, geometry.centerY - side / 2,
                           geometry.centerX - side / 2 + side,
                           geometry.centerY - side / 2 + side};

    const int footerTop = kPickerMargin + kPickerOuterRadius * 2 + 6;
    const int half = (clientWidth - kPickerMargin * 2 - 8) / 2;
    geometry.preview = RECT{kPickerMargin, footerTop, kPickerMargin + half,
                            footerTop + kPickerPreviewH};

    // 常用色那一排：铺在预览色块下面
    const int swatchTop = footerTop + kPickerPreviewH + 6;
    geometry.swatches = RECT{kPickerMargin, swatchTop, clientWidth - kPickerMargin,
                             swatchTop + 20};
    (void)clientHeight;
    return geometry;
}

bool HitRing(const PickerGeometry& geometry, int x, int y) {
    const double dx = x - geometry.centerX;
    const double dy = y - geometry.centerY;
    const double distance = std::sqrt(dx * dx + dy * dy);
    return distance <= geometry.outerRadius && distance >= geometry.innerRadius;
}

bool HitSquare(const PickerGeometry& geometry, int x, int y) {
    return x >= geometry.square.left && x < geometry.square.right &&
           y >= geometry.square.top && y < geometry.square.bottom;
}

bool HitSwatches(const PickerGeometry& geometry, int x, int y) {
    return x >= geometry.swatches.left && x < geometry.swatches.right &&
           y >= geometry.swatches.top && y < geometry.swatches.bottom;
}

double HueAtPoint(const PickerGeometry& geometry, int x, int y) {
    const double dx = x - geometry.centerX;
    const double dy = y - geometry.centerY;
    const double degrees = std::atan2(dy, dx) * 180.0 / kPi;
    return RingAngleToHue(degrees);
}

void SquareValueAtPoint(const PickerGeometry& geometry, int x, int y, double* s, double* v) {
    const double width = geometry.square.right - geometry.square.left;
    const double height = geometry.square.bottom - geometry.square.top;
    const double sx = x - geometry.square.left;
    // y 向下增大，而"越往上越亮"，所以纵向要反过来
    const double sy = geometry.square.bottom - 1 - y;
    if (s) *s = width > 0 ? Clamp01(sx / (width - 1)) : 0.0;
    if (v) *v = height > 0 ? Clamp01(sy / (height - 1)) : 0.0;
}

bool ColorAtPoint(const PickerGeometry& geometry, int x, int y, double currentHue,
                  COLORREF* color) {
    if (HitRing(geometry, x, y)) {
        // 环上：色相由角度定，饱和度和明度都取满（环本身就是"纯色"）
        if (color) *color = HsvToRgb(HueAtPoint(geometry, x, y), 1.0, 1.0);
        return true;
    }
    if (HitSquare(geometry, x, y)) {
        // 方块里：色相沿用环上选中的那个，饱和度和明度由位置定
        double s = 0, v = 0;
        SquareValueAtPoint(geometry, x, y, &s, &v);
        if (color) *color = HsvToRgb(currentHue, s, v);
        return true;
    }
    return false;
}

POINT RingMarkerPoint(const PickerGeometry& geometry, COLORREF color) {
    double h = 0, s = 0, v = 0;
    RgbToHsv(color, &h, &s, &v);
    const double radians = HueToRingAngle(h) * kPi / 180.0;
    const double radius = (geometry.outerRadius + geometry.innerRadius) / 2.0;
    POINT point{};
    point.x = geometry.centerX + static_cast<LONG>(std::cos(radians) * radius + 0.5);
    point.y = geometry.centerY + static_cast<LONG>(std::sin(radians) * radius + 0.5);
    return point;
}

POINT SquareMarkerPoint(const PickerGeometry& geometry, COLORREF color) {
    double h = 0, s = 0, v = 0;
    RgbToHsv(color, &h, &s, &v);
    const int width = geometry.square.right - geometry.square.left;
    const int height = geometry.square.bottom - geometry.square.top;
    POINT point{};
    point.x = geometry.square.left + static_cast<LONG>(s * (width - 1) + 0.5);
    point.y = geometry.square.bottom - 1 - static_cast<LONG>(v * (height - 1) + 0.5);
    (void)h;
    return point;
}

COLORREF SquarePixel(const RECT& square, int x, int y, double hue) {
    const int width = square.right - square.left;
    const int height = square.bottom - square.top;
    const double s = width > 1 ? static_cast<double>(x - square.left) / (width - 1) : 0.0;
    const double v = height > 1 ? static_cast<double>(square.bottom - 1 - y) / (height - 1) : 0.0;
    return HsvToRgb(hue, Clamp01(s), Clamp01(v));
}

bool ParseHexColor(const std::wstring& text, COLORREF* color) {
    std::wstring body = text;
    while (!body.empty() && (body.front() == L' ' || body.front() == L'\t')) body.erase(body.begin());
    while (!body.empty() && (body.back() == L' ' || body.back() == L'\t')) body.pop_back();
    if (!body.empty() && body[0] == L'#') body.erase(body.begin());
    if (body.size() == 3) {  // #abc -> #aabbcc
        std::wstring expanded;
        for (wchar_t ch : body) {
            expanded.push_back(ch);
            expanded.push_back(ch);
        }
        body = expanded;
    }
    if (body.size() != 6) return false;
    int value = 0;
    for (wchar_t ch : body) {
        const int digit = HexDigit(ch);
        if (digit < 0) return false;
        value = value * 16 + digit;
    }
    if (color) *color = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

const COLORREF* QuickPickColors(int* count) {
    static const COLORREF colors[] = {
        RGB(0, 0, 0),       RGB(128, 128, 128), RGB(192, 192, 192), RGB(255, 255, 255),
        RGB(255, 0, 0),     RGB(255, 128, 0),   RGB(255, 255, 0),   RGB(0, 255, 0),
        RGB(0, 255, 255),   RGB(0, 0, 255),     RGB(128, 0, 255),   RGB(255, 0, 255),
        RGB(128, 0, 0),     RGB(0, 128, 0),     RGB(0, 0, 128),     RGB(255, 128, 128)};
    if (count) *count = static_cast<int>(sizeof(colors) / sizeof(colors[0]));
    return colors;
}

}  // namespace dchat
