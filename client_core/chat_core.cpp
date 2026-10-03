#include "chat_core.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#include "chat_color.h"  // ChatColorEnabledFromRules：服务器可以关掉彩色聊天
#include "protocol.h"
#include "render.h"

namespace dchat {
namespace {

std::string ReadFileOrEmpty(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

/** 把一行里的 hh:mm 时间戳摘掉，返回剩下的字段。 */
std::vector<std::string> FieldsOf(const Message& message, std::string* timeOut) {
    std::vector<std::string> words = message.Words();
    if (!words.empty() && LooksLikeTime(words[0])) {
        if (timeOut) *timeOut = words[0];
        words.erase(words.begin());
    }
    return words;
}

std::string JoinWords(const std::vector<std::string>& words, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i) {
        if (!out.empty()) out += " ";
        out += words[i];
    }
    return out;
}

}  // namespace

ChatCore::ChatCore(ChatCoreDelegate* delegate)
    : delegate_(delegate), files_(std::make_unique<FileTransfers>(&connection_)) {}

ChatCore::~ChatCore() { connection_.Close(); }

bool ChatCore::Connect(const ConnectOptions& options, std::string* error) {
    host_ = options.host;
    port_ = options.port;

    if (!connection_.Connect(host_, port_, error)) return false;

    // ---- TOFU：连上就先比对服务器指纹 ----
    // 这一步必须在登录之前：指纹不对就不该把密码发出去。
    std::string fingerprint;
    TrustDecision trust;
    if (connection_.Encrypted()) {
        fingerprint = PublicKeyFingerprint(connection_.ServerPublicKey());
        const std::string key = KnownServerKey(host_, port_);
        const std::string content = ReadFileOrEmpty(knownServersPath_);
        trust = DecideTrust(LookupKnownFingerprint(content, key), fingerprint);
        if (trust.kind == TrustKind::FirstUse || trust.kind == TrustKind::Changed) {
            WriteFile(knownServersPath_, UpsertKnownFingerprint(content, key, fingerprint));
        }
    }
    if (delegate_) delegate_->OnConnected(connection_.Encrypted(), fingerprint, trust);
    if (trust.kind == TrustKind::Changed) {
        // 指纹变了：**中止**，不把密码送出去。用户确认后应删掉备忘文件重连。
        Notice("服务器指纹变了，已中止连接以防中间人。确认管理员换过密钥后，删除 " +
                   knownServersPath_ + " 再连。",
               ChatMessage::Kind::Error);
        connection_.Close();
        return false;
    }

    // ---- 接收线程：先挂上回调，再发登录请求，避免漏掉第一行 ----
    connection_.StartReceiveLoop([this](const std::string& line) { HandleLine(line); });

    if (!options.user.empty()) {
        const std::string command = options.wantRegister ? "REGISTER" : "LOGIN";
        if (!connection_.SendLine(BuildLine(command, EscapeText(options.user + " " +
                                                                 options.password)))) {
            if (error) *error = "登录请求发不出去（连接可能已断开）";
            return false;
        }
    }
    return true;
}

void ChatCore::Disconnect() { connection_.Close(); }

void ChatCore::Notice(const std::string& text, ChatMessage::Kind kind) {
    ChatMessage message;
    message.kind = kind;
    message.text = text;
    message.valid = true;
    Push(message);
}

void ChatCore::Push(ChatMessage message) {
    if (delegate_) delegate_->OnChatMessage(message);
}

void ChatCore::HandleSay(const std::string& line) {
    SayInfo info;
    if (!ParseSay(line, selfNick_, &info)) return;

    ChatMessage message;
    message.valid = true;
    message.time = info.time;
    message.nick = info.nick;
    message.text = info.text;
    message.hasColor = info.text.find('&') != std::string::npos;
    // 自己发的消息用 Say 行回显（服务器会把消息广播回给发送者）
    message.kind = (!selfNick_.empty() && info.nick == selfNick_) ? ChatMessage::Kind::Own
                                                                  : ChatMessage::Kind::Say;
    message.mentionMe = !selfNick_.empty() && MentionsNick(info.text, selfNick_);
    Push(message);
}

void ChatCore::HandleLine(const std::string& line) {
    // 文件相关的行先给文件模块，它认领了就不往界面送
    if (files_ && files_->HandleLine(line, [this](const std::string& text) {
            if (delegate_) delegate_->OnTransferProgress(text);
        })) {
        return;
    }

    const Message message = ParseLine(line);
    if (message.command == "ENC" || message.command == "PONG") return;

    if (message.command == "SAY") {
        HandleSay(line);
        return;
    }

    std::string time;
    const std::vector<std::string> words = FieldsOf(message, &time);
    const std::string body = JoinWords(words, 0);

    if (message.command == "WELCOME") {
        welcomed_ = true;
        Notice("已连接：" + body);
        return;
    }
    if (message.command == "LOGGEDIN") {
        if (!words.empty()) selfNick_ = words[0];
        if (delegate_) delegate_->OnLoggedIn(selfNick_);
        return;
    }
    if (message.command == "NAMES") {
        onlineNicks_.clear();
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i] != selfNick_) onlineNicks_.push_back(words[i]);
        }
        if (delegate_) delegate_->OnOnlineNicks(onlineNicks_);
        return;
    }
    if (message.command == "KNOWN") {
        knownNicks_ = words;
        return;
    }
    if (message.command == "RULES") {
        // 服务器可能把彩色聊天关了，界面要跟着改
        colorEnabled_ = ChatColorEnabledFromRules(body);
        if (delegate_) delegate_->OnColorSettingChanged(colorEnabled_, body);
        return;
    }
    if (message.command == "ERROR") {
        Notice(body, ChatMessage::Kind::Error);
        return;
    }
    if (message.command == "JOINED" || message.command == "LEFT") {
        Notice(body, ChatMessage::Kind::Notice);
        return;
    }
    if (message.command == "SYS") {
        Notice(body, ChatMessage::Kind::System);
        return;
    }
    if (message.command == "FILE_OFFER") {
        const FileOffer offer = ParseFileOffer(line);
        if (!offer.valid) return;
        ChatMessage card;
        card.valid = true;
        card.kind = ChatMessage::Kind::FileOffer;
        card.nick = offer.owner;
        card.fileId = offer.id;
        card.isVoice = offer.IsVoice();
        card.text = offer.Describe();
        Push(card);
        return;
    }
    if (message.command == "FILE_FAIL") {
        Notice("附件下载失败：" + body, ChatMessage::Kind::Error);
        return;
    }
    if (!body.empty()) Notice(body);
}

bool ChatCore::SendMessage(const std::string& text) {
    if (text.empty()) return false;
    // EscapeText 处理多行：协议是行式的，真换行必须转义，否则一条消息会被拆成多条命令
    return connection_.SendLine(BuildLine("MSG", EscapeText(text)));
}

void ChatCore::SetColorEnabled(bool enabled) {
    colorEnabled_ = enabled;
    if (delegate_) delegate_->OnColorSettingChanged(colorEnabled_, std::string());
}

CompletionResult ChatCore::Complete(const std::string& text) {
    return completer_.Next(text, onlineNicks_, knownNicks_);
}

void ChatCore::SubmitInput(const std::string& text) {
    if (text.empty()) return;
    if (text[0] != '/') {
        if (!SendMessage(text)) Notice("发送失败：连接可能已断开", ChatMessage::Kind::Error);
        return;
    }

    // ---- 本地指令（不发给服务器）----
    const std::size_t space = text.find(' ');
    const std::string name = text.substr(0, space == std::string::npos ? text.size() : space);
    const std::string arg = space == std::string::npos ? std::string() : text.substr(space + 1);

    if (name == "/help") {
        std::string list = "服务器指令：";
        for (const std::string& command : AllCommandNames()) list += " /" + command;
        Notice(list);
        Notice("本地指令：/send <路径> 发文件　/get <id> 下载　/voice 录音　/play <id> 放音　"
               "/chatcolor on|off　/quit 退出");
        return;
    }
    if (name == "/chatcolor") {
        if (arg == "on" || arg == "1") {
            SetColorEnabled(true);
            Notice("彩色聊天：开");
        } else if (arg == "off" || arg == "0") {
            SetColorEnabled(false);
            Notice("彩色聊天：关");
        } else {
            Notice(colorEnabled_ ? "彩色聊天当前是开的（/chatcolor off 关掉）"
                                 : "彩色聊天当前是关的（/chatcolor on 打开）");
        }
        return;
    }
    if (name == "/quit" || name == "/exit") {
        Disconnect();
        return;
    }
    if (name == "/send") {
        if (arg.empty()) {
            Notice("用法：/send <文件路径>", ChatMessage::Kind::Error);
            return;
        }
        Notice("正在上传 " + arg + " …");
        std::string error;
        const std::string localId = files_->Upload(arg, std::string(), &error);
        Notice(localId.empty() ? ("上传失败：" + error) : "文件已上传，等服务器确认附件 id…",
               localId.empty() ? ChatMessage::Kind::Error : ChatMessage::Kind::Notice);
        return;
    }
    if (name == "/get") {
        if (arg.empty()) {
            Notice("用法：/get <附件 id>", ChatMessage::Kind::Error);
            return;
        }
        std::string error;
        if (!files_->RequestDownload(arg, &error)) {
            Notice("下载请求失败：" + error, ChatMessage::Kind::Error);
        } else {
            Notice("已请求下载 " + arg + "，保存到 " + files_->DownloadDir() + "/");
        }
        return;
    }
    if (name == "/play") {
        if (arg.empty()) {
            Notice("用法：/play <附件 id>", ChatMessage::Kind::Error);
            return;
        }
        const std::string path = files_->LocalPathFor(arg);
        if (path.empty()) {
            std::string error;
            if (!files_->RequestDownload(arg, &error)) {
                Notice(error, ChatMessage::Kind::Error);
            } else {
                Notice("正在下载 " + arg + "…下好后再敲一次 /play " + arg);
            }
            return;
        }
        Notice("▶ 播放 " + path);
        std::string error;
        if (!PlayAudioFile(path, 120000, &error)) {
            Notice("播放失败：" + error, ChatMessage::Kind::Error);
        }
        return;
    }

    // 其余指令原样发给服务器（和另外两端一样）
    if (!connection_.SendLine(text)) {
        Notice("指令发不出去：连接可能已断开", ChatMessage::Kind::Error);
    }
}

}  // namespace dchat
