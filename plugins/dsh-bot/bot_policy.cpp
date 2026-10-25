#include "bot_policy.h"

#include <cstddef>

#include "bot_config.h"
#include "render.h"  // dchat::MentionsNick：和桌面端高亮 / @全体成员 判定同一套规则

namespace dchat {
namespace dshbot {

namespace {

bool IsWordChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/**
 * 把正文里所有"@某个名字"整段删掉。
 *
 * 规则和 MentionsNick 完全对齐：`@` 前不能是词内字符，名字后面不能紧跟着词内字符。
 * 名字按大小写不敏感匹配。删完把首尾空白收掉，中间空出来的连续空格压成一个。
 */
std::string StripMentions(const std::string& text, const std::vector<std::string>& names) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '@' || (i > 0 && IsWordChar(text[i - 1]))) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        bool removed = false;
        for (const std::string& name : names) {
            if (name.empty()) continue;
            if (i + 1 + name.size() > text.size()) continue;
            bool same = true;
            for (std::size_t k = 0; k < name.size(); ++k) {
                if (LowerAscii(text[i + 1 + k]) != LowerAscii(name[k])) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            const std::size_t after = i + 1 + name.size();
            if (after < text.size() && IsWordChar(text[after])) continue;
            i = after;
            // 顺手吃掉 @ 后面紧跟的一个空格，避免留下 "  你好"
            if (i < text.size() && text[i] == ' ') ++i;
            removed = true;
            break;
        }
        if (!removed) {
            out.push_back(text[i]);
            ++i;
        }
    }
    // 收尾：首尾空白去掉，中间连续空格压成一个
    std::string compact;
    compact.reserve(out.size());
    bool lastSpace = false;
    for (char ch : out) {
        const bool space = (ch == ' ' || ch == '\t');
        if (space) {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        compact.push_back(ch);
    }
    std::size_t begin = 0;
    std::size_t end = compact.size();
    while (begin < end && (compact[begin] == ' ' || compact[begin] == '\t')) ++begin;
    while (end > begin && (compact[end - 1] == ' ' || compact[end - 1] == '\t')) --end;
    return compact.substr(begin, end - begin);
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

}  // namespace

TriggerMatch MatchTrigger(const std::string& raw, const std::string& botNick,
                          const TriggerConfig& config) {
    TriggerMatch result;
    if (!config.mentionOnly) {
        result.matched = true;
        result.text = raw;
        return result;
    }
    // 名字表：主名 + 别名。大小写不敏感，重复的去掉。
    std::vector<std::string> names;
    if (!botNick.empty()) names.push_back(botNick);
    for (const std::string& alias : config.aliases) {
        bool duplicate = false;
        for (const std::string& name : names) {
            if (SameNick(name, alias)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) names.push_back(alias);
    }

    bool hit = false;
    for (const std::string& name : names) {
        if (dchat::MentionsNick(raw, name)) {
            hit = true;
            break;
        }
    }
    if (!hit) return result;

    result.matched = true;
    result.text = StripMentions(raw, names);
    // 只 @ 了一声、没说正事：给模型一句能接的话，否则提示词末尾是空的
    if (result.text.empty()) result.text = "（只是叫了你一声，没有具体内容）";
    return result;
}

bool ReplyAllowed(const ReplyPolicy& policy, const std::string& nick) {
    if (policy.mode == ReplyMode::All) return true;
    bool listed = false;
    for (const std::string& name : policy.nicks) {
        if (SameNick(name, nick)) {
            listed = true;
            break;
        }
    }
    return (policy.mode == ReplyMode::Whitelist) ? listed : !listed;
}

std::string BuildPrompt(const BrainConfig& config, const std::string& roomName,
                        const std::string& roomId, const std::string& nick,
                        const std::string& text) {
    std::string prompt = config.promptTemplate;
    prompt = ReplaceAll(prompt, "{room}", roomName);
    prompt = ReplaceAll(prompt, "{roomId}", roomId);
    prompt = ReplaceAll(prompt, "{nick}", nick);
    prompt = ReplaceAll(prompt, "{text}", text);
    // {workdir} 用子进程的工作目录：那正是沙箱允许它动手的范围，
    // 两边必须说的是同一个路径，否则模型会去写一个它其实碰不到的地方。
    prompt = ReplaceAll(prompt, "{workdir}", config.cwd.empty() ? std::string("（未设置）") : config.cwd);
    return prompt;
}

}  // namespace dshbot
}  // namespace dchat
