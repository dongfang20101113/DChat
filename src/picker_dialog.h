// 取色盘对话框：色相环 + 饱和度/明度方块 + 当前色/原色对比 + 十六进制输入 + 常用色。
//
// 从 client.cpp 搬出来的，原因有两个：
//   1. 几何和绘制必须共用一套（各写一遍就会"点到的颜色和看到的位置不一样"）；
//   2. 单独成模块就能让工具离屏渲染出图（tools/picker_mockup.cpp），
//      改颜色相关的东西时先看图再动手。
#pragma once

#include <windows.h>

namespace dchat {

// 对话框里两个控件的 ID
inline constexpr int kPickerHexEditId = 300;

/** 客户端把界面字体设进来（单独出图时不设，用系统默认）。 */
void SetPickerFonts(HFONT normal, HFONT small);

/**
 * 弹出取色盘（模态）。用户按回车 / 点确定返回 true 并把颜色写进 *out；
 * Esc 或关掉窗口返回 false（= 这次不选）。
 */
bool ShowColorPicker(HWND parent, COLORREF initial, COLORREF* out);

/**
 * 把色盘画到给定的 DC 上（工具出图用，也是对话框 WM_PAINT 走的那条路）。
 * `current` 是当前颜色，`original` 是打开时的颜色。
 */
void DrawColorPicker(HDC dc, int width, int height, COLORREF current, COLORREF original);

/** 在深色底/浅色底上都能看清的标签色。 */
COLORREF ContrastText(COLORREF background);

}  // namespace dchat
