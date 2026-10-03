// 注册路径的防护判定（**纯逻辑**，不碰服务器状态，便于单测）。
//
// 为什么单独抽出来：注册防护是"错了不会报错、只会悄悄放行或悄悄误杀"的那类代码。
//   - 判错了放行：脚本继续刷账号，等于没做
//   - 判错了拒绝：正常用户注册不了，而且他只会看到一句"稍后再试"，很难查
// 所以把判定写成纯函数，用测试把两个方向都钉死。
//
// 实测背景（tools/regflood.cpp）：加这两条规则之前，开满全部既有防护，
// 12 线程并发注册仍能跑到 363 个/秒、720 个全部成功；正常用户的消息往返
// 从 105 ms 被拖到 10 秒超时。换 IP 能绕开所有基于 IP 的限制，
// 所以这里刻意做了两层：IP 冷却（抬高成本）+ 总量上限（换多少 IP 都绕不过）。
#pragma once

#include <cstddef>
#include <string>

namespace dchat {

/** 注册请求被拒绝的原因。 */
enum class RegisterDeny {
    None,        // 允许
    AccountFull, // 账号总数已达上限
    TooSoon,     // 同一 IP 注册太频繁
};

/**
 * 判断这次注册该不该拒。
 *
 * @param accountCount     服务器现有账号数
 * @param maxAccounts      规则 maxaccounts（0 = 不限制）
 * @param secondsSinceLast 同一 IP 距上次注册成功过了多少秒
 *                         （负值表示**这个 IP 从没注册过**，必须放行）
 * @param intervalSec      规则 registerinterval（0 = 不限制）
 */
RegisterDeny CheckRegisterAllowed(std::size_t accountCount, int maxAccounts,
                                  long long secondsSinceLast, int intervalSec);

/** 拒绝原因转成给用户看的一句话（要能看懂，而不是只说"失败"）。 */
std::string RegisterDenyText(RegisterDeny reason, int maxAccounts, long long waitSeconds,
                             int intervalSec);

/** 距离下次可以注册还要等多少秒；已经可以注册时返回 0。 */
long long RegisterWaitSeconds(long long secondsSinceLast, int intervalSec);

}  // namespace dchat
