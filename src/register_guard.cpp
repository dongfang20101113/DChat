#include "register_guard.h"

namespace dchat {

RegisterDeny CheckRegisterAllowed(std::size_t accountCount, int maxAccounts,
                                  long long secondsSinceLast, int intervalSec) {
    // 顺序有讲究：**先判总量上限，再判 IP 冷却**。
    // 总量上限是硬闸（换 IP 也绕不过），它的理由对用户更有意义；
    // 而且一个已经被灌满的服务器，回"服务器账号已满"比回"你注册太快了"更准确。
    if (maxAccounts > 0 && accountCount >= static_cast<std::size_t>(maxAccounts)) {
        return RegisterDeny::AccountFull;
    }
    if (intervalSec > 0 && secondsSinceLast >= 0 && secondsSinceLast < intervalSec) {
        return RegisterDeny::TooSoon;
    }
    // secondsSinceLast < 0 表示这个 IP 之前没注册过 —— 一定放行。
    // 这一条很关键：如果把它当成 0 处理，所有人首次注册都会被拒，
    // 服务器等于直接不可用（第一版思路就是这样，写测试时才发现的）。
    return RegisterDeny::None;
}

long long RegisterWaitSeconds(long long secondsSinceLast, int intervalSec) {
    if (intervalSec <= 0) return 0;
    if (secondsSinceLast < 0) return 0;
    const long long left = static_cast<long long>(intervalSec) - secondsSinceLast;
    return left > 0 ? left : 0;
}

std::string RegisterDenyText(RegisterDeny reason, int maxAccounts, long long waitSeconds,
                             int intervalSec) {
    switch (reason) {
        case RegisterDeny::AccountFull:
            return "服务器账号数已达上限（" + std::to_string(maxAccounts) +
                   " 个），暂时无法注册新账号。请联系管理员";
        case RegisterDeny::TooSoon:
            return "注册太频繁了，请等 " + std::to_string(waitSeconds > 0 ? waitSeconds : intervalSec) +
                   " 秒后再试（同一地址 " + std::to_string(intervalSec) + " 秒内只能注册一次）";
        case RegisterDeny::None:
        default:
            return std::string();
    }
}

}  // namespace dchat
