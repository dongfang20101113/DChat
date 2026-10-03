// 交互式聊天界面（交互模式）。
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
#include <string>
#include <vector>

#include "chat_color.h"
#include "input_history.h"
#include "protocol.h"
#include "render.h"
#include "server_command.h"

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

ChatUi::ChatUi(ClientConnection* connection, Terminal* terminal, FileTransfers* files)
    : connection_(connection), terminal_(terminal), files_(files) {}

void ChatUi::SetSelfNick(const std::string& nick) { selfNick_ = nick; }

void ChatUi::SetOnlineNicks(const std::vector<std::string>& nicks) { onlineNicks_ = nicks; }

void ChatUi::SendRecordedVoice(const std::string& path) {
    const int seconds = WavDurationSeconds(path);
    PrintLine("🎤 录好了（" + FormatDuration(seconds) + "），正在上传…");
    std::string error;
    // kind 传 "voice"：服务器和另外两端就靠这一格判断"这是语音"，
    // **不看扩展名**（安卓录的是 .m4a、桌面是 .wav，扩展名不可靠）
    const std::string localId = files_->Upload(path, "voice", &error);
    if (localId.empty()) {
        PrintLine("\x1b[31m语音发送失败：" + error + "\x1b[0m");
        return;
    }
    const std::string serverId = files_->ServerIdFor(localId);
    PrintLine(serverId.empty() ? "✅ 语音已发出，等服务器确认附件 id…"
                               : ("✅ 语音已发出（id=" + serverId + "）"));
}

void ChatUi::ShowStatus(const std::string& text) {
    // 接收线程调过来的：PrintAbove 只碰 stdout 和输入行，不碰别的状态，
    // 所以这里不需要额外加锁（界面主线程也在跑，但两者都只做"擦行+打印"）
    PrintLine(text);
}

void ChatUi::PrintLine(const std::string& text) {
    // 历史区输出和输入行是"抢"同一块终端的：必须先把输入行擦掉再打印，
    // 否则消息会叠在输入框上。
    terminal_->PrintAbove(text, kPrompt, input_, 0);
}

/** 把服务器来的一行渲染成带 ANSI 颜色的终端文字。 */
std::string ChatUi::RenderServerLine(const std::string& line) {
    const Message message = ParseLine(line);
    if (message.command == "ENC" || message.command == "PONG" ||
        message.command == "FILE_CHUNK") {
        return std::string();
    }

    std::string text;
    if (message.command == "SAY") {
        SayInfo info;
        if (!ParseSay(line, selfNick_, &info)) return std::string();
        const bool mention = !selfNick_.empty() && MentionsNick(info.text, selfNick_);
        if (mention) text += "\x1b[1;33m";  // 有人叫我：加粗黄
        text += "[" + info.time + "] <" + info.nick + "> ";
        if (mention) text += AnsiReset();
        // 正文按色码切段上色
        for (const ColorSegment& segment : ParseColorSegments(info.text, 0xDDDDDD, colorEnabled_)) {
            if (segment.hasColor) text += AnsiForeground(segment.rgb);
            text += segment.text;
            if (segment.hasColor) text += AnsiReset();
        }
        return text;
    }

    if (message.command == "SYS" || message.command == "ERROR") {
        std::vector<std::string> words = message.Words();
        if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
        std::string body;
        for (const std::string& word : words) {
            if (!body.empty()) body += " ";
            body += word;
        }
        const char* color = message.command == "ERROR" ? "\x1b[31m" : "\x1b[36m";
        return std::string(color) + "[系统] " + body + AnsiReset();
    }

    if (message.command == "RULES") {
        // 里面带 chatcolor=0|1，决定要不要解析色码
        std::vector<std::string> words = message.Words();
        if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
        std::string body;
        for (const std::string& word : words) {
            if (!body.empty()) body += " ";
            body += word;
        }
        colorEnabled_ = ChatColorEnabledFromRules(body);
        return std::string("\x1b[90m[规则] ") +
               (colorEnabled_ ? "彩色聊天：开" : "彩色聊天：关") + AnsiReset();
    }

    std::vector<std::string> words = message.Words();
    if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
    std::string body;
    for (const std::string& word : words) {
        if (!body.empty()) body += " ";
        body += word;
    }
    return "\x1b[90m[" + message.command + "] " + body + AnsiReset();
}

void ChatUi::HandleServerLine(const std::string& line) {
    const Message message = ParseLine(line);
    if (message.command == "LOGGEDIN") {
        loggedIn_ = true;
        std::vector<std::string> words = message.Words();
        if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
        if (!words.empty()) selfNick_ = words[0];
    } else if (message.command == "NAMES") {
        std::vector<std::string> words = message.Words();
        if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
        onlineNicks_.clear();
        for (const std::string& word : words) {
            if (word != selfNick_) onlineNicks_.push_back(word);
        }
    }

    std::string rendered = RenderServerLine(line);
    if (rendered.empty()) return;

    // 按终端宽度硬折行：终端自己也会折，但我们得知道"输入行被推到了第几行"，
    // 所以自己折更可控。
    const int width = terminal_->Width();
    if (width <= 0) {
        PrintLine(rendered);
        return;
    }
    std::string current;
    int visible = 0;
    for (std::size_t i = 0; i < rendered.size(); ++i) {
        if (rendered[i] == '\x1b') {  // ANSI 序列不算宽度，原样抄进当前行
            const std::size_t end = rendered.find('m', i);
            if (end == std::string::npos) break;
            current += rendered.substr(i, end - i + 1);
            i = end;
            continue;
        }
        current.push_back(rendered[i]);
        ++visible;
        if (visible >= width) {
            PrintLine(current);
            current.clear();
            visible = 0;
        }
    }
    if (!current.empty() || rendered.empty()) PrintLine(current);
}

void ChatUi::CompleteInput() {
    dchat::CompletionResult result = completer_.Next(input_, onlineNicks_, knownNicks_);
    if (result.picked < 0) return;
    input_ = result.text;
    // 候选多于一个时提示一句，让用户知道多按几下能循环
    if (result.matches.size() > 1) {
        std::string hint = "\x1b[90m[候选] ";
        for (std::size_t i = 0; i < result.matches.size(); ++i) {
            if (i > 0) hint += "  ";
            hint += (static_cast<int>(i) == result.picked) ? "\x1b[1;36m" : "\x1b[90m";
            hint += result.matches[i];
        }
        hint += AnsiReset();
        PrintLine(hint);
    }
}

bool ChatUi::HandleLocalCommand(const std::string& text) {
    if (text.empty() || text[0] != '/') return false;
    const std::size_t space = text.find(' ');
    const std::string name = text.substr(0, space == std::string::npos ? text.size() : space);

    if (name == "/quit" || name == "/exit") {
        wantQuit_ = true;
        return true;
    }
    if (name == "/help") {
        PrintLine("\x1b[1m可用指令：\x1b[0m");
        std::string row = "  ";
        for (const std::string& command : dchat::AllCommandNames()) {
            row += "/" + command + "  ";
            if (row.size() > 72) {
                PrintLine(row);
                row = "  ";
            }
        }
        if (row.size() > 2) PrintLine(row);
        PrintLine("  本地指令：/send <路径> 发文件   /get <id> 下载附件");
        PrintLine("            /voice [秒数] 录音（再敲一次停）  /play <id> 播放");
        PrintLine("            /quit 退出  /clear 清屏  /chatcolor on|off 彩色开关");
        return true;
    }
    if (name == "/voice" || name == "/send") {
        if (!loggedIn_) {
            PrintLine("还没登录完成，稍等一下再发");
            return true;
        }
    }
    if (name == "/voice") {
        const std::string arg = space == std::string::npos ? std::string() : text.substr(space + 1);
        if (recorder_.Recording()) {
            // 再敲一次 /voice 就是停
            std::string path;
            std::string error;
            if (!recorder_.StopAndSave(&path, &error)) {
                PrintLine("\x1b[31m录音失败：" + error + "\x1b[0m");
                return true;
            }
            SendRecordedVoice(path);
            return true;
        }
        int seconds = 0;
        if (!arg.empty()) seconds = std::atoi(arg.c_str());
        std::string error;
        if (!recorder_.Start(seconds, &error)) {
            PrintLine("\x1b[31m录不了音：" + error + "\x1b[0m");
            return true;
        }
        PrintLine("🎤 正在录音…再敲一次 /voice 就停（最长 " +
                  std::to_string(seconds > 0 ? seconds : 64) + " 秒）");
        return true;
    }
    if (name == "/play") {
        const std::string id = space == std::string::npos ? std::string() : text.substr(space + 1);
        if (id.empty()) {
            PrintLine("用法：/play <附件 id>（语音会自动下载，也可以手动放别的音频）");
            return true;
        }
        const std::string path = files_->LocalPathFor(id);
        std::string target = path;
        if (target.empty()) {
            // 还没下过：先请求下载，等落盘后再让用户敲一次；直接提示比默默等待好
            std::string error;
            if (!files_->RequestDownload(id, &error)) {
                PrintLine("\x1b[31m" + error + "\x1b[0m");
            } else {
                PrintLine("正在下载 " + id + "…下好后再敲一次 /play " + id);
            }
            return true;
        }
        PrintLine("▶ 播放 " + target);
        std::string error;
        if (!PlayAudioFile(target, 120000, &error)) {
            PrintLine("\x1b[31m播放失败：" + error + "\x1b[0m");
        }
        return true;
    }
    if (name == "/send") {
        const std::string path =
            space == std::string::npos ? std::string() : text.substr(space + 1);
        if (path.empty()) {
            PrintLine("用法：/send <文件路径> [voice|sticker]");
            return true;
        }
        PrintLine("正在上传 " + path + " …");
        std::string error;
        const std::string localId = files_->Upload(path, std::string(), &error);
        if (localId.empty()) {
            PrintLine("\x1b[31m上传失败：" + error + "\x1b[0m");
        } else {
            // 服务器会自己分配附件 ID（形如 F1）并在 FILE_OFFER 里广播回来；
            // 那条广播很快就会到，界面上会显示正确的 /get id
            const std::string serverId = files_->ServerIdFor(localId);
            PrintLine(serverId.empty()
                          ? "✅ 文件已上传，等服务器确认附件 id…"
                          : ("✅ 文件已上传（id=" + serverId + "），可以 /get " + serverId +
                             " 下载"));
        }
        return true;
    }
    if (name == "/get") {
        const std::string id = space == std::string::npos ? std::string() : text.substr(space + 1);
        if (id.empty()) {
            PrintLine("用法：/get <附件 id>（收到附件时消息里会带 id）");
            return true;
        }
        std::string error;
        if (!files_->RequestDownload(id, &error)) {
            PrintLine("\x1b[31m下载请求失败：" + error + "\x1b[0m");
        } else {
            PrintLine("已请求下载 " + id + "，保存到 " + files_->DownloadDir() + "/");
        }
        return true;
    }
    if (name == "/clear") {
        terminal_->Write("\x1b[2J\x1b[H");
        terminal_->RedrawInput(kPrompt, input_, 0);
        return true;
    }
    if (name == "/chatcolor") {
        // 本机开关：服务器没关的话，让用户能自己关掉彩色显示
        const std::string rest = space == std::string::npos ? std::string() : text.substr(space + 1);
        if (rest == "on" || rest == "1") {
            colorEnabled_ = true;
            PrintLine("彩色聊天：开");
        } else if (rest == "off" || rest == "0") {
            colorEnabled_ = false;
            PrintLine("彩色聊天：关");
        } else {
            PrintLine(colorEnabled_ ? "彩色聊天当前是开的（/chatcolor off 关掉）"
                                    : "彩色聊天当前是关的（/chatcolor on 打开）");
            PrintLine("颜色写法：&0-&f 是十六个快捷色，&#rrggbb 是真彩色，&& 是一个 &");
        }
        return true;
    }
    return false;  // 其余（/ban /kick /say ...）交给服务器处理
}

int ChatUi::Run() {
    if (!terminal_->EnterRawMode()) {
        std::printf("这个终端不支持交互模式（不是 tty）。\n");
        std::printf("可以改用批处理模式，例如：\n");
        std::printf("  dchat-client_linux --user 名字 --pass 密码 --send \"你好\" "
                    "--expect-any \"你好\"\n");
        return 1;
    }

    PrintLine("\x1b[1mdchat\x1b[0m 已连接。输入内容回车发送，Tab 补全指令与昵称，/help 看帮助，"
              "/quit 退出。");

    InputHistory history;
    const int keyUp = EncodeSequence("\x1b[A");
    const int keyDown = EncodeSequence("\x1b[B");
    const int keyLeft = EncodeSequence("\x1b[D");
    const int keyRight = EncodeSequence("\x1b[C");

    terminal_->RedrawInput(kPrompt, input_, 0);
    while (!wantQuit_) {
        if (!connection_->Running()) {
            PrintLine("\x1b[31m[断开] 与服务器的连接已断开\x1b[0m");
            break;
        }
        const int key = terminal_->ReadKey(120);
        if (key == 0) continue;

        if (key == '\r' || key == '\n') {
            const std::string text = input_;
            input_.clear();
            terminal_->RedrawInput(kPrompt, input_, 0);
            if (text.empty()) continue;
            if (!HandleLocalCommand(text)) {
                history.Add(text);
                if (!connection_->SendLine(dchat::BuildLine("MSG", dchat::EscapeText(text)))) {
                    PrintLine("\x1b[31m[失败] 发不出去（连接可能已断开）\x1b[0m");
                }
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
