// 交互式聊天界面（终端）。
//
// 这一层只做**终端相关**的事：读键、画输入行、把渲染好的行打出去。
// 会话逻辑（登录注册、收发、指令派发、附件、TOFU）全在 client_core/chat_core 里 ——
// 和 macOS 的 Cocoa 界面共用同一份，所以两个界面不会各写一套指令解析。
//
// 布局是"历史区 + 固定输入行"：消息往上滚，输入行永远留在最后一行。
// 每输出一条消息就擦掉输入行、打印消息、再把输入行画回来。
//
// 已知取舍：**光标编辑只在行尾**。左右方向键不动光标，退格删末尾。
// 原因是终端里"光标位置"按显示格算、而输入字符串按 UTF-8 字节算，中文一个字
// 占两格但三字节，做行内编辑要把两种情况都算对才不歪；本端先把功能跑通，
// 行内编辑留到后面单独做（Windows 端是自绘输入框，没有这个约束）。
#include "ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cli_render.h"
#include "input_history.h"

namespace dchat {
namespace {

constexpr const char* kPrompt = "> ";

/** 方向键序列编码后的伪按键码（和 Terminal::ReadKey 的编码方式对应）。 */
int EncodeSequence(const char* text) {
    int code = 0x100;
    for (const char* it = text; *it != '\0'; ++it) code = code * 128 + static_cast<unsigned char>(*it);
    return code;
}

}  // namespace

ChatUi::ChatUi(Terminal* terminal)
    : terminal_(terminal), core_(std::make_unique<ChatCore>(this)) {}

ChatUi::~ChatUi() = default;

ChatCore* ChatUi::Core() { return core_.get(); }

// ---------------------------------------------------------------------------
// ChatCoreDelegate：接收线程上触发，这里只做"渲染 + 打印"
// ---------------------------------------------------------------------------

void ChatUi::OnChatMessage(const ChatMessage& message) {
    const RenderedLine rendered = RenderChatMessage(message, core_->ColorEnabled());
    if (!rendered.visible) return;
    PrintLine(rendered.text);
}

void ChatUi::OnConnected(bool encrypted, const std::string& fingerprint,
                         const TrustDecision& trust) {
    if (!encrypted) {
        PrintLine("\x1b[33m[安全] 这次连接**没有加密**（服务器不支持或握手失败）\x1b[0m");
        return;
    }
    PrintLine("[安全] " + DescribeTrust(trust));
    PrintLine("\x1b[90m[安全] 服务器指纹：" + fingerprint + "\x1b[0m");
}

void ChatUi::OnLoggedIn(const std::string& nick) {
    loggedIn_ = true;
    PrintLine("\x1b[1m已登录：\x1b[0m" + nick);
    ShowHint();
}

void ChatUi::OnDisconnected(const std::string& reason) {
    PrintLine(std::string(kAnsiError) + "[断开] " +
              (reason.empty() ? std::string("与服务器的连接已断开") : reason) + kAnsiReset);
}

void ChatUi::OnOnlineNicks(const std::vector<std::string>& nicks) {
    // 补全用 ChatCore 内部那份（含 KNOWN 名单），这里不用再存
    (void)nicks;
}

void ChatUi::OnTransferProgress(const std::string& text) { PrintLine(text); }

void ChatUi::OnColorSettingChanged(bool enabled, const std::string& rawRest) {
    // rawRest 为空表示是本地开关（/chatcolor）改的，不用重复提示
    if (!rawRest.empty()) {
        PrintLine(std::string(kAnsiDim) + "[规则] " +
                  (enabled ? "彩色聊天：开" : "彩色聊天：关") + AnsiReset());
    }
}

void ChatUi::OnVoiceCommand(const std::string& arg) {
    if (!loggedIn_) {
        PrintLine("还没登录完成，稍等一下再发");
        return;
    }
    if (recorder_.Recording()) {
        // 再敲一次 /voice 就是停
        std::string path;
        std::string error;
        if (!recorder_.StopAndSave(&path, &error)) {
            PrintLine(std::string(kAnsiError) + "录音失败：" + error + AnsiReset());
            return;
        }
        SendRecordedVoice(path);
        return;
    }
    const int seconds = arg.empty() ? 0 : std::atoi(arg.c_str());
    std::string error;
    if (!recorder_.Start(seconds, &error)) {
        PrintLine(std::string(kAnsiError) + "录不了音：" + error + AnsiReset());
        return;
    }
    PrintLine("🎤 正在录音…再敲一次 /voice 就停（最长 " +
              std::to_string(seconds > 0 ? seconds : 64) + " 秒）");
}

// ---------------------------------------------------------------------------
// 终端输出
// ---------------------------------------------------------------------------

void ChatUi::PrintLine(const std::string& text) {
    // 历史区输出和输入行是"抢"同一块终端的：必须先把输入行擦掉再打印，
    // 否则消息会叠在输入框上。
    terminal_->PrintAbove(text, kPrompt, input_, 0);
}

void ChatUi::PrintWrapped(const std::string& text) {
    // 自己按终端宽度折行：终端自己也会折，但我们得知道"输入行被推到了第几行"，
    // 所以自己折更可控。折行不能把 ANSI 序列截断（见 WrapAnsi 的注释）。
    for (const std::string& piece : WrapAnsi(text, terminal_->Width())) {
        PrintLine(piece);
    }
}

void ChatUi::ShowHint() {
    PrintLine("\x1b[1mdchat\x1b[0m 输入内容回车发送，Tab 补全指令与昵称，/help 看帮助，"
              "/quit 退出。");
}

void ChatUi::SendRecordedVoice(const std::string& path) {
    const int seconds = WavDurationSeconds(path);
    PrintLine("🎤 录好了（" + FormatDuration(seconds) + "），正在上传…");
    std::string error;
    // kind 传 "voice"：服务器和另外三端就靠这一格判断"这是语音"，
    // **不看扩展名**（安卓录的是 .m4a、桌面是 .wav，扩展名不可靠）
    const std::string localId = core_->Files().Upload(path, "voice", &error);
    if (localId.empty()) {
        PrintLine(std::string(kAnsiError) + "语音发送失败：" + error + AnsiReset());
        return;
    }
    const std::string serverId = core_->Files().ServerIdFor(localId);
    PrintLine(serverId.empty() ? "✅ 语音已发出，等服务器确认附件 id…"
                               : ("✅ 语音已发出（id=" + serverId + "）"));
}

// ---------------------------------------------------------------------------
// 输入行
// ---------------------------------------------------------------------------

void ChatUi::CompleteInput() {
    const CompletionResult result = core_->Complete(input_);
    if (result.picked < 0) return;
    input_ = result.text;
    // 候选多于一个时提示一句，让用户知道多按几下能循环
    if (result.matches.size() > 1) {
        std::string hint = std::string(kAnsiDim) + "[候选] ";
        for (std::size_t i = 0; i < result.matches.size(); ++i) {
            if (i > 0) hint += "  ";
            hint += (static_cast<int>(i) == result.picked) ? "\x1b[1;36m" : kAnsiDim;
            hint += result.matches[i];
        }
        hint += AnsiReset();
        PrintLine(hint);
    }
}

/** 本地指令里只剩"纯终端"的那两个：清屏和退出。其余在 ChatCore 里。 */
bool ChatUi::HandleTerminalOnlyCommand(const std::string& text) {
    if (text.empty() || text[0] != '/') return false;
    const std::size_t space = text.find(' ');
    const std::string name = text.substr(0, space == std::string::npos ? text.size() : space);
    if (name == "/clear") {
        terminal_->Write("\x1b[2J\x1b[H");
        terminal_->RedrawInput(kPrompt, input_, 0);
        return true;
    }
    // /voice 和 /send 在登录前拦一下：服务器会直接拒绝（"请先登录后再传文件"），
    // 用户看到的却只是一句莫名其妙的失败。本地先拦，提示更直白。
    if (name == "/voice" || name == "/send") {
        if (!loggedIn_) {
            PrintLine("还没登录完成，稍等一下再发");
            return true;
        }
    }
    return false;
}

int ChatUi::Run() {
    if (!terminal_->EnterRawMode()) {
        std::printf("这个终端不支持交互模式（不是 tty）。\n");
        std::printf("可以改用批处理模式，例如：\n");
        std::printf("  dchat-client_linux --user 名字 --pass 密码 --send \"你好\" "
                    "--expect-any \"你好\"\n");
        return 1;
    }

    ShowHint();

    InputHistory history;
    const int keyUp = EncodeSequence("\x1b[A");
    const int keyDown = EncodeSequence("\x1b[B");
    const int keyLeft = EncodeSequence("\x1b[D");
    const int keyRight = EncodeSequence("\x1b[C");

    terminal_->RedrawInput(kPrompt, input_, 0);
    while (!wantQuit_) {
        if (!core_->Connected()) {
            // 断开提示由 OnDisconnected 打过了；这里只退出循环
            break;
        }
        const int key = terminal_->ReadKey(120);
        if (key == 0) continue;

        if (key == '\r' || key == '\n') {
            const std::string text = input_;
            input_.clear();
            terminal_->RedrawInput(kPrompt, input_, 0);
            if (text.empty()) continue;
            if (!HandleTerminalOnlyCommand(text)) {
                history.Add(text);
                // 指令派发（本地指令 + 发给服务器）全在 ChatCore 里，
                // 和 macOS 界面走的是同一条路径
                core_->SubmitInput(text);
                if (text == "/quit" || text == "/exit") wantQuit_ = true;
            }
            continue;
        }
        if (key == 0x7F || key == 0x08) {  // 退格：删掉最后一个 UTF-8 字符
            if (!input_.empty()) {
                std::size_t cut = input_.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(input_[cut]) & 0xC0) == 0x80) --cut;
                input_.erase(cut);
            }
            terminal_->RedrawInput(kPrompt, input_, input_.size());
            continue;
        }
        if (key == 0x09) {  // Tab
            CompleteInput();
            terminal_->RedrawInput(kPrompt, input_, input_.size());
            continue;
        }
        if (key == 0x03) {  // Ctrl+C：直接退
            wantQuit_ = true;
            break;
        }
        if (key == 0x15) {  // Ctrl+U：清空当前输入
            input_.clear();
            terminal_->RedrawInput(kPrompt, input_, 0);
            continue;
        }
        if (key == keyUp) {
            input_ = history.Up(&input_) ? input_ : input_;
            terminal_->RedrawInput(kPrompt, input_, input_.size());
            continue;
        }
        if (key == keyDown) {
            input_ = history.Down(&input_) ? input_ : input_;
            terminal_->RedrawInput(kPrompt, input_, input_.size());
            continue;
        }
        if (key == keyLeft || key == keyRight) {
            continue;  // 行内编辑暂不支持（见文件头说明），按了不做事
        }
        if (key >= 0x100) continue;  // 其它转义序列
        if (key < 0x20) continue;    // 其它控制字符

        input_.push_back(static_cast<char>(key));
        terminal_->RedrawInput(kPrompt, input_, input_.size());
    }

    terminal_->Restore();
    return 0;
}

}  // namespace dchat
