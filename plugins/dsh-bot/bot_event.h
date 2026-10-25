// 解析 DeepSeek Harness `--json` 输出的事件行（NDJSON）
//
// `dsh --profile headless --json -` 每行一个 JSON 对象，形如：
//   {"type":"session","sessionId":"session-…","cwd":"…"}
//   {"type":"status","phase":"turn_start","turn":1}
//   {"type":"thinking","text":"…"}
//   {"type":"text","text":"秋风轻拂…"}
//   {"type":"final","text":"秋风轻拂…"}
//
// 我们只关心三样：type、text、sessionId。为此写一个**只认顶层字符串字段**的
// 小扫描器，而不是拉一个 JSON 库进来 —— 这是插件，不该为了读三行输出多带一个依赖。
// 嵌套的对象/数组整段跳过，不解析（status 里的 usage 就是嵌套的）。
//
// ⚠️ 文本字段里的转义必须**真解**：中文在 JSON 里可能被写成 \uXXXX，
// 聊天内容里的换行是 \n。不解析转义的话，群里会看到一片 "\u4f60\u597d"。
#pragma once

#include <string>

namespace dchat {
namespace dshbot {

struct DshEvent {
    std::string type;       // session / status / thinking / text / final / error
    std::string text;       // text / thinking / final 的正文
    std::string sessionId;  // session 事件里带的会话 id
    std::string phase;      // status 事件的阶段名（turn_start / step_end …）
    std::string error;      // error 事件的说明（如果那边给的是 error 类型）
};

/**
 * 解析一行。返回 false 表示这行不是合法 JSON 对象（比如进度条、日志混进了 stdout）。
 * **解析失败不该当成致命错误**：子进程的 stdout 里偶尔混进一行非 JSON 很正常，
 * 丢掉这一行接着读下一行就行。
 */
bool ParseDshEvent(const std::string& line, DshEvent* out);

}  // namespace dshbot
}  // namespace dchat
