// 谁在叫它、要不要回、要回什么内容（纯逻辑，可单测）
//
// 三件事放一个文件是因为它们**必须用同一套"提及"判定**：
// 判断"有没有 @ 我"和"把 @ 我 那一段从正文里去掉"如果各写一遍，迟早一边
// 认出 `@小d`、另一边认不出，于是机器人要么没反应，要么把 `@小d` 当成正文
// 原样复述一遍。
#pragma once

#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

struct TriggerConfig;
struct ReplyPolicy;
struct BrainConfig;

struct TriggerMatch {
    bool matched = false;
    /** 去掉 @ 之后的正文（可能为空：只 @ 了一声没说话）。 */
    std::string text;
};

/**
 * 这一行是不是在叫机器人。
 *
 * `mentionOnly = true` 时用 dchat 全局一致的提及规则（和桌面端高亮、
 * `@全体成员` 判定同一套）：`@` 前面不能紧挨着字母，后面不能紧跟着字母，
 * 所以 `a@小d` 和 `@小dx` 都不算叫它。
 */
TriggerMatch MatchTrigger(const std::string& raw, const std::string& botNick,
                          const TriggerConfig& config);

/** 白名单 / 黑名单判定（昵称大小写不敏感）。 */
bool ReplyAllowed(const ReplyPolicy& policy, const std::string& nick);

/** 按模板拼提示词：{room} / {roomId} / {nick} / {text} / {workdir} / {fileroots}。 */
std::string BuildPrompt(const BrainConfig& config, const std::vector<std::string>& sendRoots,
                        const std::string& roomName, const std::string& roomId,
                        const std::string& nick, const std::string& text);

}  // namespace dshbot
}  // namespace dchat
