// "api 模式"的大脑：直接调 OpenAI 兼容接口（DeepSeek 等），工具由机器人自己做
//
// 为什么不继续用 dsh：DSH 只以桌面应用的形式发布（`dsh.cmd` 实际是拿 Electron
// 当 node 跑 app.asar 里的 cli.js），搬到 Linux 服务器上等于把整个桌面端搬过去。
// 所以 api 模式下不再有子进程大脑，机器人自己发 HTTP、自己执行工具。
//
// 请求为什么走 curl 而不是自己写 TLS：Windows 上没有现成的 OpenSSL（机器人用的是
// CNG），Python/Node 也不保证存在；curl 在 Windows 10+ 和所有 Linux 上都自带，
// 是唯一"到处都有"的 HTTPS 客户端。key 不放在命令行（会出现在 ps 里），
// 而是写进一个 0600 的临时 curl 配置文件。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

/** 一条对话消息（只保留纯文本；工具调用不进历史，免得占满上下文）。 */
struct ApiMessage {
    std::string role;     // "user" / "assistant"
    std::string content;
};

struct ApiRunOptions {
    std::string url = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-chat";
    std::string key;
    int maxRounds = 6;
    int timeoutMs = 120000;
    /** 允许读写的目录（工具只能碰这里面的东西）。 */
    std::string workspace;
    /** 额外允许"读 + 发"的目录（只读）。 */
    std::vector<std::string> sendRoots;
    /** 系统提示；空 = 用内置的。 */
    std::string systemPrompt;
};

struct ApiTurnResult {
    bool ok = false;
    std::string answer;            // 最终回答（可能为空）
    std::vector<std::string> files;  // 模型要求发到群里的文件（已经是绝对路径）
    std::string error;
};

/** POST 一段 JSON 出去。失败时 error 里有原因（含 HTTP 状态码）。 */
bool HttpPostJson(const std::string& url, const std::vector<std::string>& headers,
                  const std::string& body, int timeoutMs, std::string* response,
                  std::string* error);

/**
 * 跑一轮对话，内部处理工具调用（读/写/改名/删除/跑命令/发文件），
 * 最多来回 options.maxRounds 轮。
 *
 * `history` 是这个房间最近的几条纯文本消息（不含本轮）。
 * `log` 用来把工具执行情况写进机器人日志（第一参数是级别说明，第二是内容）。
 */
ApiTurnResult RunApiTurn(const ApiRunOptions& options, const std::vector<ApiMessage>& history,
                         const std::string& userText,
                         const std::function<void(const std::string&)>& log);

}  // namespace dshbot
}  // namespace dchat
