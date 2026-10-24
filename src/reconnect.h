// 断线重连的退避策略（纯逻辑，不碰 socket / 界面）。
//
// 为什么单独抽出来：
//
// 1. **必须退避**。固定间隔重连在服务器挂掉时等于给自己刷日志，而且多台客户端
//    同时以固定节奏打过来，服务器刚恢复就被打垮（惊群）。
// 2. **必须有上限**。没有上限的话，用户关了服务器、你的客户端会永远每 60 秒
//    试一次，日志里全是失败，真出问题时反而看不见。
// 3. 抽成纯函数才能单测——重连这条路径在真实环境里很难稳定复现
//    （要拔网线、要服务器崩），靠手工验证等于不验证。
//
// 三端可以用同一套数字，但各自的触发机制不同（桌面端走 WM_TIMER，
// Android 走协程，网页端走 setTimeout）。
#pragma once

namespace dchat {

struct ReconnectPolicy {
    int firstDelayMs = 3000;  // 第一次等 3 秒
    int maxDelayMs = 60000;   // 之后最多等 60 秒
    int maxAttempts = 8;      // 最多试 8 次（约 5 分钟），再不行就放弃
};

/**
 * 第 `attempt` 次重连前应该等多久（attempt 从 0 开始）。
 *
 * 3s → 6s → 12s → 24s → 48s → 60s → 60s …（封顶 maxDelayMs）
 * 负数或越界的 attempt 一律按 0 / 上限处理，调用方传错也不会算出负延迟。
 */
inline int ReconnectDelayMs(const ReconnectPolicy& policy, int attempt) {
    if (attempt < 0) attempt = 0;
    if (policy.firstDelayMs <= 0) return 0;
    int delay = policy.firstDelayMs;
    for (int i = 0; i < attempt; ++i) {
        // 先判断再乘，避免整数溢出（attempt 传个 100 进来也不会炸）
        if (delay >= policy.maxDelayMs) return policy.maxDelayMs;
        delay *= 2;
        if (delay >= policy.maxDelayMs) return policy.maxDelayMs;
    }
    return delay < policy.maxDelayMs ? delay : policy.maxDelayMs;
}

/** 试了 `attempt` 次之后该不该放弃。 */
inline bool ReconnectGiveUp(const ReconnectPolicy& policy, int attempt) {
    return attempt >= policy.maxAttempts;
}

/** 成功连上之后把计数清零（不要写成 attempt = 0 散落在各处）。 */
inline void ReconnectReset(int* attempt) {
    if (attempt != nullptr) *attempt = 0;
}

}  // namespace dchat
