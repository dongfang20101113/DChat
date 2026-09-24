// 聊天协议：一行一条消息，UTF-8 编码，用 \n 分隔（兼容 \r\n 结尾）。
// 设计原则：协议层不碰任何网络 API，便于单独做单元测试。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dchat {

inline constexpr int kDefaultPort = 5555;
inline constexpr std::size_t kMaxLineBytes = 4096;  // 单行上限
inline constexpr std::size_t kMaxNickChars = 12;    // 昵称上限（按 Unicode 码点算）

struct Message {
    std::string command;  // 已转大写；空字符串表示空行或无效行
    std::string rest;     // 命令之后的原始文本（首尾空白已去掉）
    std::vector<std::string> Words() const;  // rest 按空白切分
};

enum class NickError { None, Empty, TooLong, IllegalChar, Taken };

// 解析一行（不含结尾换行符）
Message ParseLine(const std::string& line);
// 组装一行（自动大写命令、去掉换行、限制长度）；命令非法时返回空串
std::string BuildLine(const std::string& command, const std::string& rest = std::string());

// 校验并规范化昵称；返回空串表示不合法，原因写入 error
std::string NormalizeNick(const std::string& raw, NickError* error = nullptr);
const char* NickErrorText(NickError error);

std::size_t Utf8CharCount(const std::string& text);
std::string Utf8Truncate(const std::string& text, std::size_t maxChars);

// ---- 时间字段（协议里的 hh:mm，24 小时制） ----
std::string FormatTime(int hour, int minute);  // 9:5 -> "09:05"
bool LooksLikeTime(const std::string& text);   // 是否是 "hh:mm" 形式
std::string NowTimeString();                   // 本机当前时间

// ---- 常用消息构造 ----
inline std::string MakeWelcome(const std::string& serverName) { return BuildLine("WELCOME", serverName); }
inline std::string MakeSay(const std::string& nick, const std::string& text) {
    return BuildLine("SAY", nick + " " + text);
}
inline std::string MakeJoined(const std::string& nick) { return BuildLine("JOINED", nick); }
inline std::string MakeLeft(const std::string& nick) { return BuildLine("LEFT", nick); }
inline std::string MakeNames(const std::string& list) { return BuildLine("NAMES", list); }
inline std::string MakeSystem(const std::string& text) { return BuildLine("SYS", text); }
inline std::string MakeError(const std::string& text) { return BuildLine("ERROR", text); }

// 把 TCP 字节流切成一行一行（处理粘包/拆包、CRLF、超长行）
class LineBuffer {
public:
    void Append(const char* data, std::size_t len);
    // 取出一行（已去掉结尾的 \r\n 或 \n）。没有完整行时返回 false。
    bool PopLine(std::string* out);
    bool bad() const { return bad_; }  // 行太长时置位，调用方应断开连接
    void Reset() {
        buf_.clear();
        bad_ = false;
    }

private:
    std::string buf_;
    bool bad_ = false;
};

}  // namespace dchat
