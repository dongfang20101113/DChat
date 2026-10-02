// 临时对照：把"设置齿轮"和"语音"的几种候选图标画成一张图，用来挑哪个好看。
// 产物：build\icon-candidates.png
#include <objidl.h>
#include <windows.h>

#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "rounded.h"  // ui::Startup / Shutdown（GDI+ 生命周期）

namespace {

HICON LoadShellIcon(int id, int size) {
    return static_cast<HICON>(
        LoadImageW(nullptr, reinterpret_cast<LPCWSTR>(static_cast<INT_PTR>(id)), IMAGE_ICON, size,
                   size, LR_DEFAULTCOLOR));
}

// 把系统图标画进 32 位 DIB，用哨兵色抠掉背景（外壳图标是黑白的，直接画会带一块黑底）
HBITMAP IconToBitmap(HICON icon, int size) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    RECT all{0, 0, size, size};
    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(dc, &all, magenta);
    DeleteObject(magenta);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    GdiFlush();
    auto* px = static_cast<unsigned char*>(pixels);
    for (int i = 0; i < size * size; ++i) {
        unsigned char* p = px + i * 4;
        const bool background = p[0] > 200 && p[1] < 80 && p[2] > 200;  // 哨兵色还在
        p[3] = background ? 0 : 255;
        if (!background) {
            p[0] = p[1] = p[2] = 255;  // 统一成白色，深浅主题自己上色
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    return bitmap;
}

struct Candidate {
    std::wstring label;
    HICON icon = nullptr;
    std::wstring font;
    wchar_t glyph = 0;
};

}  // namespace

int main() {
    if (!ui::Startup()) {  // GDI+ 生命周期（和真界面共用一套）
        std::printf("GDI+ 初始化失败\n");
        return 1;
    }
    const int size = 32;
    std::vector<Candidate> candidates = {
        {L"shell 104", LoadShellIcon(104, size), L"", 0},
        {L"shell 105", LoadShellIcon(105, size), L"", 0},
        {L"shell 106", LoadShellIcon(106, size), L"", 0},
        {L"MDL2 E713", nullptr, L"Segoe MDL2 Assets", 0xE713},
        {L"MDL2 E712", nullptr, L"Segoe MDL2 Assets", 0xE712},
        {L"MDL2 E115(设置)", nullptr, L"Segoe MDL2 Assets", 0xE115},
        {L"MDL2 E720(麦克风)", nullptr, L"Segoe MDL2 Assets", 0xE720},
        {L"MDL2 E767", nullptr, L"Segoe MDL2 Assets", 0xE767},
        {L"Fluent E713", nullptr, L"Segoe Fluent Icons", 0xE713},
        {L"Fluent E720", nullptr, L"Segoe Fluent Icons", 0xE720},
        {L"Fluent E8BD(录音)", nullptr, L"Segoe Fluent Icons", 0xE8BD},
        {L"Symbol 2699(齿轮)", nullptr, L"Segoe UI Symbol", 0x2699},
    };

    const int cell = 120;
    const int cols = 4;
    const int rows = (static_cast<int>(candidates.size()) + cols - 1) / cols;
    const int width = cols * cell;
    const int height = rows * cell;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HBITMAP canvas = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, canvas);
    RECT all{0, 0, width, height};
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    HFONT labelFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    SetBkMode(dc, TRANSPARENT);

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int x = col * cell;
        const int y = row * cell;
        RECT frame{x + 6, y + 6, x + cell - 6, y + cell - 6};
        HBRUSH cellBg = CreateSolidBrush(RGB(42, 44, 48));
        FillRect(dc, &frame, cellBg);
        DeleteObject(cellBg);

        const int cx = x + cell / 2;
        const int iconY = y + 30;
        if (candidates[i].icon) {
            DrawIconEx(dc, cx - size / 2, iconY, candidates[i].icon, size, size, 0, nullptr,
                       DI_NORMAL);
        } else {
            HFONT font = CreateFontW(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, candidates[i].font.c_str());
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetTextColor(dc, RGB(232, 232, 232));
            const wchar_t text[2] = {candidates[i].glyph, 0};
            RECT r{cx - 24, iconY - 4, cx + 24, iconY + 32};
            DrawTextW(dc, text, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }
        HGDIOBJ oldFont = SelectObject(dc, labelFont);
        SetTextColor(dc, RGB(160, 160, 160));
        RECT lr{x + 4, y + cell - 34, x + cell - 4, y + cell - 10};
        DrawTextW(dc, candidates[i].label.c_str(), -1, &lr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
    }

    Gdiplus::Bitmap bitmap(canvas, nullptr);
    CLSID png{};
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<unsigned char> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, bytes, codecs);
    for (UINT i = 0; i < count; ++i) {
        if (std::wstring(codecs[i].MimeType) == L"image/png") png = codecs[i].Clsid;
    }
    const bool ok = bitmap.Save(L"build\\icon-candidates.png", &png, nullptr) == Gdiplus::Ok;

    SelectObject(dc, old);
    DeleteObject(canvas);
    DeleteDC(dc);
    DeleteObject(labelFont);
    for (Candidate& c : candidates) {
        if (c.icon) DestroyIcon(c.icon);
    }
    ui::Shutdown();
    std::printf(ok ? "写出 build\\icon-candidates.png\n" : "保存失败\n");
    return ok ? 0 : 1;
}
