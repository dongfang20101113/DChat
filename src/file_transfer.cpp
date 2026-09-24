#include "file_transfer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace dchat {

namespace {

const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// UTF-8 里 0x80..0xBF 是"后续字节"，截断时不能把一个字符切一半
bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// 按字节截断成合法的 UTF-8（最多 maxBytes 字节）
std::string TruncateUtf8(const std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && IsUtf8Continuation(static_cast<unsigned char>(text[cut]))) --cut;
    return text.substr(0, cut);
}

}  // namespace

std::string Base64Encode(const unsigned char* data, std::size_t len) {
    std::string out;
    if (!data || len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        const unsigned int value = (static_cast<unsigned int>(data[i]) << 16) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8) |
                                   static_cast<unsigned int>(data[i + 2]);
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[value & 0x3F]);
        i += 3;
    }
    const std::size_t rest = len - i;
    if (rest == 1) {
        const unsigned int value = static_cast<unsigned int>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const unsigned int value = (static_cast<unsigned int>(data[i]) << 16) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>* out) {
    if (!out) return false;
    out->clear();
    if (text.empty()) return true;
    if (text.size() % 4 != 0) return false;  // 标准 Base64 一定是 4 的倍数

    out->reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const char c0 = text[i], c1 = text[i + 1], c2 = text[i + 2], c3 = text[i + 3];
        const int v0 = Base64Value(static_cast<unsigned char>(c0));
        const int v1 = Base64Value(static_cast<unsigned char>(c1));
        if (v0 < 0 || v1 < 0) return false;

        const bool last = (i + 4 == text.size());
        const bool pad2 = c2 == '=';
        const bool pad1 = c3 == '=';
        if (pad2 && !pad1) return false;
        if (pad2 && !last) return false;
        if (pad1 && !last) return false;

        const int v2 = pad2 ? 0 : Base64Value(static_cast<unsigned char>(c2));
        const int v3 = pad1 ? 0 : Base64Value(static_cast<unsigned char>(c3));
        if (v2 < 0 || v3 < 0) return false;

        const unsigned int value = (static_cast<unsigned int>(v0) << 18) |
                                   (static_cast<unsigned int>(v1) << 12) |
                                   (static_cast<unsigned int>(v2) << 6) |
                                   static_cast<unsigned int>(v3);
        out->push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
        if (!pad2) out->push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        if (!pad1) out->push_back(static_cast<unsigned char>(value & 0xFF));
    }
    return true;
}

std::string SanitizeFileName(const std::string& name) {
    // 1) 只取最后一段：挡掉 "../../evil.exe"、"C:\Windows\x.exe" 这类
    std::size_t begin = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '/' || name[i] == '\\') begin = i + 1;
    }
    std::string rest = name.substr(begin);

    // 2) 替换非法字符和控制字符
    std::string cleaned;
    cleaned.reserve(rest.size());
    for (char c : rest) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            cleaned.push_back('_');
            continue;
        }
        switch (c) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*':
                cleaned.push_back('_');
                break;
            default:
                cleaned.push_back(c);
        }
    }

    // 3) 去掉结尾的空格和点（Windows 会自动去掉，留着容易和别的文件重名）
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) cleaned.pop_back();
    while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());

    // 4) 全是点（"." / ".."）也算空
    bool onlyDots = true;
    for (char c : cleaned) {
        if (c != '.') {
            onlyDots = false;
            break;
        }
    }
    if (onlyDots) cleaned.clear();

    // 5) 太长就截断，但尽量保住扩展名
    if (cleaned.size() > kMaxFileNameBytes) {
        const std::size_t dot = cleaned.find_last_of('.');
        if (dot != std::string::npos && cleaned.size() - dot <= 16 && dot > 0) {
            const std::string ext = cleaned.substr(dot);
            cleaned = TruncateUtf8(cleaned.substr(0, dot), kMaxFileNameBytes - ext.size()) + ext;
        } else {
            cleaned = TruncateUtf8(cleaned, kMaxFileNameBytes);
        }
    }

    if (cleaned.empty()) return "file";
    return cleaned;
}

std::wstring MakeUniquePathW(const std::wstring& directory, const std::wstring& fileName) {
    std::wstring prefix = directory;
    if (!prefix.empty() && prefix.back() != L'/' && prefix.back() != L'\\') prefix.push_back(L'\\');

    auto exists = [](const std::wstring& path) {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    const std::wstring first = prefix + fileName;
    if (!exists(first)) return first;

    // a.txt -> a (2).txt
    const std::size_t dot = fileName.find_last_of(L'.');
    const bool hasExt = dot != std::wstring::npos && dot > 0;
    const std::wstring stem = hasExt ? fileName.substr(0, dot) : fileName;
    const std::wstring ext = hasExt ? fileName.substr(dot) : std::wstring();
    for (int index = 2; index <= 99; ++index) {
        const std::wstring candidate =
            prefix + stem + L" (" + std::to_wstring(index) + L")" + ext;
        if (!exists(candidate)) return candidate;
    }
    return prefix + stem + L" (100)" + ext;
}

std::string FormatBytes(unsigned long long bytes) {
    char buffer[64] = {0};
    const double value = static_cast<double>(bytes);
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", bytes);
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", value / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", value / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", value / (1024.0 * 1024.0 * 1024.0));
    }
    return buffer;
}

}  // namespace dchat
