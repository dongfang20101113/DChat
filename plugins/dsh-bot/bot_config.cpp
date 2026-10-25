#include "bot_config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dchat {
namespace dshbot {

namespace {

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lower(const std::string& text) {
    std::string out = text;
    for (char& ch : out) ch = LowerAscii(ch);
    return out;
}

/** 按逗号切分并去掉空白 / 空项。昵称里不可能有逗号（协议本身就用空格分词）。 */
std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string item =
            Trim((comma == std::string::npos) ? text.substr(start) : text.substr(start, comma - start));
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

/**
 * 把 `\n` / `\t` 两个转义还原。
 *
 * 为什么配置值需要这个：promptTemplate 是多行的一段话，写在一行里可读性极差，
 * 而"一行一条 键 = 值"的格式又不允许真换行。只认这两个转义，别的反斜杠原样保留
 * （Windows 路径里的 `C:\dsh` 不能被吃掉一个 D）。
 */
std::string UnescapeValue(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char next = text[i + 1];
        if (next == 'n') {
            out.push_back('\n');
            ++i;
        } else if (next == 't') {
            out.push_back('\t');
            ++i;
        } else if (next == '\\') {
            out.push_back('\\');
            ++i;
        } else {
            out.push_back(text[i]);  // 不认识的转义原样保留
        }
    }
    return out;
}

bool ParseInt(const std::string& raw, int minValue, int maxValue, int* out) {
    if (raw.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(raw.c_str(), &end, 10);
    if (errno != 0 || end == raw.c_str() || *end != '\0') return false;
    if (value < minValue || value > maxValue) return false;
    *out = static_cast<int>(value);
    return true;
}

bool ParseBool(const std::string& raw, bool* out) {
    const std::string lower = Lower(raw);
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        *out = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        *out = false;
        return true;
    }
    return false;
}

/** 命令模板按空白分词，处理单引号和双引号（不支持引号里的转义，够用）。 */
bool SplitCommand(const std::string& text, std::vector<std::string>* out) {
    std::vector<std::string> words;
    std::string current;
    bool inWord = false;
    char quote = '\0';
    for (char ch : text) {
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
                inWord = true;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            inWord = true;  // "" 也算一个空参数
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            if (inWord) {
                words.push_back(current);
                current.clear();
                inWord = false;
            }
            continue;
        }
        current.push_back(ch);
        inWord = true;
    }
    if (quote != '\0') return false;  // 引号没闭合
    if (inWord) words.push_back(current);
    if (words.empty()) return false;
    for (const std::string& word : words) {
        if (word.empty()) return false;  // 空参数几乎肯定是手滑
    }
    *out = words;
    return true;
}

}  // namespace

bool SameNick(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

bool ParseConfig(const std::string& text, Config* out, std::string* error) {
    Config config;
    std::istringstream stream(text);
    std::string line;
    int lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            if (error != nullptr) {
                *error = "第 " + std::to_string(lineNumber) + " 行没有 '='：" + trimmed;
            }
            return false;
        }
        const std::string key = Lower(Trim(trimmed.substr(0, equals)));
        const std::string value = Trim(trimmed.substr(equals + 1));

        // 每个分支自己做类型转换，转换失败就把行号带出去。
        // 重复写 20 遍 "第几行" 很啰嗦，但错误信息里没有行号的话，
        // 面对着 60 行配置找哪个值写错了是纯浪费。
        auto bad = [&](const std::string& why) {
            if (error != nullptr) {
                *error = "第 " + std::to_string(lineNumber) + " 行 " + key + "：" + why;
            }
            return false;
        };

        if (key == "server.host") {
            if (value.empty()) return bad("主机不能为空");
            config.host = value;
        } else if (key == "server.port") {
            if (!ParseInt(value, 1, 65535, &config.port)) return bad("端口要在 1-65535 之间");
        } else if (key == "server.encrypt") {
            const std::string mode = Lower(value);
            if (mode != "auto" && mode != "off" && mode != "require") {
                return bad("只能是 auto / off / require");
            }
            config.encrypt = mode;
        } else if (key == "server.fingerprint") {
            config.fingerprint = value;
        } else if (key == "bot.nick") {
            config.nick = value;
        } else if (key == "bot.password") {
            config.password = value;
        } else if (key == "bot.register") {
            if (!ParseBool(value, &config.registerFirst)) return bad("只能是 true / false");
        } else if (key == "rooms") {
            config.rooms = SplitList(value);
        } else if (key == "rooms.autojoin") {
            if (!ParseBool(value, &config.autoJoin)) return bad("只能是 true / false");
        } else if (key == "trigger.mentiononly") {
            if (!ParseBool(value, &config.trigger.mentionOnly)) return bad("只能是 true / false");
        } else if (key == "trigger.aliases") {
            config.trigger.aliases = SplitList(value);
        } else if (key == "reply.mode") {
            const std::string mode = Lower(value);
            if (mode == "all") {
                config.reply.mode = ReplyMode::All;
            } else if (mode == "whitelist") {
                config.reply.mode = ReplyMode::Whitelist;
            } else if (mode == "blacklist") {
                config.reply.mode = ReplyMode::Blacklist;
            } else {
                return bad("只能是 all / whitelist / blacklist");
            }
        } else if (key == "reply.list") {
            config.reply.nicks = SplitList(value);
        } else if (key == "reply.acktext") {
            config.ackText = UnescapeValue(value);
        } else if (key == "history.settlems") {
            if (!ParseInt(value, 0, 60000, &config.historySettleMs)) return bad("要在 0-60000 之间");
        } else if (key == "file.maxmb") {
            if (!ParseInt(value, 0, 256, &config.fileMaxMb)) return bad("要在 0-256 之间（0 = 不许发文件）");
        } else if (key == "file.roots") {
            // 用分号分隔：Windows 路径里很少出现 ';'，而逗号在文件名里更常见
            config.fileRoots.clear();
            std::size_t start = 0;
            while (start <= value.size()) {
                const std::size_t semi = value.find(';', start);
                const std::string item = Trim((semi == std::string::npos)
                                                  ? value.substr(start)
                                                  : value.substr(start, semi - start));
                if (!item.empty()) config.fileRoots.push_back(item);
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        } else if (key == "typing.maxchars") {
            if (!ParseInt(value, 8, 4000, &config.typing.maxChars)) return bad("要在 8-4000 之间");
        } else if (key == "typing.minchars") {
            if (!ParseInt(value, 0, 4000, &config.typing.minChars)) return bad("要在 0-4000 之间");
        } else if (key == "typing.maxchunks") {
            if (!ParseInt(value, 1, 200, &config.typing.maxChunks)) return bad("要在 1-200 之间");
        } else if (key == "typing.mingapms") {
            if (!ParseInt(value, 0, 60000, &config.typing.minGapMs)) return bad("要在 0-60000 之间");
        } else if (key == "typing.percharms") {
            if (!ParseInt(value, 0, 5000, &config.typing.perCharMs)) return bad("要在 0-5000 之间");
        } else if (key == "typing.maxgapms") {
            if (!ParseInt(value, 0, 60000, &config.typing.maxGapMs)) return bad("要在 0-60000 之间");
        } else if (key == "typing.stripmarkdown") {
            if (!ParseBool(value, &config.typing.stripMarkdown)) return bad("只能是 true / false");
        } else if (key == "brain.command") {
            if (!SplitCommand(value, &config.brain.command)) return bad("命令模板不合法（引号未闭合？）");
        } else if (key == "brain.cwd") {
            config.brain.cwd = value;
        } else if (key == "brain.permissionmode") {
            config.brain.permissionMode = value;
        } else if (key == "brain.patch") {
            config.brain.patch = value;
        } else if (key == "brain.timeoutms") {
            if (!ParseInt(value, 1000, 3600000, &config.brain.timeoutMs)) {
                return bad("要在 1000-3600000 之间");
            }
        } else if (key == "brain.sessionperroom") {
            if (!ParseBool(value, &config.brain.sessionPerRoom)) return bad("只能是 true / false");
        } else if (key == "brain.maxconcurrent") {
            if (!ParseInt(value, 1, 16, &config.brain.maxConcurrent)) return bad("要在 1-16 之间");
        } else if (key == "brain.maxqueue") {
            if (!ParseInt(value, 0, 1000, &config.brain.maxQueue)) return bad("要在 0-1000 之间");
        } else if (key == "brain.prompttemplate") {
            config.brain.promptTemplate = UnescapeValue(value);
        } else if (key == "log.path") {
            config.logPath = value;
        } else if (key == "log.verbose") {
            if (!ParseBool(value, &config.verbose)) return bad("只能是 true / false");
        } else if (key == "state.sessionfile") {
            config.sessionFile = value;
        } else if (key == "reconnect.minms") {
            if (!ParseInt(value, 100, 600000, &config.reconnectMinMs)) return bad("要在 100-600000 之间");
        } else if (key == "ping.intervalms") {
            if (!ParseInt(value, 1000, 3600000, &config.pingIntervalMs)) return bad("要在 1000-3600000 之间");
        } else if (key == "ping.timeoutms") {
            if (!ParseInt(value, 1000, 3600000, &config.pingTimeoutMs)) return bad("要在 1000-3600000 之间");
        } else if (key == "reconnect.maxms") {
            if (!ParseInt(value, 100, 3600000, &config.reconnectMaxMs)) return bad("要在 100-3600000 之间");
        } else if (key == "ui.port") {
            if (!ParseInt(value, 0, 65535, &config.uiPort)) return bad("要在 0-65535 之间（0 = 不开管理界面）");
        } else {
            // 不认识的键直接报错：静默忽略会让"我明明配了黑名单却不生效"变成悬案。
            return bad("未知的配置项（是不是拼错了？）");
        }
    }

    if (!ValidateConfig(&config, error)) return false;
    *out = config;
    return true;
}

bool ValidateConfig(Config* config, std::string* error) {
    if (config == nullptr) return false;
    const Config& configRef = *config;
    if (configRef.nick.empty()) {
        if (error != nullptr) *error = "bot.nick 不能为空（机器人要用一个账号登录）";
        return false;
    }
    if (configRef.password.empty()) {
        if (error != nullptr) *error = "bot.password 不能为空";
        return false;
    }
    if (configRef.rooms.empty()) {
        if (error != nullptr) {
            *error = "rooms 不能为空：至少要写一个会话 ID（大厅写 hall）";
        }
        return false;
    }
    if (configRef.typing.minChars > configRef.typing.maxChars) {
        if (error != nullptr) *error = "typing.minchars 不能大于 typing.maxchars";
        return false;
    }
    if (configRef.typing.maxGapMs < configRef.typing.minGapMs) {
        if (error != nullptr) *error = "typing.maxgapms 不能小于 typing.mingapms";
        return false;
    }
    if (configRef.brain.command.empty()) {
        if (error != nullptr) *error = "brain.command 不能为空";
        return false;
    }
    if (configRef.brain.promptTemplate.empty()) {
        if (error != nullptr) *error = "brain.prompttemplate 不能为空（那等于让模型猜要干什么）";
        return false;
    }
    if (configRef.reply.mode != ReplyMode::All && configRef.reply.nicks.empty()) {
        if (error != nullptr) *error = "reply.mode 是 whitelist/blacklist 时 reply.list 不能为空";
        return false;
    }
    return true;
}

std::string EscapeConfigValue(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;  // 配置是行式的，回车没有意义
            case '\t': out += "\\t"; break;
            default: out.push_back(ch);
        }
    }
    return out;
}

/**
 * 把几个键的新值写回配置文本，保留注释和别的行。**不做转义** —— 见头文件里的说明：
 * 只有"允许换行的文本字段"才需要调用方先用 EscapeConfigValue 处理，
 * 路径这类值必须原样写，否则 `D:\new` 里的 `\n` 会被当成换行读回来。
 */
std::string UpdateConfigText(const std::string& text,
                             const std::vector<std::pair<std::string, std::string>>& changes) {
    // 逐行扫：认识的行就地改，不认识的原样保留（注释、空行、以后新增的键）。
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') current.pop_back();
        lines.push_back(current);
    }

    std::vector<bool> applied(changes.size(), false);
    for (std::string& line : lines) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Lower(Trim(trimmed.substr(0, equals)));
        for (std::size_t i = 0; i < changes.size(); ++i) {
            if (Lower(changes[i].first) == key) {
                line = changes[i].first + " = " + changes[i].second;
                applied[i] = true;
                break;
            }
        }
    }

    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (applied[i]) continue;
        out += changes[i].first + " = " + changes[i].second + "\n";
    }
    return out;
}

bool LoadConfigFile(const std::string& path, Config* out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) *error = "打不开配置文件：" + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    // 记事本和 PowerShell 的 `-Encoding UTF8` 都会在文件开头塞一个 BOM。
    // 不剥掉的话第一行会变成 "\xEF\xBB\xBFserver.host"，报"未知的配置项"——
    // 而用户看到的明明是 server.host，极难联想到 BOM。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    std::string parseError;
    if (!ParseConfig(text, out, &parseError)) {
        if (error != nullptr) *error = path + "：" + parseError;
        return false;
    }
    return true;
}

std::string ExampleConfig() {
    return
        "# dchat DeepSeek Harness 机器人（dchat-dsh-bot）配置\n"
        "#\n"
        "# 一行一条 `键 = 值`，# 开头是注释。值里可以写 \\n 表示换行。\n"
        "# 这份文件里有密码：chmod 600，别提交进 git。\n"
        "\n"
        "# ---- 连哪个服务器 ----\n"
        "server.host = 127.0.0.1\n"
        "server.port = 5555\n"
        "# auto = 能加密就加密（推荐）；off = 明文；require = 必须加密，握手失败就不连\n"
        "server.encrypt = auto\n"
        "# 钉住服务器公钥指纹（首次连接时日志里会打出来）。留空 = 只记日志不校验。\n"
        "server.fingerprint =\n"
        "\n"
        "# ---- 机器人账号 ----\n"
        "bot.nick = 小d\n"
        "bot.password = 换成你自己的密码\n"
        "# 账号不存在时自动注册一次（第一次启动方便；之后建议改回 false）\n"
        "bot.register = true\n"
        "\n"
        "# ---- 接入哪些会话（只回这里列的；没列的群一律不理）----\n"
        "# hall 表示大厅。多个用逗号分隔。\n"
        "rooms = hall\n"
        "# 不在群里时自动申请加入（需要审核的群会排队等群主同意）\n"
        "rooms.autojoin = true\n"
        "\n"
        "# ---- 什么情况下回话 ----\n"
        "# true = 只有被 @ 才回；false = 房间里每句话都回（调试用）\n"
        "trigger.mentiononly = true\n"
        "# 额外别名（不带 @）。主名就是 bot.nick。\n"
        "trigger.aliases =\n"
        "\n"
        "# ---- 谁能让它开口 ----\n"
        "# all = 谁都回；whitelist = 只回名单里的人；blacklist = 名单里的人不回\n"
        "reply.mode = all\n"
        "reply.list =\n"
        "# 被 @ 之后先说一句（空 = 不说）。起 DSH 要几秒，没反馈像卡死了。\n"
        "reply.acktext = （正在想…）\n"
        "# 登录后多久内收到的聊天消息当成历史回放丢掉（毫秒）。线上开着\n"
        "# keepchathistory / offlinemessages 时会重放历史，不丢的话机器人会\n"
        "# 对着几小时前 @ 过它的老消息逐条回答。慢网络可以调大。\n"
        "# 登录时服务端会重放历史（keepchathistory / offlinemessages），\n"
        "# 那一段不当作实时消息：连续这么多毫秒没有新行就算回放结束。\n"
        "history.settlems = 300\n"
        "# 允许机器人发给群里的单个文件上限（MB），0 = 不许发文件\n"
        "file.maxmb = 32\n"
        "# 额外允许**发送**（只读）的目录，分号分隔。和 brain.cwd 的区别：\n"
        "# brain.cwd 里它能改文件，这里只是能读出来发出去。\n"
        "file.roots =\n"
        "\n"
        "# ---- 回答怎么分条发 ----\n"
        "typing.maxchars = 45\n"
        "typing.minchars = 10\n"
        "typing.maxchunks = 24\n"
        "typing.mingapms = 350\n"
        "typing.percharms = 55\n"
        "typing.maxgapms = 1500\n"
        "typing.stripmarkdown = true\n"
        "\n"
        "# ---- 大脑：DeepSeek Harness ----\n"
        "# {prompt} / {session} 会被替换；默认从 stdin 喂提示词（最安全，不怕引号）。\n"
        "# {session} 展开成 --session-id <id>，用来让同一个群记住上文。\n"
        "brain.command = dsh headless --json {session} -\n"
        "# 子进程工作目录（留空 = 当前目录）\n"
        "brain.cwd =\n"
        "# read-only / workspace-write / danger-full-access\n"
        "# 群里任何人都能 @ 它，**别用 danger-full-access**\n"
        "brain.permissionmode = read-only\n"
        "# 关掉工具插件的补丁文件（推荐：插件目录下的 dsh-chat-only.yml）\n"
        "brain.patch =\n"
        "brain.timeoutms = 120000\n"
        "brain.sessionperroom = true\n"
        "brain.maxconcurrent = 1\n"
        "brain.maxqueue = 8\n"
        "brain.prompttemplate = 你在 dchat 的群聊「{room}」里被 {nick} @ 了。\\n你能改动的目录是 {workdir}（读写、改名、建目录、跑命令都在这里）。\\n另外这些目录里的文件可以直接发给对方（只读）：{fileroots}\\n想发任何文件就在回答最后单独写一行：[[file: 完整路径或相对路径]]（机器人会自己读文件上传，不需要你先复制）\\n用简体中文、口语化、简短地回复；聊天消息不要用 Markdown 标题/表格/代码块。命令、路径、报错原文、工具名照抄，不要翻译；对方用英文问就用英文答。\\n{nick} 说：{text}\n"
        "\n"
        "# ---- 日志与状态 ----\n"
        "log.path = dchat-dsh-bot.log\n"
        "log.verbose = false\n"
        "state.sessionfile = dchat-dsh-bot-sessions.txt\n"
        "\n"
        "# ---- 本机管理界面 ----\n"
        "# 只监听 127.0.0.1。浏览器打开 http://127.0.0.1:8731 就能改\n"
        "# 「接入哪些群 / 白黑名单 / 触发方式 / 分条节奏」，保存后立刻生效。\n"
        "# 0 = 不开这个界面。\n"
        "ui.port = 8731\n"
        "\n"
        "# ---- 断线重连 ----\n"
        "reconnect.minms = 3000\n"
        "reconnect.maxms = 60000\n"
        "\n"
        "# ---- 心跳 ----\n"
        "# 空闲连接可能被 NAT/代理悄悄掐掉而两端都不报错，靠 PING/PONG 探活。\n"
        "ping.intervalms = 30000\n"
        "ping.timeoutms = 90000\n";
}

}  // namespace dshbot
}  // namespace dchat
