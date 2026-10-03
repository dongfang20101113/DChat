// 取色盘的单测：HSV 换算、环/方块几何与命中、像素反解、色码解析。
// 这里最关键的是"**画出来的位置**和**点下去取到的颜色**必须对得上"——
// 两边各写一套几何的话，取到的颜色会和眼睛看到的位置差一格。
#include <cmath>
#include <cstdio>
#include <string>

#include "color_picker.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    } else {
        std::printf("  ok    %s\n", what.c_str());
    }
}

bool Near(double a, double b, double tolerance = 0.51) { return std::fabs(a - b) <= tolerance; }

bool SameColor(COLORREF a, COLORREF b, int tolerance = 1) {
    return std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b))) <= tolerance &&
           std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b))) <= tolerance &&
           std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b))) <= tolerance;
}

}  // namespace

int main() {
    using namespace dchat;

    std::printf("[1] HSV <-> RGB\n");
    {
        check(SameColor(HsvToRgb(0, 1, 1), RGB(255, 0, 0)), "色相 0 = 纯红");
        check(SameColor(HsvToRgb(120, 1, 1), RGB(0, 255, 0)), "色相 120 = 纯绿");
        check(SameColor(HsvToRgb(240, 1, 1), RGB(0, 0, 255)), "色相 240 = 纯蓝");
        check(SameColor(HsvToRgb(0, 0, 1), RGB(255, 255, 255)), "饱和度 0 + 明度 1 = 白");
        check(SameColor(HsvToRgb(123, 0, 0), RGB(0, 0, 0)), "明度 0 = 黑（色相无关）");
        check(SameColor(HsvToRgb(0, 1, 0.5), RGB(128, 0, 0)), "明度 0.5 的纯红是暗红");
        // 负角度 / 超过 360 也要能收敛回来
        check(SameColor(HsvToRgb(-120, 1, 1), RGB(0, 0, 255)), "负角度等价于 +240");
        check(SameColor(HsvToRgb(480, 1, 1), RGB(0, 255, 0)), "超过 360 会绕回来");
    }
    {
        // 往返：RGB -> HSV -> RGB 应当还原（取整误差 1 以内）
        const COLORREF samples[] = {RGB(255, 0, 0),   RGB(0, 128, 64),  RGB(12, 34, 56),
                                    RGB(200, 200, 200), RGB(255, 255, 255), RGB(0, 0, 0),
                                    RGB(1, 2, 3),     RGB(250, 240, 10)};
        bool roundTrip = true;
        for (COLORREF sample : samples) {
            double h = 0, s = 0, v = 0;
            RgbToHsv(sample, &h, &s, &v);
            if (!SameColor(HsvToRgb(h, s, v), sample)) roundTrip = false;
        }
        check(roundTrip, "8 个颜色 RGB -> HSV -> RGB 都能还原");
    }

    std::printf("[2] 色相与角度\n");
    {
        check(Near(RingAngleToHue(0), 0) && Near(RingAngleToHue(90), 90), "角度直接就是色相");
        check(Near(RingAngleToHue(-90), 270), "负角度绕到 270");
        check(Near(RingAngleToHue(450), 90), "超过一圈也收敛");
        check(Near(HueToRingAngle(270), 270), "色相反算成角度");
    }

    std::printf("[3] 色盘几何\n");
    PickerGeometry geometry = PickerLayout(kPickerWindowWidth, kPickerWindowHeight);
    {
        check(geometry.centerX == kPickerWindowWidth / 2, "色盘水平居中");
        check(geometry.outerRadius == kPickerOuterRadius &&
                  geometry.innerRadius == kPickerOuterRadius - kPickerRingWidth,
              "内外半径按常量");
        const int side = geometry.square.right - geometry.square.left;
        check(side > 0 && side < geometry.innerRadius * 2, "方块比环的内圈小（不会盖住环）");
        check(geometry.square.left >= 0 && geometry.square.bottom <= kPickerWindowHeight,
              "方块在窗口里");
        check(geometry.preview.bottom <= kPickerWindowHeight &&
                  geometry.swatches.bottom <= kPickerWindowHeight,
              "底部色块和常用色都在窗口里");
        check(geometry.preview.top > geometry.centerY + geometry.outerRadius,
              "底部在色盘下方，不重叠");
    }

    std::printf("[4] 命中判断\n");
    {
        check(HitRing(geometry, geometry.centerX + kPickerOuterRadius - 4, geometry.centerY),
              "环外沿内侧算命中");
        check(!HitRing(geometry, geometry.centerX, geometry.centerY), "正中心不是环");
        check(!HitRing(geometry, geometry.centerX + kPickerOuterRadius + 5, geometry.centerY),
              "环外不算");
        check(HitSquare(geometry, geometry.centerX, geometry.centerY), "正中心在方块里");
        check(!HitSquare(geometry, geometry.square.left - 1, geometry.centerY), "方块左边外不算");
        check(!HitSquare(geometry, geometry.square.right, geometry.centerY), "方块右边界不算（左闭右开）");
        check(HitSwatches(geometry, geometry.swatches.left + 2, geometry.swatches.top + 2),
              "常用色那一排能点中");
        check(!HitSwatches(geometry, geometry.swatches.left, geometry.swatches.bottom),
              "常用色下边界外不算");
    }

    std::printf("[5] 环上取色（位置 <-> 颜色 一致）\n");
    {
        // 正右方 = 色相 0 = 红；正下方 = 90 = 偏黄绿；正左方 = 180 = 青
        COLORREF color = 0;
        const int radius = (geometry.outerRadius + geometry.innerRadius) / 2;
        check(ColorAtPoint(geometry, geometry.centerX + radius, geometry.centerY, 0, &color) &&
                  SameColor(color, RGB(255, 0, 0)),
              "环上正右方取到纯红");
        check(ColorAtPoint(geometry, geometry.centerX - radius, geometry.centerY, 0, &color) &&
                  SameColor(color, RGB(0, 255, 255)),
              "环上正左方取到青色（色相 180）");
        // 标记点必须落在环上，而且反解回来还是原来的色相
        const COLORREF picked = HsvToRgb(200, 1, 1);
        const POINT marker = RingMarkerPoint(geometry, picked);
        check(HitRing(geometry, marker.x, marker.y), "环上的标记点确实落在环上");
        double hue = 0, s = 0, v = 0;
        RgbToHsv(picked, &hue, &s, &v);
        check(Near(HueAtPoint(geometry, marker.x, marker.y), hue, 2.0),
              "标记点反解出来的色相 = 原来的色相");
    }

    std::printf("[6] 方块取色\n");
    {
        double s = 0, v = 0;
        SquareValueAtPoint(geometry, geometry.square.left, geometry.square.top, &s, &v);
        check(Near(s, 0.0) && Near(v, 1.0), "方块左上 = 不饱和 + 最亮（白）");
        SquareValueAtPoint(geometry, geometry.square.right - 1, geometry.square.top, &s, &v);
        check(Near(s, 1.0) && Near(v, 1.0), "方块右上 = 最饱和 + 最亮（纯色）");
        SquareValueAtPoint(geometry, geometry.square.left, geometry.square.bottom - 1, &s, &v);
        check(Near(s, 0.0) && Near(v, 0.0), "方块左下 = 黑");
        SquareValueAtPoint(geometry, geometry.square.right - 1, geometry.square.bottom - 1, &s, &v);
        check(Near(s, 1.0) && Near(v, 0.0), "方块右下 = 最饱和 + 最暗");

        // 逐像素画出来的颜色 与 点下去取到的颜色 必须一致。
        // 比较 s/v 而不是 8 位通道整数：两次换算各带一次取整，通道差 1 属于量化误差，
        // 但**颜色必须落在同一个位置**，那才是"所见即所得"。
        const double hue = 210.0;
        bool consistent = true;
        for (int y = geometry.square.top; y < geometry.square.bottom; y += 7) {
            for (int x = geometry.square.left; x < geometry.square.right; x += 7) {
                const COLORREF drawn = SquarePixel(geometry.square, x, y, hue);
                COLORREF hit = 0;
                if (!ColorAtPoint(geometry, x, y, hue, &hit)) {
                    consistent = false;
                    continue;
                }
                double dh = 0, ds = 0, dv = 0, hh = 0, hs = 0, hv = 0;
                RgbToHsv(drawn, &dh, &ds, &dv);
                RgbToHsv(hit, &hh, &hs, &hv);
                if (!Near(ds, hs, 0.02) || !Near(dv, hv, 0.02)) consistent = false;
            }
        }
        check(consistent, "方块里每个采样点：画出来的颜色和点下去取到的颜色落在同一位置");

        // 方块里的标记点位置也要能对上
        const COLORREF chosen = HsvToRgb(hue, 0.35, 0.8);
        const POINT marker = SquareMarkerPoint(geometry, chosen);
        check(HitSquare(geometry, marker.x, marker.y), "方块里的标记点在方块内");
        SquareValueAtPoint(geometry, marker.x, marker.y, &s, &v);
        double ch = 0, cs = 0, cv = 0;
        RgbToHsv(chosen, &ch, &cs, &cv);
        check(Near(s, cs, 0.02) && Near(v, cv, 0.02), "标记点反解出的 s/v = 原颜色的 s/v");
    }

    std::printf("[7] 色码解析\n");
    {
        COLORREF color = 0;
        check(ParseHexColor(L"#ff0000", &color) && SameColor(color, RGB(255, 0, 0)), "#rrggbb");
        check(ParseHexColor(L"00ff00", &color) && SameColor(color, RGB(0, 255, 0)), "不带 # 也行");
        check(ParseHexColor(L"#f00", &color) && SameColor(color, RGB(255, 0, 0)), "#rgb 简写");
        check(ParseHexColor(L"  #00F  ", &color) && SameColor(color, RGB(0, 0, 255)),
              "两边空格 + 大写都认");
        check(!ParseHexColor(L"#gg0000", &color), "非十六进制字符拒绝");
        check(!ParseHexColor(L"#ff00", &color), "位数不对拒绝");
        check(!ParseHexColor(L"", &color), "空串拒绝");
    }

    std::printf("[8] 常用色\n");
    {
        int count = 0;
        const COLORREF* colors = QuickPickColors(&count);
        check(colors != nullptr && count == 16, "常用色 16 个");
        bool hasBlack = false, hasWhite = false, hasRed = false;
        for (int i = 0; i < count; ++i) {
            if (colors[i] == RGB(0, 0, 0)) hasBlack = true;
            if (colors[i] == RGB(255, 255, 255)) hasWhite = true;
            if (colors[i] == RGB(255, 0, 0)) hasRed = true;
        }
        check(hasBlack && hasWhite && hasRed, "常用色含黑/白/红");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
