// 圆角（胶囊）控件的绘制：GDI+ 抗锯齿，独立成模块以便做像素级单元测试。
#pragma once

#include <windows.h>

namespace ui {

// GDI+ 生命周期（圆角绘制依赖 GDI+，由本模块统一管理）
bool Startup();
void Shutdown();

// 画一个圆角控件：
// 1) 先用 background 铺满整个 rect —— 这一步不能省，否则圆角以外的四个角会残留
//    控件 DC 里的旧内容（通常表现为"方形底色 + 圆角块"）；
// 2) 再用 fill 填充圆角形状，并用 border 描 1px 边。
// 使用 GDI+ 的抗锯齿绘制，圆角边缘不会有锯齿。
void DrawRoundedControl(HDC dc, const RECT& rect, int radius, COLORREF background, COLORREF fill,
                        COLORREF border);

// 只填充圆角形状（不铺底色、可指定边框粗细、可传 0 表示不要边框）。
// 适合画在已经绘制好背景的画布上，例如聊天气泡。
void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border,
                     float borderWidth);

// 只画圆角轮廓（用于给记录区/输入框加圆角外框）
void OutlineRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF border);

}  // namespace ui
