// 测试用的"假大脑"：假装自己是 `dsh headless --json -`
//
// 端到端测试不能真的去调 DeepSeek Harness —— 那样每跑一次测试要几十秒、要联网、
// 要登录状态，还会把真实模型的不确定性带进断言里。这个假程序只做一件事：
// **按 DSH 的 NDJSON 格式吐事件**，于是机器人那条路上除了"模型怎么想"以外的
// 每一环（拉子进程、读事件、分条、按节奏发、白黑名单）都是真跑。
//
// 用法：
//   fake_dsh_brain [--text <正文>] [--session <id>] [--delay <毫秒>] [--fail] [--echo]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

/**
 * 取 UTF-8 的 argv。
 *
 * ⚠️ Windows 上 MinGW 的 `main(argc, argv)` 是按**系统代码页**（中文机器上是 GBK）
 * 转换来的，直接拿它当文本就会把中文变成 GBK 字节 —— 假大脑于是吐出非法 UTF-8 的
 * JSON，机器人原样发进群里，看起来像"机器人乱码"。所以必须走宽字符再转 UTF-8。
 */
std::vector<std::string> Utf8Args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> out;
    if (wide == nullptr) return out;
    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 1 ? static_cast<std::size_t>(size - 1) : 0, '\0');
        if (size > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &text[0], size, nullptr, nullptr);
        }
        out.push_back(text);
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

/**
 * 打印一条 JSON 字符串。
 *
 * 必须正经转义：假数据里带换行/引号时，如果直接塞进 JSON，机器人那边的解析器会
 * 拒收 —— 那测试就变成"验测试工具"了。
 */
std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out += "\"";
    return out;
}

void Emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = Utf8Args(argc, argv);
    std::string text;
    std::string session = "fake-session-1";
    int delayMs = 0;
    bool fail = false;
    bool echo = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string arg = args[i];
        if (arg == "--text" && i + 1 < args.size()) {
            text = args[++i];
        } else if (arg == "--session" && i + 1 < args.size()) {
            session = args[++i];
        } else if (arg == "--delay" && i + 1 < args.size()) {
            delayMs = std::atoi(args[++i].c_str());
        } else if (arg == "--patch" && i + 1 < args.size()) {
            // dsh 的 --patch：机器人会按权限模式自动挂上补丁文件。
            // 假大脑不加载任何补丁，但必须认这个参数 —— 否则整条调用链会以
            // "未知参数"退出，测试就变成在验参数解析，而不是验机器人。
            ++i;
        } else if (arg == "--fail") {
            fail = true;
        } else if (arg == "--echo") {
            echo = true;
        } else {
            std::fprintf(stderr, "fake_dsh_brain: 未知参数 %s\n", arg.c_str());
            return 2;
        }
    }

    // 把提示词读干净：机器人是从 stdin 喂进来的。不读的话写端可能被阻塞。
    std::string prompt;
    {
        char buffer[4096];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
            prompt.append(buffer, got);
        }
    }

    if (fail) {
        std::fprintf(stderr, "fake_dsh_brain: 故意失败（--fail）\n");
        return 3;
    }

    if (echo) {
        text = "提示词是：" + prompt;
    }
    if (text.empty()) {
        text = "收到。";
    }

    Emit("{\"type\":\"session\",\"sessionId\":" + JsonString(session) + ",\"cwd\":\".\"}");
    Emit("{\"type\":\"status\",\"phase\":\"turn_start\",\"turn\":1}");
    Emit("{\"type\":\"thinking\",\"text\":\"\"}");
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    Emit("{\"type\":\"text\",\"text\":" + JsonString(text) + "}");
    Emit("{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"kind\":\"completed\"}}");
    Emit("{\"type\":\"final\",\"text\":" + JsonString(text) + "}");
    return 0;
}
