// 取色盘（Windows 画图那种：色相环 + 中间的饱和度/明度方块）。
//
// 只有纯计算：HSV 换算、环/方块的几何与命中、像素反解。
// **绘制和命中必须共用这里的一套函数**——两边各写一遍，取到的颜色就会和眼睛看到的位置差一格。
#pragma once

#include <windows.h>

#include <string>

namespace dchat {

// ---- 色盘几何（客户区坐标，单位像素）----
inline constexpr int kPickerOuterRadius = 92;  // 环形外半径
inline constexpr int kPickerRingWidth = 24;    // 环形宽度
inline constexpr int kPickerSquareRatio = 70;  // 中间方块边长 = 外直径 * 70%（百分比）

inline constexpr int kPickerMargin = 14;   // 色盘四周留白
inline constexpr int kPickerPreviewH = 34; // 底部"当前颜色 / 原颜色"两块的高度
inline constexpr int kPickerFooterH = 56;  // 底部整体高度（色块 + 一行文字）

inline constexpr int kPickerWindowWidth = kPickerOuterRadius * 2 + kPickerMargin * 2;
inline constexpr int kPickerWindowHeight =
    kPickerOuterRadius * 2 + kPickerMargin * 2 + kPickerFooterH;

struct PickerGeometry {
    int centerX = 0;
    int centerY = 0;
    int outerRadius = kPickerOuterRadius;
    int innerRadius = kPickerOuterRadius - kPickerRingWidth;
    RECT square{};     // 饱和度/明度方块
    RECT preview{};    // 左右两块预览（当前色 / 原色）
    RECT swatches{};   // 常用色那一排
};

/** 按窗口客户区大小算色盘布局。 */
PickerGeometry PickerLayout(int clientWidth, int clientHeight);

// ---- HSV <-> RGB ----
// h 是 [0,360) 度，s / v 是 [0,1]。都是纯函数，好测。
COLORREF HsvToRgb(double h, double s, double v);
void RgbToHsv(COLORREF color, double* h, double* s, double* v);

/** 环上某个角度对应的色相（0 度 = 正右方，顺时针增大）。 */
double RingAngleToHue(double degrees);
/** 色相反算成角度（0-360）。 */
double HueToRingAngle(double hue);

/** 点 (x,y) 在不在色相环那一圈里（环形外沿以内、内沿以外）。 */
bool HitRing(const PickerGeometry& geometry, int x, int y);
/** 点在不在中间的方块里。 */
bool HitSquare(const PickerGeometry& geometry, int x, int y);
bool HitSwatches(const PickerGeometry& geometry, int x, int y);

/** 环上某点 -> 色相（调用方保证点在环上）。 */
double HueAtPoint(const PickerGeometry& geometry, int x, int y);
/** 方块里某点 -> 饱和度 / 明度（越往右越饱和、越往上越亮）。 */
void SquareValueAtPoint(const PickerGeometry& geometry, int x, int y, double* s, double* v);

/**
 * 色盘上一点该取什么颜色。`currentHue` 是环上当前选中的色相。
 * 返回 false 表示这一点既不在环上也不在方块里。
 */
bool ColorAtPoint(const PickerGeometry& geometry, int x, int y, double currentHue,
                  COLORREF* color);

/** 当前颜色的标记（环上的小圆圈、方块里的小圈）该画在哪。 */
POINT RingMarkerPoint(const PickerGeometry& geometry, COLORREF color);
POINT SquareMarkerPoint(const PickerGeometry& geometry, COLORREF color);

/** 中间方块里某一行第 x 列的颜色（画色盘时逐像素用，与反解互为逆运算）。 */
COLORREF SquarePixel(const RECT& square, int x, int y, double hue);

/** 解析用户输入的色码（`#rrggbb` / `rrggbb` / `#rgb`），成功返回 true。 */
bool ParseHexColor(const std::wstring& text, COLORREF* color);

/** 常用色那一排（色盘底部，方便快速点常用的）。 */
const COLORREF* QuickPickColors(int* count);

}  // namespace dchat
