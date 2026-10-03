#include "chat_color.h"

#include <cstdio>
#include <cstring>

namespace dchat {
namespace {

// 快捷色表。**必须和 Windows 的 kQuickColors、安卓 ChatColor 的 quickColors 完全一致**，
// 否则同一条聊天记录在不同端会显示成不同颜色（不会报错，只会"看起来不对劲"）。
constexpr std::uint32_t kQuickColors[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

constexpr const char kDigits[] = "0123456789abcdef";

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void AppendPlain(std::vector<ColorSegment>* out, const std::string& text, std::uint32_t rgb,
                 bool hasColor) {
    if (text.empty()) return;
    // 和上一段同色就连起来，免得切出一堆碎片（渲染时每段都要发一次 ANSI 序列）
    if (!out->empty() && out->back().hasColor == hasColor && out->back().rgb == rgb) {
        out->back().text += text;
        return;
    }
    ColorSegment segment;
    segment.text = text;
    segment.rgb = rgb;
    segment.hasColor = hasColor;
    out->push_back(segment);
}

}  // namespace

std::uint32_t QuickColorRgb(char digit) {
    for (int i = 0; i < 16; ++i) {
        if (kDigits[i] == digit) return kQuickColors[i];
    }
    return 0xFFFFFFFFu;  // 认不出来
}

const char* QuickColorDigits() { return kDigits; }

std::vector<ColorSegment> ParseColorSegments(const std::string& text, std::uint32_t defaultRgb,
                                             bool enabled, std::size_t maxSpans) {
    std::vector<ColorSegment> out;
    if (!enabled) {
        // 关了彩色聊天就整段原样返回 —— 色码留在文字里给用户看见，
        // 不能凭空消失（他会以为自己的字被吃掉了）
        AppendPlain(&out, text, defaultRgb, false);
        return out;
    }

    std::uint32_t current = defaultRgb;
    bool hasColor = false;
    std::string pending;
    std::size_t spans = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '&') {
            pending.push_back(ch);
            continue;
        }
        if (i + 1 >= text.size()) {
            pending.push_back(ch);  // 结尾孤零零一个 &，当普通字符
            break;
        }
        const char next = text[i + 1];
        if (next == '&') {  // && -> 字面量 &
            pending.push_back('&');
            ++i;
            continue;
        }
        if (next == '#') {
            // &#rrggbb：要 6 位十六进制，不够就当普通字符
            if (i + 8 >= text.size()) {
                pending.push_back(ch);
                continue;
            }
            std::uint32_t rgb = 0;
            bool ok = true;
            for (int k = 0; k < 6; ++k) {
                const int value = HexValue(text[i + 2 + static_cast<std::size_t>(k)]);
                if (value < 0) {
                    ok = false;
                    break;
                }
                rgb = (rgb << 4) | static_cast<std::uint32_t>(value);
            }
            if (!ok) {
                pending.push_back(ch);
                continue;
            }
            // 达到上限后**只停止开新段**，解析继续 —— 剩下的文字一个字都不能丢。
            // （Windows 端和安卓端都在这里踩过同一个坑，见各自的测试。）
            if (spans < maxSpans) {
                AppendPlain(&out, pending, current, hasColor);
                pending.clear();
                current = rgb;
                hasColor = true;
                ++spans;
            }
            i += 7;
            continue;
        }
        const std::uint32_t rgb = QuickColorRgb(next);
        if (rgb == 0xFFFFFFFFu) {
            pending.push_back(ch);  // &# 或 &z 这种，当普通字符
            continue;
        }
        if (spans < maxSpans) {
            AppendPlain(&out, pending, current, hasColor);
            pending.clear();
            current = rgb;
            hasColor = true;
            ++spans;
        }
        ++i;
    }

    AppendPlain(&out, pending, current, hasColor);
    return out;
}

std::string RgbToHex(std::uint32_t rgb) {
    char buffer[8] = {0};
    std::snprintf(buffer, sizeof(buffer), "#%06x", rgb & 0xFFFFFFu);
    return buffer;
}

bool HexToRgb(const std::string& text, std::uint32_t* out) {
    if (!out) return false;
    std::string body = text;
    if (!body.empty() && body[0] == '#') body.erase(0, 1);
    if (body.size() != 6) return false;
    std::uint32_t rgb = 0;
    for (char ch : body) {
        const int value = HexValue(ch);
        if (value < 0) return false;
        rgb = (rgb << 4) | static_cast<std::uint32_t>(value);
    }
    *out = rgb;
    return true;
}

std::string AnsiForeground(std::uint32_t rgb) {
    // 用真彩色（24 位）转义序列：色盘里那么多颜色，硬凑成 16 色会失真
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "\x1b[38;2;%u;%u;%um", (rgb >> 16) & 0xFFu,
                  (rgb >> 8) & 0xFFu, rgb & 0xFFu);
    return buffer;
}

const char* AnsiReset() { return "\x1b[0m"; }

bool ChatColorEnabledFromRules(const std::string& rulesLine) {
    // 服务器下发的规则行里带 `chatcolor=1|0`；没提这条就按开着算（和另外两端一致）
    const std::string needle = "chatcolor=";
    const std::size_t at = rulesLine.find(needle);
    if (at == std::string::npos) return true;
    const std::size_t valueAt = at + needle.size();
    if (valueAt >= rulesLine.size()) return true;
    return rulesLine[valueAt] != '0' && rulesLine[valueAt] != 'f' && rulesLine[valueAt] != 'F';
}

}  // namespace dchat
