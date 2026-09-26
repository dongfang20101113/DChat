#include "server_rules.h"

#include <algorithm>

namespace dchat {

namespace {

// 每条规则的取值范围（数值规则）
struct RuleRange {
    const char* name;
    long long minValue;
    long long maxValue;
    const char* unit;
    const char* description;
};

const RuleRange kRanges[] = {
    {"chatinterval", 0, 60000, "ms", "两条消息之间最少间隔（0 = 不限制）"},
    {"documentsize", 1, 4096, "MB", "单个文件最大大小"},
    {"maxservertemp", 16, 32768, "MB", "服务端保存文件 + 聊天记录缓存的总上限"},
};

const RuleRange* FindRange(const std::string& name) {
    for (const RuleRange& range : kRanges) {
        if (name == range.name) return &range;
    }
    return nullptr;
}

std::string ToLowerAscii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    return out;
}

}  // namespace

bool IsKnownRule(const std::string& name) {
    const std::string lower = ToLowerAscii(name);
    for (const RuleInfo& info : AllRuleInfos()) {
        if (lower == info.name) return true;
    }
    return false;
}

const std::vector<RuleInfo>& AllRuleInfos() {
    static const std::vector<RuleInfo> infos = {
        {"chatinterval", "<毫秒> 两条消息之间的最小间隔，0 = 不限", false},
        {"documentsize", "<MB> 单个文件最大大小", false},
        {"keepchathistory", "true|false 新加入的人能否看到之前的记录", true},
        {"maxservertemp", "<MB> 服务端缓存（文件 + 记录）总上限", false},
    };
    return infos;
}

const std::vector<std::string>& AllRuleNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const RuleInfo& info : AllRuleInfos()) out.push_back(info.name);
        return out;
    }();
    return names;
}

bool IsBoolRule(const std::string& name) {
    const std::string lower = ToLowerAscii(name);
    for (const RuleInfo& info : AllRuleInfos()) {
        if (lower == info.name) return info.isBool;
    }
    return false;
}

std::string RuleRangeText(const std::string& name) {
    const std::string lower = ToLowerAscii(name);
    if (lower == "keepchathistory") {
        return "true / false";
    }
    const RuleRange* range = FindRange(lower);
    if (!range) return std::string();
    return std::to_string(range->minValue) + " - " + std::to_string(range->maxValue) + " " +
           range->unit;
}

std::string DescribeRule(const ServerRules& rules, const std::string& name) {
    const std::string lower = ToLowerAscii(name);
    if (lower == "chatinterval") {
        return "chatinterval = " + std::to_string(rules.chatIntervalMs) + " ms（" +
               "两条消息之间最少间隔，0 = 不限制）";
    }
    if (lower == "documentsize") {
        return "documentsize = " + std::to_string(rules.documentSizeMb) +
               " MB（单个文件最大大小）";
    }
    if (lower == "keepchathistory") {
        return std::string("keepchathistory = ") + (rules.keepChatHistory ? "true" : "false") +
               "（新加入的客户端能否看到之前的聊天记录和文件）";
    }
    if (lower == "maxservertemp") {
        return "maxservertemp = " + std::to_string(rules.maxServerTempMb) +
               " MB（服务端保存文件 + 聊天记录缓存的总上限）";
    }
    return "未知规则：" + name;
}

std::string DescribeAllRules(const ServerRules& rules) {
    std::string out = "服务器规则：\n";
    for (const std::string& name : AllRuleNames()) {
        out += "  " + DescribeRule(rules, name) + "\n";
    }
    out += "  用法：/chatrule <规则> <set|add|remove> <值>，布尔规则用 true/false";
    return out;
}

unsigned long long RuleMbToBytes(int mb) {
    return static_cast<unsigned long long>(mb < 0 ? 0 : mb) * 1024ull * 1024ull;
}

RuleChange ApplyRule(ServerRules* rules, const std::string& name, RuleAction action, long long value,
                     bool boolValue) {
    RuleChange change;
    if (!rules) return change;
    const std::string lower = ToLowerAscii(name);
    change.rule = lower;
    if (!IsKnownRule(lower)) {
        change.message = "没有这条规则：" + name + "（用 /chatrule 看全部规则）";
        return change;
    }
    if (action == RuleAction::Show) {
        change.ok = true;
        change.message = DescribeRule(*rules, lower);
        return change;
    }

    if (lower == "keepchathistory") {
        if (action == RuleAction::Add || action == RuleAction::Remove) {
            change.message = "keepchathistory 是布尔规则，只能用 true / false（或 set true/false）";
            return change;
        }
        const bool next = (action == RuleAction::SetBool) ? boolValue : (value != 0);
        change.ok = true;
        change.changed = (next != rules->keepChatHistory);
        rules->keepChatHistory = next;
        change.message = std::string("keepchathistory = ") + (next ? "true" : "false") +
                         (next ? "（新加入的客户端能看到之前的聊天记录和文件）"
                               : "（新加入的客户端看不到之前的记录）");
        return change;
    }

    const RuleRange* range = FindRange(lower);
    if (!range) {
        change.message = "没有这条规则：" + name;
        return change;
    }
    if (action == RuleAction::SetBool) {
        change.message = lower + " 是数值规则，用法是 /chatrule " + lower + " set <值>";
        return change;
    }

    long long current = 0;
    if (lower == "chatinterval") current = rules->chatIntervalMs;
    if (lower == "documentsize") current = rules->documentSizeMb;
    if (lower == "maxservertemp") current = rules->maxServerTempMb;

    long long next = current;
    if (action == RuleAction::Set) {
        next = value;
    } else if (action == RuleAction::Add) {
        next = current + value;
    } else {
        next = current - value;
    }
    std::string note;
    if (next < range->minValue) {
        next = range->minValue;
        note = "（已限制到最小值 " + std::to_string(range->minValue) + "）";
    } else if (next > range->maxValue) {
        next = range->maxValue;
        note = "（已限制到最大值 " + std::to_string(range->maxValue) + "）";
    }

    // 单文件上限不能比服务端总缓存还大，那样永远存不下
    if (lower == "documentsize" && next > rules->maxServerTempMb) {
        change.message = "documentsize 不能超过 maxservertemp（当前 " +
                         std::to_string(rules->maxServerTempMb) +
                         " MB）；先调大 maxservertemp，或把 documentsize 设小一点";
        return change;
    }
    if (lower == "maxservertemp" && next < rules->documentSizeMb) {
        change.message = "maxservertemp 不能小于当前的 documentsize（" +
                         std::to_string(rules->documentSizeMb) +
                         " MB）；先调小 documentsize";
        return change;
    }

    change.ok = true;
    change.changed = (next != current);
    if (lower == "chatinterval") rules->chatIntervalMs = static_cast<int>(next);
    if (lower == "documentsize") rules->documentSizeMb = static_cast<int>(next);
    if (lower == "maxservertemp") rules->maxServerTempMb = static_cast<int>(next);
    change.message = lower + " = " + std::to_string(next) + " " + range->unit + note;
    return change;
}

std::string RulesLineForClient(const ServerRules& rules) {
    return "RULES " + std::to_string(rules.documentSizeMb) + " " +
           std::to_string(rules.chatIntervalMs) + " " + (rules.keepChatHistory ? "1" : "0");
}

std::string SerializeRules(const ServerRules& rules) {
    std::string out;
    out += "# dchat server rules (v1): name value\n";
    out += "# 改这里要重启服务器才生效；也可以在控制台用 /chatrule 改（改完自动写回这个文件）\n";
    out += "chatinterval " + std::to_string(rules.chatIntervalMs) + "      # 两条消息之间最少间隔（毫秒），0 = 不限制\n";
    out += "documentsize " + std::to_string(rules.documentSizeMb) + "        # 单个文件最大大小（MB）\n";
    out += std::string("keepchathistory ") + (rules.keepChatHistory ? "true" : "false") +
           "  # 新加入的客户端能否看到之前的聊天记录和文件\n";
    out += "maxservertemp " + std::to_string(rules.maxServerTempMb) +
           "      # 服务端保存文件 + 聊天记录缓存的总上限（MB）\n";
    return out;
}

int ParseRules(const std::string& text, ServerRules* rules) {
    if (!rules) return 0;
    // 先按行拆成"规则名 -> 值"，再统一套用（这样文件里谁先谁后都不会因为相互约束被拒）
    ServerRules parsed = *rules;
    int count = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        std::string line = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                      : end - begin);
        if (end == std::string::npos) begin = text.size() + 1;
        else begin = end + 1;
        // 去掉注释和两端空白
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::size_t first = 0, last = line.size();
        while (first < last && (line[first] == ' ' || line[first] == '\t' || line[first] == '\r')) ++first;
        while (last > first && (line[last - 1] == ' ' || line[last - 1] == '\t' || line[last - 1] == '\r')) --last;
        line = line.substr(first, last - first);
        if (line.empty()) continue;
        // 拆成两个词
        std::size_t space = 0;
        while (space < line.size() && line[space] != ' ' && line[space] != '\t') ++space;
        const std::string name = ToLowerAscii(line.substr(0, space));
        std::string value = line.substr(space);
        std::size_t valueBegin = 0;
        while (valueBegin < value.size() && (value[valueBegin] == ' ' || value[valueBegin] == '\t')) {
            ++valueBegin;
        }
        value = value.substr(valueBegin);
        std::size_t valueEnd = value.size();
        while (valueEnd > 0 && (value[valueEnd - 1] == ' ' || value[valueEnd - 1] == '\t')) --valueEnd;
        value = value.substr(0, valueEnd);
        if (!IsKnownRule(name) || value.empty()) continue;

        if (name == "keepchathistory") {
            const std::string lower = ToLowerAscii(value);
            if (lower == "true" || lower == "1") {
                parsed.keepChatHistory = true;
                ++count;
            } else if (lower == "false" || lower == "0") {
                parsed.keepChatHistory = false;
                ++count;
            }
            continue;
        }
        const RuleRange* range = FindRange(name);
        if (!range) continue;
        long long number = 0;
        try {
            std::size_t used = 0;
            number = std::stoll(value, &used);
            if (used != value.size()) continue;  // "100abc" 这种当成坏行
        } catch (...) {
            continue;
        }
        if (number < range->minValue) number = range->minValue;
        if (number > range->maxValue) number = range->maxValue;
        if (name == "chatinterval") parsed.chatIntervalMs = static_cast<int>(number);
        if (name == "documentsize") parsed.documentSizeMb = static_cast<int>(number);
        if (name == "maxservertemp") parsed.maxServerTempMb = static_cast<int>(number);
        ++count;
    }
    // 文件里可能把两个值写成互相矛盾的样子，这里把 documentsize 夹到不超过 maxservertemp
    if (parsed.documentSizeMb > parsed.maxServerTempMb) {
        parsed.documentSizeMb = parsed.maxServerTempMb;
    }
    *rules = parsed;
    return count;
}

}  // namespace dchat
