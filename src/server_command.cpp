#include "server_command.h"

#include "auth.h"  // 改密码时复用密码规则（长度 / 不能有空格）

#include <cctype>
#include <vector>

namespace dchat {

namespace {

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    // 容忍开头的 UTF-8 BOM：有些工具（例如 PowerShell 管道）写第一行时会带上它
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        begin = 3;
    }
    std::size_t end = text.size();
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const std::size_t begin = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (i > begin) words.push_back(text.substr(begin, i - begin));
    }
    return words;
}

std::string ToLowerAscii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// 把 "/Ban" 归一化成 "ban"；不是以 '/' 开头则返回空串
std::string CommandKeyword(const std::string& word) {
    if (word.empty() || word[0] != '/') return std::string();
    return ToLowerAscii(word.substr(1));
}

bool IsKnownCommand(const std::string& keyword) {
    return keyword == "ban" || keyword == "kick" || keyword == "unban" || keyword == "op" ||
           keyword == "deop" || keyword == "say" || keyword == "bans" || keyword == "banlist" ||
           keyword == "ops" || keyword == "oplist" || keyword == "changepassword" ||
           keyword == "help";
}

}  // namespace

bool ParseDuration(const std::string& text, int* outSeconds) {
    if (text.empty()) return false;
    long long total = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
        long long value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            if (value > 1000000000LL) return false;
            ++i;
        }
        long long multiplier = 1;  // 不带单位 = 秒
        if (i < text.size()) {
            const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
            switch (unit) {
                case 'w': multiplier = 7LL * 24 * 3600; break;
                case 'd': multiplier = 24LL * 3600; break;
                case 'h': multiplier = 3600; break;
                case 'm': multiplier = 60; break;
                case 's': multiplier = 1; break;
                default: return false;
            }
            ++i;
        }
        total += value * multiplier;
        if (total > 100LL * 365 * 24 * 3600) return false;
    }
    if (total <= 0) return false;
    if (outSeconds) *outSeconds = static_cast<int>(total);
    return true;
}

std::string FormatBanDuration(int seconds, bool permanent) {
    if (permanent) return "永久";
    if (seconds <= 0) return "0 秒";
    struct Unit {
        int seconds;
        const char* name;
    };
    const Unit units[] = {
        {7 * 24 * 3600, "周"}, {24 * 3600, "天"}, {3600, "小时"}, {60, "分"}, {1, "秒"}};
    std::string out;
    int remaining = seconds;
    for (const Unit& unit : units) {
        const int count = remaining / unit.seconds;
        if (count <= 0) continue;
        remaining -= count * unit.seconds;
        if (!out.empty()) out += " ";
        out += std::to_string(count);
        out += " ";
        out += unit.name;
    }
    return out;
}

bool IsCommandAttempt(const std::string& text) {
    const std::string trimmed = Trim(text);
    return !trimmed.empty() && trimmed[0] == '/';
}

bool LooksLikePasswordCommand(const std::string& text) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty() || trimmed[0] != '/') return false;
    std::string keyword;
    for (std::size_t i = 1; i < trimmed.size() && trimmed[i] != ' ' && trimmed[i] != '\t'; ++i) {
        keyword.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed[i]))));
    }
    return keyword == "changepassword";
}

std::string HistoryTextFor(const std::string& text) {
    const std::string trimmed = Trim(text);
    if (!LooksLikePasswordCommand(trimmed)) return text;  // 普通聊天 / 其余指令原样
    // 只保留 "/changepassword"，后面的密码（和名字）都不要
    std::size_t i = 1;
    while (i < trimmed.size() && trimmed[i] != ' ' && trimmed[i] != '\t') ++i;
    return trimmed.substr(0, i) + " ";
}

// 指令表：名字 / 分组（Tab 浮层里按分组显示）/ 灰色的用法说明
struct CommandSpec {
    const char* name;
    const char* group;
    const char* hint;
};
const CommandSpec kCommandSpecs[] = {
    {"ban", "玩家管理", "<昵称> [时长]"},
    {"kick", "玩家管理", "<昵称>"},
    {"unban", "玩家管理", "<昵称>"},
    {"op", "管理员", "<昵称>"},
    {"deop", "管理员", "<昵称>"},
    {"ops", "管理员", "查看管理员名单"},
    {"say", "管理员", "<公告内容>"},
    {"changepassword", "账号", "<新密码>"},
    {"bans", "其它", "查看黑名单"},
    {"help", "其它", "显示指令帮助"},
};

const std::vector<std::string>& AllCommandNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const CommandSpec& spec : kCommandSpecs) out.push_back(spec.name);
        return out;
    }();
    return names;
}

namespace {
bool StartsWithIgnoreCase(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) return false;
    return ToLowerAscii(text).compare(0, prefix.size(), ToLowerAscii(prefix)) == 0;
}

// 第一条参数是昵称的指令（Tab 会按在线名单补全）
bool CommandTakesNickName(const std::string& name) {
    return name == "ban" || name == "kick" || name == "unban" || name == "op" ||
           name == "deop";
}
}  // namespace

CompletionResult TabCompleter::Next(const std::string& text,
                                    const std::vector<std::string>& nickNames) {
    const bool newRound = (text != lastOutput_);  // 第一次按，或者用户自己改过内容
    if (newRound) {
        start_ = text;
        index_ = -1;
    }
    // 候选是按"本轮开始时的文本"算的，这样连续按 Tab 能在同一批候选里循环
    CompletionResult result = Suggest(start_, nickNames);
    result.text = text;
    result.firstOfRound = newRound;
    if (result.matches.empty()) {
        lastOutput_ = text;
        return result;
    }
    index_ = (index_ + 1) % static_cast<int>(result.matches.size());
    const std::string& picked = result.matches[static_cast<std::size_t>(index_)];
    const std::string out = result.isArgument ? (result.head + picked) : picked;
    lastOutput_ = out;
    result.text = out;
    result.picked = index_;
    return result;
}

CompletionResult Suggest(const std::string& text, const std::vector<std::string>& nickNames) {
    CompletionResult result;
    result.text = text;

    // 要补的词 = 最后一个词（用户在末尾打字）；head 是它前面的内容
    std::size_t wordBegin = text.size();
    while (wordBegin > 0 && text[wordBegin - 1] != ' ' && text[wordBegin - 1] != '\t') {
        --wordBegin;
    }
    const std::string word = text.substr(wordBegin);
    const std::string head = text.substr(0, wordBegin);
    result.head = head;

    if (wordBegin == 0) {
        // 在打指令名：必须以 '/' 开头
        if (word.empty() || word[0] != '/') return result;
        const std::string prefix = ToLowerAscii(word.substr(1));
        for (const CommandSpec& spec : kCommandSpecs) {
            const std::string name = spec.name;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            result.matches.push_back("/" + name);
            result.hints.push_back(spec.hint);
            result.groups.push_back(spec.group);
        }
        return result;
    }
    // 在打参数：只给"第一个参数是昵称"的指令补，而且只补第一个参数
    const std::vector<std::string> headWords = SplitWords(head);
    if (headWords.size() != 1 || headWords[0].empty() || headWords[0][0] != '/') return result;
    if (!CommandTakesNickName(ToLowerAscii(headWords[0].substr(1)))) return result;
    result.isArgument = true;
    for (const std::string& nick : nickNames) {
        if (!StartsWithIgnoreCase(nick, word)) continue;
        result.matches.push_back(nick);
        result.hints.push_back("");
        result.groups.push_back("在线成员");
    }
    return result;
}

std::string ServerCommandHelp() {
    return "可用指令（控制台直接输入；管理员也可以在聊天框里用）：\n"
           "  /ban <昵称> [时长]   加入黑名单并踢下线；不给时长 = 永久封禁，到时自动解封\n"
           "                       时长单位 w/d/h/m/s（周/天/小时/分/秒），可组合，例如 1h30m、2d、1w\n"
           "  /kick <昵称>         暂时踢出房间（不封禁，可立刻重新加入）\n"
           "  /unban <昵称>        把昵称移出黑名单\n"
           "  /op <昵称>           授予管理员权限（之后可在聊天框用 / 开头的指令）\n"
           "  /deop <昵称>         取消管理员权限\n"
           "  /say <文本>          发一条全服公告（客户端会大字居中、特殊颜色显示）\n"
           "  /bans                查看当前黑名单\n"
           "  /ops                 查看当前管理员名单\n"
           "  /changepassword <新密码>        改自己的密码（聊天框和控制台都能用）\n"
           "  /changepassword <昵称> <新密码>  改别人的密码（只能在这个控制台用）\n"
           "  /help                显示这份帮助";
}

ServerCommand ParseServerCommand(const std::string& line) {
    ServerCommand command;
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) return command;
    const std::vector<std::string> words = SplitWords(trimmed);
    if (words.empty()) return command;

    const std::string keyword = CommandKeyword(words[0]);
    command.knownCommand = !keyword.empty() && IsKnownCommand(keyword);
    command.keyword = keyword;
    auto nameOf = [&](std::size_t index) -> std::string {
        return index < words.size() ? words[index] : std::string();
    };

    if (!keyword.empty() && !IsKnownCommand(keyword)) {
        command.error = "未知指令：" + words[0] + "（输入 /help 查看用法）";
        return command;
    }
    if (keyword.empty()) {  // 控制台里允许不带 '/' 直接敲指令名
        const std::string bare = ToLowerAscii(words[0]);
        if (!IsKnownCommand(bare)) {
            command.error = "未知指令：" + words[0] + "（输入 /help 查看用法）";
            return command;
        }
        command.knownCommand = true;
        return ParseServerCommand("/" + trimmed);
    }

    if (keyword == "help") {
        command.kind = ServerCommand::Kind::Help;
        return command;
    }
    if (keyword == "bans" || keyword == "banlist") {
        command.kind = ServerCommand::Kind::ListBans;
        return command;
    }
    if (keyword == "ops" || keyword == "oplist") {
        command.kind = ServerCommand::Kind::ListOps;
        return command;
    }
    if (keyword == "unban" || keyword == "op" || keyword == "deop" || keyword == "kick") {
        command.name = nameOf(1);
        if (command.name.empty()) {
            command.error = "用法：/" + keyword + " <昵称>";
            return command;
        }
        if (keyword == "unban") command.kind = ServerCommand::Kind::Unban;
        if (keyword == "op") command.kind = ServerCommand::Kind::Op;
        if (keyword == "deop") command.kind = ServerCommand::Kind::Deop;
        if (keyword == "kick") command.kind = ServerCommand::Kind::Kick;
        return command;
    }
    if (keyword == "say") {
        // 公告内容取命令名之后的所有字符（保留中间的空格）
        std::size_t begin = trimmed.find(words[0]);
        begin = begin == std::string::npos ? std::string::npos : begin + words[0].size();
        std::string text =
            begin == std::string::npos ? std::string() : Trim(trimmed.substr(begin));
        if (text.empty()) {
            command.error = "用法：/say <公告内容>";
            return command;
        }
        command.kind = ServerCommand::Kind::Say;
        command.text = text;
        return command;
    }
    if (keyword == "ban") {
        command.name = nameOf(1);
        if (command.name.empty()) {
            command.error = "用法：/ban <昵称> [时长]，例如 /ban alice 1h30m；不给时长就是永久封禁";
            return command;
        }
        const std::string duration = nameOf(2);
        if (duration.empty()) {  // 不给时长 = 永久
            command.kind = ServerCommand::Kind::Ban;
            command.permanent = true;
            command.seconds = 0;
            return command;
        }
        if (words.size() > 3) {
            command.error = "时长里不要有空格，例如用 1h30m 表示 1 小时 30 分；要永久封禁就不写时长";
            return command;
        }
        if (!ParseDuration(duration, &command.seconds)) {
            command.error = "时长无法识别：" + duration + "（支持 w/d/h/m/s，可组合如 1h30m）";
            return command;
        }
        command.kind = ServerCommand::Kind::Ban;
        return command;
    }
    if (keyword == "changepassword") {
        // 一个参数 = 改自己的密码（自助，聊天框里也能用）
        // 两个参数 = 改别人的密码（只能服务器控制台）
        // 先声明"不需要管理员权限"，这样参数写错时看到的是真实原因，而不是"你没有管理员权限"
        command.opRequired = false;
        if (words.size() < 2) {
            command.error =
                "用法：/changepassword <新密码>（改自己的密码）；"
                "在服务器控制台还可以用 /changepassword <昵称> <新密码> 改别人的";
            return command;
        }
        if (words.size() > 3) {
            command.error = "参数太多了：密码里不能有空格，用法是 /changepassword <新密码> "
                            "或 /changepassword <昵称> <新密码>";
            return command;
        }
        if (words.size() == 3) {
            command.name = words[1];
            command.password = words[2];
            command.consoleOnly = true;  // 给别的账号改密码只能在服务器控制台做
        } else {
            command.password = words[1];
        }
        PasswordError passwordError = PasswordError::None;
        if (!ValidatePassword(command.password, &passwordError)) {
            command.error = PasswordErrorText(passwordError);
            return command;
        }
        command.kind = ServerCommand::Kind::ChangePassword;
        return command;
    }

    command.error = "无法识别的指令：" + words[0];
    return command;
}

}  // namespace dchat
