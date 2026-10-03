// 聊天文字里的彩色代码。
//
// 两套写法，可以混着用：
//   `#RRGGBB`   十六进制色码，例：`#ff0000红色文字`
//   `&a` ~ `&f` 单色种快捷码（和很多聊天软件的习惯一致），例：`&c红色`
// `&&` 表示一个字面的 `&`；写成 `&z` 这种认不出来的，**原样显示**（不要悄悄吞掉用户的字）。
//
// 只做纯计算：解析、排版、色板 —— 全部可以单元测试，不碰界面状态。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dchat {

// 快捷色码一共 16 个：&0 ~ &9 与 &a ~ &f（&a 起是亮色）。
inline constexpr int kQuickColorCount = 16;

// 某条消息里最多解析多少个"上色片段"——防止有人发几千个色码把排版拖慢
inline constexpr int kMaxColorSpans = 256;

/** 快捷色码对应的颜色（`&0` 是黑，`&a` 是亮绿……顺序就是常见的那一套）。 */
COLORREF QuickColor(int index);

/** 快捷色码的字符（`0`-`9` `a`-`f`），认不出来返回 0。 */
wchar_t QuickColorDigit(int index);

/** 文字里出现一个色码时，用它。`enabled` 为假表示服务器把彩色聊天关了。 */
struct ColorRun {
    std::wstring text;   // 这一段要画的文字
    COLORREF color;      // 文字颜色
    bool hasColor = false;  // 这一段是不是被色码上过色（没上色就用调用方给的默认色）
};

/**
 * 解析一段文字里的色码。
 *
 * `enabled` 为假时**不做任何解析**：色码原样留在文字里被完整显示。
 * 这是刻意的——服务器关掉彩色聊天后，用户打进去的色码不该凭空消失，
 * 否则他会以为自己的字被吃掉了。
 *
 * `defaultColor` 只是用来填第一段（还没有任何色码时那一段）的颜色。
 */
std::vector<ColorRun> ParseChatColors(const std::wstring& text, COLORREF defaultColor,
                                      bool enabled, std::size_t maxSpans = kMaxColorSpans);

/**
 * 一段已经排好位置、可以一次画出来文字。
 * x 是相对给定左边界的偏移（像素）。
 */
struct ColoredSpan {
    std::wstring text;
    COLORREF color;
    int x = 0;  // 相对行首
};

struct ColorLine {
    std::vector<ColoredSpan> spans;
    int width = 0;
};

struct ColorLayout {
    std::vector<ColorLine> lines;
    int width = 0;   // 最长那一行的宽度
    int height = 0;  // 总高度
    int lineHeight = 0;
};

/**
 * 按宽度把带颜色的文字排成若干行（自己换行，不用 DrawText 的 DT_WORDBREAK，
 * 因为要一段一段换色画，必须自己知道每一段落在第几行、从哪个 x 开始）。
 *
 * 换行规则和系统一致：优先在空格处断，一个词比整行还宽时按字符硬断；
 * 显式的 `\n` 一定换行。
 */
ColorLayout LayoutColoredText(HDC dc, const std::wstring& text, COLORREF defaultColor, bool enabled,
                              int maxWidth, HFONT font);

/** 色板里的一格。 */
struct PaletteSwatch {
    COLORREF color;
    const wchar_t* name;  // 中文名（/chatcolor help 用）
};

/** 色板：每行 8 格，覆盖常见颜色（黑白灰 + 红橙黄绿青蓝紫粉）。 */
const std::vector<PaletteSwatch>& ColorPalette();
inline constexpr int kPaletteColumns = 8;

/** 把颜色转成 `#rrggbb`（小写，和用户手打的写法一致）。 */
std::wstring ColorToHex(COLORREF color);

/** `/chatcolor help` 要显示的内容。 */
std::wstring ChatColorHelpText();

}  // namespace dchat
