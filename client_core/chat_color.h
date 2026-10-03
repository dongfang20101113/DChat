// 彩色聊天的**纯逻辑**部分：把带色码的文字切成"这一段用什么颜色"。
//
// 为什么不复用 src/chat_color.cpp：那份是 GDI 版（颜色是 COLORREF、排版要 HDC），
// Linux 上编不了。这里只保留"解析"这一层，输出 RGB，终端渲染（ANSI）在 render 里。
//
// 色码语法（三端一致，和 Windows / 安卓完全相同）：
//   `&` 后面跟 1 个十六进制数字（0-9a-f）  -> 快捷色
//   `&` 后面跟 `#rrggbb`                   -> 真彩色
//   `&&`                                   -> 一个字面量 `&`
//
// `enabled` 为假时**不做任何解析**：色码原样显示。这是刻意的——服务器关掉彩色聊天后，
// 用户打进去的色码不该凭空消失，否则他会以为自己的字被吃掉了。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dchat {

/** 一段上了色的文字。 */
struct ColorSegment {
    std::string text;
    std::uint32_t rgb = 0;      // 0xRRGGBB
    bool hasColor = false;      // 没被色码上过色 -> 用调用方的默认色
};

/** 快捷色码字符（`0`-`9` `a`-`f`）对应的颜色；认不出来返回 0xFFFFFFFF。 */
std::uint32_t QuickColorRgb(char digit);

/** 解析出来的每一段；`enabled` 为假时整段原样返回、不做任何切分。 */
std::vector<ColorSegment> ParseColorSegments(const std::string& text, std::uint32_t defaultRgb,
                                             bool enabled,
                                             std::size_t maxSpans = 64);

/** `&` 后面能跟的十六进制数字表（顺序和 Windows/安卓一致）。 */
const char* QuickColorDigits();

/** 把 RGB 转成 `#rrggbb`。 */
std::string RgbToHex(std::uint32_t rgb);

/** 解析 `#rrggbb`；失败返回 false。 */
bool HexToRgb(const std::string& text, std::uint32_t* out);

/** 终端 ANSI 真彩色前景色；`reset` 为真时返回重置序列。 */
std::string AnsiForeground(std::uint32_t rgb);
const char* AnsiReset();

/** 服务器下发的 `chatcolor=0` 之类规则：彩色聊天是否开着。 */
bool ChatColorEnabledFromRules(const std::string& rulesLine);

}  // namespace dchat
