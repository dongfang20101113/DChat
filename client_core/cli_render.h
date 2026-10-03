// 把 ChatMessage 渲染成**带 ANSI 颜色的终端文字**（纯函数，不碰终端、不碰网络）。
//
// 为什么单独抽出来：渲染是"错了不报错、只是显示不对"的那类代码 —— 颜色串错位会让
// 整行花掉、宽度算错会把输入行顶跑。抽成纯函数才能在单测里钉住，
// 而且它同时能被交叉编译到 macOS 验证（终端版和 Cocoa 版共用同一套色码解析）。
//
// 宽度约定：按**字节**算可见宽度（和这一端原来的实现一致）。中文一个字 3 字节、
// 显示 2 格，所以中文多的时候折行位置会偏早一点 —— 这是已知取舍，
// 不是 bug：要按显示格算得引入 wcwidth，那是另一件事。
#pragma once

#include <string>
#include <vector>

#include "chat_core.h"

namespace dchat {

/// 终端里用的几个固定色（ANSI SGR）
inline constexpr const char* kAnsiReset = "\x1b[0m";
inline constexpr const char* kAnsiDim = "\x1b[90m";      // 灰色：时间戳、控制行
inline constexpr const char* kAnsiSystem = "\x1b[36m";   // 青色：系统提示
inline constexpr const char* kAnsiError = "\x1b[31m";    // 红色：错误
inline constexpr const char* kAnsiMention = "\x1b[1;33m";// 加粗黄：有人 @ 我
inline constexpr const char* kAnsiBold = "\x1b[1m";

/** 一条消息渲染成的一行（或几行）终端文字。 */
struct RenderedLine {
    std::string text;
    bool visible = true;  // false = 这行不用显示（控制行）
};

/**
 * 渲染一条聊天消息。
 *
 * @param colorEnabled 服务器规则 + 用户本地开关都允许时才解析色码
 */
RenderedLine RenderChatMessage(const ChatMessage& message, bool colorEnabled);

/** 把一段可能很长的带色文字按终端宽度折行（不会把 ANSI 序列从中截断）。 */
std::vector<std::string> WrapAnsi(const std::string& text, int width);

/** 渲染一条"状态提示"（不是聊天消息），用于上传进度之类。 */
RenderedLine RenderStatusLine(const std::string& text);

}  // namespace dchat
