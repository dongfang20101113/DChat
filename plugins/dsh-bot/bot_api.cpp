#include "bot_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "bot_file.h"
#include "bot_json.h"
#include "bot_json_value.h"
#include "bot_process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dchat {
namespace dshbot {
namespace {

constexpr int kMaxReadBytes = 64 * 1024;     // 一次读文件的上限
constexpr int kMaxCommandBytes = 8 * 1024;   // 命令输出回给模型的上限
constexpr int kMaxCommandTimeoutMs = 60000;  // 命令最长跑多久

/** curl 配置文件里的值（双引号 + 反斜杠转义，curl 的 --config 语法）。 */
std::string ConfigQuote(const std::string& text) {
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string TrimText(const std::string& text) {
    const auto isSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string TempDir() {
#ifdef _WIN32
    const char* temp = std::getenv("TEMP");
    return (temp != nullptr && temp[0] != '\0') ? std::string(temp) : std::string(".");
#else
    const char* temp = std::getenv("TMPDIR");
    return (temp != nullptr && temp[0] != '\0') ? std::string(temp) : std::string("/tmp");
#endif
}

std::string UniqueTempPath(const std::string& tag) {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    static int counter = 0;
    ++counter;
    return TempDir() + "/dchat-api-" + tag + "-" + std::to_string(pid) + "-" +
           std::to_string(counter);
}

bool WriteFileText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string MessageJson(const std::string& role, const std::string& content) {
    return "{\"role\":" + JsonQuote(role) + ",\"content\":" + JsonQuote(content) + "}";
}

std::string ToolJson(const std::string& name, const std::string& description,
                     const std::string& parameters) {
    return "{\"type\":\"function\",\"function\":{\"name\":" + JsonQuote(name) +
           ",\"description\":" + JsonQuote(description) + ",\"parameters\":" + parameters + "}}";
}

std::string ObjectSchema(const std::string& props, const std::string& required) {
    return "{\"type\":\"object\",\"properties\":{" + props + "}" +
           (required.empty() ? std::string() : ",\"required\":" + required) + "}";
}

std::string Param(const std::string& name, const std::string& type, const std::string& desc) {
    return JsonQuote(name) + ":{\"type\":" + JsonQuote(type) + ",\"description\":" +
           JsonQuote(desc) + "}";
}

/** 内置系统提示：说清楚它能干什么、边界在哪。 */
std::string DefaultSystemPrompt(const ApiRunOptions& options) {
    std::string roots;
    for (const std::string& root : options.sendRoots) {
        if (!roots.empty()) roots += "、";
        roots += root;
    }
    std::string prompt =
        "你是 dchat 群聊里的一个助手。用简体中文、口语化、简短地回复；这是聊天消息，"
        "不要用 Markdown 标题、表格或代码块。命令、路径、报错原文、工具名照抄，不要翻译；"
        "对方用英文问就用英文答。\n"
        "你可以用工具真的操作这台机器：读文件、写文件、改文件、列目录、建目录、改名、删除、"
        "跑命令、把文件发到群里。能读写的目录只有：";
    prompt += options.workspace.empty() ? std::string("（当前目录）") : options.workspace;
    prompt += "\n";
    if (!roots.empty()) {
        prompt += "另外这些目录里的文件可以读、可以直接发给对方（只读，不能改）：" + roots + "\n";
    }
    prompt +=
        "规则：\n"
        "1. 需要看文件或跑命令就调工具，不要凭空猜内容。\n"
        "2. 改文件之前先读一遍，别覆盖掉自己没看过的东西。\n"
        "3. 删除、覆盖、重启服务这类不可逆操作，先跟对方确认再动手。\n"
        "4. 要发文件就调 send_file（机器人会自己上传），不要只说路径。\n"
        "5. 工具报错就把原始错误告诉对方，不要编一个成功的结果。\n";
    if (!options.systemPrompt.empty()) prompt += "\n" + options.systemPrompt + "\n";
    return prompt;
}

struct ToolOutcome {
    std::string text;   // 回给模型看的结果
    bool sendFile = false;
    std::string filePath;
};

std::string ReadFileLimited(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "打不开文件：" + path;
        return std::string();
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > static_cast<std::size_t>(kMaxReadBytes)) {
        data.resize(static_cast<std::size_t>(kMaxReadBytes));
        data += "\n…（文件太大，只给你看了前 64 KB）";
    }
    return data;
}

/** 允许读的目录：工作目录 + 只读发送目录。 */
std::string ResolveReadable(const ApiRunOptions& options, const std::string& raw) {
    std::vector<std::string> roots = options.sendRoots;
    roots.insert(roots.begin(), options.workspace);
    return ResolveSendablePath(roots, raw);
}

ToolOutcome ExecuteTool(const ApiRunOptions& options, const std::string& name,
                        const JsonValue& args) {
    ToolOutcome outcome;
    const std::string path = args.GetString("path");
    const std::string workspace = options.workspace.empty() ? std::string(".") : options.workspace;

    if (name == "list_dir") {
        const std::string dir = ResolveReadable(options, path.empty() ? workspace : path);
        if (dir.empty()) {
            outcome.text = "错误：这个路径不在允许范围内：" + path;
            return outcome;
        }
        std::error_code code;
        std::string listing;
        int count = 0;
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::path(dir), code)) {
            if (code) break;
            std::string line = entry.path().filename().string();
            std::error_code kindCode;
            if (entry.is_directory(kindCode)) {
                line += "/";
            } else if (entry.is_regular_file(kindCode)) {
                const auto size = std::filesystem::file_size(entry.path(), kindCode);
                if (!kindCode) line += "（" + std::to_string(size) + " 字节）";
            }
            listing += line + "\n";
            if (++count >= 200) {
                listing += "…（还有更多，只列了前 200 项）\n";
                break;
            }
        }
        if (code) {
            outcome.text = "错误：读不了这个目录：" + code.message();
            return outcome;
        }
        outcome.text = listing.empty() ? std::string("（空目录）") : listing;
        return outcome;
    }

    if (name == "read_file") {
        const std::string file = ResolveReadable(options, path);
        if (file.empty()) {
            outcome.text = "错误：这个路径不在允许范围内：" + path;
            return outcome;
        }
        std::string error;
        const std::string data = ReadFileLimited(file, &error);
        outcome.text = error.empty() ? data : "错误：" + error;
        return outcome;
    }

    if (name == "write_file" || name == "append_file") {
        const std::string file = ResolvePathForWrite(workspace, path);
        if (file.empty()) {
            outcome.text = "错误：只能写工作目录里的文件：" + path;
            return outcome;
        }
        const std::string content = args.GetString("content");
        const auto mode = (name == "write_file") ? (std::ios::binary | std::ios::trunc)
                                                 : (std::ios::binary | std::ios::app);
        std::ofstream out(file, mode);
        if (!out) {
            outcome.text = "错误：写不进去，检查权限：" + file;
            return outcome;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out.good()) {
            outcome.text = "错误：写入过程中出错：" + file;
            return outcome;
        }
        outcome.text = "已写入 " + std::to_string(content.size()) + " 字节到 " + file;
        return outcome;
    }

    if (name == "replace_in_file") {
        const std::string file = ResolvePathForWrite(workspace, path);
        if (file.empty()) {
            outcome.text = "错误：只能改工作目录里的文件：" + path;
            return outcome;
        }
        const std::string from = args.GetString("old");
        const std::string to = args.GetString("new");
        if (from.empty()) {
            outcome.text = "错误：old 不能为空";
            return outcome;
        }
        std::string error;
        std::string data = ReadFileLimited(file, &error);
        if (!error.empty()) {
            outcome.text = "错误：" + error;
            return outcome;
        }
        std::size_t count = 0;
        for (std::size_t at = data.find(from); at != std::string::npos;
             at = data.find(from, at + to.size())) {
            data.replace(at, from.size(), to);
            ++count;
        }
        if (count == 0) {
            outcome.text = "没有找到要替换的内容（文件没变）";
            return outcome;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            outcome.text = "错误：写不进去，检查权限：" + file;
            return outcome;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        outcome.text = "替换了 " + std::to_string(count) + " 处";
        return outcome;
    }

    if (name == "make_dir") {
        const std::string dir = ResolvePathForWrite(workspace, path);
        if (dir.empty()) {
            outcome.text = "错误：只能在允许的目录里建目录：" + path;
            return outcome;
        }
        std::error_code code;
        std::filesystem::create_directories(std::filesystem::path(dir), code);
        outcome.text = code ? ("错误：建不了目录：" + code.message()) : ("已建目录 " + dir);
        return outcome;
    }

    if (name == "move_file") {
        const std::string target = ResolvePathForWrite(workspace, args.GetString("to"));
        const std::string from = ResolvePathForWrite(workspace, path);
        if (from.empty() || target.empty()) {
            outcome.text = "错误：源和目标都必须是工作目录里的路径";
            return outcome;
        }
        std::error_code code;
        std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(target), code);
        outcome.text = code ? ("错误：改名失败：" + code.message()) : ("已改名为 " + target);
        return outcome;
    }

    if (name == "delete_file") {
        const std::string file = ResolvePathForWrite(workspace, path);
        if (file.empty()) {
            outcome.text = "错误：只能删工作目录里的文件：" + path;
            return outcome;
        }
        std::error_code code;
        const bool removed = std::filesystem::remove(std::filesystem::path(file), code);
        if (code) {
            outcome.text = "错误：删不掉：" + code.message();
        } else if (!removed) {
            outcome.text = "错误：这个文件不存在（或者已经被删了）";
        } else {
            outcome.text = "已删除 " + file;
        }
        return outcome;
    }

    if (name == "run_command") {
        const std::string command = args.GetString("command");
        if (TrimText(command).empty()) {
            outcome.text = "错误：command 不能为空";
            return outcome;
        }
        int timeoutMs = args.GetInt("timeout_sec", 30) * 1000;
        if (timeoutMs <= 0 || timeoutMs > kMaxCommandTimeoutMs) timeoutMs = 30000;
        ProcessSpec spec;
        spec.cwd = workspace;
        spec.timeoutMs = timeoutMs;
#ifdef _WIN32
        spec.argv = {"cmd", "/c", command};
#else
        spec.argv = {"/bin/sh", "-c", command};
#endif
        std::string output;
        auto onLine = [&](const std::string& line) -> bool {
            if (output.size() < static_cast<std::size_t>(kMaxCommandBytes)) {
                output += line + "\n";
            }
            return true;
        };
        BrainRunResult result;
        std::string error;
        if (!RunBrain(spec, std::string(), onLine, &result, &error)) {
            outcome.text = "错误：命令起不来：" + error;
            return outcome;
        }
        std::string text = "$ " + command + "\n";
        text += output.empty() ? std::string("（没有输出）\n") : output;
        if (output.size() >= static_cast<std::size_t>(kMaxCommandBytes)) {
            text += "…（输出太多，截断了）\n";
        }
        if (result.timedOut) {
            text += "（超时被杀了，命令可能还没跑完）";
        } else {
            text += "退出码：" + std::to_string(result.exitCode);
        }
        if (!result.stderrTail.empty()) text += "\nstderr：" + result.stderrTail.substr(0, 2000);
        outcome.text = text;
        return outcome;
    }

    if (name == "send_file") {
        const std::string file = ResolveReadable(options, path);
        if (file.empty()) {
            outcome.text = "错误：这个文件不在允许发送的范围里：" + path;
            return outcome;
        }
        std::error_code code;
        if (!std::filesystem::is_regular_file(std::filesystem::path(file), code) || code) {
            outcome.text = "错误：不是普通文件（或者是目录、或者不存在）：" + file;
            return outcome;
        }
        outcome.sendFile = true;
        outcome.filePath = file;
        outcome.text = "会把 " + file + " 发到群里（已经排上队）";
        return outcome;
    }

    outcome.text = "错误：没有这个工具：" + name;
    return outcome;
}

}  // namespace

bool HttpPostJson(const std::string& url, const std::vector<std::string>& headers,
                  const std::string& body, int timeoutMs, std::string* response,
                  std::string* error) {
    if (response != nullptr) response->clear();
    if (error != nullptr) error->clear();
    const std::string bodyPath = UniqueTempPath("body");
    const std::string confPath = UniqueTempPath("conf");
    if (!WriteFileText(bodyPath, body)) {
        if (error != nullptr) *error = "写不了临时请求体：" + bodyPath;
        return false;
    }
    // key 放配置文件（POSIX 上 0600），不放命令行：命令行会出现在 ps 里
    std::string conf = "url = " + ConfigQuote(url) + "\n";
    conf += "request = \"POST\"\n";
    conf += "silent\nshow-error\n";
    conf += "max-time = " + std::to_string((timeoutMs + 999) / 1000) + "\n";
    conf += "header = " + ConfigQuote("Content-Type: application/json") + "\n";
    for (const std::string& header : headers) {
        conf += "header = " + ConfigQuote(header) + "\n";
    }
    conf += "data-binary = " + ConfigQuote("@" + bodyPath) + "\n";
    if (!WriteFileText(confPath, conf)) {
        std::remove(bodyPath.c_str());
        if (error != nullptr) *error = "写不了临时配置文件：" + confPath;
        return false;
    }
#ifndef _WIN32
    ::chmod(confPath.c_str(), 0600);
#endif
    ProcessSpec spec;
    spec.argv = {"curl", "-sS", "--config", confPath};
    spec.timeoutMs = timeoutMs + 15000;
    std::string output;
    auto onLine = [&](const std::string& line) -> bool {
        output += line + "\n";
        return true;
    };
    BrainRunResult result;
    std::string startError;
    const auto startedAt = std::chrono::steady_clock::now();
    const bool started = RunBrain(spec, std::string(), onLine, &result, &startError);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - startedAt)
                             .count();
    std::remove(bodyPath.c_str());
    std::remove(confPath.c_str());
    // 每次请求都记下"起没起、退出码、耗时、回了多少字节"：连不上/超时/被墙时
    // 这一行就是第一现场（api 模式联调全靠它）。
    std::fprintf(stderr, "[api] curl 结束：started=%d exit=%d timedOut=%d 耗时=%lldms 字节=%zu\n",
                 started ? 1 : 0, result.exitCode, result.timedOut ? 1 : 0,
                 static_cast<long long>(elapsed), output.size());
    std::fflush(stderr);
    if (!started) {
        if (error != nullptr) *error = "起不了 curl（是不是没装？）：" + startError;
        return false;
    }
    if (result.timedOut) {
        if (error != nullptr) *error = "请求超时（" + std::to_string(timeoutMs) + " ms）";
        return false;
    }
    if (result.exitCode != 0) {
        if (error != nullptr) {
            *error = "curl 退出码 " + std::to_string(result.exitCode) +
                     (result.stderrTail.empty()
                          ? std::string()
                          : "：" + TrimText(result.stderrTail.substr(0, 400)));
        }
        return false;
    }
    if (response != nullptr) *response = output;
    return true;
}

ApiTurnResult RunApiTurn(const ApiRunOptions& options, const std::vector<ApiMessage>& history,
                         const std::string& userText,
                         const std::function<void(const std::string&)>& log) {
    ApiTurnResult turn;
    if (TrimText(options.key).empty()) {
        turn.error = "还没配置 API Key（在管理界面「大脑」里填，或者给这个群单独填一个）";
        return turn;
    }
    const std::string systemPrompt = DefaultSystemPrompt(options);
    const std::string toolsJson =
        "[" +
        ToolJson("list_dir", "列出一个目录里有哪些文件和子目录",
                 ObjectSchema(Param("path", "string", "目录路径，留空是工作目录"), "[]")) +
        "," +
        ToolJson("read_file", "读一个文本文件的内容（最多 64 KB）",
                 ObjectSchema(Param("path", "string", "文件路径"), "[\"path\"]")) +
        "," +
        ToolJson("write_file", "把内容写进文件（覆盖原有内容）",
                 ObjectSchema(Param("path", "string", "文件路径") + "," +
                                  Param("content", "string", "要写入的完整内容"),
                              "[\"path\",\"content\"]")) +
        "," +
        ToolJson("append_file", "在文件末尾追加内容",
                 ObjectSchema(Param("path", "string", "文件路径") + "," +
                                  Param("content", "string", "追加的内容"),
                              "[\"path\",\"content\"]")) +
        "," +
        ToolJson("replace_in_file", "把文件里的一段文本替换成另一段",
                 ObjectSchema(Param("path", "string", "文件路径") + "," +
                                  Param("old", "string", "要被替换的原文（必须精确匹配）") + "," +
                                  Param("new", "string", "替换成什么"),
                              "[\"path\",\"old\",\"new\"]")) +
        "," +
        ToolJson("make_dir", "新建目录（父目录不存在会自动建）",
                 ObjectSchema(Param("path", "string", "目录路径"), "[\"path\"]")) +
        "," +
        ToolJson("move_file", "改名或移动文件",
                 ObjectSchema(Param("path", "string", "原路径") + "," +
                                  Param("to", "string", "新路径"),
                              "[\"path\",\"to\"]")) +
        "," +
        ToolJson("delete_file", "删除一个文件",
                 ObjectSchema(Param("path", "string", "要删的文件"), "[\"path\"]")) +
        "," +
        ToolJson("run_command", "在这台机器上跑一条 shell 命令并拿到输出",
                 ObjectSchema(Param("command", "string", "要执行的命令") + "," +
                                  Param("timeout_sec", "integer", "最多等几秒，默认 30"),
                              "[\"command\"]")) +
        "," +
        ToolJson("send_file", "把一个文件发到当前群聊",
                 ObjectSchema(Param("path", "string", "要发出去的文件路径"), "[\"path\"]")) +
        "]";

    std::string messages = "[" + MessageJson("system", systemPrompt);
    for (const ApiMessage& item : history) {
        if (item.content.empty()) continue;
        messages += "," + MessageJson(item.role.empty() ? "user" : item.role, item.content);
    }
    messages += "," + MessageJson("user", userText) + "]";

    const int rounds = options.maxRounds <= 0 ? 6 : options.maxRounds;
    for (int round = 0; round < rounds; ++round) {
        const std::string body = "{\"model\":" + JsonQuote(options.model) +
                                 ",\"messages\":" + messages + ",\"tools\":" + toolsJson +
                                 ",\"tool_choice\":\"auto\",\"stream\":false}";
        std::vector<std::string> headers;
        headers.push_back("Authorization: Bearer " + TrimText(options.key));
        std::string response;
        std::string error;
        log("第 " + std::to_string(round + 1) + " 轮请求 " + options.model + "（" +
            std::to_string(body.size()) + " 字节）");
        if (!HttpPostJson(options.url, headers, body, options.timeoutMs, &response, &error)) {
            turn.error = error;
            return turn;
        }
        JsonValue root;
        std::string parseError;
        if (!ParseJsonValue(response, &root, &parseError)) {
            turn.error = "接口返回的不是合法 JSON：" + parseError + "（原始内容：" +
                         response.substr(0, 300) + "）";
            return turn;
        }
        if (const JsonValue* apiError = root.Find("error")) {
            turn.error = "接口报错：" + apiError->GetString("message", "未知错误");
            return turn;
        }
        const std::vector<JsonValue> choices = root.GetArray("choices");
        if (choices.empty()) {
            turn.error = "接口没有返回 choices（原始内容：" + response.substr(0, 300) + "）";
            return turn;
        }
        const JsonValue* message = choices[0].Find("message");
        if (message == nullptr) {
            turn.error = "接口返回里没有 message（原始内容：" + response.substr(0, 300) + "）";
            return turn;
        }
        const std::string content = message->GetString("content");
        const std::vector<JsonValue> toolCalls = message->GetArray("tool_calls");
        if (toolCalls.empty()) {
            turn.answer = content;
            turn.ok = true;
            return turn;
        }

        std::string callsJson;
        int index = 0;
        for (const JsonValue& call : toolCalls) {
            std::string id = call.GetString("id");
            if (id.empty()) id = "call_" + std::to_string(round) + "_" + std::to_string(index);
            const JsonValue* function = call.Find("function");
            const std::string name =
                function != nullptr ? function->GetString("name") : std::string();
            const std::string rawArgs =
                function != nullptr ? function->GetString("arguments") : std::string("{}");
            if (!callsJson.empty()) callsJson += ",";
            callsJson += "{\"id\":" + JsonQuote(id) +
                         ",\"type\":\"function\",\"function\":{\"name\":" + JsonQuote(name) +
                         ",\"arguments\":" + JsonQuote(rawArgs) + "}}";
            ++index;
        }
        messages += ",{\"role\":\"assistant\",\"content\":" +
                    (content.empty() ? std::string("null") : JsonQuote(content)) +
                    ",\"tool_calls\":[" + callsJson + "]}";

        index = 0;
        for (const JsonValue& call : toolCalls) {
            const JsonValue* function = call.Find("function");
            const std::string name =
                function != nullptr ? function->GetString("name") : std::string();
            const std::string rawArgs =
                function != nullptr ? function->GetString("arguments") : std::string("{}");
            std::string id = call.GetString("id");
            if (id.empty()) id = "call_" + std::to_string(round) + "_" + std::to_string(index);
            ++index;
            JsonValue args;
            std::string argsError;
            if (!ParseJsonValue(rawArgs.empty() ? std::string("{}") : rawArgs, &args, &argsError)) {
                log("工具参数不是合法 JSON：" + argsError);
            }
            ToolOutcome outcome = ExecuteTool(options, name, args);
            log("工具 " + name + " → " + TrimText(outcome.text).substr(0, 200));
            if (outcome.sendFile) turn.files.push_back(outcome.filePath);
            messages += ",{\"role\":\"tool\",\"tool_call_id\":" + JsonQuote(id) +
                        ",\"content\":" + JsonQuote(outcome.text) + "}";
        }
    }
    turn.error = "工具来回太多（超过 " + std::to_string(rounds) + " 轮），先停了";
    return turn;
}

}  // namespace dshbot
}  // namespace dchat
