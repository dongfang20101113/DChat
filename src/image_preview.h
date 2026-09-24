// 图片/视频预览：图片显示缩略图，视频显示**第一帧**。
// 用的是 Windows 自带的缩略图提供者（IShellItemImageFactory）：
// 不引第三方库，图片走系统的解码器、视频交给系统已装的解码器出第一帧。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dchat {

// 按扩展名判断（大小写不敏感，文件名可以是带路径的）
bool IsImageFile(const std::string& fileName);
bool IsVideoFile(const std::string& fileName);
bool IsPreviewableFile(const std::string& fileName);

// 图片是"自动下载"的（视频不自动下载整份，只取第一帧预览）
bool ShouldAutoDownload(const std::string& fileName);

// 生成缩略图；成功返回 HBITMAP（调用方负责 DeleteObject），失败返回 nullptr
HBITMAP LoadThumbnail(const std::wstring& path, int maxSize);

// 把缩略图编码成 PNG（发送端上传给服务器用，GDI+ 自带的 PNG 编码器）
std::vector<unsigned char> EncodePng(HBITMAP bitmap);
// 把 PNG 字节解回位图（接收端用）；失败返回 nullptr
HBITMAP DecodePng(const std::vector<unsigned char>& bytes);

}  // namespace dchat
