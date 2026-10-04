#include "protocol.h"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace dchat {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && (IsSpace(text[end - 1]) || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return text.substr(begin, end - begin);
}

std::string ToUpperAscii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

bool IsAsciiLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// 命令名允许的字符：字母、数字、下划线。
// 注意：文件传输用的 FILE_SEND / FILE_DATA 带下划线，如果这里只允许字母，
// BuildLine 会判定"命令非法"并返回空串，转发出去就变成了一个空行。
bool IsCommandChar(char c) {
    return IsAsciiLetter(c) || (c >= '0' && c <= '9') || c == '_';
}

// 昵称里不允许出现的字符
bool IsNickChar(char c) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    switch (c) {
        case ' ':
        case '\t':
        case ',':
        case ':':
        case '/':
        case '<':
        case '>':
        case '"':
        case '\'':
        case '|':
        case '\\':
            return false;
        default:
            return true;
    }
}

}  // namespace

std::vector<std::string> Message::Words() const {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && IsSpace(rest[i])) ++i;
        const std::size_t begin = i;
        while (i < rest.size() && !IsSpace(rest[i])) ++i;
        if (i > begin) words.push_back(rest.substr(begin, i - begin));
    }
    return words;
}

std::size_t Utf8CharCount(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++count;  // 非续字节即为一个字符的开头
    return count;
}

std::string Utf8Truncate(const std::string& text, std::size_t maxChars) {
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        if ((c & 0xC0) != 0x80 && count == maxChars) break;
        if ((c & 0xC0) != 0x80) ++count;
        ++index;
    }
    return text.substr(0, index);
}

std::string FormatTime(int hour, int minute) {
    hour %= 24;
    if (hour < 0) hour += 24;
    minute %= 60;
    if (minute < 0) minute += 60;
    char buffer[8] = {0};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return buffer;
}

bool LooksLikeTime(const std::string& text) {
    if (text.size() != 5 || text[2] != ':') return false;
    const int digits[4] = {0, 1, 3, 4};
    for (int index : digits) {
        if (text[static_cast<std::size_t>(index)] < '0' || text[static_cast<std::size_t>(index)] > '9') {
            return false;
        }
    }
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    return hour < 24 && minute < 60;
}

bool ParseUint64(const std::string& text, unsigned long long* out) {
    if (!out || text.empty()) return false;
    unsigned long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;  // 含正负号/空格/字母一律拒绝
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        // 溢出就失败：回绕出来的小数字会让续传位置跳到文件开头附近，
        // 后果是"悄悄拼出一个坏文件"，比直接拒绝严重得多
        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
        value = value * 10ull + digit;
    }
    *out = value;
    return true;
}

std::string NowTimeString() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    local = *std::localtime(&now);
#endif
    return FormatTime(local.tm_hour, local.tm_min);
}

// ---- 多行文本的转义 ----
//
// 只有两条规则，扫一遍即可：`\` -> `\\`，换行 -> `\n`。
// 反转义遇到不认识的转义**原样保留**，免得把旧客户端发来的真实反斜杠吃掉。

std::string EscapeText(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            // \r\n 归一成一个换行；单独的 \r 也当换行（从别处粘贴来的文本常见）
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string UnescapeText(const std::string& escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        const char c = escaped[i];
        if (c != '\\' || i + 1 >= escaped.size()) {
            out.push_back(c);
            continue;
        }
        const char next = escaped[i + 1];
        if (next == '\\') {
            out.push_back('\\');
            ++i;
        } else if (next == 'n') {
            out.push_back('\n');
            ++i;
        } else {
            out.push_back(c);  // 不认识的转义：原样保留
        }
    }
    return out;
}

std::size_t CountTextLines(const std::string& escaped) {
    if (escaped.empty()) return 0;
    std::size_t lines = 1;
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '\\') continue;
        if (i + 1 >= escaped.size()) break;
        const char next = escaped[i + 1];
        if (next == '\\') {
            ++i;  // \\ 是字面反斜杠，不算换行
            continue;
        }
        if (next == 'n') {
            ++lines;
            ++i;
        }
    }
    return lines;
}

Message ParseLine(const std::string& line) {
    Message message;
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) return message;

    std::size_t split = 0;
    while (split < trimmed.size() && !IsSpace(trimmed[split])) ++split;
    message.command = ToUpperAscii(trimmed.substr(0, split));
    message.rest = Trim(trimmed.substr(split));
    return message;
}

std::string BuildLine(const std::string& command, const std::string& rest) {
    if (command.empty()) return std::string();
    for (char c : command)
        if (!IsCommandChar(c)) return std::string();

    std::string safe;
    safe.reserve(rest.size());
    for (char c : rest)
        if (c != '\r' && c != '\n') safe.push_back(c);

    std::string line = ToUpperAscii(command);
    if (!safe.empty()) line += " " + safe;
    if (line.size() > kMaxLineBytes) line.resize(kMaxLineBytes);
    return line;
}

std::string NormalizeNick(const std::string& raw, NickError* error) {
    auto fail = [&](NickError kind) {
        if (error) *error = kind;
        return std::string();
    };

    const std::string nick = Trim(raw);
    if (nick.empty()) return fail(NickError::Empty);
    if (Utf8CharCount(nick) > kMaxNickChars) return fail(NickError::TooLong);
    for (char c : nick)
        if (!IsNickChar(c)) return fail(NickError::IllegalChar);
    if (error) *error = NickError::None;
    return nick;
}

const char* NickErrorText(NickError error) {
    switch (error) {
        case NickError::None:
            return "";
        case NickError::Empty:
            return "昵称不能为空";
        case NickError::TooLong:
            return "昵称太长（最多 12 个字符）";
        case NickError::IllegalChar:
            return "昵称不能包含空格、逗号、冒号、斜杠等字符";
        case NickError::Taken:
            return "昵称已被占用";
    }
    return "昵称不合法";
}

void LineBuffer::Append(const char* data, std::size_t len) {
    if (bad_) return;
    buf_.append(data, len);
    // 收到一大段迟迟没有换行的数据：立刻判定异常，避免缓冲区无界增长
    if (buf_.find('\n') == std::string::npos && buf_.size() > kMaxLineBytes) bad_ = true;
}

bool LineBuffer::PopLine(std::string* out) {
    if (bad_) return false;
    const std::size_t pos = buf_.find('\n');
    if (pos == std::string::npos) {
        if (buf_.size() > kMaxLineBytes) bad_ = true;  // 迟迟没有换行，视为异常数据
        return false;
    }
    std::string line = buf_.substr(0, pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    buf_.erase(0, pos + 1);
    if (line.size() > kMaxLineBytes) {
        bad_ = true;
        return false;
    }
    if (out) *out = line;
    return true;
}

}  // namespace dchat
