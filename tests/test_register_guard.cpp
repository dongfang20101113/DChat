// 注册防护判定的单元测试。
//
// 为什么这块必须有测试：判定错了**两个方向都会静默出错**——
//   - 判松了：脚本继续刷账号，等于没做防护（而且日志上看不出来）
//   - 判紧了：正常用户注册不了，他只会看到一句"请稍后再试"，极难排查
// 所以两个方向都要钉死，尤其是"首次注册必须放行"这条边界。
#include <cstdio>
#include <string>

#include "register_guard.h"
#include "server_rules.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

}  // namespace

int main() {
    std::printf("== dchat register guard tests ==\n");
    using dchat::CheckRegisterAllowed;
    using dchat::RegisterDeny;
    using dchat::RegisterDenyText;
    using dchat::RegisterWaitSeconds;

    {
        std::printf("[1] 首次注册必须放行（最容易写错的边界）\n");
        // -1 表示"这个 IP 之前没注册过"。如果实现里把它当成 0 处理，
        // 所有人首次注册都会被拒 —— 服务器等于直接不可用。
        check(CheckRegisterAllowed(0, 0, -1, 0) == RegisterDeny::None,
              "没记录 + 无限制 -> 放行");
        check(CheckRegisterAllowed(0, 1000, -1, 60) == RegisterDeny::None,
              "**没记录 + 有冷却 -> 仍然放行**（新 IP 不该被拦）");
        check(CheckRegisterAllowed(999, 1000, -1, 3600) == RegisterDeny::None,
              "接近上限但没满 + 新 IP -> 放行");
    }

    {
        std::printf("[2] 同 IP 冷却（registerinterval）\n");
        check(CheckRegisterAllowed(0, 0, 0, 60) == RegisterDeny::TooSoon, "刚注册过 -> 拒");
        check(CheckRegisterAllowed(0, 0, 30, 60) == RegisterDeny::TooSoon, "30 秒 < 60 秒 -> 拒");
        check(CheckRegisterAllowed(0, 0, 59, 60) == RegisterDeny::TooSoon, "59 秒 -> 拒（边界内）");
        check(CheckRegisterAllowed(0, 0, 60, 60) == RegisterDeny::None,
              "正好 60 秒 -> 放行（边界不算超）");
        check(CheckRegisterAllowed(0, 0, 61, 60) == RegisterDeny::None, "61 秒 -> 放行");
        check(CheckRegisterAllowed(0, 0, 0, 0) == RegisterDeny::None, "规则为 0 -> 不限制");
        check(CheckRegisterAllowed(0, 0, 0, -5) == RegisterDeny::None,
              "规则是负数（异常输入）-> 按不限制处理，不能拒绝所有人");
    }

    {
        std::printf("[3] 账号总量上限（maxaccounts）\n");
        check(CheckRegisterAllowed(0, 100, -1, 0) == RegisterDeny::None, "0 / 100 -> 放行");
        check(CheckRegisterAllowed(99, 100, -1, 0) == RegisterDeny::None, "99 / 100 -> 放行");
        check(CheckRegisterAllowed(100, 100, -1, 0) == RegisterDeny::AccountFull,
              "100 / 100 -> 拒（到上限就不许再加）");
        check(CheckRegisterAllowed(101, 100, -1, 0) == RegisterDeny::AccountFull,
              "101 / 100 -> 拒（超了更要拒）");
        check(CheckRegisterAllowed(999999, 0, -1, 0) == RegisterDeny::None,
              "上限 0 -> 不限制，账号再多也放行");
    }

    {
        std::printf("[4] 两条规则同时开：总量上限优先\n");
        // 已满 + 又太快：应该报"服务器已满"，因为这条对用户更有意义，
        // 而且它才是换 IP 也绕不过的那道闸
        check(CheckRegisterAllowed(100, 100, 0, 60) == RegisterDeny::AccountFull,
              "两个都触发 -> 报「账号已满」");
    }

    {
        std::printf("[5] 等待秒数\n");
        check(RegisterWaitSeconds(-1, 60) == 0, "新 IP -> 不用等");
        check(RegisterWaitSeconds(0, 60) == 60, "刚注册 -> 等满 60 秒");
        check(RegisterWaitSeconds(30, 60) == 30, "过了 30 秒 -> 还等 30 秒");
        check(RegisterWaitSeconds(60, 60) == 0, "到点 -> 不用等");
        check(RegisterWaitSeconds(999, 60) == 0, "早就过了 -> 不用等");
        check(RegisterWaitSeconds(0, 0) == 0, "不限制 -> 不用等");
    }

    {
        std::printf("[6] 拒绝原因要说人话\n");
        const std::string full = RegisterDenyText(RegisterDeny::AccountFull, 5000, 0, 0);
        check(full.find("5000") != std::string::npos, "账号满的提示要带上限数字");
        check(full.find("管理员") != std::string::npos, "要告诉用户该找谁");

        const std::string soon = RegisterDenyText(RegisterDeny::TooSoon, 0, 42, 60);
        check(soon.find("42") != std::string::npos, "太快的提示要告诉还要等几秒");
        check(soon.find("60") != std::string::npos, "也要说明冷却窗口是多少");

        check(RegisterDenyText(RegisterDeny::None, 0, 0, 0).empty(), "允许时不产生提示");
    }

    {
        std::printf("[7] 规则本身能配到\n");
        // 两条规则必须出现在规则表里，否则管理员根本开不了
        dchat::ServerRules rules;
        check(rules.registerIntervalSec == 0, "registerinterval 默认 0（不限制）");
        check(rules.maxAccounts == 0, "maxaccounts 默认 0（不限制）");

        const auto& names = dchat::AllRuleNames();
        bool hasInterval = false;
        bool hasMax = false;
        for (const std::string& name : names) {
            if (name == "registerinterval") hasInterval = true;
            if (name == "maxaccounts") hasMax = true;
        }
        check(hasInterval, "规则表里有 registerinterval");
        check(hasMax, "规则表里有 maxaccounts");

        // 能通过 ApplyRule 设上，并且能读回来
        dchat::RuleChange interval =
            dchat::ApplyRule(&rules, "registerinterval", dchat::RuleAction::Set, 120, false);
        check(interval.ok, "能设置 registerinterval");
        check(rules.registerIntervalSec == 120, "设置后值正确");
        dchat::RuleChange maxAcc =
            dchat::ApplyRule(&rules, "maxaccounts", dchat::RuleAction::Set, 5000, false);
        check(maxAcc.ok, "能设置 maxaccounts");
        check(rules.maxAccounts == 5000, "设置后值正确");

        // 序列化后能再解析回来（否则重启就丢配置）
        const std::string text = dchat::SerializeRules(rules);
        check(text.find("registerinterval 120") != std::string::npos, "写进规则文件");
        check(text.find("maxaccounts 5000") != std::string::npos, "写进规则文件");
        dchat::ServerRules reloaded;
        const int count = dchat::ParseRules(text, &reloaded);
        check(count > 0, "能解析回来");
        check(reloaded.registerIntervalSec == 120, "重新解析后 registerinterval 正确");
        check(reloaded.maxAccounts == 5000, "重新解析后 maxaccounts 正确");

        // 越界值不是拒绝，而是**钳制到上限**（这是所有数值规则的统一行为，
        // 提示里会说明「已限制到最大值」）。测试要盯的是"钳到哪儿"，不是"拒不拒"。
        dchat::ServerRules bounded;
        dchat::RuleChange tooBig =
            dchat::ApplyRule(&bounded, "registerinterval", dchat::RuleAction::Set, 999999, false);
        check(tooBig.ok, "超过上限的值仍然接受（钳制语义）");
        check(bounded.registerIntervalSec == 86400, "被钳制到上限 86400");
        check(tooBig.message.find("最大值") != std::string::npos, "提示里说明了被钳制");

        // add 语义要基于当前值，而不是从 0 开始
        dchat::ServerRules forAdd;
        dchat::ApplyRule(&forAdd, "registerinterval", dchat::RuleAction::Set, 60, false);
        dchat::ApplyRule(&forAdd, "registerinterval", dchat::RuleAction::Add, 30, false);
        check(forAdd.registerIntervalSec == 90, "add 是基于当前值累加（60+30=90）");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
