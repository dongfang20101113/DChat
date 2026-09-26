// 服务器运行规则（用 /chatrule 指令修改，只能从服务器控制台改）
//
//   /chatrule                     列出所有规则和当前值
//   /chatrule <规则>               只看某条规则
//   /chatrule <规则> set <值>      设成某个值
//   /chatrule <规则> add <值>      在当前值上加
//   /chatrule <规则> remove <值>   在当前值上减
//   /chatrule <规则> true|false   布尔规则（keepchathistory）的写法
#pragma once

#include <string>
#include <vector>

namespace dchat {

struct ServerRules {
    int chatIntervalMs = 0;        // chatinterval：两条消息之间最少间隔（毫秒），0 = 不限制
    int documentSizeMb = 64;       // documentsize：单个文件最大 MB（默认 64，和以前一致）
    bool keepChatHistory = false;  // keepchathistory：新加入的人能否看到之前的聊天记录和文件
    int maxServerTempMb = 1048;    // maxservertemp：服务端保存文件 + 聊天记录缓存的总上限（MB）
};

enum class RuleAction { Show, Set, Add, Remove, SetBool };

struct RuleChange {
    bool ok = false;        // 指令本身合法
    bool changed = false;   // 值是否真的被改了
    std::string rule;       // 规则名（小写）
    std::string message;    // 给用户看的中文说明
};

bool IsKnownRule(const std::string& name);
const std::vector<std::string>& AllRuleNames();

// 规则元数据：名字 + Tab 候选浮层里显示的灰色说明 + 是不是 true/false 的布尔规则。
// /chatrule 的补全用它，避免把规则名和说明写两遍。
struct RuleInfo {
    const char* name;
    const char* hint;
    bool isBool;
};
const std::vector<RuleInfo>& AllRuleInfos();
bool IsBoolRule(const std::string& name);

// 规则取值范围的文字说明（错误提示里要用）
std::string RuleRangeText(const std::string& name);
// 一条规则的当前值说明，例如 "chatinterval = 500 ms（两条消息之间最少间隔）"
std::string DescribeRule(const ServerRules& rules, const std::string& name);
// 所有规则的多行说明
std::string DescribeAllRules(const ServerRules& rules);

// 应用一次修改。value 用于数值规则，boolValue 用于布尔规则
RuleChange ApplyRule(ServerRules* rules, const std::string& name, RuleAction action, long long value,
                     bool boolValue);

// 打包成发给客户端的一行（客户端据此调整自己的"单文件上限"等本地检查）
std::string RulesLineForClient(const ServerRules& rules);

// ---- dchat-rules.txt 的读写（一行一条：规则名 值；以 # 开头的是注释）----
std::string SerializeRules(const ServerRules& rules);
// 解析规则文件：非法行跳过、超出范围的夹到范围内。
// 返回成功读到的规则条数；rules 就地更新（文件里没写的规则保持原值）
int ParseRules(const std::string& text, ServerRules* rules);

// 数值上限辅助（MB -> 字节）
unsigned long long RuleMbToBytes(int mb);

}  // namespace dchat
