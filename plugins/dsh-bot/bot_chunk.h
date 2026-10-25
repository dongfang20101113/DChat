// 把一段回答切成一条条聊天消息（纯逻辑，可单测）
//
// 为什么不能整段发：dchat 一条消息就是一屏。DeepSeek 的回合一上来经常是 200-800 字，
// 整段发出去等于往群里丢一堵墙，手机上要滑三屏。切成短句按节奏发，读起来才像"有人
// 在跟你说话"，而不是"贴了一段文档"。
//
// 切分规则（按优先级）：
//   1. 句末标点（。！？；!?;…）后必须断 —— 这是最自然的换气口；
//   2. 超过上限时，优先回头找最近的逗号/顿号断，找不到才硬切；
//   3. 太短的片段（"嗯。"）并到下一句，否则一条两个字刷屏；
//   4. 总条数有上限，超了把尾巴并进最后一条并截断（宁可不说完，也别刷屏）。
#pragma once

#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

struct TypingConfig;  // 见 bot_config.h

/** 把 Markdown 记号抹平成聊天语气（`**粗体**` -> `粗体`，`# 标题` -> `标题`）。 */
std::string FlattenMarkdown(const std::string& text);

/** 按配置切分。返回的每一段都非空，且不超过 typing.maxChars（末段截断除外）。 */
std::vector<std::string> SplitForChat(const std::string& text, const TypingConfig& config);

/** 这一条发出去之后该等多久再发下一条（毫秒）。 */
int ChunkDelayMs(const std::string& chunk, const TypingConfig& config);

/** UTF-8 码点数（按码点算长度，不是按字节 —— 否则中文一条只能装 20 个字）。 */
std::size_t CodePointCount(const std::string& text);

/** 按码点截断（不会把一个 UTF-8 字符切两半）。 */
std::string TruncateCodePoints(const std::string& text, std::size_t maxCodePoints);

}  // namespace dshbot
}  // namespace dchat
