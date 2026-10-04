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

// ---- 多行文本的转义 ----
//
// 协议是**行式**的：BuildLine 会把 \r 和 \n 直接丢掉，所以聊天内容里不可能带真换行。
// 要支持多行消息（以及"最多几行"这条规则），必须在发送前把换行转义成两个字符。
//
// 规则很小，就两条，扫一遍即可：
//     反斜杠 -> 两个反斜杠
//     换行   -> 反斜杠 + n    （\r\n 和单独的 \r 也归一成换行）
//
// 反转义遇到不认识的转义（比如 "\x"）**原样保留**，这样旧客户端发来的、
// 恰好含反斜杠的普通文本不会被吃掉字符。
//
// 老客户端完全不受影响：它们看到的是字面量 "\n"，只是不好看，不会崩。
std::string EscapeText(const std::string& text);
std::string UnescapeText(const std::string& escaped);

// 转义后的文本有多少行（没有任何换行时是 1；空串算 0 行）
std::size_t CountTextLines(const std::string& escaped);

// ---- 时间字段（协议里的 hh:mm，24 小时制） ----
std::string FormatTime(int hour, int minute);  // 9:5 -> "09:05"
bool LooksLikeTime(const std::string& text);   // 是否是 "hh:mm" 形式
std::string NowTimeString();                   // 本机当前时间

/**
 * 解析十进制无符号整数。
 *
 * 为什么单独有这个函数：断点续传的起点是从网络上来的字符串，
 * 直接 std::stoull 遇到非数字会**抛异常**（服务器上抛异常等于整条连接崩掉），
 * 而 atoll 会把垃圾解析成 0 —— 那更糟：客户端说"我从 100MB 处续传"，
 * 服务器理解成"从头发"，两边的写指针就此错位，最后拼出一个坏文件。
 * 所以必须"解析失败就明确失败"，让调用方去拒绝这次请求。
 */
bool ParseUint64(const std::string& text, unsigned long long* out);

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
