#include "chat_color.h"

#include <algorithm>
#include <cwchar>

namespace dchat {
namespace {

// 16 个快捷色：和常见聊天软件的习惯一致（0-7 暗色，8-15 亮色）
const COLORREF kQuickColors[kQuickColorCount] = {
    RGB(0, 0, 0),       RGB(0, 0, 170),     RGB(0, 170, 0),     RGB(0, 170, 170),
    RGB(170, 0, 0),     RGB(170, 0, 170),   RGB(170, 85, 0),    RGB(170, 170, 170),
    RGB(85, 85, 85),    RGB(85, 85, 255),   RGB(85, 255, 85),   RGB(85, 255, 255),
    RGB(255, 85, 85),   RGB(255, 85, 255),  RGB(255, 255, 85),  RGB(255, 255, 255)};

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// 从 text[pos] 开始认一个色码；认出来就返回长度并把颜色写进 color
std::size_t MatchColorCode(const std::wstring& text, std::size_t pos, COLORREF* color) {
    if (pos >= text.size()) return 0;
    if (text[pos] == L'&') {
        if (pos + 1 >= text.size()) return 0;
        const wchar_t next = text[pos + 1];
        if (next == L'&') return 2;  // 转义的 &，颜色不变
        int index = -1;
        if (next >= L'0' && next <= L'9') index = next - L'0';
        if (next >= L'a' && next <= L'f') index = next - L'a' + 10;
        if (next >= L'A' && next <= L'F') index = next - L'A' + 10;
        if (index < 0) return 0;  // 认不出来就原样显示
        *color = kQuickColors[index];
        return 2;
    }
    if (text[pos] == L'#') {
        if (pos + 6 >= text.size()) return 0;
        int value = 0;
        for (int i = 1; i <= 6; ++i) {
            const int digit = HexValue(text[pos + i]);
            if (digit < 0) return 0;
            value = value * 16 + digit;
        }
        *color = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
        return 7;
    }
    return 0;
}

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t'; }

}  // namespace

COLORREF QuickColor(int index) {
    if (index < 0 || index >= kQuickColorCount) return RGB(255, 255, 255);
    return kQuickColors[index];
}

wchar_t QuickColorDigit(int index) {
    if (index < 0 || index >= kQuickColorCount) return 0;
    return index < 10 ? static_cast<wchar_t>(L'0' + index)
                      : static_cast<wchar_t>(L'a' + index - 10);
}

std::vector<ColorRun> ParseChatColors(const std::wstring& text, COLORREF defaultColor,
                                      bool enabled, std::size_t maxSpans) {
    std::vector<ColorRun> runs;
    ColorRun current;
    current.color = defaultColor;
    if (!enabled) {
        // 服务器关了彩色聊天：**一个字符都不动**，色码原样显示
        current.text = text;
        runs.push_back(current);
        return runs;
    }

    COLORREF active = defaultColor;
    bool activeSet = false;
    for (std::size_t i = 0; i < text.size();) {
        COLORREF found = active;
        const std::size_t length = MatchColorCode(text, i, &found);
        if (length > 0) {
            if (text[i] == L'&' && length == 2 && text[i + 1] == L'&') {
                current.text.push_back(L'&');  // 转义：显示一个 &
                i += length;
                continue;
            }
            // 色码本身不显示，只换色
            if (!current.text.empty()) {
                runs.push_back(current);
                current = ColorRun{};
            }
            active = found;
            activeSet = true;
            current.color = active;
            current.hasColor = true;
            i += length;
            if (runs.size() >= maxSpans) {
                current.text.append(text, i, std::wstring::npos);
                break;
            }
            continue;
        }
        current.text.push_back(text[i]);
        ++i;
    }
    if (!current.text.empty()) runs.push_back(current);
    if (runs.empty()) runs.push_back(ColorRun{});  // 空消息也要有一段
    (void)activeSet;
    return runs;
}

std::wstring ColorToHex(COLORREF color) {
    wchar_t buffer[8] = {0};
    std::swprintf(buffer, 8, L"#%02x%02x%02x", GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

const std::vector<PaletteSwatch>& ColorPalette() {
    static const std::vector<PaletteSwatch> swatches = {
        {RGB(0, 0, 0), L"黑"},        {RGB(64, 64, 64), L"深灰"},   {RGB(128, 128, 128), L"灰"},
        {RGB(192, 192, 192), L"浅灰"}, {RGB(255, 255, 255), L"白"},  {RGB(128, 0, 0), L"暗红"},
        {RGB(255, 0, 0), L"红"},      {RGB(255, 128, 128), L"粉红"}, {RGB(255, 128, 0), L"橙"},
        {RGB(255, 192, 0), L"琥珀"},   {RGB(255, 255, 0), L"黄"},    {RGB(128, 128, 0), L"橄榄"},
        {RGB(0, 128, 0), L"深绿"},     {RGB(0, 255, 0), L"绿"},      {RGB(128, 255, 128), L"浅绿"},
        {RGB(0, 128, 128), L"青"},     {RGB(0, 255, 255), L"亮青"},   {RGB(128, 255, 255), L"淡青"},
        {RGB(0, 0, 128), L"深蓝"},     {RGB(0, 0, 255), L"蓝"},      {RGB(128, 128, 255), L"浅蓝"},
        {RGB(128, 0, 128), L"紫"},     {RGB(255, 0, 255), L"品红"},   {RGB(255, 128, 255), L"浅紫"},
        {RGB(165, 42, 42), L"棕"},     {RGB(210, 180, 140), L"棕褐"}, {RGB(255, 215, 0), L"金"},
        {RGB(0, 100, 0), L"墨绿"},     {RGB(70, 130, 180), L"钢蓝"},  {RGB(255, 105, 180), L"桃红"},
        {RGB(75, 0, 130), L"靛"},      {RGB(240, 230, 140), L"卡其"}};
    return swatches;
}

std::wstring ChatColorHelpText() {
    std::wstring text = L"彩色文字用法：\n";
    text += L"  #rrggbb   十六进制色码，例：#ff0000这是红字\n";
    text += L"  &0~&f     快捷色码（16 色），例：&c这是红字\n";
    text += L"  色码之后一直到下一个色码为止，都是那个颜色\n";
    text += L"  &&        想显示一个 & 就写两个\n";
    text += L"快捷色码对照：";
    for (int i = 0; i < kQuickColorCount; ++i) {
        text += L"\n  &";
        text.push_back(QuickColorDigit(i));
        text += L"  ";
        text += ColorToHex(QuickColor(i));
    }
    text += L"\n更多颜色：/chatcolor choose（弹出色板，选一个就会把色码放进输入框）";
    return text;
}

ColorLayout LayoutColoredText(HDC dc, const std::wstring& text, COLORREF defaultColor, bool enabled,
                              int maxWidth, HFONT font) {
    ColorLayout layout;
    if (maxWidth <= 0) return layout;

    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    layout.lineHeight = metrics.tmHeight + metrics.tmExternalLeading;

    const std::vector<ColorRun> runs = ParseChatColors(text, defaultColor, enabled);
    ColorLine line;
    int x = 0;
    auto flushLine = [&]() {
        layout.width = (std::max)(layout.width, line.width);
        layout.lines.push_back(line);
        line = ColorLine{};
        x = 0;
    };
    auto measure = [&](const std::wstring& piece) {
        SIZE size{};
        GetTextExtentPoint32W(dc, piece.c_str(), static_cast<int>(piece.size()), &size);
        return size.cx;
    };
    auto append = [&](const std::wstring& piece, COLORREF color) {
        // 和上一段同色就并进去，少画几次
        if (!line.spans.empty() && line.spans.back().color == color) {
            line.spans.back().text += piece;
        } else {
            ColoredSpan span;
            span.text = piece;
            span.color = color;
            span.x = x;
            line.spans.push_back(span);
        }
        const int width = measure(piece);
        x += width;
        line.width = x;
    };

    for (const ColorRun& run : runs) {
        std::size_t i = 0;
        while (i < run.text.size()) {
            if (run.text[i] == L'\n') {  // 显式换行
                flushLine();
                ++i;
                continue;
            }
            // 取一个"词"：一段非空白，或者一段空白（空白也参与换行判断）
            const bool space = IsSpace(run.text[i]);
            std::size_t end = i;
            while (end < run.text.size() && run.text[end] != L'\n' && IsSpace(run.text[end]) == space) {
                ++end;
            }
            std::wstring word = run.text.substr(i, end - i);
            i = end;

            if (space) {
                append(word, run.color);  // 空格先放上，行尾的空格会被裁掉
                continue;
            }
            int wordWidth = measure(word);
            if (x > 0 && x + wordWidth > maxWidth) {
                // 行尾空格不留
                while (!line.spans.empty() && !line.spans.back().text.empty() &&
                       IsSpace(line.spans.back().text.back())) {
                    const int trimmed = measure(std::wstring(1, line.spans.back().text.back()));
                    line.spans.back().text.pop_back();
                    line.width -= trimmed;
                    x -= trimmed;
                    if (line.spans.back().text.empty()) line.spans.pop_back();
                }
                flushLine();
            }
            // 一个词比整行还宽：按字符硬断（否则会溢出气泡）
            while (wordWidth > maxWidth) {
                std::wstring head;
                int headWidth = 0;
                std::size_t take = 0;
                for (; take < word.size(); ++take) {
                    const int charWidth = measure(std::wstring(1, word[take]));
                    if (headWidth + charWidth > maxWidth && !head.empty()) break;
                    head.push_back(word[take]);
                    headWidth += charWidth;
                }
                if (head.empty()) break;
                append(head, run.color);
                flushLine();
                word.erase(0, take);
                wordWidth = measure(word);
            }
            if (!word.empty()) append(word, run.color);
        }
    }
    if (!line.spans.empty() || layout.lines.empty()) flushLine();

    layout.height = static_cast<int>(layout.lines.size()) * layout.lineHeight;
    if (oldFont) SelectObject(dc, oldFont);
    return layout;
}

}  // namespace dchat
