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
    // ---- 2026-10 新增 ----
    {"uploadrate", 0, 1048576, "KB/s", "单个客户端上传限速（0 = 不限制）"},
    {"downloadrate", 0, 1048576, "KB/s", "单个客户端下载限速（0 = 不限制）"},
    {"maxtextlen", 0, 4096, "字符", "单条消息最大字符数（Unicode 码点，0 = 不限制）"},
    {"maxtextlines", 0, 200, "行", "单条消息最大行数（0 = 不限制）"},
    // ---- 2026-10 新增：公网加固 ----
    {"maxconns", 0, 100000, "个", "同时连接总数上限（0 = 不限制）"},
    {"maxconnsperip", 0, 10000, "个", "同一 IP 的同时连接数上限（0 = 不限制）"},
    {"loginfails", 0, 10000, "次", "同一 IP 在 5 分钟内允许的登录失败次数（0 = 不限制）"},
    {"handshaketimeout", 0, 600, "秒", "连上后多少秒内必须登录（0 = 不限制）"},
    // ---- 2026-10 新增：注册路径防护（实测过：没有这两条时，换 IP 疯狂注册可拖死正常用户）----
    {"registerinterval", 0, 86400, "秒", "同一 IP 两次注册之间的最小间隔（0 = 不限制）"},
    {"maxaccounts", 0, 10000000, "个", "账号总数上限（0 = 不限制）"},
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
        {"chatcolor", "true|false 聊天里能否用彩色代码（#RRGGBB / &a）", true},
        {"maxservertemp", "<MB> 服务端缓存（文件 + 记录）总上限", false},
        {"uploadrate", "<KB/s> 单客户端上传限速，0 = 不限", false},
        {"downloadrate", "<KB/s> 单客户端下载限速，0 = 不限", false},
        {"maxtextlen", "<字符> 单条消息最大字符数，0 = 不限", false},
        {"maxtextlines", "<行> 单条消息最大行数，0 = 不限", false},
        {"maxconns", "<个> 同时连接总数上限，0 = 不限", false},
        {"maxconnsperip", "<个> 同一 IP 的连接数上限，0 = 不限", false},
        {"loginfails", "<次> 同 IP 每 5 分钟允许的登录失败次数，0 = 不限", false},
        {"handshaketimeout", "<秒> 连上后多久必须登录，0 = 不限", false},
        {"registerinterval", "<秒> 同 IP 两次注册的最小间隔，0 = 不限", false},
        {"maxaccounts", "<个> 账号总数上限，0 = 不限", false},
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
    if (lower == "keepchathistory" || lower == "chatcolor") {
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
    if (lower == "chatcolor") {
        return std::string("chatcolor = ") + (rules.chatColor ? "true" : "false") +
               "（聊天里能否用彩色代码；关掉后色码原样显示，不会把字删掉）";
    }
    if (lower == "maxservertemp") {
        return "maxservertemp = " + std::to_string(rules.maxServerTempMb) +
               " MB（服务端保存文件 + 聊天记录缓存的总上限）";
    }
    if (lower == "uploadrate") {
        return "uploadrate = " + std::to_string(rules.uploadRateKbps) +
               " KB/s（单个客户端上传限速，0 = 不限制）";
    }
    if (lower == "downloadrate") {
        return "downloadrate = " + std::to_string(rules.downloadRateKbps) +
               " KB/s（单个客户端下载限速，0 = 不限制）";
    }
    if (lower == "maxtextlen") {
        return "maxtextlen = " + std::to_string(rules.maxTextLength) +
               " 字符（单条消息最大字符数，按 Unicode 码点算，0 = 不限制）";
    }
    if (lower == "maxtextlines") {
        return "maxtextlines = " + std::to_string(rules.maxTextLines) +
               " 行（单条消息最大行数，0 = 不限制）";
    }
    if (lower == "maxconns") {
        return "maxconns = " + std::to_string(rules.maxConnections) +
               " 个（同时连接总数上限，0 = 不限制）";
    }
    if (lower == "maxconnsperip") {
        return "maxconnsperip = " + std::to_string(rules.maxConnectionsPerIp) +
               " 个（同一 IP 的同时连接数上限，0 = 不限制）";
    }
    if (lower == "loginfails") {
        return "loginfails = " + std::to_string(rules.loginFailLimit) +
               " 次（同一 IP 在 " + std::to_string(kLoginFailWindowSec) +
               " 秒内允许的登录失败次数，0 = 不限制）";
    }
    if (lower == "handshaketimeout") {
        return "handshaketimeout = " + std::to_string(rules.handshakeTimeoutSec) +
               " 秒（连上后必须在这个时间内登录，0 = 不限制）";
    }
    if (lower == "registerinterval") {
        return "registerinterval = " + std::to_string(rules.registerIntervalSec) +
               " 秒（同一 IP 两次注册之间的最小间隔，0 = 不限制）";
    }
    if (lower == "maxaccounts") {
        return "maxaccounts = " + std::to_string(rules.maxAccounts) +
               " 个（账号总数上限，0 = 不限制）";
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

    if (lower == "chatcolor") {
        if (action == RuleAction::Add || action == RuleAction::Remove) {
            change.message = "chatcolor 是布尔规则，只能用 true / false（或 set true/false）";
            return change;
        }
        const bool next = (action == RuleAction::SetBool) ? boolValue : (value != 0);
        change.ok = true;
        change.changed = (next != rules->chatColor);
        rules->chatColor = next;
        change.message = std::string("chatcolor = ") + (next ? "true" : "false") +
                         (next ? "（允许彩色代码）" : "（禁止；用户打进去的色码会原样显示）");
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
    if (lower == "uploadrate") current = rules->uploadRateKbps;
    if (lower == "downloadrate") current = rules->downloadRateKbps;
    if (lower == "maxtextlen") current = rules->maxTextLength;
    if (lower == "maxtextlines") current = rules->maxTextLines;
    if (lower == "maxconns") current = rules->maxConnections;
    if (lower == "maxconnsperip") current = rules->maxConnectionsPerIp;
    if (lower == "loginfails") current = rules->loginFailLimit;
    if (lower == "handshaketimeout") current = rules->handshakeTimeoutSec;
    if (lower == "registerinterval") current = rules->registerIntervalSec;
    if (lower == "maxaccounts") current = rules->maxAccounts;

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
    if (lower == "uploadrate") rules->uploadRateKbps = static_cast<int>(next);
    if (lower == "downloadrate") rules->downloadRateKbps = static_cast<int>(next);
    if (lower == "maxtextlen") rules->maxTextLength = static_cast<int>(next);
    if (lower == "maxtextlines") rules->maxTextLines = static_cast<int>(next);
    if (lower == "maxconns") rules->maxConnections = static_cast<int>(next);
    if (lower == "maxconnsperip") rules->maxConnectionsPerIp = static_cast<int>(next);
    if (lower == "loginfails") rules->loginFailLimit = static_cast<int>(next);
    if (lower == "handshaketimeout") rules->handshakeTimeoutSec = static_cast<int>(next);
    if (lower == "registerinterval") rules->registerIntervalSec = static_cast<int>(next);
    if (lower == "maxaccounts") rules->maxAccounts = static_cast<int>(next);
    change.message = lower + " = " + std::to_string(next) + " " + range->unit + note;
    return change;
}

std::string RulesLineForClient(const ServerRules& rules) {
    // 前三个字段的位置和含义**绝对不能动**：老客户端按位置解析它们。
    // 新字段一律追加在末尾——桌面端的解析器只读 fields[0]，
    // 安卓端的 ServerLine.Rules 也是按位置读并且容忍缺字段，
    // 所以"只追加不重排"能保证新旧客户端都能正常工作。
    //
    // 注意：加固那 4 条（maxconns / maxconnsperip / loginfails / handshaketimeout）
    // **刻意不发给客户端**——它们是服务端的资源保护策略，客户端知道也没用，
    // 而且这样能保持客户端契约不变。
    return "RULES " + std::to_string(rules.documentSizeMb) + " " +
           std::to_string(rules.chatIntervalMs) + " " + (rules.keepChatHistory ? "1" : "0") + " " +
           std::to_string(rules.uploadRateKbps) + " " + std::to_string(rules.downloadRateKbps) +
           " " + std::to_string(rules.maxTextLength) + " " + std::to_string(rules.maxTextLines) +
           // chatcolor 用**具名**追加而不是插进位置序列里：老客户端按位置读前面几个字段，
           // 追加在末尾不影响它们；新客户端按名字找，顺序以后再变也不会错。
           " chatcolor=" + (rules.chatColor ? "1" : "0");
}

int RateLimiter::Consume(std::size_t bytes) {
    if (kbps <= 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    const double rate = static_cast<double>(kbps) * 1024.0;  // 字节/秒

    if (last.time_since_epoch().count() == 0) {
        last = now;
        tokens = rate;  // 开局先给满一桶，避免第一条就被卡住
    }
    const double elapsed = std::chrono::duration<double>(now - last).count();
    last = now;
    tokens = std::min(tokens + elapsed * rate, rate);  // 桶容量 = 1 秒的量

    const double need = static_cast<double>(bytes);
    if (tokens >= need) {
        tokens -= need;
        return 0;
    }
    // 不够：把桶清零，让调用方等够"欠账"的时间，下一轮自然就补回来了
    const double deficit = need - tokens;
    tokens = 0.0;
    return static_cast<int>(deficit / rate * 1000.0 + 0.5);
}

std::string SerializeRules(const ServerRules& rules) {
    std::string out;
    out += "# dchat server rules (v1): name value\n";
    out += "# 改这里要重启服务器才生效；也可以在控制台用 /chatrule 改（改完自动写回这个文件）\n";
    out += "chatinterval " + std::to_string(rules.chatIntervalMs) + "      # 两条消息之间最少间隔（毫秒），0 = 不限制\n";
    out += "documentsize " + std::to_string(rules.documentSizeMb) + "        # 单个文件最大大小（MB）\n";
    out += std::string("keepchathistory ") + (rules.keepChatHistory ? "true" : "false") +
           "  # 新加入的客户端能否看到之前的聊天记录和文件\n";
    out += std::string("chatcolor ") + (rules.chatColor ? "true" : "false") +
           "        # 聊天里能否用彩色代码（#RRGGBB / &a）；关掉后色码原样显示\n";
    out += "maxservertemp " + std::to_string(rules.maxServerTempMb) +
           "      # 服务端保存文件 + 聊天记录缓存的总上限（MB）\n";
    out += "uploadrate " + std::to_string(rules.uploadRateKbps) +
           "      # 单个客户端上传限速（KB/s），0 = 不限制\n";
    out += "downloadrate " + std::to_string(rules.downloadRateKbps) +
           "      # 单个客户端下载限速（KB/s），0 = 不限制\n";
    out += "maxtextlen " + std::to_string(rules.maxTextLength) +
           "      # 单条消息最大字符数（Unicode 码点），0 = 不限制\n";
    out += "maxtextlines " + std::to_string(rules.maxTextLines) +
           "      # 单条消息最大行数，0 = 不限制\n";
    out += "maxconns " + std::to_string(rules.maxConnections) +
           "      # 同时连接总数上限，0 = 不限制（公网建议 200）\n";
    out += "maxconnsperip " + std::to_string(rules.maxConnectionsPerIp) +
           "      # 同一 IP 的连接数上限，0 = 不限制（公网建议 8）\n";
    out += "loginfails " + std::to_string(rules.loginFailLimit) +
           "      # 同一 IP 每 5 分钟允许的登录失败次数，0 = 不限制（公网建议 10）\n";
    out += "handshaketimeout " + std::to_string(rules.handshakeTimeoutSec) +
           "      # 连上后多少秒内必须登录，0 = 不限制（公网建议 30）\n";
    out += "registerinterval " + std::to_string(rules.registerIntervalSec) +
           "      # 同一 IP 两次注册的最小间隔（秒），0 = 不限制（公网建议 60）\n";
    out += "maxaccounts " + std::to_string(rules.maxAccounts) +
           "      # 账号总数上限，0 = 不限制（换 IP 也绕不过这道闸）\n";
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

        if (name == "chatcolor") {
            const std::string lower = ToLowerAscii(value);
            if (lower == "true" || lower == "1") {
                parsed.chatColor = true;
                ++count;
            } else if (lower == "false" || lower == "0") {
                parsed.chatColor = false;
                ++count;
            }
            continue;
        }
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
        if (name == "uploadrate") parsed.uploadRateKbps = static_cast<int>(number);
        if (name == "downloadrate") parsed.downloadRateKbps = static_cast<int>(number);
        if (name == "maxtextlen") parsed.maxTextLength = static_cast<int>(number);
        if (name == "maxtextlines") parsed.maxTextLines = static_cast<int>(number);
        if (name == "maxconns") parsed.maxConnections = static_cast<int>(number);
        if (name == "maxconnsperip") parsed.maxConnectionsPerIp = static_cast<int>(number);
        if (name == "loginfails") parsed.loginFailLimit = static_cast<int>(number);
        if (name == "handshaketimeout") parsed.handshakeTimeoutSec = static_cast<int>(number);
        if (name == "registerinterval") parsed.registerIntervalSec = static_cast<int>(number);
        if (name == "maxaccounts") parsed.maxAccounts = static_cast<int>(number);
        ++count;
    }
    // 文件里可能把两个值写成互相矛盾的样子，这里把 documentsize 夹到不超过 maxservertemp
    if (parsed.documentSizeMb > parsed.maxServerTempMb) {
        parsed.documentSizeMb = parsed.maxServerTempMb;
    }
    *rules = parsed;
    return count;
}

// ---------------------------------------------------------------------------
// LoginFailTracker：按 IP 统计登录失败，用于防爆破
// ---------------------------------------------------------------------------

namespace {

/** 窗口是否已经过期。 */
bool WindowExpired(const std::chrono::steady_clock::time_point& start) {
    if (start.time_since_epoch().count() == 0) return true;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    return elapsed >= kLoginFailWindowSec;
}

}  // namespace

bool LoginFailTracker::IsBlocked(const std::string& ip, int limit) const {
    if (limit <= 0 || ip.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(ip);
    if (it == records_.end()) return false;
    if (WindowExpired(it->second.windowStart)) return false;  // 窗口过了，等于没锁
    return it->second.failures >= limit;
}

void LoginFailTracker::NoteFailure(const std::string& ip) {
    if (ip.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Record& record = records_[ip];
    if (WindowExpired(record.windowStart)) {
        // 开一个新窗口
        record.failures = 0;
        record.windowStart = std::chrono::steady_clock::now();
    }
    ++record.failures;
}

void LoginFailTracker::Clear(const std::string& ip) {
    if (ip.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    records_.erase(ip);
}

void LoginFailTracker::Sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = records_.begin(); it != records_.end();) {
        if (WindowExpired(it->second.windowStart)) {
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t LoginFailTracker::TrackedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

}  // namespace dchat
