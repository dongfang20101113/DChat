// 服务端指令解析测试：/ban（含永久）、/kick、/unban、/op、/deop、时长写法、
// 以及"哪些聊天内容算指令"的判定规则。
#include <cstdio>
#include <string>

#include "server_command.h"
#include "server_rules.h"  // 规则名列表（/chatrule 的 Tab 补全用）

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

using Kind = dchat::ServerCommand::Kind;
bool Is(Kind actual, Kind expected) { return actual == expected; }
}  // namespace

int main() {
    std::printf("== dchat server command tests ==\n");

    {
        std::printf("[1] /ban：带时间与不带时间（永久）\n");
        const auto timed = dchat::ParseServerCommand("/ban alice 1h");
        check(Is(timed.kind, Kind::Ban) && timed.name == "alice" && timed.seconds == 3600 &&
                  !timed.permanent,
              "/ban alice 1h 解析正确");

        const auto forever = dchat::ParseServerCommand("/ban bob");
        check(Is(forever.kind, Kind::Ban) && forever.name == "bob" && forever.permanent &&
                  forever.seconds == 0,
              "不给时长就是永久封禁");
        check(forever.error.empty(), "永久封禁不报错");

        const auto noName = dchat::ParseServerCommand("/ban");
        check(!noName.error.empty() && Is(noName.kind, Kind::Unknown), "/ban 缺昵称给出提示");
        const auto spaced = dchat::ParseServerCommand("/ban alice 1h 30m");
        check(!spaced.error.empty(), "时长里带空格时提示写法");
        const auto bad = dchat::ParseServerCommand("/ban alice 10x");
        check(!bad.error.empty(), "无法识别的单位会报错");
    }

    {
        std::printf("[2] /op 与 /deop\n");
        const auto op = dchat::ParseServerCommand("/op alice");
        check(Is(op.kind, Kind::Op) && op.name == "alice", "/op 解析正确");
        const auto opNoName = dchat::ParseServerCommand("/op");
        check(!opNoName.error.empty(), "/op 缺昵称给出提示");
        const auto deop = dchat::ParseServerCommand("/deop 小雷");
        check(Is(deop.kind, Kind::Deop) && deop.name == "小雷", "/deop 支持中文昵称");
        const auto deopNoName = dchat::ParseServerCommand("/deop");
        check(!deopNoName.error.empty(), "/deop 缺昵称给出提示");
    }

    {
        std::printf("[3] /kick 与 /unban\n");
        check(Is(dchat::ParseServerCommand("/kick bob").kind, Kind::Kick), "/kick 解析正确");
        check(!dchat::ParseServerCommand("/kick").error.empty(), "/kick 缺昵称给出提示");
        check(Is(dchat::ParseServerCommand("/unban bob").kind, Kind::Unban), "/unban 解析正确");
        check(!dchat::ParseServerCommand("/unban").error.empty(), "/unban 缺昵称给出提示");
    }

    {
        std::printf("[3.5] /say 全服公告\n");
        const auto say = dchat::ParseServerCommand("/say 服务器将在 10 分钟后维护");
        check(Is(say.kind, Kind::Say), "/say 解析正确");
        check(say.text == "服务器将在 10 分钟后维护", "公告内容完整（含空格）");
        const auto spaced = dchat::ParseServerCommand("/say   大家好   我是管理员   ");
        check(spaced.text == "大家好   我是管理员", "公告内部空格保留，首尾空格去掉");
        const auto empty = dchat::ParseServerCommand("/say");
        check(!empty.error.empty() && Is(empty.kind, Kind::Unknown), "/say 没内容时给出用法提示");
        check(dchat::IsCommandAttempt("/say 你好"), "/say 也算命令");
        check(dchat::IsCommandAttempt("/SAY 你好"), "/say 大小写不敏感");
        check(!dchat::IsCommandAttempt("公告：/say 你好"), "'/' 不在首位 → 普通消息");
    }

    {
        std::printf("[4] 哪些内容会被当成指令\n");
        check(dchat::IsCommandAttempt("/ban alice 1h"), "以 / 开头 → 命令");
        check(dchat::IsCommandAttempt("/BAN alice"), "大小写不敏感");
        check(dchat::IsCommandAttempt("/op alice"), "/op 也算命令");
        check(dchat::IsCommandAttempt("/bann alice 1h"), "指令名打错（/bann）也算命令尝试，不会当成聊天发出去");
        check(dchat::IsCommandAttempt("/banana"), "任何 / 开头的内容都算命令尝试");
        check(!dchat::IsCommandAttempt("ban alice 1h"), "没有 / 开头 → 普通消息");
        check(!dchat::IsCommandAttempt("你好，/ban 是什么意思？"), "/ 不在首位 → 普通消息");
        check(!dchat::IsCommandAttempt(""), "空消息不是命令");

        // 打错的指令名：能解析出"未知指令"的提示，且标记为不可识别
        const auto typo = dchat::ParseServerCommand("/bann alice 1h");
        check(!typo.knownCommand && !typo.error.empty(), "打错的指令名给出未知指令提示");
        const auto badArgs = dchat::ParseServerCommand("/ban");
        check(badArgs.knownCommand && !badArgs.error.empty(), "指令名对但参数不对：算已知指令，给出用法提示");
    }

    {
        std::printf("[5] 时长写法\n");
        struct Case {
            const char* text;
            int seconds;
        };
        const Case cases[] = {{"30s", 30},     {"10m", 600},       {"2h", 7200},
                              {"1d", 86400},   {"1w", 604800},     {"1h30m", 5400},
                              {"1d12h", 129600}, {"2w3d", 1468800}, {"90", 90},
                              {"1h30", 3630},  {"1W", 604800},     {"10M", 600}};
        for (const Case& item : cases) {
            int seconds = 0;
            const bool ok = dchat::ParseDuration(item.text, &seconds);
            char label[128] = {0};
            std::snprintf(label, sizeof(label), "%s 解析为 %d 秒", item.text, item.seconds);
            check(ok && seconds == item.seconds, label);
        }
        for (const char* text : {"", "abc", "1x", "0s", "-5s", "s10", "1h30x", "12.5m", "200y"}) {
            int seconds = 0;
            char label[128] = {0};
            std::snprintf(label, sizeof(label), "拒绝非法时长 %s", text[0] ? text : "(空)");
            check(!dchat::ParseDuration(text, &seconds), label);
        }
    }

    {
        std::printf("[6] 时长与封禁显示\n");
        check(dchat::FormatBanDuration(5400, false) == "1 小时 30 分", "5400 秒");
        check(dchat::FormatBanDuration(0, true) == "永久", "永久封禁显示为「永久」");
        check(dchat::FormatBanDuration(45, false) == "45 秒", "45 秒");
        check(dchat::FormatBanDuration(604800, false) == "1 周", "1 周");
    }

    {
        std::printf("[7] 其它指令与容错\n");
        check(Is(dchat::ParseServerCommand("/help").kind, Kind::Help), "/help");
        check(Is(dchat::ParseServerCommand("/bans").kind, Kind::ListBans), "/bans");
        check(Is(dchat::ParseServerCommand("/ops").kind, Kind::ListOps), "/ops 列出管理员名单");
        check(Is(dchat::ParseServerCommand("/oplist").kind, Kind::ListOps), "/oplist 是 /ops 的别名");
        check(Is(dchat::ParseServerCommand("ops").kind, Kind::ListOps), "控制台里不带 / 也能用");
        check(Is(dchat::ParseServerCommand("bans").kind, Kind::ListBans), "控制台里不带 / 也能用");
        const auto unknown = dchat::ParseServerCommand("/foo bar");
        check(!unknown.error.empty(), "未知指令给出提示");
        const auto upper = dchat::ParseServerCommand("/BAN Alice 10M");
        check(Is(upper.kind, Kind::Ban) && upper.name == "Alice" && upper.seconds == 600,
              "指令大小写不敏感，昵称保持原样");
        check(Is(dchat::ParseServerCommand("").kind, Kind::Unknown), "空行不产生指令");
        const std::string help = dchat::ServerCommandHelp();
        check(help.find("/op") != std::string::npos && help.find("/deop") != std::string::npos,
              "帮助里包含 /op 与 /deop");
        check(help.find("/ops") != std::string::npos, "帮助里包含 /ops");
        check(help.find("永久") != std::string::npos, "帮助里说明了永久封禁");
        check(help.find("w/d/h/m/s") != std::string::npos, "帮助里说明了时长单位");
        // 有些工具往标准输入写第一行会带 UTF-8 BOM
        const auto bom = dchat::ParseServerCommand("\xEF\xBB\xBF/ban alice 2h");
        check(Is(bom.kind, Kind::Ban) && bom.name == "alice" && bom.seconds == 7200,
              "开头的 UTF-8 BOM 不影响解析");
    }

    {
        std::printf("[8] /changepassword\n");
        // 一个参数 = 改自己的密码：自助操作，不需要管理员，聊天框里也能用
        const auto self = dchat::ParseServerCommand("/changepassword newpass123");
        check(Is(self.kind, Kind::ChangePassword) && self.name.empty() &&
                  self.password == "newpass123",
              "一个参数 = 改自己的密码");
        check(!self.consoleOnly, "改自己的密码不是控制台专用");
        check(!self.opRequired, "改自己的密码不需要管理员权限");

        // 两个参数 = 改别人的密码：只能服务器控制台
        const auto other = dchat::ParseServerCommand("/changepassword alice newpass123");
        check(Is(other.kind, Kind::ChangePassword) && other.name == "alice" &&
                  other.password == "newpass123",
              "两个参数 = 给指定账号改密码");
        check(other.consoleOnly, "给别的账号改密码标记为仅控制台可用");
        check(!other.opRequired, "这条走的是控制台路径，不需要聊天框管理员权限");

        check(!dchat::ParseServerCommand("/changepassword").error.empty(), "不给参数时提示用法");
        check(!dchat::ParseServerCommand("/changepassword alice newpass123 extra").error.empty(),
              "参数太多时提示用法（密码里不能有空格）");
        const auto tooShort = dchat::ParseServerCommand("/changepassword alice ab");
        check(!tooShort.error.empty() && tooShort.error.find("至少") != std::string::npos,
              "密码太短时给出密码规则，而不是权限问题");
        check(!tooShort.opRequired, "参数写错时也按自助指令处理，先报真实原因");
        const auto manyWords = dchat::ParseServerCommand("/changepassword alice ab cd");
        check(!manyWords.error.empty() && !manyWords.opRequired,
              "参数太多时也按自助指令处理");
        check(!dchat::ParseServerCommand("/changepassword alice 123456 789").error.empty(),
              "密码里带空格时被拒绝");
        const auto upper = dchat::ParseServerCommand("/ChangePassword alice NewPass123");
        check(Is(upper.kind, Kind::ChangePassword) && upper.name == "alice" &&
                  upper.password == "NewPass123",
              "指令大小写不敏感，密码保持原样");
        check(dchat::ParseServerCommand("/changepassword alice newpass123").keyword ==
                  "changepassword",
              "keyword 用于日志（不会记录密码）");
        check(Is(dchat::ParseServerCommand("changepassword alice newpass123").kind,
                 Kind::ChangePassword),
              "控制台里不带 / 也能用");

        const std::string help = dchat::ServerCommandHelp();
        check(help.find("/changepassword") != std::string::npos, "帮助里包含 /changepassword");

        // 客户端用这个判断来决定"这一行不要记进输入历史"，所以参数写错也要认出来
        check(dchat::LooksLikePasswordCommand("/changepassword newpass123"),
              "识别出改自己的密码");
        check(dchat::LooksLikePasswordCommand("/ChangePassword alice newpass123"),
              "大小写不敏感");
        check(dchat::LooksLikePasswordCommand("  /changepassword ab"),
              "参数不合规则也照样识别（免得把密码记进历史）");
        check(dchat::LooksLikePasswordCommand("/changepassword"),
              "只写了指令名也识别");
        check(!dchat::LooksLikePasswordCommand("changepassword newpass123"),
              "没有 '/' 是普通消息");
        check(!dchat::LooksLikePasswordCommand("/changepasswordx ab"),
              "指令名多了字符就不算（比如 changepasswordx）");
        check(!dchat::LooksLikePasswordCommand("你好 /changepassword x"),
              "'/' 不在首位是普通消息");
        check(!dchat::LooksLikePasswordCommand("/help"), "别的指令不算");

        // 按 ↑ 翻历史时该记什么：聊天和指令都记，只有改密码要把密码去掉
        check(dchat::HistoryTextFor("你好啊") == "你好啊", "普通聊天原样记进历史");
        check(dchat::HistoryTextFor("/help") == "/help", "指令原样记进历史（能按 ↑ 翻回来）");
        check(dchat::HistoryTextFor("/ban alice 1h") == "/ban alice 1h", "带参数的指令也原样记");
        check(dchat::HistoryTextFor("/changepassword mypass123") == "/changepassword ",
              "改密码只记指令名，密码不进历史");
        check(dchat::HistoryTextFor("/changepassword alice mypass123") == "/changepassword ",
              "给别的账号改密码也只记指令名");
        check(dchat::HistoryTextFor("  /ChangePassword mypass123  ") == "/ChangePassword ",
              "大小写不敏感，长度也去掉");
        check(dchat::HistoryTextFor("/changepassword").find("mypass") == std::string::npos,
              "只写指令名时也不会凭空带出密码");
    }

    {
        std::printf("[9] Tab 指令补全\n");
        dchat::TabCompleter completer;
        const std::vector<std::string> noNicks;
        check(completer.Next("/", noNicks).text == "/ban", "输入 / 后第一次 Tab 补到第一个指令");
        check(completer.Next("/ban", noNicks).text == "/kick", "继续按 Tab 循环到下一个");
        check(completer.Next("/kick", noNicks).text == "/unban", "继续循环（按分组顺序）");
        const dchat::CompletionResult listed = dchat::Suggest("/", noNicks);
        check(listed.hints.size() == listed.matches.size() &&
                  listed.groups.size() == listed.matches.size(),
              "每个候选都有对应的用法说明和分组（面板要用）");
        check(listed.matches[0] == "/ban" && listed.hints[0] == "<昵称> [时长]" &&
                  listed.groups[0] == "玩家管理",
              "/ban 带 <昵称> [时长] 说明、归到「玩家管理」");
        check(dchat::Suggest("/say", noNicks).hints[0] == "<公告内容>", "/say 的说明是 <公告内容>");
        std::vector<std::string> groupOrder;
        bool contiguous = true;
        for (const std::string& group : listed.groups) {
            if (groupOrder.empty() || groupOrder.back() != group) {
                for (const std::string& old : groupOrder) {
                    if (old == group) contiguous = false;
                }
                groupOrder.push_back(group);
            }
        }
        check(contiguous, "同一分组的候选连续排列（面板据此画分组标题）");
        check(groupOrder.size() >= 3, "指令至少分成 3 个分组");
        const dchat::CompletionResult nickList =
            dchat::Suggest("/kick ", std::vector<std::string>{"alice"});
        check(nickList.groups.size() == 1 && nickList.groups[0] == "在线成员" &&
                  nickList.hints[0].empty(),
              "昵称候选归到「在线成员」，没有用法说明");
        dchat::TabCompleter firstPress;
        const dchat::CompletionResult first = firstPress.Next("/", noNicks);
        check(first.firstOfRound && first.matches.size() == dchat::AllCommandNames().size(),
              "第一次按 Tab 会带回全部候选（界面用它列出候选）");
        check(!first.matches.empty() && first.matches[0] == "/ban", "候选里是带 '/' 的指令名");
        check(!firstPress.Next("/ban", noNicks).firstOfRound, "同一轮里后续按 Tab 不再报候选");
        check(first.picked == 0, "第一次按 Tab 选中第 0 个候选");
        const dchat::CompletionResult restart = completer.Next("/ki", noNicks);
        check(restart.picked == 0 && restart.text == "/kick" && restart.firstOfRound,
              "用户改成别的前缀时重新开始一轮（按新前缀补全）");
        const dchat::CompletionResult live = dchat::Suggest("/", noNicks);
        check(live.matches.size() == dchat::AllCommandNames().size() && live.picked == -1,
              "Suggest 只给候选列表、不选中任何一项（界面用它实时刷新浮层）");
        check(dchat::Suggest("/he", noNicks).matches.size() == 1 &&
                  dchat::Suggest("/he", noNicks).matches[0] == "/help",
              "Suggest 会按前缀过滤");
        check(dchat::Suggest("/kick ", std::vector<std::string>{"alice", "bob"}).head == "/kick ",
              "Suggest 会把'正在补的词之前的内容'带回来（点候选时要用）");
        std::string walk = "/";
        dchat::TabCompleter around;
        for (std::size_t i = 0; i <= dchat::AllCommandNames().size(); ++i) {
            walk = around.Next(walk, noNicks).text;
        }
        check(walk == "/ban", "循环一整圈回到第一个");

        dchat::TabCompleter byPrefix;
        check(byPrefix.Next("/ki", noNicks).text == "/kick", "按前缀补全");
        check(byPrefix.Next("/kick", noNicks).text == "/kick", "只有一个候选时保持不动");
        dchat::TabCompleter upper;
        const std::string chFirst = upper.Next("/CH", noNicks).text;
        check(chFirst == "/chatrule", "/CH 按前缀补全（大小写不敏感）");
        check(upper.Next(chFirst, noNicks).text == "/changepassword",
              "同一前缀有多个候选时继续按 Tab 轮到下一个");
        dchat::TabCompleter help;
        check(help.Next("/he", noNicks).text == "/help", "补全 /help");

        dchat::TabCompleter plain;
        check(plain.Next("你好啊", noNicks).text == "你好啊", "普通聊天不补全");
        check(plain.Next("", noNicks).text == "", "空输入不补全");
        dchat::TabCompleter args;
        check(args.Next("/ban alice 1h", noNicks).text == "/ban alice 1h",
              "已经在打第二个参数时不补全");
        dchat::TabCompleter none;
        check(none.Next("/zzz", noNicks).text == "/zzz", "没有匹配就原样返回");
        dchat::TabCompleter edited;
        check(edited.Next("/", noNicks).text == "/ban", "从头开始");
        check(edited.Next("/x", noNicks).text == "/x", "用户改过内容后重新开始一轮");
        check(edited.Next("/k", noNicks).text == "/kick", "再改一次又能正常补全");
    }

    {
        std::printf("[10] Tab 参数补全（在线昵称）\n");
        const std::vector<std::string> nicks = {"alice", "bob", "小明"};

        dchat::TabCompleter kick;
        dchat::CompletionResult r1 = kick.Next("/kick ", nicks);
        check(r1.isArgument && r1.text == "/kick alice", "打字 /kick + 空格 后 Tab 补第一个在线成员");
        check(r1.matches.size() == 3 && r1.matches[0] == "alice", "候选就是在线名单");
        check(kick.Next("/kick alice", nicks).text == "/kick bob", "继续按 Tab 循环到下一个人");
        check(kick.Next("/kick bob", nicks).text == "/kick 小明", "中文昵称也能补");
        check(kick.Next("/kick 小明", nicks).text == "/kick alice", "循环回第一个");

        dchat::TabCompleter byPrefix2;
        check(byPrefix2.Next("/kick b", nicks).text == "/kick bob", "按前缀补昵称");
        check(byPrefix2.Next("/kick bob", nicks).text == "/kick bob", "只有一个匹配时不动");

        dchat::TabCompleter ban;
        check(ban.Next("/ban ", nicks).text == "/ban alice", "/ban 也能补昵称");
        dchat::TabCompleter op;
        check(op.Next("/op b", nicks).text == "/op bob", "/op 也能补昵称");
        dchat::TabCompleter unban;
        check(unban.Next("/unban ", nicks).text == "/unban alice", "/unban 也能补昵称");

        dchat::TabCompleter say;
        check(say.Next("/say ", nicks).text == "/say ", "/say 不补昵称");
        dchat::TabCompleter own;
        check(own.Next("/changepassword ", nicks).text == "/changepassword ",
              "/changepassword 不补昵称（那条是控制台专用）");
        dchat::TabCompleter second;
        check(second.Next("/ban alice ", nicks).text == "/ban alice ",
              "第二个参数（时长）不补全");
        dchat::TabCompleter plainText;
        check(plainText.Next("kick ", nicks).text == "kick ",
              "没写 '/' 的普通消息不补全");
        dchat::TabCompleter noMatch;
        check(noMatch.Next("/kick zzz", nicks).text == "/kick zzz", "没有匹配的昵称就原样");
        dchat::TabCompleter empty;
        check(empty.Next("/kick ", std::vector<std::string>()).text == "/kick ",
              "没有人在线时不补全");
    }

    {
        std::printf("[11] /cp 简写与 /ip（仅控制台）\n");
        const std::vector<std::string> noNicks;
        const auto cpSelf = dchat::ParseServerCommand("/cp newpass123");
        check(Is(cpSelf.kind, Kind::ChangePassword) && cpSelf.name.empty() &&
                  cpSelf.password == "newpass123",
              "/cp 一个参数 = 改自己的密码（和 /changepassword 行为一致）");
        check(!cpSelf.consoleOnly && !cpSelf.opRequired,
              "/cp 自助改密码：不需要权限、也不是控制台专用");
        const auto cpOther = dchat::ParseServerCommand("/cp alice newpass123");
        check(Is(cpOther.kind, Kind::ChangePassword) && cpOther.name == "alice" &&
                  cpOther.consoleOnly,
              "/cp <昵称> <新密码> 同样是控制台专用");
        check(Is(dchat::ParseServerCommand("/CP newpass123").kind, Kind::ChangePassword),
              "/CP 大小写不敏感");
        check(!dchat::ParseServerCommand("/cp").error.empty(), "/cp 不给参数时提示用法");
        check(!dchat::ParseServerCommand("/cp 123").error.empty(),
              "/cp 的密码规则和 /changepassword 一样");
        check(dchat::LooksLikePasswordCommand("/cp mypass123"),
              "/cp 里的密码同样不进输入历史");
        check(dchat::HistoryTextFor("/cp mypass123") == "/cp ",
              "/cp 记进历史时只留指令名");

        const auto ip = dchat::ParseServerCommand("/ip alice");
        check(Is(ip.kind, Kind::ShowIp) && ip.name == "alice" && ip.consoleOnly,
              "/ip <昵称> 解析正确，并标记为仅控制台可用");
        check(!dchat::ParseServerCommand("/ip").error.empty(), "/ip 不给昵称时提示用法");
        check(!dchat::ParseServerCommand("/ip alice extra").error.empty(),
              "/ip 参数太多时提示用法");
        const std::string helpText = dchat::ServerCommandHelp();
        check(helpText.find("/cp") != std::string::npos &&
                  helpText.find("/ip") != std::string::npos,
              "帮助里列出了 /cp 和 /ip");
        const auto listed = dchat::Suggest("/", noNicks);
        bool hasCp = false, hasIp = false;
        for (const std::string& name : listed.matches) {
            if (name == "/cp") hasCp = true;
            if (name == "/ip") hasIp = true;
        }
        check(hasCp && hasIp, "Tab 候选浮层里也有 /cp 和 /ip");
        check(dchat::Suggest("/cp", noNicks).matches.size() == 1, "输入 /cp 能补全到 /cp");
    }

    {
        std::printf("[12] /chatrule 解析\n");
        const auto list = dchat::ParseServerCommand("/chatrule");
        check(Is(list.kind, Kind::ChatRule) && list.rule.empty() && list.consoleOnly,
              "/chatrule 不带参数 = 列出全部规则，且仅控制台可用");
        const auto one = dchat::ParseServerCommand("/chatrule chatinterval");
        check(Is(one.kind, Kind::ChatRule) && one.rule == "chatinterval" &&
                  one.ruleAction.empty(),
              "/chatrule <规则> = 只看这一条");
        const auto set = dchat::ParseServerCommand("/chatrule chatinterval set 500");
        check(Is(set.kind, Kind::ChatRule) && set.ruleAction == "set" && set.ruleValue == "500",
              "set 写法解析正确");
        const auto add = dchat::ParseServerCommand("/chatrule documentsize add 8");
        check(add.ruleAction == "add" && add.ruleValue == "8", "add 写法解析正确");
        const auto remove = dchat::ParseServerCommand("/chatrule maxservertemp remove 64");
        check(remove.ruleAction == "remove" && remove.ruleValue == "64", "remove 写法解析正确");
        const auto boolean = dchat::ParseServerCommand("/chatrule keepchathistory true");
        check(boolean.ruleAction == "setbool" && boolean.ruleValue == "true",
              "布尔规则直接写 true/false");
        check(dchat::ParseServerCommand("/chatrule keepchathistory false").ruleValue == "false",
              "false 也认");
        check(!dchat::ParseServerCommand("/chatrule nosuchrule set 1").error.empty(),
              "未知规则名在解析阶段就报错");
        check(!dchat::ParseServerCommand("/chatrule chatinterval set").error.empty(),
              "set 后面没跟值会提示取值范围");
        check(!dchat::ParseServerCommand("/chatrule chatinterval 500").error.empty(),
              "缺少 set/add/remove 时提示用法");
        check(!dchat::ParseServerCommand("/chatrule chatinterval set 5 5").error.empty(),
              "参数太多会报错");
        check(!dchat::ParseServerCommand("/chatrule keepchathistory true false").error.empty(),
              "布尔规则多写一个参数会报错");
        check(dchat::ServerCommandHelp().find("/chatrule") != std::string::npos,
              "帮助里列出了 /chatrule");
    }

    {
        std::printf("[13] Tab 参数补全（/chatrule 的规则名与取值）\n");
        const std::vector<std::string> noNicks;

        const dchat::CompletionResult rules = dchat::Suggest("/chatrule ", noNicks);
        check(rules.isArgument && rules.matches.size() == dchat::AllRuleNames().size(),
              "打 /chatrule + 空格 会列出全部规则名");
        check(rules.matches.size() == 4 && rules.matches[0] == "chatinterval" &&
                  rules.groups[0] == "服务器规则",
              "规则名按顺序给出，并分到「服务器规则」组");
        bool hintsOk = rules.hints.size() == rules.matches.size();
        for (const std::string& hint : rules.hints) {
            if (hint.empty()) hintsOk = false;  // 每个规则名后面都带灰色说明
        }
        check(hintsOk, "每个规则名都带灰色用法说明");

        const dchat::CompletionResult one = dchat::Suggest("/chatrule ch", noNicks);
        check(one.matches.size() == 1 && one.matches[0] == "chatinterval", "按前缀过滤规则名");
        check(dchat::Suggest("/chatrule keep", noNicks).matches[0] == "keepchathistory",
              "keep 前缀能补到 keepchathistory");

        dchat::TabCompleter ruleTab;
        check(ruleTab.Next("/chatrule ch", noNicks).text == "/chatrule chatinterval",
              "Tab 把规则名补进输入框");
        dchat::TabCompleter ruleCycle;
        check(ruleCycle.Next("/chatrule ", noNicks).text == "/chatrule chatinterval",
              "规则名也能按 Tab 循环");
        check(ruleCycle.Next("/chatrule chatinterval", noNicks).text == "/chatrule documentsize",
              "再按 Tab 轮到下一条规则");

        const dchat::CompletionResult actions = dchat::Suggest("/chatrule chatinterval ", noNicks);
        check(actions.matches.size() == 3 && actions.matches[0] == "set" &&
                  actions.matches[2] == "remove",
              "数值规则的第 2 个参数补 set / add / remove");
        check(dchat::Suggest("/chatrule chatinterval se", noNicks).matches.size() == 1 &&
                  dchat::Suggest("/chatrule chatinterval se", noNicks).matches[0] == "set",
              "set 也能按前缀过滤");

        const dchat::CompletionResult boolArg = dchat::Suggest("/chatrule keepchathistory ", noNicks);
        bool hasSet = false, hasTrue = false, hasFalse = false;
        for (const std::string& match : boolArg.matches) {
            if (match == "set") hasSet = true;
            if (match == "true") hasTrue = true;
            if (match == "false") hasFalse = true;
        }
        check(hasTrue && hasFalse, "布尔规则第 2 个参数直接给 true / false");
        check(hasSet, "布尔规则也接受先写 set");
        check(dchat::Suggest("/chatrule keepchathistory set ", noNicks).matches.size() == 2,
              "set 之后只剩 true / false 两个候选");
        check(dchat::Suggest("/chatrule keepchathistory t", noNicks).matches[0] == "true",
              "true 按前缀补全");

        dchat::TabCompleter boolTab;
        check(boolTab.Next("/chatrule keepchathistory t", noNicks).text ==
                  "/chatrule keepchathistory true",
              "Tab 补布尔值");
        check(dchat::Suggest("/chatrule chatinterval set ", noNicks).matches.empty(),
              "数值规则的值没有候选（自己填数字）");
        check(dchat::Suggest("/chatrule nosuchrule t", noNicks).matches.empty(),
              "不认识的规则名不会冒出 true/false");
        check(dchat::Suggest("/chatrule chatinterval set 5 ", noNicks).matches.empty(),
              "第 4 个参数没有候选");
    }

    {
        std::printf("[14] Tab 参数补全（/ip 与已注册名单）\n");
        const std::vector<std::string> online = {"alice", "bob"};
        const std::vector<std::string> known = {"alice", "bob", "carol", "dave"};

        const dchat::CompletionResult ip = dchat::Suggest("/ip ", online, known);
        check(ip.isArgument && ip.matches.size() == 4, "/ip 补在线 + 已注册的人");
        check(ip.matches[0] == "alice" && ip.matches[1] == "bob" && ip.matches[2] == "carol",
              "在线成员排在前面，然后是已注册但不在线的");
        check(ip.groups[0] == "在线成员" && ip.groups[2] == "已注册玩家",
              "候选按「在线成员 / 已注册玩家」分组");
        check(dchat::Suggest("/ip ca", online, known).matches.size() == 1 &&
                  dchat::Suggest("/ip ca", online, known).matches[0] == "carol",
              "/ip 能补到不在线的人");

        dchat::TabCompleter ipTab;
        check(ipTab.Next("/ip ca", online, known).text == "/ip carol", "Tab 补 /ip 的昵称");
        dchat::TabCompleter banTab;
        check(banTab.Next("/ban ca", online, known).text == "/ban carol",
              "/ban 也能补不在线的人");
        dchat::TabCompleter opTab;
        check(opTab.Next("/op d", online, known).text == "/op dave", "/op 也能补");

        const dchat::CompletionResult dedup = dchat::Suggest("/ban ", online, known);
        int aliceCount = 0;
        for (const std::string& match : dedup.matches) {
            if (match == "alice") ++aliceCount;
        }
        check(aliceCount == 1, "在线的人不会在候选里出现两次");
        const dchat::CompletionResult onlyOnline = dchat::Suggest("/kick ", online);
        check(onlyOnline.matches.size() == 2 && onlyOnline.groups[0] == "在线成员",
              "没给已注册名单时只补在线成员（老行为不变）");
        check(dchat::Suggest("/say ", online, known).matches.empty(), "/say 仍然不补昵称");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
