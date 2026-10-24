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

    std::printf("[6] 状态机：什么时候该连、什么时候不该连\n");
    {
        ReconnectState state;

        // 没登录成功过（比如密码错、连不上）—— 不该自动重连
        Check(state.OnDisconnected(false, false) < 0, "没登录成功过就不重连");

        // 用户自己点的断开 —— 不该自动重连（否则会出现「我关了它自己又连上」）
        Check(state.OnDisconnected(true, true) < 0, "用户主动断开就不重连");
        Check(!state.wanted, "主动断开后状态是「不想连」");

        // 登录过、被动掉线 —— 该连，第一次等 3 秒
        Check(state.OnDisconnected(true, false) == 3000, "被动掉线第一次等 3 秒");
        Check(state.wanted, "状态是「想连」");

        // 连续失败按退避走
        Check(state.OnAttemptFailed() == 6000, "失败一次后等 6 秒");
        Check(state.OnAttemptFailed() == 12000, "再失败等 12 秒");
        Check(state.attempt == 2, "尝试次数记到 2");

        // 连上就清零
        state.OnSucceeded();
        Check(!state.wanted && state.attempt == 0, "连上后清零");
        Check(state.OnDisconnected(true, false) == 3000, "再次掉线重新从 3 秒开始");

        // 一直失败到放弃
        ReconnectState giveUp;
        giveUp.OnDisconnected(true, false);
        int last = -1;
        for (int i = 0; i < giveUp.policy.maxAttempts; ++i) {
            last = giveUp.OnAttemptFailed();
        }
        Check(last < 0, "试满次数后返回负数（放弃）");
        Check(!giveUp.wanted, "放弃后状态是「不想连」");
        Check(giveUp.OnAttemptFailed() < 0, "放弃之后再问仍然是不连");

        // 放弃后用户手动连上，状态机不能残留
        giveUp.OnSucceeded();
        Check(giveUp.attempt == 0, "放弃后重新成功也要从零开始");
    }

    std::printf("\n%d 项检查，失败 %d 项。\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
