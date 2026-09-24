#include "render.h"

#include "protocol.h"

namespace dchat {

namespace {

// 取出开头可能存在的 "hh:mm" 时间字段，并把它从 rest 里去掉
std::string TakeTimePrefix(std::string& rest) {
    if (rest.empty()) return std::string();
    const std::size_t space = rest.find(' ');
    const std::string head = (space == std::string::npos) ? rest : rest.substr(0, space);
    if (!LooksLikeTime(head)) return std::string();
    rest = (space == std::string::npos) ? std::string() : rest.substr(space + 1);
    return head;
}

// 取第一个词，剩下的留在 rest 里
std::string TakeFirstWord(std::string& rest) {
    if (rest.empty()) return std::string();
    const std::size_t space = rest.find(' ');
    if (space == std::string::npos) {
        const std::string word = rest;
        rest.clear();
        return word;
    }
    const std::string word = rest.substr(0, space);
    rest.erase(0, space + 1);
    return word;
}

bool IsWordChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

int NickColorIndex(const std::string& nick, int paletteSize) {
    if (paletteSize <= 0) return 0;
    unsigned int hash = 2166136261u;  // FNV-1a
    for (unsigned char c : nick) {
        hash ^= c;
        hash *= 16777619u;
    }
    return static_cast<int>(hash % static_cast<unsigned int>(paletteSize));
}

bool MentionsNick(const std::string& text, const std::string& nick) {
    if (nick.empty() || text.empty()) return false;
    const std::size_t length = nick.size();
    for (std::size_t i = 0; i + 1 + length <= text.size(); ++i) {
        if (text[i] != '@') continue;
        if (i > 0 && IsWordChar(text[i - 1])) continue;  // 前面挨着词内字符，不算提及
        bool same = true;
        for (std::size_t k = 0; k < length; ++k) {
            if (ToLowerAscii(text[i + 1 + k]) != ToLowerAscii(nick[k])) {
                same = false;
                break;
            }
        }
        if (!same) continue;
        const std::size_t after = i + 1 + length;
        if (after < text.size() && IsWordChar(text[after])) continue;  // 后面还有词内字符
        return true;
    }
    return false;
}

bool MentionsAll(const std::string& text) { return MentionsNick(text, "all"); }

bool ParseSay(const std::string& rawLine, const std::string& selfNick, SayInfo* out) {
    const Message msg = ParseLine(rawLine);
    if (msg.command != "SAY") return false;

    std::string rest = msg.rest;
    const std::string time = TakeTimePrefix(rest);
    const std::string nick = TakeFirstWord(rest);
    if (nick.empty()) return false;

    if (out) {
        out->time = time;
        out->nick = nick;
        out->text = rest;
        out->own = !selfNick.empty() && nick == selfNick;
        out->mention = !out->own && (MentionsNick(rest, selfNick) || MentionsAll(rest));
    }
    return true;
}

bool MentionsMe(const std::string& rawLine, const std::string& selfNick) {
    SayInfo info;
    if (!ParseSay(rawLine, selfNick, &info)) return false;
    return info.mention;
}

NoticeInfo ParseNotice(const std::string& rawLine) {
    NoticeInfo info;
    const Message msg = ParseLine(rawLine);
    if (msg.command.empty()) return info;

    std::string rest = msg.rest;
    info.time = TakeTimePrefix(rest);
    info.isNotice = true;

    if (msg.command == "WELCOME") {
        info.text = "已连接到服务器 " + rest;
    } else if (msg.command == "JOINED") {
        info.text = rest + " 加入了聊天室";
    } else if (msg.command == "LEFT") {
        info.text = rest + " 离开了聊天室";
    } else if (msg.command == "NAMES") {
        info.text = "在线成员：" + rest;
    } else if (msg.command == "SYS") {
        info.text = rest;
    } else if (msg.command == "LOGGEDIN") {
        // 服务器发的"登录/注册已通过"，<文本> 就是用户名
        info.text = rest;
    } else if (msg.command == "ERROR") {
        info.text = rest;
        info.isError = true;
    } else if (msg.command == "PONG") {
        info.text = "服务器回应正常（PONG）";
    } else if (msg.command == "ANNOUNCE") {
        info.text = rest;
        info.isAnnouncement = true;
    } else {
        info.text = rawLine;
        info.isNotice = false;
    }
    return info;
}

bool ShouldCountUnread(bool windowActive) { return !windowActive; }

bool ShouldFlash(bool windowActive, bool isMention) { return !windowActive && isMention; }

std::string FormatUnreadTitle(const std::string& base, int unreadCount) {
    if (unreadCount <= 0) return base;
    return "【" + std::to_string(unreadCount) + "】" + base;
}

}  // namespace dchat
