// 聊天记录的"显示规则"：解析协议行、判定 @提及、昵称配色、未读提示。
// 这一层不依赖任何 Windows API，方便单元测试；界面层负责把这些结果画成气泡。
#pragma once

#include <string>

namespace dchat {

inline constexpr int kNickPaletteSize = 8;  // 昵称调色板大小

// 同一个昵称永远映射到同一个颜色下标（FNV-1a 哈希）
int NickColorIndex(const std::string& nick, int paletteSize = kNickPaletteSize);

// @提及判断：@ 前面不能是"词内字符"，昵称后面也不能紧跟词内字符，
// 所以 @alicex 不会命中 @alice。只有 ASCII 字母数字下划线算词内字符，
// 因此 "@alice，看这个" 和 "@alice你好" 都算提及（中文不用空格分词）。
bool MentionsNick(const std::string& text, const std::string& nick);

// @all：广播式提及，所有人都算被提到
bool MentionsAll(const std::string& text);

// 这一行对我而言算不算"有人叫我"（@我 或 @all，自己发的不算）
bool MentionsMe(const std::string& rawLine, const std::string& selfNick);

// ---- 一条 SAY 消息解析出来的内容（气泡要用的信息） ----
struct SayInfo {
    std::string time;      // "21:05"，可能为空（兼容不带时间的旧格式）
    std::string nick;      // 发言者
    std::string text;      // 正文
    bool own = false;      // 是不是自己发的（决定气泡靠左还是靠右）
    bool mention = false;  // 是否 @我 或 @all（气泡高亮）
};

// 解析 SAY 行；不是 SAY 或缺参数时返回 false
bool ParseSay(const std::string& rawLine, const std::string& selfNick, SayInfo* out);

// ---- 系统提示（加入/离开/在线名单/错误等） ----
struct NoticeInfo {
    std::string time;      // "21:05"，可能为空
    std::string text;      // 要显示的文本
    bool isError = false;  // 错误信息（红色）
    bool isAnnouncement = false;  // 全服公告（客户端会大字居中、特殊颜色显示）
    bool isNotice = false; // 是否是能识别的协议行（false 表示未知命令，按原文显示）
};

NoticeInfo ParseNotice(const std::string& rawLine);

// ---- 未读提示规则 ----
bool ShouldCountUnread(bool windowActive);            // 窗口不在前台时，新消息计入未读
bool ShouldFlash(bool windowActive, bool isMention);  // 只有"不在前台 + 有人叫我"才闪任务栏
std::string FormatUnreadTitle(const std::string& base, int unreadCount);  // 有未读时前置【N】

}  // namespace dchat
