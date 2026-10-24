#include "chat_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "chat_color.h"  // ChatColorEnabledFromRules：服务器可以关掉彩色聊天
#include "file_transfer.h"  // Base64Encode / Base64Decode（群名、公告都走 Base64）
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

/**
 * 群名 / 群公告 / 群昵称在协议里都是 Base64（它们可能含空格，而协议是按空格分字段的）。
 * 解不出来就返回空串 —— 显示成空总比显示成一串乱码强。
 */
std::string DecodeB64(const std::string& text) {
    std::vector<unsigned char> bytes;
    if (!Base64Decode(text, &bytes)) return std::string();
    return std::string(bytes.begin(), bytes.end());
}

std::string EncodeB64(const std::string& text) {
    return Base64Encode(text);
}

}  // namespace

ChatCore::ChatCore(ChatCoreDelegate* delegate)
    : delegate_(delegate), files_(std::make_unique<FileTransfers>(&connection_)) {}

ChatCore::~ChatCore() { connection_.Close(); }

bool ChatCore::Connect(const ConnectOptions& options, std::string* error) {
    if (!ConnectOnly(options, error)) return false;
    if (options.user.empty()) return true;
    SetCredentials(options.user, options.password, options.wantRegister);
    if (!SubmitCredentials()) {
        if (error) *error = "登录请求发不出去（连接可能已断开）";
        return false;
    }
    return true;
}

bool ChatCore::ConnectOnly(const ConnectOptions& options, std::string* error) {
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
    return true;
}

void ChatCore::SetCredentials(const std::string& user, const std::string& password,
                              bool wantRegister) {
    pendingUser_ = user;
    pendingPassword_ = password;
    pendingRegister_ = wantRegister;
}

bool ChatCore::SubmitCredentials() {
    if (pendingUser_.empty()) return true;
    const std::string command = pendingRegister_ ? "REGISTER" : "LOGIN";
    return connection_.SendLine(
        BuildLine(command, EscapeText(pendingUser_ + " " + pendingPassword_)));
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
        // 声明多会话能力。服务端收到这条才会发 ROOMLIST / RINFO 这些；
        // 不声明就一直是"只有大厅"的老样子 —— **这也正是老客户端零影响的由来**。
        connection_.SendLine(BuildLine("FEATURES", "rooms1"));
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
    if (message.command == "RFILE_OFFER") {
        // RFILE_OFFER <房间ID> <上传者> <附件ID> <文件名B64> <字节数> <有缩略图> [种类]
        //
        // 大厅走 FILE_OFFER，其余会话在**最前面多一层房间号**。以前这里完全没处理，
        // 结果群里的附件卡片在共用这套解析的客户端（Linux CLI、网页版）上根本不出现
        // ——只能看到一行原始协议文本。桌面端有自己的解析器，所以只有这两个端受影响。
        //
        // 按房间分：就在当前会话就原地显示，不是当前会话就在正文前标上房间名
        // （和 RMSG 的处理方式一致）。
        if (words.size() >= 3) {
            std::string payload;
            for (std::size_t i = 1; i < words.size(); ++i) {
                if (!payload.empty()) payload += " ";
                payload += words[i];
            }
            const FileOffer offer = ParseFileOffer(payload);
            if (offer.valid) {
                ChatMessage card;
                card.valid = true;
                card.kind = ChatMessage::Kind::FileOffer;
                card.nick = offer.owner;
                card.fileId = offer.id;
                card.isVoice = offer.IsVoice();
                card.text = offer.Describe();
                if (words[0] != currentRoom_) {
                    const RoomInfo* room = FindRoom(words[0]);
                    const std::string label =
                        (room != nullptr && !room->name.empty()) ? room->name : words[0];
                    card.text = "[" + label + "] " + card.text;
                }
                Push(card);
            }
        }
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
    // 多会话那几条命令（ROOMLIST / RINFO / RSET / RMSG / GHIT …）认领掉
    if (HandleRoomLine(message.command, words, time)) return;

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
        Notice("多会话：/rooms 会话列表　/open <id> 切换　/create <id> <名> 建群　"
               "/find <关键词> 搜群　/join <id> 入群　/leave <id> 退群　"
               "/members 成员　/set <项> <值> 改群设置");
        Notice("群管理：/audit 待审申请　/ok <昵称> 同意　/no <昵称> 拒绝　"
               "/gkick <昵称> 踢人　/delete <id> 解散群（仅群主）");
        Notice("　　　　/role <昵称> <admin|member> 改角色　/announce [内容] 看/设群公告");
        Notice("私聊与黑名单：/block <昵称>　/unblock <昵称>　/blocks 看黑名单　"
               "/nick <群昵称> 改自己在当前群的昵称");
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
    if (name == "/voice") {
        // 录音是**界面层**的事：终端版用 MicRecorder 起 sox/arecord 子进程，
        // Cocoa 版还没做（会回一句"这个界面暂不支持录音"）。
        // 核心只负责"把这件事转交给界面"，这样两个界面共用同一套指令解析。
        if (delegate_) {
            delegate_->OnVoiceCommand(arg);
        } else {
            Notice("这个界面还不支持录音", ChatMessage::Kind::Error);
        }
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
        if (localId.empty()) {
            Notice("上传失败：" + error, ChatMessage::Kind::Error);
            return;
        }
        // 服务器会自己分配附件 id（形如 F1）并在 FILE_OFFER 里广播回来；
        // 那条广播很快就到。能立刻拿到 id 就说清楚怎么下载。
        const std::string serverId = files_->ServerIdFor(localId);
        Notice(serverId.empty()
                   ? "文件已上传，等服务器确认附件 id…"
                   : ("文件已上传（id=" + serverId + "），可以 /get " + serverId + " 下载"));
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

    // ---- 多会话 ----
    //
    // 这几条是**本地指令**：它们翻译成协议命令发出去（GCREATE / GSET …），
    // 而不是像别的 /foo 那样原样丢给服务器 —— 因为服务端只认协议命令，
    // 你在聊天框里敲 `/gcreate ...` 它是不认识的。
    if (name == "/rooms") {
        ShowRooms();
        return;
    }
    if (name == "/open") {
        SwitchRoom(arg);
        return;
    }
    if (name == "/create") {
        const std::size_t split = arg.find(' ');
        if (split == std::string::npos) {
            Notice("用法：/create <群ID> <群名>（ID 只能用 3-16 位小写字母/数字/下划线/减号）",
                   ChatMessage::Kind::Error);
            return;
        }
        const std::string id = arg.substr(0, split);
        const std::string display = arg.substr(split + 1);
        if (SendRoom("GCREATE", id + " " + EncodeB64(display))) {
            Notice("正在建群「" + display + "」（id=" + id + "）…");
        } else {
            Notice("建群失败：连接可能已断开", ChatMessage::Kind::Error);
        }
        return;
    }
    if (name == "/find") {
        if (arg.empty()) {
            Notice("用法：/find <关键词>", ChatMessage::Kind::Error);
            return;
        }
        groupHits_.clear();
        SendRoom("GSEARCH", EncodeB64(arg));
        return;
    }
    if (name == "/join") {
        if (arg.empty()) {
            Notice("用法：/join <群ID>", ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GJOIN", arg);
        return;
    }
    if (name == "/leave") {
        if (arg.empty()) {
            Notice("用法：/leave <群ID>（大厅不能退）", ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GLEAVE", arg);
        return;
    }
    if (name == "/members") {
        if (currentRoom_.empty()) {
            Notice("还没进任何会话", ChatMessage::Kind::Error);
            return;
        }
        if (members_.empty()) {
            Notice("成员表还没拉下来（或这个会话没有成员概念，比如大厅）");
            return;
        }
        for (const MemberInfo& member : members_) {
            std::string line = member.nick + "  " + member.role;
            if (!member.display.empty()) line += "  群昵称：" + member.display;
            Notice(line);
        }
        return;
    }
    if (name == "/set") {
        // /set <设置名> <值>：作用于**当前会话**，走 GSET（服务端会校验我是不是管理员）
        const std::size_t split = arg.find(' ');
        if (currentRoom_.empty() || split == std::string::npos) {
            Notice("用法：/set <设置名> <值>（先用 /open 切到一个群里）", ChatMessage::Kind::Error);
            return;
        }
        const std::string key = arg.substr(0, split);
        const std::string value = arg.substr(split + 1);
        // 群名和公告可能含空格，协议要求走 Base64；其余是纯值
        const std::string encoded =
            (key == "name" || key == "announcement") ? EncodeB64(value) : value;
        SendRoom("GSET", currentRoom_ + " " + key + " " + encoded);
        return;
    }

    if (name == "/gkick") {
        // 为什么叫 /gkick 不叫 /kick：**/kick 是服务器管理员指令**（把人踢出服务器），
        // 已经在服务端的指令表里。同名会把它顶掉，那是很危险的一脚。
        if (currentRoom_.empty() || arg.empty()) {
            Notice("用法：/gkick <昵称>（先用 /open 切到一个群里；你自己是群主或管理员）",
                   ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GKICK", currentRoom_ + " " + arg);
        return;
    }
    if (name == "/delete") {
        // 解散群聊：必须显式给 id，避免手滑把当前群点没了
        if (arg.empty()) {
            Notice("用法：/delete <群ID>（解散不可恢复；仅群主可用）", ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GDELETE", arg);
        return;
    }
    if (name == "/audit") {
        if (currentRoom_.empty()) {
            Notice("先用 /open 切到一个群里", ChatMessage::Kind::Error);
            return;
        }
        pending_.clear();
        SendRoom("GREQS", currentRoom_);
        return;
    }
    if (name == "/ok" || name == "/no") {
        if (currentRoom_.empty() || arg.empty()) {
            Notice("用法：" + name + " <昵称>（先用 /audit 看谁在等）", ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GREQ", currentRoom_ + " " + arg + (name == "/ok" ? " ok" : " no"));
        return;
    }
    if (name == "/announce") {
        // 不带参数 = 看当前群的公告；带参数 = 设公告（要管理员）
        if (currentRoom_.empty()) {
            Notice("先用 /open 切到一个群里", ChatMessage::Kind::Error);
            return;
        }
        const RoomInfo* room = FindRoom(currentRoom_);
        if (arg.empty()) {
            if (room != nullptr && !room->announcement.empty()) {
                Notice("群公告：" + room->announcement);
            } else {
                Notice("这个群没有公告　（要设的话：/announce <内容>）");
            }
            return;
        }
        SendRoom("GSET", currentRoom_ + " announcement " + EncodeB64(arg));
        return;
    }
    if (name == "/role") {
        // /role <昵称> <owner|admin|member>：改成员角色（群主或管理员）
        const std::size_t split = arg.find(' ');
        if (currentRoom_.empty() || split == std::string::npos) {
            Notice("用法：/role <昵称> <admin|member>（群主可以把别人设为管理员）",
                   ChatMessage::Kind::Error);
            return;
        }
        const std::string target = arg.substr(0, split);
        const std::string role = arg.substr(split + 1);
        if (role != "owner" && role != "admin" && role != "member") {
            Notice("角色只能是 owner / admin / member", ChatMessage::Kind::Error);
            return;
        }
        SendRoom("GMEMBER", currentRoom_ + " " + target + " " + role);
        return;
    }
    if (name == "/block" || name == "/unblock") {
        if (arg.empty()) {
            Notice("用法：" + name + " <昵称>", ChatMessage::Kind::Error);
            return;
        }
        connection_.SendLine(BuildLine(name == "/block" ? "BLOCK" : "UNBLOCK", arg));
        return;
    }
    if (name == "/blocks") {
        blocks_.clear();
        connection_.SendLine(BuildLine("BLOCKS"));
        return;
    }

    // 其余指令**当普通消息发出去**，不是原样甩给协议层。
    //
    // 服务器并不在协议层认 `/指令`：它是在 `MSG` 的正文里判断"这行以 / 开头"，
    // 再交给 ParseServerCommand 解析（见 server.cpp 的 IsCommandAttempt 那段）。
    // 所以直接把 `/say hi` 当成一行协议命令发过去，服务器只会看到一条不认识的
    // 命令（协议解析会把命令名转成大写，于是报「未知命令: /SAY」）。
    //
    // 这条以前一直是错的：Linux CLI 和网页版共用这套核心，/ban /kick /op /say
    // /dm /nick 这些**全都不生效**，只有桌面端（自己拼消息）是好的。
    if (!SendMessage(text)) {
        Notice("指令发不出去：连接可能已断开", ChatMessage::Kind::Error);
    }
}

// ---------------------------------------------------------------- 多会话

bool ChatCore::SendRoom(const std::string& command, const std::string& rest) {
    return connection_.SendLine(BuildLine(command, rest));
}

RoomInfo* ChatCore::FindRoom(const std::string& id) {
    for (RoomInfo& room : rooms_) {
        if (room.id == id) return &room;
    }
    return nullptr;
}

const RoomInfo* ChatCore::FindRoom(const std::string& id) const {
    for (const RoomInfo& room : rooms_) {
        if (room.id == id) return &room;
    }
    return nullptr;
}

void ChatCore::UpsertRoom(const RoomInfo& incoming) {
    RoomInfo* existing = FindRoom(incoming.id);
    if (existing == nullptr) {
        rooms_.push_back(incoming);
        return;
    }
    // **只覆盖服务端确实给了值的字段**。ROOMLIST 和 RINFO 带的信息量不一样
    // （ROOMLIST 没有群主/加群方式/是否公开），谁后到不能把已有字段清成空 ——
    // 否则会出现"群主一会儿有一会儿没有"这种看起来像服务端抽风的怪现象。
    if (!incoming.name.empty()) existing->name = incoming.name;
    if (!incoming.kind.empty()) existing->kind = incoming.kind;
    if (!incoming.owner.empty()) existing->owner = incoming.owner;
    if (incoming.members > 0) existing->members = incoming.members;
    if (incoming.maxMembers > 0) existing->maxMembers = incoming.maxMembers;
    if (!incoming.myRole.empty()) existing->myRole = incoming.myRole;
    if (!incoming.joinMode.empty()) existing->joinMode = incoming.joinMode;
    existing->listed = incoming.listed;
}

bool ChatCore::SwitchRoom(const std::string& id) {
    if (id.empty()) {
        Notice("用法：/open <会话ID>（先 /rooms 看有哪些）", ChatMessage::Kind::Error);
        return false;
    }
    const RoomInfo* room = FindRoom(id);
    if (room == nullptr) {
        Notice("没有这个会话：" + id + "（先 /rooms 看看）", ChatMessage::Kind::Error);
        return false;
    }
    // **必须真的发出去**：服务端记住"这个人当前在哪个房间"，MSG 按那个路由。
    // 只在本地改个变量的话，话还是说进原来那个房间（大厅）。
    if (!connection_.SendLine(BuildLine("ROOM", id))) {
        Notice("切换失败：连接可能已断开", ChatMessage::Kind::Error);
        return false;
    }
    currentRoom_ = id;
    members_.clear();  // 成员表按房间来，切了就等新的 RMEMBERS 填
    Notice("已切换到「" + (room->name.empty() ? id : room->name) + "」，之后的发言都发到这里");
    return true;
}

void ChatCore::ShowRooms() {
    if (rooms_.empty()) {
        Notice(roomsCapable_ ? "还没有会话（服务端没下发房间列表）"
                             : "连的是老服务端，只有大厅");
        return;
    }
    for (const RoomInfo& room : rooms_) {
        std::string line = (room.id == currentRoom_) ? "* " : "  ";
        line += room.name.empty() ? room.id : room.name;
        line += " (" + room.id + ")";
        if (room.kind == "group") {
            line += "  " + std::to_string(room.members) + "/" + std::to_string(room.maxMembers) + "人";
            if (!room.myRole.empty()) line += " " + room.myRole;
            line += room.listed ? " 公开" : " 未公开";
        } else if (room.kind == "dm") {
            line += "  私聊";
        }
        Notice(line);
    }
    Notice("切会话：/open <id>　建群：/create <id> <名称>　搜群：/find <关键词>");
}

bool ChatCore::HandleRoomLine(const std::string& command, const std::vector<std::string>& words,
                              const std::string& time) {
    if (command == "FEATURES") {
        // FEATURES <能力列表>：服务端确认支持哪些能力
        roomsCapable_ = false;
        for (const std::string& word : words) {
            if (word == "rooms1") roomsCapable_ = true;
        }
        if (roomsCapable_) {
            Notice("服务端支持群聊 / 私聊：/rooms 看会话，/help 看多会话指令");
        } else {
            Notice("服务端不支持多会话，只能在大厅聊天");
        }
        return true;
    }
    if (command == "ROOMLIST") {
        // ROOMLIST <房间ID> <kind> <显示名B64> <成员数> <最大人数> <我的角色>
        if (words.size() >= 6) {
            RoomInfo room;
            room.id = words[0];
            room.kind = words[1];
            room.name = DecodeB64(words[2]);
            room.members = std::atoi(words[3].c_str());
            room.maxMembers = std::atoi(words[4].c_str());
            room.myRole = words[5];
            UpsertRoom(room);
        }
        return true;
    }
    if (command == "ROOMLIST_END") {
        // 列表发完了。第一次登录时落到大厅，别让用户对着空会话发呆。
        if (currentRoom_.empty() && !rooms_.empty()) {
            SwitchRoom(rooms_.front().id);
        } else {
            Notice("会话列表已更新，/rooms 查看");
        }
        return true;
    }
    if (command == "RINFO") {
        // RINFO <房间ID> <kind> <显示名B64> <群主> <成员数> <最大人数> <我的角色> <加群方式> <listed>
        if (words.size() >= 9) {
            RoomInfo room;
            room.id = words[0];
            room.kind = words[1];
            room.name = DecodeB64(words[2]);
            room.owner = words[3];
            room.members = std::atoi(words[4].c_str());
            room.maxMembers = std::atoi(words[5].c_str());
            room.myRole = words[6];
            room.joinMode = words[7];
            room.listed = (words[8] == "1");
            UpsertRoom(room);
            if (room.id == currentRoom_) members_.clear();  // 紧跟着就是新的成员表
        }
        return true;
    }
    if (command == "RSET") {
        // RSET <房间ID> <设置名> <值>
        if (words.size() >= 3) {
            RoomInfo* room = FindRoom(words[0]);
            if (room != nullptr) {
                const std::string& key = words[1];
                const std::string value = JoinWords(words, 2);
                if (key == "name") {
                    const std::string decoded = DecodeB64(value);
                    if (!decoded.empty()) room->name = decoded;
                } else if (key == "maxmembers") {
                    room->maxMembers = std::atoi(value.c_str());
                } else if (key == "joinmode") {
                    room->joinMode = value;
                } else if (key == "listed") {
                    // 服务端发的是 true/false；老版本发过 1/0，两种都认
                    room->listed = (value == "1" || value == "true");
                } else if (key == "announcement") {
                    room->announcement = DecodeB64(value);
                } else if (key == "cachemaxmb") {
                    room->cacheMaxMb = std::atoi(value.c_str());
                } else if (key == "importantnotice") {
                    room->importantNotice = (value == "true");
                } else if (key == "fileencryption") {
                    room->fileEncryption = (value == "true");
                }
            }
        }
        return true;
    }
    if (command == "RMEMBERS") {
        // RMEMBERS <房间ID> <昵称> <角色> <群昵称B64>
        if (words.size() >= 3) {
            MemberInfo member;
            member.nick = words[1];
            member.role = words[2];
            if (words.size() > 3) member.display = DecodeB64(words[3]);
            members_.push_back(member);
        }
        return true;
    }
    if (command == "RMEMBERS_END") {
        ++membersVersion_;  // 这批成员收全了
        return true;
    }
    if (command == "RMSG") {
        // RMSG <时间> <房间ID> <昵称> <文本>　非大厅消息
        //
        // ⚠️ 注意索引：FieldsOf() 已经把 hh:mm 摘掉了，所以这里是
        // words[0]=房间、words[1]=昵称、words[2..]=正文 —— **不是**协议原文里的
        // 第 2/3/4 个字段。（第一版就是照原文编号写的，昵称和正文全错位。)
        if (words.size() >= 2) {
            ChatMessage line;
            line.valid = true;
            const bool own = (words[1] == selfNick_);
            line.kind = own ? ChatMessage::Kind::Own : ChatMessage::Kind::Say;
            line.nick = words[1];
            line.time = time;
            line.text = JoinWords(words, 2);
            // 不在当前会话里就标上房间 —— 终端是一条流，不标就分不清是哪条线来的
            if (words[0] != currentRoom_) {
                const RoomInfo* room = FindRoom(words[0]);
                const std::string label =
                    (room != nullptr && !room->name.empty()) ? room->name : words[0];
                line.text = "[" + label + "] " + line.text;
            }
            Push(line);
        }
        return true;
    }
    if (command == "REVENT") {
        // REVENT <时间> <房间ID> <文本>　（同样：时间已被 FieldsOf 摘掉）
        if (!words.empty()) {
            const RoomInfo* room = FindRoom(words[0]);
            const std::string label =
                (room != nullptr && !room->name.empty()) ? room->name : words[0];
            Notice("[" + label + "] " + JoinWords(words, 1));
        }
        return true;
    }
    if (command == "GHIT") {
        // GHIT <房间ID> <群名B64> <成员数> <最大人数> <加群方式>
        if (words.size() >= 5) {
            GroupHit hit;
            hit.id = words[0];
            hit.name = DecodeB64(words[1]);
            hit.members = std::atoi(words[2].c_str());
            hit.maxMembers = std::atoi(words[3].c_str());
            hit.joinMode = words[4];
            groupHits_.push_back(hit);
            Notice("搜到：" + (hit.name.empty() ? hit.id : hit.name) + " (" + hit.id + ")  " +
                   std::to_string(hit.members) + "/" + std::to_string(hit.maxMembers) + "人  " +
                   hit.joinMode);
        }
        return true;
    }
    if (command == "GHIT_END") {
        ++groupHitsVersion_;  // 搜索结果收全了
        if (groupHits_.empty()) {
            Notice("没搜到群（群必须由管理员勾选「向公众展示」才会出现在搜索里）");
        } else {
            Notice("加入：/join <id>");
        }
        return true;
    }
    if (command == "GDMLINK") {
        // GDMLINK <房间ID> <对方昵称>：私聊已经建好
        if (words.size() >= 2) {
            RoomInfo room;
            room.id = words[0];
            room.kind = "dm";
            room.name = words[1];
            UpsertRoom(room);
            Notice("私聊已建立：" + words[1] + "（" + words[0] + "），/open " + words[0] + " 切过去");
        }
        return true;
    }
    if (command == "GREQ") {
        // GREQ <时间> <房间ID> <昵称>（时间已被 FieldsOf 摘掉）
        // 两个来源：GREQS 的逐行回应，以及有人申请入群时推给管理员的主动通知。
        if (words.size() >= 2) {
            // 去重：GREQS 的回应和"有人刚申请"的主动通知走的是同一种行，
            // 先 /audit 再有人申请的话，同一个人会进来两次。
            bool exists = false;
            for (const std::string& known : pending_) {
                if (known == words[1]) exists = true;
            }
            if (!exists) pending_.push_back(words[1]);
            const RoomInfo* room = FindRoom(words[0]);
            const std::string label =
                (room != nullptr && !room->name.empty()) ? room->name : words[0];
            Notice("[" + label + "] " + words[1] + " 申请入群　同意 /ok " + words[1] +
                   "　拒绝 /no " + words[1]);
        }
        return true;
    }
    if (command == "GREQ_END") {
        ++pendingVersion_;  // 待审名单收全了
        if (pending_.empty()) Notice("现在没有待审核的入群申请");
        return true;
    }
    if (command == "GBLOCKS") {
        // GBLOCKS <昵称>　注意这一条**不带时间戳**（服务端是 BuildLine 直接发的）
        if (!words.empty()) blocks_.push_back(words[0]);
        return true;
    }
    if (command == "GBLOCKS_END") {
        if (blocks_.empty()) {
            Notice("黑名单是空的");
        } else {
            for (const std::string& nick : blocks_) Notice("黑名单：" + nick);
        }
        return true;
    }
    if (command == "RHIST") {
        // RHIST <房间ID> <序号> <原始协议行>　切进房间时服务端把历史一条条补发
        //
        // 处理方式是把**内层那一行**重新走一遍普通解析，而不是原样显示 ——
        // 不处理的话这些原始协议行会直接漏进聊天区（第一版就是这样：
        // 屏幕上会出现 "webgroup 1 REVENT 07:53 ..." 这种机器码）。
        // FieldsOf 不会剥这里的"时间"（第二个字段是序号），所以从 words[2] 开始才是正文。
        if (words.size() > 2) {
            // **按序号去重**：服务端每次收到 ROOM 都会把历史重放一遍。
            // 不去重的话，切进切出几次屏幕上就会堆出好几份历史 + 好几个
            // 「以上是历史消息」（用户报的"切房后还有上个房间的记录"就是这个）。
            // 序号在同一房间内单调递增，所以"序号 <= 已经显示过的"一定见过。
            const std::string room = words[0];
            unsigned long long seq = 0;
            bool numeric = !words[1].empty();
            for (char ch : words[1]) {
                if (ch < '0' || ch > '9') numeric = false;
            }
            if (numeric) {
                try {
                    seq = std::stoull(words[1]);
                } catch (...) {
                    seq = 0;
                }
            }
            if (seq != 0) {
                const auto it = lastHistorySeq_.find(room);
                if (it != lastHistorySeq_.end() && seq <= it->second) return true;  // 见过，跳过
                lastHistorySeq_[room] = seq;
            }
            HandleLine(JoinWords(words, 2));
        }
        return true;
    }
    if (command == "RHIST_END") {
        Notice("—— 以上是历史消息 ——");
        return true;
    }
    return false;
}

}  // namespace dchat
