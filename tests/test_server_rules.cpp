// 服务器规则（/chatrule）的单元测试：默认值、set/add/remove、取值范围、
// 布尔规则、规则之间的约束、给客户端的那一行 RULES
#include <cstdio>
#include <string>

#include "server_rules.h"

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

using dchat::RuleAction;
using dchat::ServerRules;
}  // namespace

int main() {
    std::printf("== dchat server rules tests ==\n");

    {
        std::printf("[1] 默认值\n");
        ServerRules rules;
        check(rules.chatIntervalMs == 0, "chatinterval 默认 0（不限制）");
        check(rules.documentSizeMb == 64, "documentsize 默认 64 MB");
        check(!rules.keepChatHistory, "keepchathistory 默认关闭");
        check(rules.maxServerTempMb == 1048, "maxservertemp 默认 1048 MB");
        // 2026-10 新增的 4 条，默认全部是 0（不限制），保证升级后行为不变
        check(rules.uploadRateKbps == 0, "uploadrate 默认 0（不限制）");
        check(rules.downloadRateKbps == 0, "downloadrate 默认 0（不限制）");
        check(rules.maxTextLength == 0, "maxtextlen 默认 0（不限制）");
        check(rules.maxTextLines == 0, "maxtextlines 默认 0（不限制）");
        // 公网加固这 4 条：默认值是"不限制"，但握手超时默认 30 秒（防挂机占连接）
        check(rules.maxConnections == 0, "maxconns 默认 0（不限制）");
        check(rules.maxConnectionsPerIp == 0, "maxconnsperip 默认 0（不限制）");
        check(rules.loginFailLimit == 0, "loginfails 默认 0（不限制）");
        check(rules.handshakeTimeoutSec == 30, "handshaketimeout 默认 30 秒（不是 0）");
        check(dchat::AllRuleNames().size() == 12, "一共 12 条规则");
        check(dchat::IsKnownRule("chatinterval") && dchat::IsKnownRule("DOCUMENTSIZE") &&
                  dchat::IsKnownRule("keepchathistory") &&
                  dchat::IsKnownRule("maxservertemp") && dchat::IsKnownRule("UPLOADRATE") &&
                  dchat::IsKnownRule("downloadrate") && dchat::IsKnownRule("maxtextlen") &&
                  dchat::IsKnownRule("maxtextlines") && dchat::IsKnownRule("maxconns") &&
                  dchat::IsKnownRule("maxconnsperip") && dchat::IsKnownRule("loginfails") &&
                  dchat::IsKnownRule("handshaketimeout"),
              "十二条规则名都能识别（大小写不敏感）");
        check(!dchat::IsKnownRule("nope"), "不认识的规则名返回非法");
        check(dchat::RuleMbToBytes(1) == 1024ull * 1024ull, "MB 转字节");
    }

    {
        std::printf("[2] set / add / remove\n");
        ServerRules rules;
        dchat::RuleChange change =
            dchat::ApplyRule(&rules, "chatinterval", RuleAction::Set, 500, false);
        check(change.ok && change.changed && rules.chatIntervalMs == 500, "set 设成绝对值");
        change = dchat::ApplyRule(&rules, "chatinterval", RuleAction::Add, 250, false);
        check(change.ok && rules.chatIntervalMs == 750, "add 在当前值上加");
        change = dchat::ApplyRule(&rules, "chatinterval", RuleAction::Remove, 1000, false);
        check(change.ok && rules.chatIntervalMs == 0, "remove 减过头会夹到最小值 0");
        check(change.message.find("最小值") != std::string::npos, "被夹住时提示里会说");
        change = dchat::ApplyRule(&rules, "chatinterval", RuleAction::Set, 999999, false);
        check(change.ok && rules.chatIntervalMs == 60000, "超过范围会被夹到 60000 ms");
        change = dchat::ApplyRule(&rules, "chatinterval", RuleAction::Show, 0, false);
        check(change.ok && !change.changed && change.message.find("60000") != std::string::npos,
              "只看不改时返回当前值");
    }

    {
        std::printf("[3] 规则之间的约束\n");
        ServerRules rules;
        dchat::RuleChange change =
            dchat::ApplyRule(&rules, "documentsize", RuleAction::Set, 2000, false);
        check(!change.ok && rules.documentSizeMb == 64,
              "documentsize 超过 maxservertemp 时被拒绝（默认 1048）");
        check(change.message.find("maxservertemp") != std::string::npos, "提示里指出要先调大哪个");
        change = dchat::ApplyRule(&rules, "maxservertemp", RuleAction::Set, 16, false);
        check(!change.ok && rules.maxServerTempMb == 1048,
              "maxservertemp 不能小于当前 documentsize");
        change = dchat::ApplyRule(&rules, "maxservertemp", RuleAction::Set, 2048, false);
        check(change.ok && rules.maxServerTempMb == 2048, "先把 maxservertemp 调大");
        change = dchat::ApplyRule(&rules, "documentsize", RuleAction::Set, 2000, false);
        check(change.ok && rules.documentSizeMb == 2000, "再调大 documentsize 就成功了");
    }

    {
        std::printf("[4] 布尔规则与未知规则\n");
        ServerRules rules;
        dchat::RuleChange change =
            dchat::ApplyRule(&rules, "keepchathistory", RuleAction::SetBool, 0, true);
        check(change.ok && change.changed && rules.keepChatHistory, "true 打开聊天记录保留");
        change = dchat::ApplyRule(&rules, "keepchathistory", RuleAction::SetBool, 0, false);
        check(change.ok && !rules.keepChatHistory, "false 关闭");
        change = dchat::ApplyRule(&rules, "keepchathistory", RuleAction::Add, 1, false);
        check(!change.ok && change.message.find("true") != std::string::npos,
              "布尔规则不能用 add/remove");
        change = dchat::ApplyRule(&rules, "documentsize", RuleAction::SetBool, 0, true);
        check(!change.ok, "数值规则不能用 true/false");
        change = dchat::ApplyRule(&rules, "nosuchrule", RuleAction::Set, 1, false);
        check(!change.ok && change.message.find("没有这条规则") != std::string::npos,
              "未知规则给出提示");
    }

    {
        std::printf("[5] 规则说明与发给客户端的 RULES 行\n");
        ServerRules rules;
        rules.chatIntervalMs = 300;
        rules.documentSizeMb = 8;
        rules.keepChatHistory = true;
        const std::string all = dchat::DescribeAllRules(rules);
        bool allThere = true;
        for (const std::string& name : dchat::AllRuleNames()) {
            if (all.find(name) == std::string::npos) allThere = false;
        }
        check(allThere, "全部规则说明里四条都在");
        check(dchat::DescribeRule(rules, "chatinterval").find("300") != std::string::npos,
              "说明里带当前值");
        check(dchat::DescribeRule(rules, "keepchathistory").find("true") != std::string::npos,
              "布尔规则显示 true/false");
        check(dchat::RulesLineForClient(rules) == "RULES 8 300 1 0 0 0 0",
              "发给客户端的 RULES 行格式正确（新字段默认 0）");
        rules.keepChatHistory = false;
        check(dchat::RulesLineForClient(rules) == "RULES 8 300 0 0 0 0 0", "关闭时第 3 位是 0");
        check(dchat::RuleRangeText("chatinterval").find("60000") != std::string::npos,
              "取值范围说明里有上限");
        check(dchat::RuleRangeText("keepchathistory") == "true / false", "布尔规则的范围说明");
    }

    {
        std::printf("[6] dchat-rules.txt 读写\n");
        ServerRules rules;
        rules.chatIntervalMs = 400;
        rules.documentSizeMb = 8;
        rules.keepChatHistory = true;
        rules.maxServerTempMb = 512;
        const std::string text = dchat::SerializeRules(rules);
        check(text.size() > 0 && text[0] == '#', "序列化出来的文件以注释行开头");
        check(text.find("chatinterval 400") != std::string::npos, "数值规则写进去了");
        check(text.find("keepchathistory true") != std::string::npos, "布尔规则写进去了");

        ServerRules loaded;
        check(dchat::ParseRules(text, &loaded) == 12, "十二条规则都能读回来（8 条 + 4 条加固）");
        check(loaded.chatIntervalMs == 400 && loaded.documentSizeMb == 8 &&
                  loaded.keepChatHistory && loaded.maxServerTempMb == 512,
              "写出去再读回来完全一致（往返正确）");

        ServerRules messy;
        const int count = dchat::ParseRules(
            "# 这是注释\n"
            "chatinterval 250     # 行尾注释也认\n"
            "documentsize abc\n"
            "nosuchrule 5\n"
            "\n"
            "maxservertemp 999999\n"
            "keepchathistory maybe\n",
            &messy);
        check(count == 2, "只认有效行：注释 / 坏值 / 未知规则都被跳过");
        check(messy.chatIntervalMs == 250, "行尾注释不影响取值");
        check(messy.documentSizeMb == 64, "坏值那一行保持默认");
        check(messy.maxServerTempMb == 32768, "超出范围的会被夹到上限");
        check(!messy.keepChatHistory, "布尔值写错时保持默认");

        ServerRules conflict;
        check(dchat::ParseRules("documentsize 512\nmaxservertemp 128\n", &conflict) == 2 &&
                  conflict.documentSizeMb == 128,
              "文件里 documentsize 比 maxservertemp 大时会被夹下来");
        check(dchat::ParseRules("", &conflict) == 0, "空文件读到 0 条（保持原值）");
        check(dchat::ParseRules("chatinterval 300", nullptr) == 0, "传空指针不会崩");
    }

    {
        std::printf("[9] 规则元数据（Tab 补全用）\n");
        const std::vector<dchat::RuleInfo>& infos = dchat::AllRuleInfos();
        check(infos.size() == 12, "十二条规则各有一份元数据");
        bool hintsOk = true;
        bool namesOk = true;
        for (std::size_t i = 0; i < infos.size(); ++i) {
            if (infos[i].hint == nullptr || std::string(infos[i].hint).empty()) hintsOk = false;
            if (std::string(infos[i].name) != dchat::AllRuleNames()[i]) namesOk = false;
        }
        check(hintsOk, "每条规则都带一句灰色说明");
        check(namesOk, "元数据里的规则名和 AllRuleNames 顺序一致");
        check(dchat::IsBoolRule("keepchathistory") && !dchat::IsBoolRule("chatinterval"),
              "只有 keepchathistory 是布尔规则");
        check(dchat::IsBoolRule("KEEPCHATHISTORY"), "布尔规则的判断大小写不敏感");
        check(!dchat::IsBoolRule("nosuchrule"), "不认识的规则不是布尔规则");
        check(dchat::IsKnownRule("chatinterval") && dchat::IsKnownRule("keepchathistory") &&
                  dchat::IsKnownRule("maxservertemp") && dchat::IsKnownRule("documentsize"),
              "IsKnownRule 仍然认得这四条规则");
        check(!dchat::IsKnownRule("chatintervalX"), "IsKnownRule 不会把别的前缀当规则");
    }

    // ------------------------------------------------------------------
    // 2026-10 新增：限速与文本限制
    // ------------------------------------------------------------------
    {
        std::printf("[10] 新增规则：set / add / remove 与范围\n");
        ServerRules rules;

        dchat::RuleChange up = dchat::ApplyRule(&rules, "uploadrate", RuleAction::Set, 128, false);
        check(up.ok && up.changed && rules.uploadRateKbps == 128, "uploadrate set 128");
        up = dchat::ApplyRule(&rules, "uploadrate", RuleAction::Add, 128, false);
        check(rules.uploadRateKbps == 256, "uploadrate add 128 -> 256");
        up = dchat::ApplyRule(&rules, "uploadrate", RuleAction::Remove, 100, false);
        check(rules.uploadRateKbps == 156, "uploadrate remove 100 -> 156");

        // 0 是合法值（表示不限制），不能被夹到别的数
        up = dchat::ApplyRule(&rules, "uploadrate", RuleAction::Set, 0, false);
        check(rules.uploadRateKbps == 0, "uploadrate 可以设回 0（不限制）");

        // 负数会被夹到最小值 0
        up = dchat::ApplyRule(&rules, "downloadrate", RuleAction::Set, -50, false);
        check(rules.downloadRateKbps == 0, "downloadrate 负数被夹到 0");
        check(up.ok && up.message.find("最小值") != std::string::npos, "会提示被夹到最小值");

        // 超上限会被夹住
        dchat::ApplyRule(&rules, "downloadrate", RuleAction::Set, 99999999, false);
        check(rules.downloadRateKbps == 1048576, "downloadrate 上限 1048576 KB/s");

        dchat::ApplyRule(&rules, "maxtextlen", RuleAction::Set, 2000, false);
        check(rules.maxTextLength == 2000, "maxtextlen set 2000");
        dchat::ApplyRule(&rules, "maxtextlines", RuleAction::Set, 20, false);
        check(rules.maxTextLines == 20, "maxtextlines set 20");

        // 布尔规则的写法对数值规则应当被拒绝
        const dchat::RuleChange bad = dchat::ApplyRule(&rules, "maxtextlen", RuleAction::SetBool, 0, true);
        check(!bad.ok && bad.message.find("数值规则") != std::string::npos,
              "对数值规则用 true/false 会被拒绝并说明原因");

        // Show 动作只读不改
        const dchat::RuleChange shown = dchat::ApplyRule(&rules, "maxtextlines", RuleAction::Show, 0, false);
        check(shown.ok && shown.message.find("20") != std::string::npos, "Show 能读出当前值");
    }

    {
        std::printf("[11] RULES 下发行的向后兼容\n");
        ServerRules rules;
        rules.documentSizeMb = 32;
        rules.chatIntervalMs = 500;
        rules.keepChatHistory = true;
        rules.uploadRateKbps = 128;
        rules.downloadRateKbps = 256;
        rules.maxTextLength = 1000;
        rules.maxTextLines = 10;
        const std::string line = dchat::RulesLineForClient(rules);

        // 前三个字段的位置和含义**绝对不能变**，否则老客户端会读错
        check(line.rfind("RULES 32 500 1 ", 0) == 0,
              "前 3 个字段仍是 documentsize/chatinterval/keepchathistory，位置不变");
        check(line == "RULES 32 500 1 128 256 1000 10", "新增字段按顺序追加在末尾");

        // 字段总数
        std::size_t spaces = 0;
        for (char c : line) {
            if (c == ' ') ++spaces;
        }
        check(spaces == 7, "一共 8 个字段（7 个空格）");

        // 默认值下也要能生成合法行
        ServerRules plain;
        check(dchat::RulesLineForClient(plain) == "RULES 64 0 0 0 0 0 0", "默认值的下发行");
    }

    {
        std::printf("[12] 新增规则的序列化 / 解析往返\n");
        ServerRules rules;
        rules.uploadRateKbps = 64;
        rules.downloadRateKbps = 512;
        rules.maxTextLength = 500;
        rules.maxTextLines = 8;

        const std::string text = dchat::SerializeRules(rules);
        check(text.find("uploadrate 64") != std::string::npos, "序列化含 uploadrate");
        check(text.find("maxtextlines 8") != std::string::npos, "序列化含 maxtextlines");

        ServerRules loaded;
        check(dchat::ParseRules(text, &loaded) >= 4, "往返能读回规则");
        check(loaded.uploadRateKbps == 64 && loaded.downloadRateKbps == 512 &&
                  loaded.maxTextLength == 500 && loaded.maxTextLines == 8,
              "往返后 4 个新值都对");

        // 超范围的坏行应当被夹住而不是整份文件失败
        ServerRules clamped;
        dchat::ParseRules("uploadrate 99999999\nmaxtextlines 99999\n", &clamped);
        check(clamped.uploadRateKbps == 1048576, "解析时超上限被夹住");
        check(clamped.maxTextLines == 200, "maxtextlines 超上限被夹住");

        // "100abc" 这种坏行要跳过（沿用已有的严格解析约定）
        ServerRules bad;
        const int before = bad.uploadRateKbps;
        dchat::ParseRules("uploadrate 100abc\n", &bad);
        check(bad.uploadRateKbps == before, "带尾巴的坏值被跳过，不改动原值");
    }

    {
        std::printf("[13] 令牌桶限速器\n");
        dchat::RateLimiter limiter;

        // 0 = 不限制：永远不用等
        limiter.Configure(0);
        check(limiter.Consume(1024 * 1024) == 0, "限速为 0 时永不等待");

        // 128 KB/s：开局有一整桶（128 KB），一次要 64 KB 不用等
        limiter.Configure(128);
        check(limiter.Consume(64 * 1024) == 0, "开局桶是满的，64 KB 不用等");
        // 再要 64 KB 刚好用完
        check(limiter.Consume(64 * 1024) == 0, "再用掉 64 KB（桶刚好用尽）");
        // 第三次就该等了：再要 64 KB，桶空了，需要约 500 ms
        const int wait = limiter.Consume(64 * 1024);
        check(wait >= 400 && wait <= 600, "桶空了以后要等约 500 ms（128 KB/s 下 64 KB）");

        // 一次要得比整桶还多：也应当给出合理的等待时间，而不是死循环
        dchat::RateLimiter small;
        small.Configure(1);  // 1 KB/s
        const int big = small.Consume(4096);  // 要 4 KB
        check(big > 0 && big <= 8000, "一次要超过整桶容量时也能给出等待时间");

        // Configure 会重置状态
        limiter.Configure(1024);
        check(limiter.Consume(512 * 1024) == 0, "重新配置后桶又是满的");
    }

    // ------------------------------------------------------------------
    // 2026-10 新增：公网加固
    // ------------------------------------------------------------------
    {
        std::printf("[14] 公网加固规则：set / 范围 / 不下发给客户端\n");
        ServerRules rules;

        dchat::ApplyRule(&rules, "maxconns", RuleAction::Set, 200, false);
        check(rules.maxConnections == 200, "maxconns set 200");
        dchat::ApplyRule(&rules, "maxconnsperip", RuleAction::Set, 8, false);
        check(rules.maxConnectionsPerIp == 8, "maxconnsperip set 8");
        dchat::ApplyRule(&rules, "loginfails", RuleAction::Set, 10, false);
        check(rules.loginFailLimit == 10, "loginfails set 10");
        dchat::ApplyRule(&rules, "handshaketimeout", RuleAction::Set, 15, false);
        check(rules.handshakeTimeoutSec == 15, "handshaketimeout set 15");

        // handshaketimeout 允许设成 0（= 不限制）
        dchat::ApplyRule(&rules, "handshaketimeout", RuleAction::Set, 0, false);
        check(rules.handshakeTimeoutSec == 0, "handshaketimeout 可以设回 0（不限制）");

        // 超上限会被夹住
        dchat::ApplyRule(&rules, "maxconns", RuleAction::Set, 99999999, false);
        check(rules.maxConnections == 100000, "maxconns 上限 100000");
        dchat::ApplyRule(&rules, "handshaketimeout", RuleAction::Set, 99999, false);
        check(rules.handshakeTimeoutSec == 600, "handshaketimeout 上限 600 秒");

        // **加固规则刻意不下发给客户端**：客户端知道也没用，还能保持契约不变
        ServerRules out;
        out.maxConnections = 200;
        out.maxConnectionsPerIp = 8;
        out.loginFailLimit = 10;
        out.handshakeTimeoutSec = 15;
        const std::string line = dchat::RulesLineForClient(out);
        check(line == "RULES 64 0 0 0 0 0 0",
              "加固规则不占用 RULES 下发行的字段（仍是 8 个字段）");
        check(line.find("200") == std::string::npos && line.find("15") == std::string::npos,
              "加固规则的值确实没出现在下发行里");

        // 描述文本要能看出是哪一类
        check(dchat::DescribeRule(out, "maxconns").find("200") != std::string::npos,
              "DescribeRule 能读出 maxconns");
        check(dchat::DescribeRule(out, "loginfails").find("300") != std::string::npos,
              "loginfails 的说明里写清窗口是 300 秒");
    }

    {
        std::printf("[15] 防爆破追踪器（LoginFailTracker）\n");
        dchat::LoginFailTracker tracker;

        // limit <= 0 = 不限制：怎么失败都不锁
        for (int i = 0; i < 100; ++i) tracker.NoteFailure("10.0.0.1");
        check(!tracker.IsBlocked("10.0.0.1", 0), "limit=0 时永不封锁");
        check(!tracker.IsBlocked("10.0.0.1", -5), "负的 limit 也当不限制");

        // limit = 3：第 3 次之前不锁，第 3 次开始锁
        dchat::LoginFailTracker t2;
        check(!t2.IsBlocked("1.2.3.4", 3), "还没有失败记录时不锁");
        t2.NoteFailure("1.2.3.4");
        check(!t2.IsBlocked("1.2.3.4", 3), "失败 1 次（未达上限）不锁");
        t2.NoteFailure("1.2.3.4");
        check(!t2.IsBlocked("1.2.3.4", 3), "失败 2 次（未达上限）不锁");
        t2.NoteFailure("1.2.3.4");
        check(t2.IsBlocked("1.2.3.4", 3), "失败 3 次（达到上限）开始锁");
        t2.NoteFailure("1.2.3.4");
        check(t2.IsBlocked("1.2.3.4", 3), "继续失败仍然锁着");

        // 不同 IP 互不影响——这是"按 IP"的关键语义
        check(!t2.IsBlocked("5.6.7.8", 3), "别的 IP 不受影响");

        // 登录成功立刻清零，免得本人打错几次把自己锁住
        t2.Clear("1.2.3.4");
        check(!t2.IsBlocked("1.2.3.4", 3), "Clear 之后解锁");

        // 空 IP 不能崩，也不该被记
        dchat::LoginFailTracker t3;
        t3.NoteFailure("");
        check(!t3.IsBlocked("", 1), "空 IP 永远不锁");
        check(t3.TrackedCount() == 0, "空 IP 不进表");
        t3.NoteFailure("9.9.9.9");
        check(t3.TrackedCount() == 1, "正常 IP 会进表");
        t3.Sweep();
        check(t3.TrackedCount() == 1, "窗口没过期时 Sweep 不会误删");
        t3.Clear("9.9.9.9");
        check(t3.TrackedCount() == 0, "Clear 会把记录删掉（不是只把计数归零）");

        // 窗口长度是固定常量 300 秒
        check(dchat::kLoginFailWindowSec == 300, "失败窗口固定 300 秒（5 分钟）");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
