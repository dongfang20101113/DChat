// 断线重连退避策略的单元测试。
//
// 这段逻辑看着简单，但错一次就是"服务器挂了以后客户端疯狂重连"或
// "网络抖一下就再也不试了"。两种都很烦，而且都不容易在手工测试里发现。
#include <cstdio>

#include "reconnect.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [失败] %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace dchat;
    const ReconnectPolicy policy;  // 用默认值：3s 起、60s 封顶、8 次

    std::printf("[1] 退避序列\n");
    Check(ReconnectDelayMs(policy, 0) == 3000, "第 1 次等 3 秒");
    Check(ReconnectDelayMs(policy, 1) == 6000, "第 2 次等 6 秒");
    Check(ReconnectDelayMs(policy, 2) == 12000, "第 3 次等 12 秒");
    Check(ReconnectDelayMs(policy, 3) == 24000, "第 4 次等 24 秒");
    Check(ReconnectDelayMs(policy, 4) == 48000, "第 5 次等 48 秒");
    Check(ReconnectDelayMs(policy, 5) == 60000, "第 6 次封顶到 60 秒");
    Check(ReconnectDelayMs(policy, 99) == 60000, "再多次也不超过 60 秒");

    std::printf("[2] 单调不减\n");
    {
        bool monotonic = true;
        int previous = 0;
        for (int attempt = 0; attempt < 20; ++attempt) {
            const int delay = ReconnectDelayMs(policy, attempt);
            if (delay < previous) monotonic = false;
            previous = delay;
        }
        Check(monotonic, "延迟随次数单调不减（不会越试越快）");
    }

    std::printf("[3] 异常输入\n");
    Check(ReconnectDelayMs(policy, -5) == 3000, "负数按第 1 次处理");
    Check(ReconnectDelayMs(policy, 1000) == 60000, "超大次数不会整数溢出（返回封顶值）");
    {
        ReconnectPolicy zero;
        zero.firstDelayMs = 0;
        Check(ReconnectDelayMs(zero, 3) == 0, "firstDelayMs 为 0 时返回 0（立即重连）");
    }
    {
        ReconnectPolicy tiny;
        tiny.firstDelayMs = 100;
        tiny.maxDelayMs = 250;
        Check(ReconnectDelayMs(tiny, 1) == 200, "翻倍后仍低于封顶时正常翻倍");
        Check(ReconnectDelayMs(tiny, 2) == 250, "翻倍越界时夹到封顶值");
    }

    std::printf("[4] 放弃条件\n");
    Check(!ReconnectGiveUp(policy, 0), "第 1 次之前不放弃");
    Check(!ReconnectGiveUp(policy, policy.maxAttempts - 1), "最后一次之前不放弃");
    Check(ReconnectGiveUp(policy, policy.maxAttempts), "试满次数就放弃");
    Check(ReconnectGiveUp(policy, policy.maxAttempts + 5), "超过次数仍然放弃");

    std::printf("[5] 重连成功后清零\n");
    {
        int attempt = 5;
        ReconnectReset(&attempt);
        Check(attempt == 0, "连上之后计数清零");
        ReconnectReset(nullptr);
        Check(true, "传 nullptr 不崩");
    }

    std::printf("\n%d 项检查，失败 %d 项。\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
