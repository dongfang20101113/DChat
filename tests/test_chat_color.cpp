// 彩色文字代码的单测：解析、快捷码、关闭时的行为、排版换行。
// 重点验证"服务器关掉彩色聊天后色码原样显示"这条——用户明确要求过。
#include <cstdio>
#include <string>

#include "chat_color.h"

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

std::wstring Join(const std::vector<dchat::ColorRun>& runs) {
    std::wstring out;
    for (const dchat::ColorRun& run : runs) out += run.text;
    return out;
}

}  // namespace

int main() {
    using namespace dchat;

    // ---- 十六进制色码 ----
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"#ff0000红字", RGB(1, 2, 3), true);
        check(runs.size() == 1, "#rrggbb 解析成一段");
        check(runs[0].text == L"红字", "色码本身不显示");
        check(runs[0].color == RGB(255, 0, 0), "#ff0000 = 纯红");
        check(runs[0].hasColor, "这一段被上过色");
    }
    {
        const std::vector<ColorRun> runs =
            ParseChatColors(L"正常#00ff00绿#0000ff蓝", RGB(9, 9, 9), true);
        check(runs.size() == 3, "三段：默认 + 绿 + 蓝");
        check(runs[0].text == L"正常" && runs[0].color == RGB(9, 9, 9), "第一段用默认色");
        check(runs[1].text == L"绿" && runs[1].color == RGB(0, 255, 0), "&绿段正确");
        check(runs[2].text == L"蓝" && runs[2].color == RGB(0, 0, 255), "新色码覆盖旧色码");
    }
    {
        // 大小写都要认
        const std::vector<ColorRun> runs = ParseChatColors(L"#AbCdEfX", RGB(0, 0, 0), true);
        check(runs.size() == 1 && runs[0].color == RGB(0xAB, 0xCD, 0xEF), "#AbCdEf 大小写都能认");
    }

    // ---- 快捷色码 ----
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"&c红&a绿", RGB(0, 0, 0), true);
        check(runs.size() == 2, "&c 和 &a 各成一段");
        check(runs[0].color == QuickColor(12) && runs[1].color == QuickColor(10), "&c / &a 取到对的颜色");
        check(runs[0].text == L"红" && runs[1].text == L"绿", "快捷码本身不显示");
    }
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"&z不认识", RGB(7, 7, 7), true);
        check(runs.size() == 1 && runs[0].text == L"&z不认识", "认不出来的 &z 原样显示，不吞字");
        check(runs[0].color == RGB(7, 7, 7), "认不出来就不改颜色");
    }
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"a&&b", RGB(0, 0, 0), true);
        check(runs.size() == 1 && runs[0].text == L"a&b", "&& 转义成一个 &");
    }

    // ---- 服务器关掉彩色聊天 ----
    {
        const std::vector<ColorRun> runs =
            ParseChatColors(L"#ff0000红&a绿&&x", RGB(3, 3, 3), /*enabled=*/false);
        check(runs.size() == 1, "关闭时只有一段");
        check(runs[0].text == L"#ff0000红&a绿&&x", "**关闭后色码原样显示（一个字符都不少）**");
        check(runs[0].color == RGB(3, 3, 3), "关闭时用默认色");
        check(!runs[0].hasColor, "关闭时不算上色");
    }

    // ---- 半截色码 ----
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"#ff00", RGB(0, 0, 0), true);
        check(runs.size() == 1 && runs[0].text == L"#ff00", "位数不够的 #ff00 原样显示");
    }
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"#ff00zz", RGB(0, 0, 0), true);
        check(runs.size() == 1 && runs[0].text == L"#ff00zz", "有非十六进制字符就不当色码");
    }
    {
        const std::vector<ColorRun> runs = ParseChatColors(L"结尾一个&", RGB(0, 0, 0), true);
        check(runs.size() == 1 && runs[0].text == L"结尾一个&", "结尾单独的 & 原样显示");
    }

    // ---- 颜色转十六进制 ----
    {
        check(ColorToHex(RGB(255, 0, 0)) == L"#ff0000", "ColorToHex 纯红");
        check(ColorToHex(RGB(0, 171, 205)) == L"#00abcd", "ColorToHex 小写六位");
    }

    // ---- 色板 ----
    {
        const std::vector<PaletteSwatch>& palette = ColorPalette();
        check(palette.size() >= 24, "色板至少 24 色（覆盖常见颜色）");
        check(palette.size() % kPaletteColumns == 0, "色板行数整齐（每行 8 个）");
        bool hasRed = false, hasBlue = false, hasWhite = false, hasBlack = false;
        for (const PaletteSwatch& swatch : palette) {
            if (swatch.color == RGB(255, 0, 0)) hasRed = true;
            if (swatch.color == RGB(0, 0, 255)) hasBlue = true;
            if (swatch.color == RGB(255, 255, 255)) hasWhite = true;
            if (swatch.color == RGB(0, 0, 0)) hasBlack = true;
        }
        check(hasRed && hasBlue && hasWhite && hasBlack, "色板含红/蓝/白/黑");
    }

    // ---- 帮助文本 ----
    {
        const std::wstring help = ChatColorHelpText();
        check(help.find(L"#rrggbb") != std::wstring::npos, "帮助里有 #rrggbb 写法");
        check(help.find(L"&a") != std::wstring::npos, "帮助里有 &a 这种快捷码");
        check(help.find(L"#ff0000") != std::wstring::npos, "帮助里列出了快捷码对应的十六进制");
    }

    // ---- 快捷色码唯一性 ----
    {
        bool unique = true;
        for (int i = 0; i < kQuickColorCount; ++i) {
            for (int j = i + 1; j < kQuickColorCount; ++j) {
                if (QuickColor(i) == QuickColor(j)) unique = false;
            }
        }
        check(unique, "16 个快捷色互不重复");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
