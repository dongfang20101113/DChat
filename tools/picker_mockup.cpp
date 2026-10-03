// 取色盘的出图工具：把色盘离屏画成 PNG，改颜色相关的东西时先看图再动手。
// 用法：build\picker_mockup.exe [输出目录]
#include <objidl.h>
#include <windows.h>

#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "color_picker.h"  // kPickerWindowWidth / Height
#include "picker_dialog.h"
#include "rounded.h"        // ui::Startup / Shutdown

int main(int argc, char** argv) {
    // 控制台里中文要能正常输出
    SetConsoleOutputCP(65001);
    if (!ui::Startup()) {
        std::printf("GDI+ 初始化失败\n");
        return 1;
    }
    const std::string outDir = argc > 1 ? argv[1] : "build";
    const int width = dchat::kPickerWindowWidth;

    // 三张图：默认（白）、一个暖色、一个深色，用来看不同颜色下的标记对不对
    struct Sample {
        const char* name;
        COLORREF current;
        COLORREF original;
    };
    const Sample samples[] = {
        {"picker-default.png", RGB(255, 255, 255), RGB(255, 255, 255)},
        {"picker-warm.png", RGB(230, 120, 40), RGB(60, 130, 220)},
        {"picker-dark.png", RGB(30, 40, 160), RGB(255, 255, 255)},
    };

    bool ok = true;
    for (const Sample& sample : samples) {
        const int height = dchat::kPickerWindowHeight;
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
        RECT whole{0, 0, width, height};
        HBRUSH background = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(dc, &whole, background);
        DeleteObject(background);
        dchat::DrawColorPicker(dc, width, height, sample.current, sample.original);
        GdiFlush();

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
        const std::string path = outDir + "\\" + sample.name;
        const std::wstring wide(path.begin(), path.end());
        const bool saved = bitmap.Save(wide.c_str(), &png, nullptr) == Gdiplus::Ok;
        std::printf(saved ? "写出 %s\n" : "保存失败 %s\n", path.c_str());
        ok = ok && saved;

        SelectObject(dc, old);
        DeleteObject(canvas);
        DeleteDC(dc);
    }
    ui::Shutdown();
    return ok ? 0 : 1;
}
