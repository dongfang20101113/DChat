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
