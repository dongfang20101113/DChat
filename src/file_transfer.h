// 文件传输的公共部分：Base64 编解码、文件名清理、分块大小、字节数格式化。
// 只依赖标准库，方便单独做单元测试。
//
// 传输方式（广播式，和 IRC 的 DCC 思路类似）：
//   发送方 FILE_SEND -> 一串 FILE_CHUNK（每块 Base64）-> FILE_END
//   服务器把这几条转成 FILE_FROM / FILE_DATA / FILE_END 转发给房间里的其他人
//   接收方一边收一边写进 received\ 目录
// 服务器不落地文件，只做转发。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dchat {

// 每个 FILE_CHUNK 携带的原始字节数。
// 协议是"一行一条"，单行上限 4096 字节，Base64 还会把体积撑大约 1/3，
// 所以这里不能大：2048 字节 -> 2732 个 Base64 字符，加上命令名/昵称/传输 ID 仍在安全范围。
inline constexpr std::size_t kFileChunkBytes = 2048;
// 单个文件大小上限（客户端和服务器都按这个检查）
inline constexpr unsigned long long kMaxFileBytes = 64ull * 1024 * 1024;
// 文件名长度上限（清理后按字节算）
inline constexpr std::size_t kMaxFileNameBytes = 120;

// 标准 Base64（带 '=' 填充）
std::string Base64Encode(const unsigned char* data, std::size_t len);
inline std::string Base64Encode(const std::string& data) {
    return Base64Encode(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}
// 解码失败（有非法字符、长度不对）返回 false，out 不保证可用
bool Base64Decode(const std::string& text, std::vector<unsigned char>* out);

// 清理收到的文件名：只保留最后一段（挡掉 ../ 这类路径穿越）、替换 Windows
// 非法字符、去掉结尾的空格和点、限制长度。结果保证不为空（兜底 "file"）。
std::string SanitizeFileName(const std::string& name);

// 目录里已有同名文件时换成 "名字 (2).扩展名"，最多试 99 次。
// 用宽字符版本（Windows 文件 API），这样中文/emoji 文件名也能正确处理。
// directory 末尾没有分隔符时会自动补一个。
std::wstring MakeUniquePathW(const std::wstring& directory, const std::wstring& fileName);

// 1234567 -> "1.2 MB"；1024 -> "1.0 KB"
std::string FormatBytes(unsigned long long bytes);

// 一条 FILE_SEND 命令的字段（**按位置**，服务器就是这么解析的）：
//
//     FILE_SEND <传输ID> <文件名(Base64)> <字节数> [有缩略图] [种类]
//
//     字段数只能是 3 或 4：
//       - 普通文件、不带缩略图   → 3 格：id 名字 字节数
//       - 普通文件、带缩略图     → 4 格：id 名字 字节数 1        （"1" 表示接着会发缩略图）
//       - 语音（带种类）         → 4 格：id 名字 字节数 voice    （第 4 格就是种类，不是标记）
//
// **种类和缩略图标记会抢第 4 格**，所以带种类的附件一律不发缩略图标记。
// 抽成这个函数是因为"按位置解析"最容易写错，而写错的表现是**服务器直接拒绝整条命令**：
//     if (!SendRawLine(dchat::BuildLine("FILE_SEND", BuildFileSendRest(...)))) { ... }
std::string BuildFileSendRest(const std::string& transferId, const std::string& nameBase64,
                              unsigned long long bytes, const std::string& kind,
                              bool hasThumbnail);

// `FILE_OFFER` 最后一格的"种类"说的是不是语音？
//
// 取值与安卓端一致：`voice` = 语音；`0`/`file` = 普通文件；`1`/`sticker` = 贴纸。
// **这一格缺失时不能当成语音**：老服务器不会发它，那时按普通文件卡片显示才是对的
// （用户点一下还能下载），当成语音会得到一个点了没反应的灰气泡。
//
// 大小写不敏感：种类是两端自己发的字符串，宽容一点不会出错。
bool IsVoiceKind(const std::string& kind);

}  // namespace dchat
