#include "image_preview.h"

#include <shobjidl.h>  // IShellItemImageFactory
#include <objidl.h>
#include <gdiplus.h>   // 缩略图的 PNG 编解码

#include <cctype>
#include <cstddef>
#include <cstring>

using namespace Gdiplus;

namespace dchat {

namespace {

std::string ExtensionOf(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool InList(const std::string& ext, const char* const* list, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (ext == list[i]) return true;
    }
    return false;
}

}  // namespace

bool IsImageFile(const std::string& fileName) {
    static const char* const kExts[] = {"png", "jpg",  "jpeg", "bmp", "gif",
                                        "webp", "ico", "tif",  "tiff"};
    return InList(ExtensionOf(fileName), kExts, sizeof(kExts) / sizeof(kExts[0]));
}

bool IsVideoFile(const std::string& fileName) {
    static const char* const kExts[] = {"mp4", "mkv",  "avi", "mov", "wmv", "flv",
                                        "webm", "m4v", "mpg", "mpeg", "ts"};
    return InList(ExtensionOf(fileName), kExts, sizeof(kExts) / sizeof(kExts[0]));
}

bool IsPreviewableFile(const std::string& fileName) {
    return IsImageFile(fileName) || IsVideoFile(fileName);
}

bool ShouldAutoDownload(const std::string& fileName) {
    return IsImageFile(fileName);  // 图片自动下；视频只取第一帧预览
}

// 退路：图片不靠 shell，直接用 GDI+ 解码再按比例缩小。
// （实测有些系统/图片类型 IShellItemImageFactory 会拒绝给缩略图，
//   而图片本身 GDI+ 一定能解，所以两条件都走一遍。）
HBITMAP LoadImageWithGdiPlus(const std::wstring& path, int maxSize) {
    Bitmap source(path.c_str(), FALSE);
    if (source.GetLastStatus() != Ok) return nullptr;
    const int width = static_cast<int>(source.GetWidth());
    const int height = static_cast<int>(source.GetHeight());
    if (width <= 0 || height <= 0) return nullptr;
    double scale = 1.0;
    if (width > maxSize || height > maxSize) {
        const double byWidth = static_cast<double>(maxSize) / width;
        const double byHeight = static_cast<double>(maxSize) / height;
        scale = byWidth < byHeight ? byWidth : byHeight;
    }
    int targetW = static_cast<int>(width * scale);
    int targetH = static_cast<int>(height * scale);
    if (targetW < 1) targetW = 1;
    if (targetH < 1) targetH = 1;
    Bitmap target(targetW, targetH, PixelFormat32bppPARGB);
    if (target.GetLastStatus() != Ok) return nullptr;
    {
        Graphics graphics(&target);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&source, Rect(0, 0, targetW, targetH));
    }
    HBITMAP result = nullptr;
    target.GetHBITMAP(Color(255, 32, 32, 32), &result);
    return result;
}

HBITMAP LoadThumbnail(const std::wstring& path, int maxSize) {
    if (path.empty() || maxSize <= 0) return nullptr;
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
        !factory) {
        return LoadImageWithGdiPlus(path, maxSize);  // 不是图片就返回 nullptr
    }
    const SIZE size{maxSize, maxSize};
    HBITMAP bitmap = nullptr;
    // THUMBNAILONLY：只要真正的缩略图（图片的缩小图 / 视频的第一帧），
    // 拿不到就返回失败，而不是退化成文件图标
    const HRESULT result = factory->GetImage(size, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY,
                                             &bitmap);
    factory->Release();
    if (FAILED(result) || !bitmap) return LoadImageWithGdiPlus(path, maxSize);
    return bitmap;
}

namespace {

bool PngEncoderClsid(CLSID* clsid) {
    UINT count = 0, size = 0;
    if (GetImageEncodersSize(&count, &size) != Ok || size == 0) return false;
    std::vector<unsigned char> buffer(size);
    auto* info = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, size, info) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<unsigned char> EncodePng(HBITMAP bitmap) {
    std::vector<unsigned char> out;
    if (!bitmap) return out;
    Bitmap image(bitmap, nullptr);
    if (image.GetLastStatus() != Ok) return out;
    CLSID clsid{};
    if (!PngEncoderClsid(&clsid)) return out;
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return out;
    if (image.Save(stream, &clsid, nullptr) == Ok) {
        HGLOBAL global = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &global)) && global) {
            const SIZE_T size = GlobalSize(global);
            if (void* data = GlobalLock(global)) {
                if (size > 0) {
                    const auto* begin = static_cast<unsigned char*>(data);
                    out.assign(begin, begin + size);
                }
                GlobalUnlock(global);
            }
        }
    }
    stream->Release();
    return out;
}

HBITMAP DecodePng(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!global) return nullptr;
    void* data = GlobalLock(global);
    if (!data) {
        GlobalFree(global);
        return nullptr;
    }
    std::memcpy(data, bytes.data(), bytes.size());
    GlobalUnlock(global);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(global, TRUE, &stream)) || !stream) {
        GlobalFree(global);
        return nullptr;
    }
    HBITMAP result = nullptr;
    {
        Bitmap image(stream);
        if (image.GetLastStatus() == Ok) {
            image.GetHBITMAP(Color(255, 32, 32, 32), &result);
        }
    }
    stream->Release();  // 顺带释放 global
    return result;
}

}  // namespace dchat
