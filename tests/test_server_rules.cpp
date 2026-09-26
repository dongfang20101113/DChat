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
        check(dchat::AllRuleNames().size() == 4, "一共 4 条规则");
        check(dchat::IsKnownRule("chatinterval") && dchat::IsKnownRule("DOCUMENTSIZE") &&
                  dchat::IsKnownRule("keepchathistory") &&
                  dchat::IsKnownRule("maxservertemp"),
              "四条规则名都能识别（大小写不敏感）");
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
        check(dchat::RulesLineForClient(rules) == "RULES 8 300 1",
              "发给客户端的 RULES 行格式正确");
        rules.keepChatHistory = false;
        check(dchat::RulesLineForClient(rules) == "RULES 8 300 0", "关闭时最后一位是 0");
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
        check(dchat::ParseRules(text, &loaded) == 4, "四条规则都能读回来");
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
        check(infos.size() == 4, "四条规则各有一份元数据");
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
