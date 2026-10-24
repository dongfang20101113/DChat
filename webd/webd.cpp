// dchat 网页版网关（dchat-webd）
//
// ## 为什么必须有这一层
//
// dchat 的协议是 **5555 端口上的裸 TCP** + 自定义 ECDH/AES-GCM 握手。浏览器的
// WebSocket / fetch 都跑不到裸 TCP 上，所以"网页直接连服务器"是做不到的。
//
// 这个网关一边用 HTTP/WebSocket 对浏览器，一边用**真实的 dchat 协议**连服务器。
// 关键点是它**不复刻协议**：直接复用 client_core 的 ChatCore + ClientConnection，
// 也就是桌面端和 Linux 端用的同一份实现。带来的好处有两个：
//   1. 网页版天然具备全部能力（群聊、私聊、文件、黑名单、审核……），
//      因为浏览器发过来的每一行最终都进了 ChatCore::SubmitInput；
//   2. 协议只在**一个地方**实现。哪天协议改了，网关照旧跟着变，不会多出一份要对齐的代码。
//
// 浏览器侧**一行加密代码都没有**：加密在网关和服务器之间，浏览器只看到明文文本。
//
// ## 对浏览器的协议（行式文本，Tab 分隔）
//
// 浏览器 → 网关
//   LOGIN \t <用户> \t <密码> \t <0|1 注册>     建一条 dchat 连接并登录
//   INPUT \t <一行文本>                          等价于在客户端输入框里敲这行
//   QUIT
//
// 网关 → 浏览器
//   STATE \t <未连接|连接中|已连接|已登录> \t <说明>
//   SAY   \t <昵称> \t <时间> \t <正文> \t <0|1 是不是自己> \t <0|1 有没有 @我>
//   SYS   \t <正文>          （服务器通知：进出、设置变更、公告……）
//   ERROR \t <正文>
//   INFO  \t <正文>          （网关自己的提示）
//
// 用法：
//   dchat-webd --port 8080 --server 127.0.0.1:5555 --www ./webd-page
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>  // mkdir：网页版上传的临时目录
#include <thread>
#include <vector>

#include "chat_core.h"
#include "file_transfer.h"
#include "socket_util.h"

namespace {

// ---------------------------------------------------------------- SHA-1
//
// WebSocket 握手要把 Sec-WebSocket-Key 和固定 GUID 拼起来做 SHA-1 再 Base64。
// 这里手写一份（三十来行）而不是链 OpenSSL：WebSocket 只需要**这一处**哈希，
// 为它引入一个平台相关的加密后端不值得 —— 而且手写之后这个网关在 Windows 上
// 也能编、也能测（真正部署是 Linux，但开发和验证都在 Windows 上做）。
struct Sha1 {
    unsigned int h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    static unsigned int Rol(unsigned int v, int bits) { return (v << bits) | (v >> (32 - bits)); }

    void Block(const unsigned char* p) {
        unsigned int w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<unsigned int>(p[i * 4]) << 24) |
                   (static_cast<unsigned int>(p[i * 4 + 1]) << 16) |
                   (static_cast<unsigned int>(p[i * 4 + 2]) << 8) |
                   static_cast<unsigned int>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            unsigned int f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const unsigned int temp = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string Digest(const std::string& text) {
        std::vector<unsigned char> data(text.begin(), text.end());
        const unsigned long long bitLength = static_cast<unsigned long long>(data.size()) * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0);
        for (int i = 7; i >= 0; --i) {
            data.push_back(static_cast<unsigned char>((bitLength >> (i * 8)) & 0xFF));
        }
        for (std::size_t i = 0; i + 63 < data.size(); i += 64) Block(&data[i]);
        std::string out;
        for (int i = 0; i < 5; ++i) {
            out.push_back(static_cast<char>((h[i] >> 24) & 0xFF));
            out.push_back(static_cast<char>((h[i] >> 16) & 0xFF));
            out.push_back(static_cast<char>((h[i] >> 8) & 0xFF));
            out.push_back(static_cast<char>(h[i] & 0xFF));
        }
        return out;
    }
};

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

/**
 * URL 里的 %XX 解码。
 *
 * 必须做：前端传 `?server=127.0.0.1%3A5611`，那个 `%3A` 就是冒号。不解码的话
 * 整个串会被当成主机名去解析，然后连不上 —— 而错误码看起来又像"服务器没开"，
 * 很难查（第一次就是卡在这：测试服务端日志里一条连接都没有）。
 */
std::string PercentDecode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

bool SendAll(dchat::sock::Handle handle, const void* data, std::size_t length) {
    return dchat::sock::SendAll(handle, data, length);
}

// 附件 ID → 网关本机上的落地路径。
//
// 浏览器下载文件是走 **HTTP**（GET /file/<id>），而下载动作发生在某个 WebSocket
// 会话的 FileTransfers 里。两边的对象不是同一个，所以用这张表把"文件已经在哪"
// 传出去。
//
// ⚠️ 这是**全局**的，不分会话：同一台网关上多人同时用、附件 id 又恰好撞上时，
// 理论上会串。但附件 id 是服务器按全局序号分配的（F1、F2…），不会撞；
// 真要多人隔离，得在 id 前面再加会话标识。
std::mutex g_filesMutex;
std::map<std::string, std::string> g_downloadedFiles;

void RememberDownloaded(const std::string& id, const std::string& path) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    g_downloadedFiles[id] = path;
}

std::string DownloadedPath(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    const auto it = g_downloadedFiles.find(id);
    return (it == g_downloadedFiles.end()) ? std::string() : it->second;
}

// ---------------------------------------------------------------- 一个浏览器会话
//
// 下面这几个是「op 管理会话」，定义在文件后部的管理员页面那一节，
// 但 Session 登录成功时要调，所以先声明。
//
// 为什么管理权限不放在 HTTP 登录里、而挂在**已经登录的聊天会话**上：
// 需求是"登录后这个人如果是 op，就记住他，只有他能进 /admin"。
// 聊天登录本来就走真实的 dchat 认证（账号+密码，服务器校验），
// 所以 op 身份直接从这个会话里取，不用再让他在管理页输一遍口令。
bool IsOpNick(const std::string& nick);
std::string MintOpSession(const std::string& nick);
bool OpSessionNick(const std::string& id, std::string* nick);
std::string RandomHex(int bytes);
// 网页版单文件上传上限，跟着 chatrule 的 webuploadmb 走（定义在管理员那一节）
int WebUploadLimitMb();

class Session : public dchat::ChatCoreDelegate,
                public std::enable_shared_from_this<Session> {
public:
    Session(dchat::sock::Handle handle, const std::string& serverHost, int serverPort)
        : handle_(handle), serverHost_(serverHost), serverPort_(serverPort) {}
    ~Session() override {
        std::fprintf(stderr, "[session] 销毁（dchat 连接随之关闭）\n");
        std::fflush(stderr);
        if (core_) core_->Disconnect();
        if (handle_ != dchat::sock::kInvalid) dchat::sock::Close(handle_);
    }

    // ---- WebSocket 的收 ----
    void Start() {
        SendLine(std::string("INFO\t网页版已就绪，输入账号登录"));
        StartRoomListPump();  // 登录后左侧的会话选择器靠它更新
    }

    /** 处理浏览器发来的一行（见文件头的协议说明）。 */
    void OnBrowserLine(const std::string& line) {
        // 日志只留前一段：上传/语音的帧里是几十 KB 的 Base64，
        // 原样打出来会把日志撑爆，还泄漏文件内容。
        std::fprintf(stderr, "[ws] 浏览器输入: %.120s%s\n", line.c_str(),
                     line.size() > 120 ? "…" : "");
        std::fflush(stderr);
        const std::size_t tab = line.find('\t');
        const std::string command = Lower(Trim(tab == std::string::npos ? line : line.substr(0, tab)));
        const std::string rest = (tab == std::string::npos) ? std::string() : line.substr(tab + 1);

        if (command == "login") {
            std::vector<std::string> parts;
            std::size_t from = 0;
            while (true) {
                const std::size_t at = rest.find('\t', from);
                parts.push_back(rest.substr(from, at == std::string::npos ? std::string::npos
                                                                         : at - from));
                if (at == std::string::npos) break;
                from = at + 1;
            }
            if (parts.size() < 2) {
                SendLine(std::string("ERROR\t登录需要用户名和密码"));
                return;
            }
            const std::string user = parts[0];
            const std::string password = parts[1];
            const bool wantRegister = (parts.size() > 2 && parts[2] == "1");
            DoLogin(user, password, wantRegister);
            return;
        }
        if (command == "input") {
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            // `/get <附件id>` 特殊处理：把它变成"网关先下到本地，再让浏览器来取"。
            //
            // 为什么不能直接 SubmitInput：ChatCore 的下载接口是把内容写进**本机磁盘**，
            // 浏览器拿不到字节。所以这里下载完成后登记到全局表，
            // 再告诉浏览器去 GET /file/<id>（那个路由会流式把文件吐给浏览器）。
            // 切房：先让浏览器清屏，并忘掉该房间的历史位置。
            //
            // 两件事必须一起做：清屏是为了不把上一个房间的消息留在屏幕上
            // （用户报的"切房后还有上个房间的聊天记录"）；忘掉历史位置是因为
            // 清屏之后服务端重放的历史还得能重新显示出来，否则是空白。
            if (rest.rfind("/open ", 0) == 0) {
                const std::string target = Trim(rest.substr(6));
                if (!target.empty()) core_->ForgetDisplayedHistory(target);
                SendLine(std::string("CLEAR"));
                core_->SubmitInput(rest);
                return;
            }
            if (rest.rfind("/get ", 0) == 0) {
                StartBrowserDownload(Trim(rest.substr(5)));
                return;
            }
            core_->SubmitInput(rest);
            return;
        }
        if (command == "upload") {
            // 浏览器选的文件：UPLOAD \t <文件名> \t <Base64 内容>
            //
            // 落地方式：解码写进网关本机的临时文件，再走正常的 /send 上传流程。
            // 这样浏览器侧不用关心 dchat 的分块协议，核心层也一行不改。
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            const std::size_t tab2 = rest.find('\t');
            if (tab2 == std::string::npos) {
                SendLine(std::string("ERROR\t上传格式不对"));
                return;
            }
            std::string name = rest.substr(0, tab2);
            // 只保留最后一段文件名，挡掉 ../ 和路径分隔符
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (name.empty()) name = "file";
            const std::string b64 = Trim(rest.substr(tab2 + 1));
            std::vector<unsigned char> bytes;
            if (!dchat::Base64Decode(b64, &bytes) || bytes.empty()) {
                SendLine(std::string("ERROR\t上传内容解码失败"));
                return;
            }
            // 网页版走的是"一帧把整个文件塞进 WebSocket"，太大的文件不现实。
            // 桌面端没有这个限制（它直接读盘分块发）。
            // 上限由 chatrule 的 webuploadmb 决定，不再是写死的 8 MB。
            const int limitMb = WebUploadLimitMb();
            if (bytes.size() > static_cast<std::size_t>(limitMb) * 1024ull * 1024ull) {
                SendLine("ERROR\t网页版单文件上限 " + std::to_string(limitMb) +
                         " MB（由 chatrule 的 webuploadmb 控制），更大的文件请用桌面客户端");
                return;
            }
            ++uploadSerial_;
            // 临时文件放进 webd-uploads/<序号>/<原名> 这样的子目录。
            //
            // **为什么要绕这一下**：dchat 上传时拿路径的 basename 当显示名，
            // 如果把文件直接命名为 webd-upload-1-xxx.txt，接收方看到的就是那个
            // 带前缀的丑名字。放进子目录后 basename 仍然是原名。
            const std::string base = "webd-uploads";
            ::mkdir(base.c_str(), 0755);  // 已存在会返回 -1，无害
            const std::string dir = base + "/" + std::to_string(uploadSerial_);
            ::mkdir(dir.c_str(), 0755);
            const std::string path = dir + "/" + name;
            FILE* file = std::fopen(path.c_str(), "wb");
            if (file == nullptr) {
                SendLine(std::string("ERROR\t网关写不了临时文件"));
                return;
            }
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            SendLine("INFO\t正在上传 " + name + "（" + std::to_string(bytes.size()) + " 字节）…");
            core_->SubmitInput("/send " + path);
            return;
        }
        // ---- 房间面板要的三张"列表"：光是敲 /find 只会把结果打成一堆文字，
        //      图形界面要的是结构化数据，所以这里读 ChatCore 收好的结果再推出去 ----
        if (command == "find") {
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            const std::string keyword = Trim(rest);
            if (keyword.empty()) {
                SendLine(std::string("ERROR\t搜什么？给个关键词"));
                return;
            }
            core_->SubmitInput("/find " + keyword);
            DumpWhenReady(0, core_->GroupHitsVersion());
            return;
        }
        if (command == "audit") {
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            core_->SubmitInput("/audit");
            DumpWhenReady(2, core_->PendingVersion());
            return;
        }
        if (command == "members") {
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            // 成员表是进房间时服务端推的，这里不用再请求，直接把手上这份发过去
            DumpMembers();
            return;
        }
        if (command == "voice") {
            // VOICE \t <WAV 的 Base64>
            //
            // 录音在浏览器里完成（16 kHz / 单声道 / 16 位 PCM WAV，和另外三端一致），
            // 网关这边只做"落盘 + 以 kind=voice 上传"——服务器和另外几端就靠
            // kind 这一格判断这是语音，**不看扩展名**。
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            std::vector<unsigned char> bytes;
            if (!dchat::Base64Decode(Trim(rest), &bytes) || bytes.empty()) {
                SendLine(std::string("ERROR\t语音数据解码失败"));
                return;
            }
            const int limitMb = WebUploadLimitMb();
            if (bytes.size() > static_cast<std::size_t>(limitMb) * 1024ull * 1024ull) {
                SendLine("ERROR\t语音太大（上限 " + std::to_string(limitMb) +
                         " MB），录短一点");
                return;
            }
            ++uploadSerial_;
            ::mkdir("webd-uploads", 0755);
            const std::string dir = "webd-uploads/" + std::to_string(uploadSerial_);
            ::mkdir(dir.c_str(), 0755);
            const std::string path = dir + "/voice.wav";
            FILE* file = std::fopen(path.c_str(), "wb");
            if (file == nullptr) {
                SendLine(std::string("ERROR\t网关写不了临时文件"));
                return;
            }
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            std::string error;
            const std::string localId = core_->Files().Upload(path, "voice", &error);
            if (localId.empty()) {
                SendLine("ERROR\t语音发送失败：" + error);
                return;
            }
            SendLine("INFO\t🎤 语音已发出（" + std::to_string(bytes.size()) + " 字节）");
            return;
        }
        if (command == "quit") {
            Close();
            return;
        }
        SendLine(std::string("ERROR\t不认识的消息：") + command);
    }

    void Close() {
        closed_ = true;
        if (core_) core_->Disconnect();
        if (handle_ != dchat::sock::kInvalid) {
            dchat::sock::Close(handle_);
            handle_ = dchat::sock::kInvalid;
        }
    }

    // ---- ChatCoreDelegate：把核心层的事件翻成浏览器的行 ----
    void OnChatMessage(const dchat::ChatMessage& message) override {
        switch (message.kind) {
            case dchat::ChatMessage::Kind::Say:
            case dchat::ChatMessage::Kind::Own: {
                std::string line = "SAY\t" + message.nick + "\t" + message.time + "\t" +
                                   message.text + "\t" +
                                   (message.kind == dchat::ChatMessage::Kind::Own ? "1" : "0") + "\t" +
                                   (message.mentionMe ? "1" : "0");
                SendLine(line);
                return;
            }
            case dchat::ChatMessage::Kind::Error:
                SendLine("ERROR\t" + message.text);
                return;
            case dchat::ChatMessage::Kind::FileOffer:
                // 附件卡片：带 isVoice，页面据此画「▶ 播放」还是「⬇ 下载」
                SendLine("FILEOFFER\t" + message.fileId + "\t" +
                         (message.isVoice ? "1" : "0") + "\t" + message.text);
                return;
            case dchat::ChatMessage::Kind::System:
            case dchat::ChatMessage::Kind::Notice:
            default:
                SendLine("SYS\t" + message.text);
                return;
        }
    }
    void OnConnected(bool encrypted, const std::string& fingerprint,
                     const dchat::TrustDecision&) override {
        SendLine("STATE\t连接中\t" + std::string(encrypted ? "已加密" : "明文") + "，服务器指纹 " +
                 fingerprint);
    }
    void OnLoggedIn(const std::string& nick) override {
        SendLine("STATE\t已登录\t" + nick);
        // ---- 登录成功才判 op ----
        //
        // 「有且仅有该用户能访问 /admin」就落在这一段：
        //   1. 只有登录成功（服务器认了这个账号密码）才会走到这里；
        //   2. 昵称在 dchat-admins.txt 里才算 op；
        //   3. op 才发一个随机会话号，浏览器把它存进 Cookie 才能进 /admin。
        // 非 op 拿到的永远是 ADMINSTATE 0，什么号都没有，也就进不去。
        if (IsOpNick(nick)) {
            const std::string id = MintOpSession(nick);
            SendLine("ADMINSTATE\t1\t" + id);
        } else {
            SendLine(std::string("ADMINSTATE\t0"));
        }
    }
    void OnDisconnected(const std::string& reason) override {
        SendLine("STATE\t未连接\t" + reason);
    }
    void OnOnlineNicks(const std::vector<std::string>&) override {}
    void OnTransferProgress(const std::string& text) override { SendLine("SYS\t" + text); }
    void OnColorSettingChanged(bool enabled, const std::string&) override {
        SendLine("SYS\t彩色聊天：" + std::string(enabled ? "开" : "关"));
    }
    void OnVoiceCommand(const std::string&) override {
        // 录音在网页版是**浏览器**的事（getUserMedia + 自己拼 WAV），核心只负责
        // "把这件事转交给界面"。有人手敲 /voice 时提示一句就够了。
        SendLine(std::string("INFO\t网页版录音请点输入框右边的 🎤 按钮"));
    }

    /** 发一帧 WebSocket 文本帧。**加锁**：接收线程和 WS 线程都会调。 */
    void SendLine(const std::string& text);

    /**
     * 发一个 WebSocket ping 帧。
     *
     * 两个作用：①让中间设备（家用路由器、运营商 NAT）看到双向流量，别把连接
     * 当空闲掐掉；②浏览器收到 ping 会自动回 pong，那条回包能让我们的接收循环
     * 确认对端还活着。
     */
    void SendPing() {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (handle_ == dchat::sock::kInvalid) return;
        const char frame[2] = {static_cast<char>(0x89), 0};  // FIN + opcode=9 + 长度 0
        SendAll(handle_, frame, sizeof(frame));
    }

    /**
     * 会话列表变了就推一份结构化的给浏览器（左侧选择器用）。
     *
     * **为什么是轮询而不是回调**：ChatCore 没有"房间列表变了"的通知，而列表会因
     * **别人**建群 / 把你踢了 / 改了群名而变化 —— 光靠"自己发过哪些指令"推不出来。
     * 一秒比一次、而且只在签名变化时才发，开销可以忽略。
     */
    void StartRoomListPump() {
        std::weak_ptr<Session> weak = shared_from_this();
        std::thread([weak]() {
            std::string last;
            int ticks = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                auto self = weak.lock();
                if (!self) return;
                // 每 25 秒一个 ping：保活 + 让浏览器回 pong（回包证明它还活着）
                if (++ticks % 25 == 0) self->SendPing();
                if (!self->core_ || !self->core_->LoggedIn()) continue;
                const auto& rooms = self->core_->Rooms();
                const std::string current = self->core_->CurrentRoom();
                std::string signature = current + ";";
                for (const auto& room : rooms) {
                    signature += room.id + "|" + room.name + "|" + room.myRole + "|" +
                                 std::to_string(room.members) + "|" +
                                 std::to_string(room.maxMembers) + "|" + room.kind + ";";
                }
                // **设置值也要进签名**：不然群主在别的窗口改了「群文件加密」，
                // 这边的设置页不会重推，页面就一直停在旧值上。
                const dchat::RoomInfo* here = nullptr;
                for (const auto& room : rooms) {
                    if (room.id == current) here = &room;
                }
                if (here != nullptr) {
                    signature += "S" + std::to_string(here->cacheMaxMb) +
                                 (here->importantNotice ? "1" : "0") +
                                 (here->fileEncryption ? "1" : "0") + (here->listed ? "1" : "0") +
                                 here->joinMode + here->announcement + "|";
                }
                if (signature == last) continue;
                last = signature;
                // 先报行数 + 当前会话，再逐行发，最后 ROOMS_END 收尾
                // —— 和协议里的 ROOMLIST / ROOMLIST_END 是同一个套路。
                self->SendLine("ROOMS\t" + std::to_string(rooms.size()) + "\t" + current);
                for (const auto& room : rooms) {
                    self->SendLine("ROOM\t" + room.id + "\t" + room.kind + "\t" + room.name + "\t" +
                                   std::to_string(room.members) + "\t" +
                                   std::to_string(room.maxMembers) + "\t" + room.myRole);
                }
                self->SendLine(std::string("ROOMS_END"));
                // 当前会话的 8 项设置，单独推给网页设置页。
                //
                // 为什么不塞进 ROOM 行：ROOM 已经是 7 段固定格式，聊天页按下标取值，
                // 再往后追加字段会牵动三个端的解析；而且设置只有「当前房间」用得着。
                if (here != nullptr) {
                    const std::pair<const char*, std::string> rows[] = {
                        {"name", here->name},
                        {"maxmembers", std::to_string(here->maxMembers)},
                        {"joinmode", here->joinMode},
                        {"listed", here->listed ? "true" : "false"},
                        {"cachemaxmb", std::to_string(here->cacheMaxMb)},
                        {"announcement", here->announcement},
                        {"importantnotice", here->importantNotice ? "true" : "false"},
                        {"fileencryption", here->fileEncryption ? "true" : "false"},
                    };
                    self->SendLine("SETTINGS\t" + here->id + "\t" + here->kind + "\t" +
                                   here->myRole + "\t" +
                                   std::to_string(sizeof(rows) / sizeof(rows[0])));
                    for (const auto& row : rows) {
                        self->SendLine(std::string("SETTING\t") + row.first + "\t" + row.second);
                    }
                    self->SendLine(std::string("SETTINGS_END"));
                }
            }
        }).detach();
    }

    bool closed() const { return closed_; }
    dchat::sock::Handle handle() const { return handle_; }

private:
    /**
     * 浏览器点了 `/get <附件id>`：先让网关把文件下到本机，再告诉浏览器来取。
     *
     * 下载是**异步**的（服务器一块块推过来），所以这里起一个线程轮询
     * FileTransfers::LocalPathFor()：拿到路径就登记 + 通知浏览器。
     * 不能阻塞在 WS 线程上等 —— 那条线程还要收浏览器的其他消息。
     */
    // ---- 房间面板要的三张列表：推结构化数据，不是一堆文字 ----

    void DumpGroupHits() {
        if (!core_) return;
        const auto& hits = core_->LastGroupHits();
        SendLine("GHITS\t" + std::to_string(hits.size()));
        for (const auto& hit : hits) {
            SendLine("GHIT\t" + hit.id + "\t" + hit.name + "\t" + std::to_string(hit.members) +
                     "\t" + std::to_string(hit.maxMembers) + "\t" + hit.joinMode);
        }
        SendLine(std::string("GHITS_END"));
    }

    void DumpMembers() {
        if (!core_) return;
        const auto& members = core_->CurrentMembers();
        SendLine("MEMBERS\t" + std::to_string(members.size()));
        for (const auto& member : members) {
            SendLine("MEMBER\t" + member.nick + "\t" + member.role + "\t" + member.display);
        }
        SendLine(std::string("MEMBERS_END"));
    }

    void DumpPending() {
        if (!core_) return;
        const auto& pending = core_->PendingRequests();
        SendLine("PENDING\t" + std::to_string(pending.size()));
        for (const std::string& nick : pending) SendLine("REQ\t" + nick);
        SendLine(std::string("PENDING_END"));
    }

    /**
     * 等这一批收全了再推。
     *
     * `kind`：0 = 搜索结果（等 GHIT_END），2 = 待审申请（等 GREQ_END）。
     * 轮询 50 ms、最多 4 秒；超时就把手上这份推过去——总比让面板一直转圈好。
     * 用版本号而不是 size() 判断，是因为两次搜索结果可能一样多。
     */
    void DumpWhenReady(int kind, unsigned before) {
        std::weak_ptr<Session> weak = shared_from_this();
        std::thread([weak, kind, before]() {
            for (int i = 0; i < 80; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto self = weak.lock();
                if (!self || !self->core_) return;
                const unsigned now = (kind == 0) ? self->core_->GroupHitsVersion()
                                                 : self->core_->PendingVersion();
                if (now == before) continue;
                if (kind == 0) self->DumpGroupHits();
                else self->DumpPending();
                return;
            }
            auto self = weak.lock();
            if (!self) return;
            if (kind == 0) self->DumpGroupHits();
            else self->DumpPending();
        }).detach();
    }

    void StartBrowserDownload(const std::string& id) {
        if (id.empty() || !core_) {
            SendLine(std::string("ERROR\t用法：/get <附件id>"));
            return;
        }
        dchat::FileTransfers& files = core_->Files();
        const std::string ready = files.LocalPathFor(id);
        if (!ready.empty()) {
            RememberDownloaded(id, ready);
            SendLine("FILE\t" + id + "\t" + ready);
            return;
        }
        std::string error;
        if (!files.RequestDownload(id, &error)) {
            SendLine("ERROR\t下载失败：" + error);
            return;
        }
        SendLine("INFO\t正在从服务器取 " + id + " …");
        std::weak_ptr<Session> weak = shared_from_this();
        std::thread([weak, id]() {
            for (int i = 0; i < 600; ++i) {  // 最多等 60 秒
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                auto self = weak.lock();
                if (!self) return;
                if (!self->core_) return;
                const std::string path = self->core_->Files().LocalPathFor(id);
                if (path.empty()) continue;
                RememberDownloaded(id, path);
                self->SendLine("FILE\t" + id + "\t" + path);
                return;
            }
            auto self = weak.lock();
            if (self) self->SendLine("ERROR\t下载超时：" + id);
        }).detach();
    }

    void DoLogin(const std::string& user, const std::string& password, bool wantRegister) {
        if (core_) {
            core_->Disconnect();
            core_.reset();
        }
        core_ = std::make_unique<dchat::ChatCore>(this);
        dchat::ConnectOptions options;
        options.host = serverHost_;
        options.port = serverPort_;
        options.user = user;
        options.password = password;
        options.wantRegister = wantRegister;
        std::string error;
        SendLine(std::string("STATE\t连接中\t正在连 ") + serverHost_ + ":" +
                 std::to_string(serverPort_));
        if (!core_->Connect(options, &error)) {
            std::fprintf(stderr, "[session] 登录失败：%s\n", error.c_str());
            std::fflush(stderr);
            SendLine("ERROR\t连接失败：" + error);
            SendLine(std::string("STATE\t未连接\t连接失败"));
            core_.reset();
        }
    }

    dchat::sock::Handle handle_ = dchat::sock::kInvalid;
    std::string serverHost_;
    int serverPort_ = 5555;
    std::unique_ptr<dchat::ChatCore> core_;
    std::mutex sendMutex_;
    unsigned long long uploadSerial_ = 0;
    bool closed_ = false;
};

std::mutex g_sessionsMutex;
std::vector<std::shared_ptr<Session>> g_sessions;

/** 组一个 WebSocket 文本帧。载荷小于 126 字节时用 7 位长度，够用（一行聊天不会超过）。 */
std::string MakeWsFrame(const std::string& text) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + opcode=1（文本）
    const std::size_t size = text.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((size >> (i * 8)) & 0xFF));
    }
    frame += text;
    return frame;
}

void Session::SendLine(const std::string& text) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (handle_ == dchat::sock::kInvalid) return;
    const std::string frame = MakeWsFrame(text);
    SendAll(handle_, frame.data(), frame.size());
}

}  // namespace

// ---------------------------------------------------------------- 内嵌页面
//
// 页面**嵌进二进制**而不是从磁盘读：部署时只拷一个可执行文件，不会出现
// "忘了拷 webd-page 目录，首页 404"这种事。要改界面就重新编，代价可以接受。
const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>dchat 网页版</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; font:14px/1.5 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
         background:#1b1b1f; color:#e6e6e6; height:100vh; display:flex; flex-direction:column; }
  header { padding:10px 16px; background:#232329; border-bottom:1px solid #33333a;
           display:flex; align-items:center; gap:12px; flex-wrap:wrap; }
  header b { font-size:16px; }
  #state { color:#8a8a94; font-size:12px; }
  #state.on { color:#4caf7d; }
  main { flex:1; display:flex; min-height:0; position:relative; }
  #rooms { width:220px; flex:0 0 220px; overflow-y:auto; background:#202026;
           border-right:1px solid #33333a; padding:8px 0; }
  .room { padding:10px 14px; cursor:pointer; border-left:3px solid transparent;
          font-size:13px; display:flex; flex-direction:column; gap:2px; }
  .room:hover { background:#2a2a31; }
  .room.on { background:#2a2a31; border-left-color:#2f6fed; }
  .room .nm { color:#e6e6e6; }
  .room .sub { color:#8a8a94; font-size:11.5px; }
  #log { flex:1; overflow-y:auto; padding:12px 16px; }
  #menu { display:none; background:none; border:0; color:#e6e6e6; font-size:20px;
          padding:0 6px; cursor:pointer; }
  /* ---- 手机窄屏：侧栏变成抽屉，输入区固定在底部、不被键盘顶飞 ---- */
  @media (max-width: 640px) {
    #rooms { position:absolute; z-index:5; top:0; bottom:0; left:0; width:76%;
             max-width:280px; transform:translateX(-102%); transition:transform .18s; }
    body.drawer #rooms { transform:translateX(0); box-shadow:0 0 0 100vmax #0008; }
    #menu { display:block; }
    header { padding:8px 10px; gap:8px; }
    header b { font-size:15px; }
    main { min-height:0; }
    #log { padding:10px 12px; }
    footer { padding:8px 10px; gap:6px; padding-bottom:calc(8px + env(safe-area-inset-bottom)); }
    footer input, footer button { font-size:16px; }  /* ≥16px 才不会让 iOS 自动放大 */
    .room { padding:12px 14px; }  /* 触摸目标大一点 */
  }
  .row { margin:6px 0; white-space:pre-wrap; word-break:break-word; }
  .nick { color:#7aa7ff; }
  .time { color:#666; font-size:12px; margin:0 6px; }
  .own .nick { color:#4caf7d; }
  .sys { color:#8a8a94; font-size:13px; }
  .err { color:#ff6b6b; }
  #login { position:fixed; inset:0; background:#1b1b1f; display:flex; align-items:center;
           justify-content:center; padding:0 16px; }
  #login form { background:#232329; padding:24px; border-radius:12px; width:320px;
                max-width:100%;  /* 手机上不能超屏，否则右侧会被切掉 */
                display:flex; flex-direction:column; gap:10px; border:1px solid #33333a; }
  .other { text-align:center; font-size:13px; color:#8a8a94; }
  .other a { color:#7aa7ff; }
  input { padding:9px 11px; border-radius:8px; border:1px solid #3a3a42;
          background:#2a2a31; color:#e6e6e6; font:inherit; }
  button { padding:9px 14px; border-radius:8px; border:0; background:#2f6fed; color:#fff;
           font:inherit; cursor:pointer; }
  button.ghost { background:#2f2f37; }
  footer { display:flex; gap:8px; padding:10px 16px; background:#232329; border-top:1px solid #33333a; }
  footer input { flex:1; }
  a { color:#7aa7ff; }
</style></head><body>
<header>
  <button id="menu" title="会话列表">☰</button>
  <b>dchat</b><span id="state">未连接</span><span id="where"></span>
  <span style="flex:1"></span>
  <button id="btnPanel" class="ghost" title="房间" style="padding:4px 9px;font-size:13px">＋</button>
  <a id="adminlink" href="/admin" style="display:none;font-size:13px">管理</a>
  <a id="rset" href="#" style="display:none;font-size:13px">房间设置</a>
  <a href="/download" style="font-size:13px">下载客户端</a>
</header>
<main><aside id="rooms"></aside><div id="log"></div></main>
<footer>
  <input id="input" placeholder="说点什么…（/help 看指令，/rooms 看会话，/get <id> 下载文件）" disabled>
  <button class="ghost" id="pick" disabled title="发文件（上限 __UPLOAD_MB__ MB）">📎</button>
  <button class="ghost" id="mic" disabled title="按住说话（点一下开始，再点一下发送）">🎤</button>
  <button id="send" disabled>发送</button>
</footer>
<input type="file" id="file" style="display:none">
<div id="login"><form id="loginForm">
  <b style="font-size:16px">登录 dchat</b>
  <input id="user" placeholder="昵称" autocomplete="username">
  <input id="pass" type="password" placeholder="密码" autocomplete="current-password">
  <input id="host" value="dchat.xin:5555" placeholder="服务器:端口">
  <label style="font-size:13px;color:#8a8a94">
    <input type="checkbox" id="reg" style="width:auto"> 没有账号，注册一个新的
  </label>
  <label style="font-size:13px;color:#8a8a94">
    <input type="checkbox" id="remember" style="width:auto"> 记住账号，下次自动登录
  </label>
  <button type="submit">连接</button>
  <div class="other">想装客户端？<a href="/download">下载 Windows / Android / Linux 版</a></div>
</form></div>
<script>
// 这个 URL 属于哪个房间（由网关在服务端填进 __ROOM_ID__，不是前端读 pathname）。
//
// 为什么不在前端读 location.pathname：网关可能挂在子路径下被反向代理，
// 前端算出来的那一段未必和服务端路由一致。房间号在网关侧已校验过只含
// [a-z0-9_-]，填进字符串字面量不会有转义问题。
window.DCHAT_ROOM = "__ROOM_ID__";
// 单文件上传上限（MB），由服务器 chatrule 的 webuploadmb 决定、网关填进来。
// 前端这道检查只是为了早一步拦住、少传一趟——真正的闸在网关和服务器上。
const UPLOAD_MB = __UPLOAD_MB__;
</script>
<script>
const logEl = document.getElementById('log');
const stateEl = document.getElementById('state');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
let ws = null;
let roomBuf = [];      // 正在收的一批会话
let currentRoom = '';  // 网关告诉我们"当前在哪个会话"

// URL 里指定的房间（`/room/<房间名>` 进入时由服务端注入；从根路径进来是空串）
const pageRoom = window.DCHAT_ROOM || '';
// 想要待在哪个房间。它会跟着左侧点击/前进后退变，**不能**直接改 pageRoom，
// 否则断线重连后会被拽回最初那个房间。
let wantRoom = pageRoom;
let openedPageRoom = false;  // 每次登录后只自动进一次，之后听用户的

/** 同步「房间设置」入口：没房间就藏起来。 */
function syncRoomLink() {
  const rset = document.getElementById('rset');
  if (wantRoom) {
    rset.href = '/room/' + encodeURIComponent(wantRoom) + '/settings';
    rset.style.display = '';
  } else {
    rset.style.display = 'none';
  }
}
syncRoomLink();

/** 切到一个房间：改 URL + 更新设置入口 + 真的发 /open。 */
function goRoom(id, push) {
  if (!id) return;
  wantRoom = id;
  if (push) history.pushState({room: id}, '', '/room/' + encodeURIComponent(id));
  syncRoomLink();
  send('INPUT\t/open ' + id);
}

// 浏览器前进/后退：地址变了就跟着切房间
window.addEventListener('popstate', () => {
  const parts = location.pathname.replace(/^\/+/, '').split('/');
  const id = (parts[0] === 'room' && parts[1]) ? decodeURIComponent(parts[1]) : '';
  if (id && id !== wantRoom) goRoom(id, false);
});

function add(kind, text) {
  const d = document.createElement('div');
  d.className = 'row ' + kind;
  d.textContent = text;
  logEl.appendChild(d);
  logEl.scrollTop = logEl.scrollHeight;
}
function setState(text, on) {
  stateEl.textContent = text;
  stateEl.className = on ? 'on' : '';
}
function send(line) { if (ws && ws.readyState === 1) ws.send(line); }

// ---- 连接：手动登录和自动登录共用这一个函数 ----
let credentials = null;      // 有值 = 断开后要自动重连
let reconnectDelay = 2000;

function connect(host, user, pass, reg) {
  document.getElementById('login').style.display = 'none';
  setState('连接中…', false);
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?server=' + encodeURIComponent(host));
  ws.onopen = () => {
    reconnectDelay = 2000;  // 连上了就把退避重置
    setState('已连接', true);
    send('LOGIN\t' + user + '\t' + pass + '\t' + (reg ? '1' : '0'));
  };
  ws.onmessage = (ev) => {
    const f = ev.data.split('\t');
    switch (f[0]) {
      case 'SAY': {
        const own = f[4] === '1';
        const d = document.createElement('div');
        d.className = 'row' + (own ? ' own' : '');
        d.innerHTML = '';
        const n = document.createElement('span'); n.className = 'nick'; n.textContent = f[1];
        const t = document.createElement('span'); t.className = 'time'; t.textContent = f[2];
        const x = document.createElement('span'); x.textContent = f[3];
        d.appendChild(n); d.appendChild(t); d.appendChild(x);
        logEl.appendChild(d); logEl.scrollTop = logEl.scrollHeight;
        break;
      }
      // ---- 左侧会话选择器：网关每秒比对一次，变了才推 ----
      case 'ROOMS':   roomBuf = []; currentRoom = f[2] || ''; break;
      case 'ROOM':    roomBuf.push(f); break;
      case 'ROOMS_END': renderRooms(); break;
      // 切房清屏：网关在 /open 时发这个，避免上一个房间的消息留在屏幕上
      case 'CLEAR':   logEl.innerHTML = ''; break;
      // 登录成功时网关已经判过 op 了：op 才会带会话号（f[2]）。
      // 把号存进 Cookie，/admin 就是靠它鉴权的 —— 没有号的人进不去。
      case 'ADMINSTATE':
        if (f[1] === '1' && f[2]) {
          try { document.cookie = 'dchat_op=' + f[2] + '; path=/admin; SameSite=Strict'; } catch (e) {}
          document.getElementById('adminlink').style.display = '';
        } else {
          try { document.cookie = 'dchat_op=; path=/admin; Max-Age=0'; } catch (e) {}
          document.getElementById('adminlink').style.display = 'none';
        }
        break;
      case 'SYS':   add('sys', f[1]); break;
      case 'ERROR': add('err', f[1]); break;
      case 'INFO':  add('sys', f[1]); break;
      case 'FILE': {
        const d = document.createElement('div');
        d.className = 'row sys';
        const a = document.createElement('a');
        a.href = '/file/' + encodeURIComponent(f[1]);
        a.textContent = '⬇ 点这里下载 ' + f[1];
        d.appendChild(a);
        logEl.appendChild(d); logEl.scrollTop = logEl.scrollHeight;
        break;
      }
      case 'STATE':
        setState(f[1] + (f[2] ? '　' + f[2] : ''), f[1] === '已登录');
        if (f[1] === '已登录') {
          inputEl.disabled = false; sendEl.disabled = false; pickEl.disabled = false;
          const micEl = document.getElementById('mic');
          if (micEl) micEl.disabled = false;
          inputEl.focus();
          // `/房间名` 进来的：登录成功后自动切到那个房间。
          // 断线重连后要再走一遍——服务端那边是个全新会话，房间是空的。
          if (wantRoom && !openedPageRoom) {
            openedPageRoom = true;
            add('sys', '正在进入 ' + wantRoom + ' …');
            send('INPUT\t/open ' + wantRoom);
          }
        } else {
          openedPageRoom = false;
        }
        break;
      // 房间面板 / 语音 / 指令面板的帧交给第二个 script（它挂在这个钩子上）
      default: if (window.__EXTRA_FRAME__) window.__EXTRA_FRAME__(f); break;
    }
  };
  ws.onclose = () => {
    setState('已断开', false);
    inputEl.disabled = true; sendEl.disabled = true; pickEl.disabled = true;
    // **自动重连**：只要还留着账号就退避重试（2s→4s→8s…最多 30s）。
    // 以前没有这一步，网络一抖就得手动刷新页面。
    if (credentials) {
      const seconds = Math.round(reconnectDelay / 1000);
      add('sys', '连接断开，' + seconds + ' 秒后自动重连…');
      setTimeout(() => connect(credentials.host, credentials.user, credentials.pass, false),
                 reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 2, 30000);
    }
  };
  ws.onerror = () => add('err', '连接出错');
}

document.getElementById('loginForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const host = document.getElementById('host').value.trim() || 'dchat.xin:5555';
  const user = document.getElementById('user').value;
  const pass = document.getElementById('pass').value;
  const reg = document.getElementById('reg').checked;
  credentials = {host: host, user: user, pass: pass, reg: false};
  // 记住账号：**明文存在这台浏览器的 localStorage 里**。
  // 页面上写明了这一点 —— 公共电脑上别勾。
  try {
    if (document.getElementById('remember').checked) {
      localStorage.setItem('dchat_login', JSON.stringify({host, user, pass}));
    } else {
      localStorage.removeItem('dchat_login');
    }
  } catch (err) { /* 隐私模式下 localStorage 会抛异常，忽略即可 */ }
  add('sys', '正在连接 ' + host + ' …');
  connect(host, user, pass, reg);
});

// 打开页面时若存过账号，自动填好并直接连
(function autoLogin() {
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem('dchat_login') || 'null'); } catch (e) { saved = null; }
  if (!saved || !saved.user) return;
  document.getElementById('host').value = saved.host || 'dchat.xin:5555';
  document.getElementById('user').value = saved.user;
  document.getElementById('pass').value = saved.pass || '';
  document.getElementById('remember').checked = true;
  credentials = {host: saved.host, user: saved.user, pass: saved.pass, reg: false};
  add('sys', '已用记住的账号自动登录…');
  connect(saved.host, saved.user, saved.pass, false);
})();

function submit() {
  const text = inputEl.value.trim();
  if (!text) return;
  send('INPUT\t' + text);
  inputEl.value = '';
}
const roomsEl = document.getElementById('rooms');
const whereEl = document.getElementById('where');

/** 把一批 ROOM 行画成左侧列表。点一下就切会话（等价于 /open <id>）。 */
function renderRooms() {
  roomsEl.innerHTML = '';
  let currentName = '';
  for (const f of roomBuf) {
    // ROOM \t id \t kind \t 名字 \t 成员数 \t 最大人数 \t 我的角色
    const id = f[1], kind = f[2], name = f[3], members = f[4], max = f[5], role = f[6];
    const row = document.createElement('div');
    row.className = 'room' + (id === currentRoom ? ' on' : '');
    const nm = document.createElement('span');
    nm.className = 'nm';
    nm.textContent = name || id;
    const sub = document.createElement('span');
    sub.className = 'sub';
    sub.textContent = (kind === 'hall' ? '大厅' : members + '/' + max + ' 人') +
                      (role && role !== 'member' ? '　' + role : '');
    row.append(nm, sub);
    row.addEventListener('click', () => {
      if (id !== currentRoom) goRoom(id, true);  // 让地址栏变成 /room/<id>，可收藏可分享
      document.body.classList.remove('drawer');  // 手机上选完自动收起
    });
    roomsEl.appendChild(row);
    if (id === currentRoom) currentName = name || id;
  }
  whereEl.textContent = currentName ? '　· ' + currentName : '';
  // 从首页进来（地址里没房间）时，等网关告诉我们当前在哪个会话，
  // 再把「房间设置」入口显出来——不然那个链接永远是隐藏的。
  if (!wantRoom && currentRoom) {
    wantRoom = currentRoom;
    syncRoomLink();
  }
}

// 手机上的 ☰：展开/收起会话抽屉
document.getElementById('menu').addEventListener('click', () => {
  document.body.classList.toggle('drawer');
});

sendEl.addEventListener('click', submit);
inputEl.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });

// 发文件：读成 Base64 一帧发过去。网关那边解码写临时文件再走 /send。
// 上限由 chatrule 的 webuploadmb 决定（以前是写死的 8 MB）——
// 网页版是把整个文件塞进一帧 WebSocket，所以它和桌面端的 documentsize 分开。
// （桌面端没这个限制，它直接读盘分块发）。
const pickEl = document.getElementById('pick');
const fileEl = document.getElementById('file');
pickEl.addEventListener('click', () => fileEl.click());
fileEl.addEventListener('change', () => {
  const f = fileEl.files[0];
  if (!f) return;
  if (f.size > UPLOAD_MB * 1024 * 1024) {
    add('err', '网页版单文件上限 ' + UPLOAD_MB + ' MB（服务器设置），更大的文件请用桌面客户端');
    fileEl.value = '';
    return;
  }
  const reader = new FileReader();
  reader.onload = () => {
    const b64 = String(reader.result).split(',')[1] || '';
    send('UPLOAD\t' + f.name + '\t' + b64);
    add('sys', '正在上传 ' + f.name + ' …');
  };
  reader.readAsDataURL(f);
  fileEl.value = '';
});
</script>
<!-- ==================== 房间面板 / 语音 / 指令面板 ==================== -->
<style>
#rpanel{position:fixed;inset:0;background:rgba(8,8,10,.72);display:none;z-index:50;
  align-items:center;justify-content:center;padding:16px}
#rpanel.on{display:flex}
.sheet{background:#232329;border:1px solid #3a3a42;border-radius:14px;width:520px;
  max-width:100%;max-height:88vh;display:flex;flex-direction:column;overflow:hidden}
.tabs{display:flex;gap:6px;padding:10px 12px;border-bottom:1px solid #33333a;flex-wrap:wrap}
.tabs button{padding:6px 12px;border-radius:8px;border:0;background:#2a2a31;color:#c9c9d1;
  font:inherit;font-size:13px;cursor:pointer}
.tabs button.on{background:#2f6fed;color:#fff}
.pbody{padding:14px 16px;overflow:auto;flex:1}
.pbody input,.pbody select{padding:8px 10px;border-radius:8px;border:1px solid #3a3a42;
  background:#2a2a31;color:#e6e6e6;font:inherit;width:100%;box-sizing:border-box}
.prow{display:flex;gap:8px;align-items:center;padding:9px 0;border-bottom:1px solid #2c2c33}
.prow .g{flex:1;min-width:0}
.prow .g b{font-size:14px}.prow .g span{display:block;color:#8a8a94;font-size:12px}
.prow button{padding:6px 10px;border-radius:7px;border:0;background:#2f6fed;color:#fff;
  font:inherit;font-size:13px;cursor:pointer;white-space:nowrap}
.prow button.ghost{background:#33333c}
.prow button.warn{background:#a33}
.pbody .tip{color:#8a8a94;font-size:12px;padding:10px 0}
.pbody h4{margin:14px 0 6px;font-size:13px;color:#8a8a94;font-weight:600}
@media(max-width:640px){.sheet{max-height:92vh}.pbody{padding:12px}}
</style>
<div id="rpanel"><div class="sheet">
  <div class="tabs">
    <button data-tab="find" class="on">搜索</button>
    <button data-tab="create">建群</button>
    <button data-tab="members">成员</button>
    <button data-tab="manage">管理</button>
    <button data-tab="cmd">指令</button>
    <span style="flex:1"></span>
    <button id="rclose" class="ghost">关闭</button>
  </div>
  <div class="pbody" id="ptab-find">
    <input id="findKw" placeholder="群关键词（群必须开了「向公众展示」才搜得到）">
    <div style="height:8px"></div>
    <button id="findGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font:inherit;cursor:pointer">搜索</button>
    <div id="findList"></div>
  </div>
  <div class="pbody" id="ptab-create" style="display:none">
    <input id="newId" placeholder="群 ID（3-16 位小写字母/数字/_/-）">
    <div style="height:8px"></div>
    <input id="newName" placeholder="群名称">
    <div style="height:8px"></div>
    <button id="createGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font:inherit;cursor:pointer">创建</button>
    <div class="tip">ID 建好不能改；名称以后能在「房间设置」里改。</div>
  </div>
  <div class="pbody" id="ptab-members" style="display:none">
    <button id="membersGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">刷新成员表</button>
    <div id="memberList"></div>
  </div>
  <div class="pbody" id="ptab-manage" style="display:none">
    <h4>群公告</h4>
    <input id="annText" placeholder="留空 = 查看；填内容 = 设为公告">
    <div style="height:8px"></div>
    <button id="annGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font:inherit;cursor:pointer">设置公告</button>
    <h4>入群申请</h4>
    <button id="auditGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">刷新待审</button>
    <div id="pendingList"></div>
    <h4>我</h4>
    <input id="nickText" placeholder="群昵称（只在这个群显示）">
    <div style="height:8px"></div>
    <button id="nickGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">改群昵称</button>
    <h4>危险操作</h4>
    <button id="leaveGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#a33;color:#fff;font:inherit;cursor:pointer">退出当前群</button>
    <div style="height:8px"></div>
    <button id="deleteGo" style="width:100%;padding:10px;border:0;border-radius:8px;background:#a33;color:#fff;font:inherit;cursor:pointer">解散当前群（仅群主）</button>
    <div class="tip" id="manageTip"></div>
  </div>
  <div class="pbody" id="ptab-cmd" style="display:none">
    <div class="tip">客户端指令的图形入口。管理员指令要 op 身份，服务器会再校验一次。</div>
    <h4>玩家管理（op）</h4>
    <input id="cmdNick" placeholder="昵称">
    <div style="height:8px"></div>
    <div style="display:flex;gap:8px"><button id="cmdBan" style="flex:1;padding:9px;border:0;border-radius:8px;background:#a33;color:#fff;font:inherit;cursor:pointer">封禁</button>
    <button id="cmdKick" style="flex:1;padding:9px;border:0;border-radius:8px;background:#a33;color:#fff;font:inherit;cursor:pointer">踢出</button>
    <button id="cmdUnban" style="flex:1;padding:9px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">解封</button></div>
    <h4>公告 / 管理员</h4>
    <input id="cmdSay" placeholder="全服公告内容">
    <div style="height:8px"></div>
    <div style="display:flex;gap:8px"><button id="cmdSayGo" style="flex:1;padding:9px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font:inherit;cursor:pointer">发公告</button>
    <button id="cmdOps" style="flex:1;padding:9px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">看管理员</button></div>
    <h4>黑名单</h4>
    <input id="blockNick" placeholder="昵称">
    <div style="height:8px"></div>
    <div style="display:flex;gap:8px"><button id="blockGo" style="flex:1;padding:9px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">屏蔽</button>
    <button id="unblockGo" style="flex:1;padding:9px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">取消屏蔽</button>
    <button id="blocksGo" style="flex:1;padding:9px;border:0;border-radius:8px;background:#33333c;color:#e6e6e6;font:inherit;cursor:pointer">看名单</button></div>
    <div class="tip" id="cmdTip"></div>
  </div>
</div></div>
<script>
// ============================================================================
// 房间面板 / 语音 / 指令面板
//
// 和上面的聊天脚本共用全局作用域（两个 classic script 共享同一个全局词法环境），
// 所以 send() / add() / currentRoom 能直接用；帧通过上面 switch 的 default
// 分支转进来（window.__EXTRA_FRAME__）。
// ============================================================================

let myRole = 'member';   // 我在当前房间的角色（SETTINGS 帧带过来）
let panelRoom = '';      // 面板针对的房间

function q(id) { return document.getElementById(id); }
function pickTab(name) {
  for (const b of document.querySelectorAll('#rpanel .tabs button[data-tab]')) {
    b.classList.toggle('on', b.dataset.tab === name);
  }
  for (const id of ['find', 'create', 'members', 'manage', 'cmd']) {
    q('ptab-' + id).style.display = (id === name) ? '' : 'none';
  }
  if (name === 'manage') {
    const can = (myRole === 'owner' || myRole === 'admin');
    q('manageTip').textContent = can
      ? '你是' + (myRole === 'owner' ? '群主' : '管理员') + '，下面的操作都能用。'
      : '你是普通成员：公告/审核/踢人会被服务器拒绝（改群昵称、退群仍可用）。';
  }
}
function openPanel(tab) {
  q('rpanel').classList.add('on');
  pickTab(tab || 'find');
  if ((tab || 'find') === 'members') send('MEMBERS');
}
function closePanel() { q('rpanel').classList.remove('on'); }

q('btnPanel').addEventListener('click', () => openPanel('manage'));
q('rclose').addEventListener('click', closePanel);
q('rpanel').addEventListener('click', (e) => { if (e.target.id === 'rpanel') closePanel(); });
for (const b of document.querySelectorAll('#rpanel .tabs button[data-tab]')) {
  b.addEventListener('click', () => {
    pickTab(b.dataset.tab);
    if (b.dataset.tab === 'members') send('MEMBERS');
  });
}
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closePanel(); });

// ---- 搜索 ----
q('findGo').addEventListener('click', () => {
  const kw = q('findKw').value.trim();
  if (!kw) return;
  q('findList').innerHTML = '<div class="tip">搜索中…</div>';
  send('FIND\t' + kw);
});
q('findKw').addEventListener('keydown', (e) => { if (e.key === 'Enter') q('findGo').click(); });

function joinModeText(mode) {
  return mode === 'direct' ? '可直接加入' : mode === 'auto' ? '自动审核' : '需管理员同意';
}
function renderHits(rows) {
  const box = q('findList');
  box.innerHTML = '';
  if (!rows.length) {
    box.innerHTML = '<div class="tip">没搜到。群必须由管理员打开「向公众展示」才会出现。</div>';
    return;
  }
  for (const h of rows) {
    const row = document.createElement('div'); row.className = 'prow';
    const g = document.createElement('div'); g.className = 'g';
    const b = document.createElement('b'); b.textContent = h.name || h.id;
    const s = document.createElement('span');
    s.textContent = h.id + '　' + h.members + '/' + h.max + ' 人　' + joinModeText(h.mode);
    g.append(b, s);
    const btn = document.createElement('button'); btn.textContent = '加入';
    btn.addEventListener('click', () => { send('INPUT\t/join ' + h.id); add('sys', '已请求加入 ' + h.id); });
    row.append(g, btn);
    box.appendChild(row);
  }
}

// ---- 建群 ----
q('createGo').addEventListener('click', () => {
  const id = q('newId').value.trim(), name = q('newName').value.trim();
  if (!id || !name) { add('err', '群 ID 和名称都要填'); return; }
  send('INPUT\t/create ' + id + ' ' + name);
  add('sys', '正在建群 ' + id + ' …');
  closePanel();
});

// ---- 成员 ----
q('membersGo').addEventListener('click', () => send('MEMBERS'));
function roleText(role) { return role === 'owner' ? '群主' : role === 'admin' ? '管理员' : '成员'; }
function renderMembers(rows) {
  const box = q('memberList');
  box.innerHTML = '';
  if (!rows.length) { box.innerHTML = '<div class="tip">没有成员数据（先进一个群，再点刷新）。</div>'; return; }
  const can = (myRole === 'owner' || myRole === 'admin');
  for (const m of rows) {
    const row = document.createElement('div'); row.className = 'prow';
    const g = document.createElement('div'); g.className = 'g';
    const b = document.createElement('b');
    b.textContent = m.nick + (m.display ? '（' + m.display + '）' : '');
    const s = document.createElement('span'); s.textContent = roleText(m.role);
    g.append(b, s); row.appendChild(g);
    if (can && m.role !== 'owner') {
      const r = document.createElement('button'); r.className = 'ghost';
      r.textContent = (m.role === 'admin') ? '降为成员' : '设为管理员';
      r.addEventListener('click', () => {
        send('INPUT\t/role ' + m.nick + ' ' + (m.role === 'admin' ? 'member' : 'admin'));
      });
      const k = document.createElement('button'); k.className = 'warn'; k.textContent = '踢出';
      k.addEventListener('click', () => { if (confirm('把 ' + m.nick + ' 踢出该群？')) send('INPUT\t/gkick ' + m.nick); });
      row.append(r, k);
    }
    box.appendChild(row);
  }
}

// ---- 管理 ----
q('annGo').addEventListener('click', () => {
  const t = q('annText').value.trim();
  if (!t) { send('INPUT\t/announce'); return; }
  send('INPUT\t/announce ' + t);
  add('sys', '已提交公告');
});
q('auditGo').addEventListener('click', () => {
  q('pendingList').innerHTML = '<div class="tip">拉取中…</div>';
  send('AUDIT');
});
q('nickGo').addEventListener('click', () => {
  const n = q('nickText').value.trim();
  if (!n) { add('err', '群昵称不能为空'); return; }
  send('INPUT\t/nick ' + n);
});
q('leaveGo').addEventListener('click', () => {
  if (!panelRoom) { add('err', '还没进任何群'); return; }
  if (!confirm('退出 ' + panelRoom + ' ？')) return;
  send('INPUT\t/leave ' + panelRoom);
});
q('deleteGo').addEventListener('click', () => {
  if (!panelRoom) { add('err', '还没进任何群'); return; }
  if (!confirm('解散 ' + panelRoom + ' ？不可恢复。')) return;
  send('INPUT\t/delete ' + panelRoom);
});
function renderPending(rows) {
  const box = q('pendingList');
  box.innerHTML = '';
  if (!rows.length) { box.innerHTML = '<div class="tip">没有待审的申请。</div>'; return; }
  for (const nick of rows) {
    const row = document.createElement('div'); row.className = 'prow';
    const g = document.createElement('div'); g.className = 'g';
    const b = document.createElement('b'); b.textContent = nick;
    g.appendChild(b); row.appendChild(g);
    const ok = document.createElement('button'); ok.textContent = '同意';
    ok.addEventListener('click', () => { send('INPUT\t/ok ' + nick); row.remove(); });
    const no = document.createElement('button'); no.className = 'warn'; no.textContent = '拒绝';
    no.addEventListener('click', () => { send('INPUT\t/no ' + nick); row.remove(); });
    row.append(ok, no);
    box.appendChild(row);
  }
}

// ---- 指令面板（走和聊天框完全相同的路径，权限由服务器判）----
q('cmdBan').addEventListener('click', () => { const n = q('cmdNick').value.trim(); if (n) send('INPUT\t/ban ' + n); });
q('cmdKick').addEventListener('click', () => { const n = q('cmdNick').value.trim(); if (n) send('INPUT\t/kick ' + n); });
q('cmdUnban').addEventListener('click', () => { const n = q('cmdNick').value.trim(); if (n) send('INPUT\t/unban ' + n); });
q('cmdSayGo').addEventListener('click', () => { const t = q('cmdSay').value.trim(); if (t) send('INPUT\t/say ' + t); });
q('cmdOps').addEventListener('click', () => send('INPUT\t/ops'));
q('blockGo').addEventListener('click', () => { const n = q('blockNick').value.trim(); if (n) send('INPUT\t/block ' + n); });
q('unblockGo').addEventListener('click', () => { const n = q('blockNick').value.trim(); if (n) send('INPUT\t/unblock ' + n); });
q('blocksGo').addEventListener('click', () => send('INPUT\t/blocks'));

// ---- 收网关推来的结构化列表 ----
let pendingHits = null, pendingMembers = null, pendingReqs = null;
let pendingPlay = '';

window.__EXTRA_FRAME__ = function (f) {
  switch (f[0]) {
    case 'SETTINGS':
      panelRoom = f[1] || '';
      myRole = f[3] || 'member';
      break;
    case 'GHITS':    pendingHits = []; break;
    case 'GHIT':
      if (pendingHits) {
        pendingHits.push({id: f[1], name: f[2], members: f[3], max: f[4], mode: f[5]});
      }
      break;
    case 'GHITS_END':
      if (pendingHits) { renderHits(pendingHits); pendingHits = null; }
      break;
    case 'MEMBERS':   pendingMembers = []; break;
    case 'MEMBER':
      if (pendingMembers) pendingMembers.push({nick: f[1], role: f[2], display: f[3] || ''});
      break;
    case 'MEMBERS_END':
      if (pendingMembers) { renderMembers(pendingMembers); pendingMembers = null; }
      break;
    case 'PENDING':   pendingReqs = []; break;
    case 'REQ':       if (pendingReqs) pendingReqs.push(f[1]); break;
    case 'PENDING_END':
      if (pendingReqs) { renderPending(pendingReqs); pendingReqs = null; }
      break;
    case 'FILEOFFER': {
      // 附件卡片：语音给「▶ 播放」，普通文件给下载
      const d = document.createElement('div'); d.className = 'row sys';
      const a = document.createElement('a');
      a.href = '#';
      a.textContent = (f[2] === '1' ? '▶ 播放语音 ' : '⬇ 下载文件 ') + f[1] +
                      (f[3] ? '（' + f[3] + '）' : '');
      a.addEventListener('click', (e) => {
        e.preventDefault();
        pendingPlay = (f[2] === '1' ? f[1] : '');
        send('INPUT\t/get ' + f[1]);
        if (f[2] !== '1') add('sys', '正在取 ' + f[1] + '，取好后点这里下载');
      });
      d.appendChild(a);
      const logEl = document.getElementById('log');
      logEl.appendChild(d);
      logEl.scrollTop = logEl.scrollHeight;
      break;
    }
    case 'FILE': {
      // 网关已经把附件落到本机：语音直接放，其它给下载链接
      if (pendingPlay && f[1] === pendingPlay) {
        const audio = new Audio('/file/' + encodeURIComponent(f[1]));
        audio.controls = true;
        audio.play().catch(() => add('sys', '浏览器拦了自动播放，点播放键即可'));
        const d = document.createElement('div'); d.className = 'row sys';
        d.appendChild(audio);
        document.getElementById('log').appendChild(d);
        pendingPlay = '';
      }
      break;
    }
  }
};

// ============================================================================
// 语音：在浏览器里录 16 kHz / 单声道 / 16 位 PCM WAV
//
// 格式和桌面端、安卓端**必须一致**，否则发出去对方放不出来
// （src/voice_notes.h：16000 Hz、单声道、16 位、2 MB 上限、最短 1 秒）。
// 浏览器没有现成的 WAV 录音，所以自己从 Web Audio 取 PCM 再拼 WAV 头。
// ============================================================================
let voice = null;

function wavHeader(dataBytes, sampleRate) {
  const buf = new ArrayBuffer(44);
  const v = new DataView(buf);
  const put = (off, s) => { for (let i = 0; i < s.length; i++) v.setUint8(off + i, s.charCodeAt(i)); };
  put(0, 'RIFF'); v.setUint32(4, 36 + dataBytes, true); put(8, 'WAVE');
  put(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true);
  v.setUint16(22, 1, true);              // 单声道
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, sampleRate * 2, true); // 字节率
  v.setUint16(32, 2, true);              // 块对齐
  v.setUint16(34, 16, true);             // 16 位
  put(36, 'data'); v.setUint32(40, dataBytes, true);
  return new Uint8Array(buf);
}

async function startVoice() {
  if (!navigator.mediaDevices || !window.AudioContext) {
    add('err', '这个浏览器不支持录音（需要 https 或 localhost）');
    return;
  }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({audio: {channelCount: 1}});
    const ctx = new AudioContext({sampleRate: 16000});
    const src = ctx.createMediaStreamSource(stream);
    const proc = ctx.createScriptProcessor(4096, 1, 1);
    const chunks = [];
    proc.onaudioprocess = (e) => { chunks.push(new Float32Array(e.inputBuffer.getChannelData(0))); };
    src.connect(proc);
    proc.connect(ctx.destination);
    voice = {stream, ctx, src, proc, chunks, startedAt: Date.now()};
    q('mic').textContent = '⏹';
    q('mic').style.color = '#ff6b6b';
    add('sys', '🎤 正在录音…再点一下发送（最多 64 秒）');
    voice.timer = setTimeout(() => stopVoice(), 64000);
  } catch (err) {
    add('err', '录不了音：' + err.message);
  }
}

async function stopVoice() {
  const v = voice;
  if (!v) return;
  voice = null;
  clearTimeout(v.timer);
  q('mic').textContent = '🎤';
  q('mic').style.color = '';
  v.stream.getTracks().forEach((t) => t.stop());
  try { v.proc.disconnect(); } catch (e) {}
  try { await v.ctx.close(); } catch (e) {}

  const seconds = (Date.now() - v.startedAt) / 1000;
  if (seconds < 1) { add('err', '太短了（至少 1 秒）'); return; }

  let total = 0;
  for (const c of v.chunks) total += c.length;
  const all = new Float32Array(total);
  let off = 0;
  for (const c of v.chunks) { all.set(c, off); off += c.length; }

  // 有些浏览器不认 sampleRate 请求，所以按真实采样率重采样到 16 kHz
  const srcRate = v.ctx.sampleRate || 16000;
  const ratio = srcRate / 16000;
  const outLen = Math.floor(all.length / ratio);
  const data = new Uint8Array(outLen * 2);
  for (let i = 0; i < outLen; i++) {
    let s = all[Math.floor(i * ratio)] || 0;
    s = s < -1 ? -1 : (s > 1 ? 1 : s);
    const v16 = s < 0 ? Math.round(s * 0x8000) : Math.round(s * 0x7fff);
    data[i * 2] = v16 & 0xFF;
    data[i * 2 + 1] = (v16 >> 8) & 0xFF;
  }
  if (data.length > 2 * 1024 * 1024) { add('err', '超过 64 秒上限，没发'); return; }

  const header = wavHeader(data.length, 16000);
  const whole = new Uint8Array(header.length + data.length);
  whole.set(header, 0);
  whole.set(data, header.length);
  let bin = '';
  for (let i = 0; i < whole.length; i++) bin += String.fromCharCode(whole[i]);
  send('VOICE\t' + btoa(bin));
  add('sys', '🎤 语音已提交（' + Math.round(seconds) + ' 秒，' + whole.length + ' 字节）');
}

q('mic').addEventListener('click', () => { voice ? stopVoice() : startVoice(); });
</script>
</body></html>
)HTML";

// ============================================================================
// 房间设置页（`/<房间名>/settings`）
//
// 数据来源：网关每秒比对一次当前会话的 8 项设置，变了就推 SETTINGS/SETTING 帧。
// 写回：浏览器发 `INPUT\t/set <项> <值>`，走和聊天框**同一条**路径
//       （ChatCore 翻译成 GSET，服务端按「我是不是这个群的主/管」校验）。
//       所以这里不拦权限——拦了也是假的，服务器才是权威。
// 群名和群公告由 ChatCore 负责 Base64 编码，这个页面传明文即可。
// ============================================================================
const char* kRoomSettingsHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>房间设置 · dchat</title>
<style>
:root{color-scheme:dark}
body{margin:0;background:#1b1b1f;color:#e6e6e6;font:15px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
header{display:flex;align-items:center;gap:12px;padding:10px 16px;background:#232329;border-bottom:1px solid #33333a}
header b{font-size:15px}
#state{color:#8a8a94;font-size:13px}
#state.on{color:#7ddca0}
a{color:#7aa7ff;text-decoration:none;font-size:13px}
.wrap{max-width:640px;margin:0 auto;padding:18px 16px 64px}
.row{padding:14px 0;border-bottom:1px solid #2e2e35}
.nm{font-weight:600;font-size:14px}
.hint{color:#8a8a94;font-size:12px;margin:2px 0 8px}
input[type=text],input[type=number],input[type=password],textarea,select{box-sizing:border-box;
  width:100%;padding:9px 11px;border-radius:8px;border:1px solid #3a3a42;background:#2a2a31;
  color:#e6e6e6;font:inherit}
textarea{min-height:88px;resize:vertical}
.toggle{display:flex;align-items:center;gap:10px}
.toggle input{width:22px;height:22px}
button{padding:12px;width:100%;border:0;border-radius:8px;background:#2f6fed;color:#fff;
  font:inherit;cursor:pointer;margin-top:20px}
button:disabled{opacity:.45;cursor:default}
.ok{background:#1d3a29;color:#7ddca0;padding:10px 14px;border-radius:8px;font-size:13px;margin-bottom:14px}
.bad{background:#3a1d1d;color:#ff9b9b;padding:10px 14px;border-radius:8px;font-size:13px;margin-bottom:14px}
#login{position:fixed;inset:0;background:rgba(10,10,12,.95);display:flex;align-items:center;
  justify-content:center;padding:16px}
#login form{background:#232329;border:1px solid #33333a;border-radius:12px;padding:24px;
  width:320px;max-width:100%;display:flex;flex-direction:column;gap:12px}
@media(max-width:640px){.wrap{padding:14px 12px 64px}}
</style></head><body>
<header>
  <a id="back" href="/">← 返回聊天</a>
  <b>房间设置</b>
  <span style="flex:1"></span>
  <span id="state">未连接</span>
</header>
<div class="wrap">
  <div id="msg"></div>
  <div id="panel" style="display:none">
    <div class="row">
      <div class="nm">群聊名称</div>
      <div class="hint">1&ndash;32 个字符，所有人都会看到</div>
      <input type="text" id="f-name" maxlength="32">
    </div>
    <div class="row">
      <div class="nm">群公告</div>
      <div class="hint">最多 500 个字符，进群时展示</div>
      <textarea id="f-ann" maxlength="500"></textarea>
    </div>
    <div class="row">
      <div class="nm">加群方式</div>
      <div class="hint">别人加这个群时怎么处理</div>
      <select id="f-join">
        <option value="approval">管理员审核（默认）</option>
        <option value="direct">直接添加</option>
        <option value="auto">自动审核</option>
      </select>
    </div>
    <div class="row">
      <div class="nm">最大人数</div>
      <div class="hint">2&ndash;500 人</div>
      <input type="number" id="f-max" min="2" max="500" step="1">
    </div>
    <div class="row">
      <div class="nm">总缓存最大大小</div>
      <div class="hint">服务端为这个群保留的聊天记录 + 文件预算（MB）</div>
      <input type="number" id="f-cache" min="1" step="1">
    </div>
    <div class="row">
      <div class="nm">向公众展示</div>
      <div class="hint">打开后别人能用 /find 搜到这个群</div>
      <label class="toggle"><input type="checkbox" id="f-listed"> 允许被搜索到</label>
    </div>
    <div class="row">
      <div class="nm">消息重要提醒</div>
      <div class="hint">打开后群里的重要消息会额外提醒</div>
      <label class="toggle"><input type="checkbox" id="f-imp"> 启用重要提醒</label>
    </div>
    <div class="row">
      <div class="nm">群文件加密</div>
      <div class="hint">打开后这个群的文件落盘时加密（防磁盘 / 备份泄露）</div>
      <label class="toggle"><input type="checkbox" id="f-enc"> 加密存储群文件</label>
    </div>
    <button id="save">保存</button>
    <p class="hint" id="rolegap" style="display:none">
      你在这个群里不是群主或管理员，改不了这些设置。
    </p>
    <p class="hint">只有群主和管理员能改；服务器会再校验一次。</p>
  </div>
</div>
<div id="login"><form id="loginForm">
  <b style="font-size:16px">登录 dchat</b>
  <div class="hint">改房间设置要先登录（只有群主和管理员改得动）</div>
  <input type="text" id="user" placeholder="昵称" autocomplete="username">
  <input type="password" id="pass" placeholder="密码" autocomplete="current-password">
  <input type="text" id="host" value="dchat.xin:5555" placeholder="服务器:端口">
  <button type="submit">登录</button>
</form></div>
<script>
window.DCHAT_ROOM = "__ROOM_ID__";
</script>
<script>
const ROOM = window.DCHAT_ROOM || '';
let ws = null, credentials = null, reconnectDelay = 2000, myRole = '';
let original = {};   // 服务端下发的原值，用来算「到底改了哪几项」

function el(id) { return document.getElementById(id); }
function setState(t, on) { el('state').textContent = t; el('state').className = on ? 'on' : ''; }
function send(line) { if (ws && ws.readyState === 1) ws.send(line); }
function msg(text, bad) {
  const d = document.createElement('div');
  d.className = bad ? 'bad' : 'ok';
  d.textContent = text;
  el('msg').innerHTML = '';
  el('msg').appendChild(d);
}
if (ROOM) el('back').href = '/room/' + encodeURIComponent(ROOM);

function connect(host, user, pass) {
  el('login').style.display = 'none';
  setState('连接中…', false);
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?server=' + encodeURIComponent(host));
  ws.onopen = () => {
    reconnectDelay = 2000;
    setState('已连接', true);
    send('LOGIN\t' + user + '\t' + pass + '\t0');
  };
  ws.onmessage = (ev) => {
    const f = ev.data.split('\t');
    if (f[0] === 'STATE') {
      setState(f[1] + (f[2] ? '　' + f[2] : ''), f[1] === '已登录');
      // 登录成功后进这个房间：SETTINGS 帧描述的是「当前会话」的设置，不进房就没有
      if (f[1] === '已登录' && ROOM) send('INPUT\t/open ' + ROOM);
      return;
    }
    if (f[0] === 'SETTINGS') { original = {}; myRole = f[3] || ''; return; }
    if (f[0] === 'SETTING') { original[f[1]] = f[2]; return; }
    if (f[0] === 'SETTINGS_END') { fill(); return; }
    if (f[0] === 'ERROR') { msg(f[1], true); return; }
    // SYS / INFO 是聊天流的噪音，设置页不显示
  };
  ws.onclose = () => {
    setState('已断开', false);
    if (credentials) {
      setTimeout(() => connect(credentials.host, credentials.user, credentials.pass),
                 reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 2, 30000);
    }
  };
  ws.onerror = () => msg('连接出错', true);
}

/** 把服务端下发的 8 项填进表单。 */
function fill() {
  const put = (id, value) => {
    const e = el(id);
    if (!e) return;
    if (e.type === 'checkbox') e.checked = (value === 'true');
    else e.value = value;
  };
  put('f-name', original.name || '');
  put('f-ann', original.announcement || '');
  put('f-join', original.joinmode || 'approval');
  put('f-max', original.maxmembers || '');
  put('f-cache', original.cachemaxmb || '');
  put('f-listed', original.listed || 'false');
  put('f-imp', original.importantnotice || 'false');
  put('f-enc', original.fileencryption || 'false');
  el('panel').style.display = '';
  // 能不能改由服务器说了算，这里只是提前把按钮灰掉，少一次白跑
  const canEdit = (myRole === 'owner' || myRole === 'admin');
  el('rolegap').style.display = canEdit ? 'none' : '';
  el('save').disabled = !canEdit;
}

function save() {
  const now = {
    name: el('f-name').value,
    announcement: el('f-ann').value,
    joinmode: el('f-join').value,
    maxmembers: el('f-max').value,
    cachemaxmb: el('f-cache').value,
    listed: el('f-listed').checked ? 'true' : 'false',
    importantnotice: el('f-imp').checked ? 'true' : 'false',
    fileencryption: el('f-enc').checked ? 'true' : 'false'
  };
  let sent = 0;
  for (const key of Object.keys(now)) {
    // 空值跳过：数值框被清空时服务器只会回一个非法值错误，不如不提交
    if (now[key] === '') continue;
    if ((original[key] || '') === now[key]) continue;
    send('INPUT\t/set ' + key + ' ' + now[key]);
    ++sent;
  }
  msg(sent ? ('已提交 ' + sent + ' 项修改，服务端确认后这里会自动刷新') : '没有改动', false);
}

el('save').addEventListener('click', save);
el('loginForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const host = el('host').value.trim() || 'dchat.xin:5555';
  credentials = {host: host, user: el('user').value, pass: el('pass').value};
  try {
    localStorage.setItem('dchat_login',
      JSON.stringify({host: credentials.host, user: credentials.user, pass: credentials.pass}));
  } catch (err) { /* 隐私模式忽略 */ }
  connect(credentials.host, credentials.user, credentials.pass);
});

// 和聊天页共用同一个 localStorage 键：登过一次，两边都自动登录
(function autoLogin() {
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem('dchat_login') || 'null'); } catch (e) { saved = null; }
  if (!saved || !saved.user) {
    if (ROOM) msg('要改「' + ROOM + '」的设置，先登录。', true);
    return;
  }
  el('host').value = saved.host || 'dchat.xin:5555';
  el('user').value = saved.user;
  el('pass').value = saved.pass || '';
  credentials = {host: saved.host, user: saved.user, pass: saved.pass};
  connect(saved.host, saved.user, saved.pass);
})();
</script>
</body></html>
)HTML";

const char* kDownloadHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>下载 dchat</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; font:15px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
         background:#1b1b1f; color:#e6e6e6; }
  .wrap { max-width:820px; margin:0 auto; padding:40px 20px; }
  h1 { font-size:26px; margin:0 0 6px; }
  p.sub { color:#8a8a94; margin:0 0 28px; }
  .card { background:#232329; border:1px solid #33333a; border-radius:12px; padding:18px 20px;
          margin-bottom:14px; }
  .card h2 { font-size:16px; margin:0 0 6px; }
  .meta { color:#8a8a94; font-size:13px; margin-bottom:12px; }
  a.btn { display:inline-block; padding:9px 16px; border-radius:8px; background:#2f6fed;
          color:#fff; text-decoration:none; font-size:14px; }
  a.btn.alt { background:#2f2f37; }
  code { background:#2a2a31; padding:2px 6px; border-radius:5px; font-size:12.5px;
         word-break:break-all; }
  .sum { color:#6f6f78; font-size:12px; margin-top:10px; }
  .foot { color:#666; font-size:12px; margin-top:26px; }
</style></head><body><div class="wrap">
<h1>dchat</h1>
<p class="sub">三个端任选其一，连的是同一台服务器：<code>dchat.xin:5555</code></p>
<div id="list"></div>
<div class="card">
  <h2>源码</h2>
  <div class="meta">自己编译或者想改都行。</div>
  <a class="btn alt" href="https://github.com/dongfang20101113/DChat">GitHub 仓库</a>
</div>
<div class="foot">
  网页版就在这个站里：<a href="/">打开聊天</a><br>
  校验值来自 <a href="/dl/SHA256SUMS">SHA256SUMS</a>，可以下载后自己核：
  <code>sha256sum 文件名</code>（Windows 用 <code>certutil -hashfile 文件名 SHA256</code>）
</div>
</div>
<script>
const items = [
  { t:'Windows 桌面版', d:'解压即用，不用安装。群聊 / 私聊 / 语音 / 文件都在这里。',
    f:'dchat-windows-x64.zip' },
  { t:'Android 版', d:'APK，下载后直接装。',
    f:'dchat-android.apk' },
  { t:'Linux 服务端', d:'自己搭一台。包里是 dchat_server（服务端）、dchat_client_linux（终端客户端）、dchat_webd（网页版网关）。',
    f:'dchat-linux-x86_64.tar.gz' },
];

// 校验值不写死在页面里：从 /dl/SHA256SUMS 读。这样换安装包不用重编网关。
async function loadSums() {
  try {
    const r = await fetch('/dl/SHA256SUMS');
    if (!r.ok) return {};
    const out = {};
    for (const line of (await r.text()).split('\n')) {
      const m = line.trim().split(/\s+/);
      if (m.length >= 2) out[m[1]] = m[0];
    }
    return out;
  } catch (e) { return {}; }
}

(async () => {
  const sums = await loadSums();
  const list = document.getElementById('list');
  for (const it of items) {
    const card = document.createElement('div');
    card.className = 'card';
    const h = document.createElement('h2'); h.textContent = it.t;
    const meta = document.createElement('div'); meta.className = 'meta'; meta.textContent = it.d;
    const a = document.createElement('a');
    a.className = 'btn'; a.href = '/dl/' + encodeURIComponent(it.f);
    a.textContent = '下载 ' + it.f;
    card.append(h, meta, a);
    if (sums[it.f]) {
      const s = document.createElement('div');
      s.className = 'sum';
      s.innerHTML = 'SHA-256 <code>' + sums[it.f] + '</code>';
      card.appendChild(s);
    }
    list.appendChild(card);
  }
})();
</script></body></html>
)HTML";

namespace {

struct Config {
    int port = 8080;
    std::string serverHost = "127.0.0.1";
    int serverPort = 5555;
    std::string dlDir = "./dchat-downloads";
    // 管理通道：指向服务器 --admin-port 开的那个**仅回环**端口。
    // 0 = 不启用管理员页面（此时 /admin 直接 404，攻击面为零）。
    std::string adminHost = "127.0.0.1";
    int adminPort = 0;
    std::string adminTokenFile = "/var/lib/dchat/dchat-admin-token.txt";
    // op 名单（服务器工作目录里的 dchat-admins.txt）。
    //
    // 网页管理页的**唯一**权限依据就是它：昵称在名单里 = op = 能进 /admin。
    // 注意这是「读文件」而不是问服务器——服务器那边 IsOp() 用的是内存表，
    // 但 /op 和 /deop **改完立刻落盘**，所以读文件拿到的就是当前名单。
    std::string adminsFile = "dchat-admins.txt";
};

// 定义在文件后半部分（ServeConnection 之后）；这里先声明，路由那边要用。
void ServeAdmin(dchat::sock::Handle handle, const std::string& rawRequest);

Config g_config;

bool ReadRequest(dchat::sock::Handle handle, std::string* out) {
    std::string buffer;
    char chunk[4096];
    std::size_t headerEnd = std::string::npos;
    while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        const std::ptrdiff_t got = dchat::sock::Receive(handle, chunk, sizeof(chunk));
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(got));
        if (buffer.size() > 64 * 1024) return false;  // 请求头不该这么大
    }

    // **请求体也要读**。之前只读到请求头结束就返回，于是表单 POST 上来 body 是空的
    // —— 登录密码根本收不到。按 Content-Length 把剩余字节读完再交出去。
    std::size_t contentLength = 0;
    {
        const std::string head = Lower(buffer.substr(0, headerEnd));
        const std::string needle = "content-length:";
        const std::size_t at = head.find(needle);
        if (at != std::string::npos) {
            contentLength = static_cast<std::size_t>(
                std::atoi(buffer.c_str() + static_cast<std::ptrdiff_t>(at + needle.size())));
        }
    }
    if (contentLength > 64 * 1024) return false;  // 不信任客户端声明的长度
    const std::size_t want = headerEnd + 4 + contentLength;
    while (buffer.size() < want) {
        const std::ptrdiff_t got = dchat::sock::Receive(handle, chunk, sizeof(chunk));
        if (got <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(got));
    }
    *out = buffer;
    return true;
}

// ============================================================================
// 房间页的路由与注入
//
// URL 约定：
//   /                  聊天，不预设房间（和以前一样）
//   /<房间名>            聊天，登录后自动进这个房间
//   /<房间名>/settings   这个房间的设置
//   /admin             服务器整体设置（chatrule），和房间分开
// ============================================================================

/**
 * 一段 path 能不能当房间号用。
 *
 * **字符集收得很死是故意的**：这个值会被填进页面里的 JS 字符串字面量
 * （`window.DCHAT_ROOM = "..."`），放宽就等于开一个 XSS 口子。
 * 服务器那边的群 ID 本来就是 [a-z0-9_-]，所以这个限制挡不住合法的群。
 * 大厅是 "hall"，私聊是 "dm-xxxx"，同样落在这个集合里。
 */
bool IsWebRoomId(const std::string& text) {
    if (text.size() < 2 || text.size() > 64) return false;
    for (char ch : text) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'z';
        if (!digit && !lower && ch != '_' && ch != '-') return false;
    }
    // 房间名就是房间名，不需要保留字黑名单了 —— 房间全在 /room/ 下面，
    // 和 /admin、/download 这些静态路径不在同一个命名空间。
    // 唯一还要收死的是字符集：这个值会进页面里的 JS 字符串字面量。
    return true;
}

/** 把页面里的占位符换成实际值。房间号已过 IsWebRoomId，不含要转义的字符；上限是数字。 */
std::string FillPage(const char* page, const std::string& roomId) {
    std::string html = page;
    auto replaceAll = [&html](const std::string& token, const std::string& value) {
        std::size_t at = html.find(token);
        while (at != std::string::npos) {
            html.replace(at, token.size(), value);
            at = html.find(token, at + value.size());
        }
    };
    replaceAll("__ROOM_ID__", roomId);
    // 上传上限放进页面，页面上的提示文字和前端那道预检才和服务器一致
    replaceAll("__UPLOAD_MB__", std::to_string(WebUploadLimitMb()));
    return html;
}

void ServeConnection(dchat::sock::Handle handle) {
    dchat::sock::SetNoSigpipe(handle);
    dchat::sock::SetNoDelay(handle);
    dchat::sock::SetReceiveTimeout(handle, 30000);

    std::string request;
    if (!ReadRequest(handle, &request)) {
        dchat::sock::Close(handle);
        return;
    }

    // ---- 第一行：请求行 ----
    const std::size_t lineEnd = request.find("\r\n");
    std::string firstLine = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);
    std::string method;
    std::string path;
    {
        const std::size_t sp1 = firstLine.find(' ');
        const std::size_t sp2 = firstLine.rfind(' ');
        if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1) {
            method = firstLine.substr(0, sp1);
            path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }
    const std::string headers = Lower(request);

    // ---- WebSocket 升级 ----
    if (headers.find("upgrade: websocket") != std::string::npos) {
        std::string key;
        {
            const std::string needle = "sec-websocket-key:";
            const std::size_t at = headers.find(needle);
            if (at != std::string::npos) {
                std::size_t begin = at + needle.size();
                while (begin < headers.size() && (headers[begin] == ' ' || headers[begin] == '\t')) {
                    ++begin;
                }
                const std::size_t end = headers.find("\r\n", begin);
                key = request.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            }
        }
        if (key.empty()) {
            dchat::sock::Close(handle);
            return;
        }
        Sha1 sha;
        const std::string accept =
            dchat::Base64Encode(sha.Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        const std::string reply = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        SendAll(handle, reply.data(), reply.size());

        // ?server=host:port 允许前端指定服务器；默认用命令行给的
        std::string host = g_config.serverHost;
        int port = g_config.serverPort;
        const std::size_t q = path.find("server=");
        if (q != std::string::npos) {
            std::string value = path.substr(q + 7);
            const std::size_t andAt = value.find('&');
            if (andAt != std::string::npos) value = value.substr(0, andAt);
            value = PercentDecode(value);  // 前端会把冒号编成 %3A
            const std::size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                host = value.substr(0, colon);
                const int parsed = std::atoi(value.c_str() + colon + 1);
                if (parsed > 0 && parsed < 65536) port = parsed;
            } else if (!value.empty()) {
                host = value;
            }
        }
        std::fprintf(stderr, "[session] 上游 %s:%d\n", host.c_str(), port);
        std::fflush(stderr);

        auto session = std::make_shared<Session>(handle, host, port);
        {
            std::lock_guard<std::mutex> lock(g_sessionsMutex);
            g_sessions.push_back(session);
        }
        session->Start();

        // ---- WebSocket 帧循环：浏览器发来的帧一律带掩码（RFC 6455 要求） ----
        std::string pending;
        char buffer[4096];
        int idleTimeouts = 0;
        while (!session->closed()) {
            const std::ptrdiff_t got = dchat::sock::Receive(handle, buffer, sizeof(buffer));
            if (got == 0) {
                std::fprintf(stderr, "[ws] 对端关闭连接\n");
                std::fflush(stderr);
                break;
            }
            if (got < 0) {
                // **超时不是断开**。这里以前写成 `got <= 0 -> break`，等于给浏览器
                // 加了个"30 秒不说话就踢掉"的规则——用户看到的"自动断联"就是它。
                //
                // 现在：超时就继续等，只数着次数。连续 120 次（30 秒 × 120 = 1 小时）
                // 一次数据都没有，才认为连接是死的，免得线程永远挂着。
                if (++idleTimeouts >= 120) {
                    std::fprintf(stderr, "[ws] 连续 1 小时无数据，判定为死连接\n");
                    std::fflush(stderr);
                    break;
                }
                continue;
            }
            idleTimeouts = 0;
            pending.append(buffer, static_cast<std::size_t>(got));
            while (pending.size() >= 2) {
                const unsigned char b0 = static_cast<unsigned char>(pending[0]);
                const unsigned char b1 = static_cast<unsigned char>(pending[1]);
                const int opcode = b0 & 0x0F;
                std::size_t length = b1 & 0x7F;
                std::size_t offset = 2;
                if (length == 126) {
                    if (pending.size() < 4) break;
                    length = (static_cast<unsigned char>(pending[2]) << 8) |
                             static_cast<unsigned char>(pending[3]);
                    offset = 4;
                } else if (length == 127) {
                    if (pending.size() < 10) break;
                    length = 0;
                    for (int i = 0; i < 8; ++i) {
                        length = (length << 8) | static_cast<unsigned char>(pending[2 + i]);
                    }
                    offset = 10;
                }
                const bool masked = (b1 & 0x80) != 0;
                const std::size_t maskBytes = masked ? 4 : 0;
                if (pending.size() < offset + maskBytes + length) break;
                std::string payload = pending.substr(offset + maskBytes, length);
                if (masked) {
                    for (std::size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<char>(payload[i] ^
                                                       pending[offset + (i % 4)]);
                    }
                }
                pending.erase(0, offset + maskBytes + length);
                if (opcode == 0x8) {  // close
                    session->Close();
                    break;
                }
                if (opcode == 0x9) {  // ping -> pong
                    std::string pong;
                    pong.push_back(static_cast<char>(0x8A));
                    pong.push_back(static_cast<char>(payload.size()));
                    pong += payload;
                    SendAll(handle, pong.data(), pong.size());
                    continue;
                }
                if (opcode == 0x1 || opcode == 0x0) session->OnBrowserLine(payload);            }
        }
        session->Close();
        {
            std::lock_guard<std::mutex> lock(g_sessionsMutex);
            g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), session),
                             g_sessions.end());
        }
        return;
    }

    // ---- 普通 HTTP：页面 + 静态下载 ----
    auto reply = [&](const std::string& status, const std::string& type,
                     const std::string& body) {
        const std::string head = "HTTP/1.1 " + status +
                                 "\r\nContent-Type: " + type +
                                 "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        SendAll(handle, head.data(), head.size());
        SendAll(handle, body.data(), body.size());
    };

    // 路由要**剥掉查询串**再比：`/?_=123` 之前直接 404（截图时踩到的）。
    // 注意 WebSocket 分支在上面，那条要用查询串里的 ?server=，不能用这个。
    std::string route = path;
    const std::size_t qmark = route.find('?');
    if (qmark != std::string::npos) route = route.substr(0, qmark);

    if (route == "/" || route == "/index.html") {
        // 也要走一次 FillRoom（房间号填空）：否则页面里留着字面量 "__ROOM_ID__"，
        // 前端会把它当成房间名去 /open，等于每次打开首页都白报一条错。
        reply("200 OK", "text/html; charset=utf-8", FillPage(kIndexHtml, ""));
    } else if (route == "/download") {
        reply("200 OK", "text/html; charset=utf-8", kDownloadHtml);
    } else if (g_config.adminPort > 0 && (route == "/admin" || route.rfind("/admin/", 0) == 0)) {
        // 管理员页面。adminPort 为 0 时这条分支根本不会命中，/admin 直接落到 404。
        ServeAdmin(handle, request);
    } else if (path.rfind("/dl/", 0) == 0) {
        // 下载：文件名里不许出现 .. 或 /
        std::string name = path.substr(4);
        const std::size_t q = name.find('?');
        if (q != std::string::npos) name = name.substr(0, q);
        if (name.empty() || name.find("..") != std::string::npos ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            reply("400 Bad Request", "text/plain; charset=utf-8", "bad name");
        } else {
            const std::string full = g_config.dlDir + "/" + name;
            FILE* file = std::fopen(full.c_str(), "rb");
            if (file == nullptr) {
                reply("404 Not Found", "text/plain; charset=utf-8",
                      "没有这个文件：" + name + "（放在 " + g_config.dlDir + " 里）");
            } else {
                std::fseek(file, 0, SEEK_END);
                const long size = std::ftell(file);
                std::fseek(file, 0, SEEK_SET);
                const std::string head = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: application/octet-stream\r\n"
                                         "Content-Disposition: attachment; filename=\"" + name +
                                         "\"\r\nContent-Length: " + std::to_string(size) +
                                         "\r\nConnection: close\r\n\r\n";
                SendAll(handle, head.data(), head.size());
                char chunk[65536];
                std::size_t left = static_cast<std::size_t>(size);
                while (left > 0) {
                    const std::size_t want = std::min(left, sizeof(chunk));
                    const std::size_t got = std::fread(chunk, 1, want, file);
                    if (got == 0) break;
                    if (!SendAll(handle, chunk, got)) break;
                    left -= got;
                }
                std::fclose(file);
            }
        }
    } else if (path.rfind("/file/", 0) == 0) {
        // 浏览器取一个已经从 dchat 服务器下载到网关本机的附件。
        // 文件名用附件 ID（不是原始文件名），避免中文/空格在头里出问题。
        std::string id = path.substr(6);
        const std::size_t q = id.find('?');
        if (q != std::string::npos) id = id.substr(0, q);
        const std::string full = DownloadedPath(id);
        FILE* file = (id.empty() || full.empty()) ? nullptr : std::fopen(full.c_str(), "rb");
        if (file == nullptr) {
            reply("404 Not Found", "text/plain; charset=utf-8",
                  "这个附件还没下载到网关（先在聊天框发 /get " + id + "）");
        } else {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            const std::string head = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/octet-stream\r\n"
                                     "Content-Disposition: attachment; filename=\"" + id +
                                     "\"\r\nContent-Length: " + std::to_string(size) +
                                     "\r\nConnection: close\r\n\r\n";
            SendAll(handle, head.data(), head.size());
            char chunk[65536];
            std::size_t left = static_cast<std::size_t>(size);
            while (left > 0) {
                const std::size_t want = std::min(left, sizeof(chunk));
                const std::size_t got = std::fread(chunk, 1, want, file);
                if (got == 0) break;
                if (!SendAll(handle, chunk, got)) break;
                left -= got;
            }
            std::fclose(file);
        }
    } else {
        // ---- 房间页：/room/<房间名> 和 /room/<房间名>/settings ----
        //
        // **为什么房间要挂在 /room/ 前面**：/admin 已经占了一段，以后再往里加
        // /files、/settings、/search 之类，房间名迟早和它们撞上（群叫 admin 怎么办？）。
        // 加一层前缀之后，命名空间彻底分开：静态功能在根上，房间一律在 /room/ 下面。
        // 副作用是房间名不再需要保留字黑名单——/room/admin 就是那个群，不冲突。
        std::vector<std::string> segments;
        if (route.size() > 1) {
            std::size_t from = 1;  // 跳过开头的 /
            while (true) {
                const std::size_t slash = route.find('/', from);
                segments.push_back(route.substr(
                    from, slash == std::string::npos ? std::string::npos : slash - from));
                if (slash == std::string::npos) break;
                from = slash + 1;
            }
            // 尾部多一个斜杠（/room/abc/）当同一个地址，别让手输的人吃 404
            if (segments.size() > 1 && segments.back().empty()) segments.pop_back();
        }
        const bool isRoomPath = !segments.empty() && segments[0] == "room";
        if (isRoomPath && segments.size() == 1) {
            // /room 单独访问：当成首页（没指定哪个房间）
            reply("200 OK", "text/html; charset=utf-8", FillPage(kIndexHtml, ""));
        } else if (isRoomPath && segments.size() == 2 && IsWebRoomId(segments[1])) {
            reply("200 OK", "text/html; charset=utf-8", FillPage(kIndexHtml, segments[1]));
        } else if (isRoomPath && segments.size() == 3 && segments[2] == "settings" &&
                   IsWebRoomId(segments[1])) {
            reply("200 OK", "text/html; charset=utf-8",
                  FillPage(kRoomSettingsHtml, segments[1]));
        } else {
            reply("404 Not Found", "text/plain; charset=utf-8",
                  "not found　（房间是 /room/<房间名>，房间设置是 /room/<房间名>/settings）");
        }
    }
    (void)method;
}

// ============================================================================
// 管理员页面
//
// ## 安全模型（整个功能的地基，改之前先读完）
//
// 1. **规则写权限只在服务器回环端口上**。网页这边只是"转发"，自己不留写规则的能力。
// 2. **口令不落 Cookie**：登录后只把随机 session id 放 Cookie
//    （HttpOnly + SameSite=Strict + Path=/admin），口令原文不离开服务器。
// 3. **CSRF 双提交**：表单隐藏字段 == 会话 id，服务端再比一次。
// 4. **失败限流**：5 分钟内错 5 次锁住。
// 5. 默认关闭：网关没配 --admin 时 /admin 直接 404。
// ============================================================================

struct AdminRuleMeta {
    const char* name;
    bool isBool;
    const char* hint;
};

/** 只是页面文案。规则**存不存在**和**范围**都以服务器 LIST / SET 结果为准。 */
const AdminRuleMeta kAdminRules[] = {
    {"chatinterval", false, "两条消息之间最少间隔（毫秒），0 = 不限制"},
    {"documentsize", false, "单个文件最大大小（MB）"},
    {"keepchathistory", true, "新加入的客户端能否看到之前的聊天记录和文件"},
    {"chatcolor", true, "聊天里能否用彩色代码（关掉后色码原样显示）"},
    {"maxservertemp", false, "服务端保存文件 + 聊天记录缓存的总上限（MB）"},
    {"uploadrate", false, "单个客户端上传限速（KB/s），0 = 不限制"},
    {"downloadrate", false, "单个客户端下载限速（KB/s），0 = 不限制"},
    {"maxtextlen", false, "单条消息最大字符数（Unicode 码点），0 = 不限制"},
    {"maxtextlines", false, "单条消息最大行数，0 = 不限制"},
    {"maxconns", false, "同时连接总数上限，0 = 不限制"},
    {"maxconnsperip", false, "同一 IP 的同时连接数上限，0 = 不限制"},
    {"loginfails", false, "同一 IP 在 5 分钟内允许的登录失败次数，0 = 不限制"},
    {"handshaketimeout", false, "连上后多少秒内必须登录，0 = 不限制"},
    {"registerinterval", false, "同一 IP 两次注册的最小间隔（秒），0 = 不限制"},
    {"maxaccounts", false, "账号总数上限，0 = 不限制"},
    {"offlinemessages", false, "登录时最多补发多少条离线消息，0 = 不补发"},
    {"webuploadmb", false, "网页版单文件上传上限（MB），不能超过 documentsize"},
};

std::string RandomHex(int bytes) {
    static std::atomic<unsigned long long> counter{0};
    unsigned long long seed = static_cast<unsigned long long>(std::time(nullptr)) ^
                              (static_cast<unsigned long long>(counter.fetch_add(1)) << 32);
    std::string out;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const unsigned char byte = static_cast<unsigned char>((seed >> 33) & 0xFF);
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// op 管理会话
//
// 一条规则：**能不能进 /admin 只看这里**。
//   登录成功（服务器认证过账号密码）+ 昵称在 dchat-admins.txt 里 → 发号
//   其余情况 → 不发号；浏览器手上没有有效号就进不去
//
// 号是"我这次登录是 op"的凭据，绑昵称、12 小时过期。
// 它**不是共享口令**：每个人登录拿到的是自己的号，没有号的人拿不到别人的。
// ---------------------------------------------------------------------------

struct OpSession {
    std::string nick;
    std::time_t expires = 0;
};

std::mutex g_opMutex;
std::map<std::string, OpSession> g_opSessions;
constexpr int kOpSessionSec = 12 * 3600;

/** 读 op 名单文件。一行一个昵称，# 开头是注释（和服务端 dchat-admins.txt 同格式）。 */
bool IsOpNick(const std::string& nick) {
    if (nick.empty()) return false;
    FILE* file = std::fopen(g_config.adminsFile.c_str(), "rb");
    // 读不到名单就当谁都不是 op —— 失败时关闭，不是失败时放行
    if (file == nullptr) return false;
    std::string text;
    char chunk[1024];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, got);
    std::fclose(file);
    std::size_t from = 0;
    while (from <= text.size()) {
        const std::size_t end = text.find('\n', from);
        std::string line =
            text.substr(from, end == std::string::npos ? std::string::npos : end - from);
        from = (end == std::string::npos) ? text.size() + 1 : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line == nick) return true;
    }
    return false;
}

/** 24 字节强随机 → 48 位十六进制。用 /dev/urandom，不用 LCG。 */
std::string RandomSessionId() {
    unsigned char bytes[24];
    FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (urandom != nullptr) {
        const std::size_t got = std::fread(bytes, 1, sizeof(bytes), urandom);
        std::fclose(urandom);
        if (got == sizeof(bytes)) {
            static const char* kHex = "0123456789abcdef";
            std::string out;
            for (unsigned char byte : bytes) {
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0x0F]);
            }
            return out;
        }
    }
    return RandomHex(24);  // 极端情况（没有 /dev/urandom）退回老办法，至少不会崩
}

/** 发一个 op 会话号，顺手清掉已经过期的。 */
std::string MintOpSession(const std::string& nick) {
    const std::string id = RandomSessionId();
    const std::time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(g_opMutex);
    for (auto it = g_opSessions.begin(); it != g_opSessions.end();) {
        if (it->second.expires < now) {
            it = g_opSessions.erase(it);
        } else {
            ++it;
        }
    }
    g_opSessions[id] = OpSession{nick, now + kOpSessionSec};
    return id;
}

/** 校验会话号；有效就把它对应的昵称写进 nick。 */
bool OpSessionNick(const std::string& id, std::string* nick) {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(g_opMutex);
    const auto it = g_opSessions.find(id);
    if (it == g_opSessions.end()) return false;
    if (it->second.expires < std::time(nullptr)) {
        g_opSessions.erase(it);
        return false;
    }
    if (nick != nullptr) *nick = it->second.nick;
    return true;
}

// 原来这里有个 SlowEqualsStr，是用来比对共享管理口令的。
// 口令登录已经整个删掉（改成 op 账号鉴权），它也就没用了，删掉免得留个死函数。

/** 每次现读：换口令不用重启网关，也少一份内存副本。 */
std::string AdminToken() {
    FILE* file = std::fopen(g_config.adminTokenFile.c_str(), "rb");
    if (file == nullptr) return std::string();
    char buffer[256] = {0};
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    std::string token(buffer, got);
    while (!token.empty() &&
           (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    return token;
}

/** 连服务器回环管理端口：认证 + 一条请求 + 读回（短连接，每次重新认证）。 */
std::string AdminCall(const std::string& request) {
    if (g_config.adminPort <= 0) return "ERR 管理通道未启用";
    const std::string token = AdminToken();
    if (token.empty()) return "ERR 读不到管理口令文件";
    const dchat::sock::Handle sock = dchat::sock::CreateTcp();
    if (sock == dchat::sock::kInvalid) return "ERR 建 socket 失败";
    dchat::sock::SetNoSigpipe(sock);
    dchat::sock::SetReceiveTimeout(sock, 5000);
    const dchat::sock::Address address =
        dchat::sock::MakeAddress(g_config.adminHost, g_config.adminPort);
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        dchat::sock::Close(sock);
        return "ERR 连不上管理端口";
    }
    const std::string payload = "ADMIN " + token + "\n" + request + "\n";
    dchat::sock::SendAll(sock, payload.data(), payload.size());
    std::string reply;
    char chunk[4096];
    while (true) {
        const std::ptrdiff_t got = dchat::sock::Receive(sock, chunk, sizeof(chunk));
        if (got <= 0) break;
        reply.append(chunk, static_cast<std::size_t>(got));
        if (reply.find("RULES_END") != std::string::npos) break;
        if (request != "LIST" && reply.find("\nOK ") != std::string::npos) break;
        if (request != "LIST" && reply.find("\nERR ") != std::string::npos) break;
    }
    dchat::sock::Close(sock);
    const std::size_t firstNewline = reply.find('\n');
    if (firstNewline != std::string::npos && reply.rfind("OK 已认证", 0) == 0) {
        reply = reply.substr(firstNewline + 1);
    }
    return reply;
}

// ---------------------------------------------------------------------------
// 网页版单文件上传上限
//
// 以前这个值是写死在这份文件里的 8 MB 常量：服务器上的 chatrule 管不到它，
// 管理员把 documentsize 调到 200 MB，网页版还是只让传 8 MB，而且页面上那句
// 「上限 8 MB」也不会变。现在它归 chatrule 的 webuploadmb 管。
//
// 值从管理通道的 LIST 拿（和 /admin 页面完全同一个来源，不会出现两套数）。
// 缓存 5 秒：页面每次请求都会问，但没必要每次都连一次 5556。
// ---------------------------------------------------------------------------
constexpr int kWebUploadFallbackMb = 8;

int WebUploadLimitMb() {
    static std::mutex mutex;
    static int cached = kWebUploadFallbackMb;
    static std::time_t stamp = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const std::time_t now = std::time(nullptr);
    if (stamp != 0 && now - stamp < 5) return cached;

    int value = kWebUploadFallbackMb;
    const std::string listing = AdminCall("LIST");
    std::size_t from = 0;
    while (from < listing.size()) {
        const std::size_t end = listing.find('\n', from);
        const std::string line =
            listing.substr(from, end == std::string::npos ? std::string::npos : end - from);
        from = (end == std::string::npos) ? listing.size() : end + 1;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        if (line.substr(0, tab) != "webuploadmb") continue;
        const int parsed = std::atoi(line.c_str() + static_cast<std::ptrdiff_t>(tab) + 1);
        if (parsed > 0) value = parsed;
        break;
    }
    cached = value;
    stamp = now;
    return cached;
}

/** 从 Cookie 头里取某个名字的值。 */
std::string CookieValue(const std::string& headLower, const std::string& head,
                        const std::string& name) {
    const std::size_t at = headLower.find("cookie:");
    if (at == std::string::npos) return std::string();
    const std::size_t end = headLower.find("\r\n", at);
    const std::string line =
        head.substr(at, end == std::string::npos ? std::string::npos : end - at);
    const std::string needle = name + "=";
    const std::size_t named = line.find(needle);
    if (named == std::string::npos) return std::string();
    const std::size_t begin = named + needle.size();
    std::size_t stop = line.find(';', begin);
    if (stop == std::string::npos) stop = line.size();
    return Trim(line.substr(begin, stop - begin));
}

/** 管理页的 op 会话 Cookie 名。由聊天页登录成功后写进浏览器。 */
const char* const kOpCookieName = "dchat_op";

std::string OpSessionFromRequest(const std::string& headLower, const std::string& head) {
    return CookieValue(headLower, head, kOpCookieName);
}

/**
 * 没权限时看到的页面。
 *
 * 这里**故意不放任何输入框**：管理权限只来自"已经用 op 账号登录了聊天"，
 * 不能再给一条旁路（那又变回共享口令了）。
 */
std::string AdminNeedOpPage(const std::string& why) {
    std::string html =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>dchat 管理</title><style>"
        ":root{color-scheme:dark}body{margin:0;background:#1b1b1f;color:#e6e6e6;"
        "font:15px/1.6 -apple-system,\"Segoe UI\",\"Microsoft YaHei\",sans-serif;"
        "display:flex;align-items:center;justify-content:center;height:100vh;padding:0 16px}"
        ".box{background:#232329;border:1px solid #33333a;border-radius:12px;padding:24px;"
        "width:420px;max-width:100%}.msg{color:#ff9b9b;font-size:13px;margin-top:10px}"
        "a{color:#7aa7ff}p{color:#8a8a94;font-size:13px;margin:8px 0 0}"
        "</style></head><body><div class=\"box\">"
        "<b style=\"font-size:16px\">需要 op 权限</b>"
        "<p>管理页只对 <b>服务器管理员（op）</b> 开放。</p>"
        "<p>请先用你的 dchat 账号到聊天页登录；如果这个账号在 "
        "<code>dchat-admins.txt</code> 里，登录后这里就能进。</p>"
        "<p><a href=\"/\">← 去聊天页登录</a></p>";
    if (!why.empty()) html += "<div class=\"msg\">" + why + "</div>";
    html += "</div></body></html>";
    return html;
}

std::string AdminPage(const std::string& sessionId, const std::string& nick,
                      const std::string& message) {
    const std::string listing = AdminCall("LIST");
    std::map<std::string, std::string> values;
    std::size_t from = 0;
    while (from < listing.size()) {
        const std::size_t end = listing.find('\n', from);
        const std::string line =
            listing.substr(from, end == std::string::npos ? std::string::npos : end - from);
        from = (end == std::string::npos) ? listing.size() : end + 1;
        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) values[line.substr(0, tab)] = line.substr(tab + 1);
    }
    if (values.empty()) {
        return AdminNeedOpPage("读不到规则列表：" + Trim(listing).substr(0, 200));
    }

    std::string html =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>dchat 服务器规则</title><style>"
        ":root{color-scheme:dark}body{margin:0;background:#1b1b1f;color:#e6e6e6;"
        "font:15px/1.6 -apple-system,\"Segoe UI\",\"Microsoft YaHei\",sans-serif}"
        ".wrap{max-width:760px;margin:0 auto;padding:28px 16px}"
        "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8a8a94;margin:0 0 20px;font-size:13px}"
        ".row{display:flex;align-items:center;gap:12px;padding:11px 0;"
        "border-bottom:1px solid #2e2e35;flex-wrap:wrap}"
        ".nm{flex:1;min-width:180px}.nm b{font-size:14px}"
        ".nm span{display:block;color:#8a8a94;font-size:12px}"
        "input[type=number]{width:110px;box-sizing:border-box;padding:7px 9px;border-radius:7px;"
        "border:1px solid #3a3a42;background:#2a2a31;color:#e6e6e6;font:inherit}"
        "input[type=checkbox]{width:22px;height:22px}"
        "button{padding:11px 20px;border:0;border-radius:8px;background:#2f6fed;color:#fff;"
        "font:inherit;cursor:pointer;margin-top:18px}"
        ".ok{background:#1d3a29;color:#7ddca0;padding:10px 14px;border-radius:8px;"
        "font-size:13px;margin-bottom:16px}.bad{background:#3a1d1d;color:#ff9b9b;"
        "padding:10px 14px;border-radius:8px;font-size:13px;margin-bottom:16px}"
        "@media(max-width:640px){.nm{min-width:100%}input[type=number]{width:100%}"
        "button{width:100%}}"
        "</style></head><body><div class=\"wrap\">"
        "<h1>服务器规则</h1>"
        "<p class=\"sub\">改完点最下面保存。这里的修改与服务端控制台的 "
        "<code>/chatrule</code> 完全等价——走同一条路径，会立刻写盘并广播给在线客户端。</p>"
        "<p class=\"sub\">当前身份：<b>" + nick + "</b>（op）　"
        "<a href=\"/\" style=\"color:#7aa7ff\">回聊天</a></p>";
    if (!message.empty()) {
        const bool bad = (message.rfind("✗", 0) == 0);
        html += "<div class=\"" + std::string(bad ? "bad" : "ok") + "\">" + message + "</div>";
    }
    html += "<form method=\"post\" action=\"/admin/set\">";
    html += "<input type=\"hidden\" name=\"csrf\" value=\"" + sessionId + "\">";
    for (const AdminRuleMeta& meta : kAdminRules) {
        const auto it = values.find(meta.name);
        if (it == values.end()) continue;  // 服务器没这条就不显示，避免页面和服务器脱节
        html += "<div class=\"row\"><div class=\"nm\"><b>" + std::string(meta.name) + "</b><span>" +
                meta.hint + "</span></div>";
        if (meta.isBool) {
            html += "<input type=\"checkbox\" name=\"" + std::string(meta.name) + "\" value=\"true\"";
            if (it->second == "true") html += " checked";
            html += ">";
        } else {
            html += "<input type=\"number\" name=\"" + std::string(meta.name) + "\" value=\"" +
                    it->second + "\" step=\"1\">";
        }
        html += "</div>";
    }
    html += "<button type=\"submit\">保存</button></form></div></body></html>";
    return html;
}

/** 处理所有 /admin 请求（登录、列表、保存）。 */
void ServeAdmin(dchat::sock::Handle handle, const std::string& rawRequest) {
    const std::size_t headerEnd = rawRequest.find("\r\n\r\n");
    const std::string head =
        (headerEnd == std::string::npos) ? rawRequest : rawRequest.substr(0, headerEnd);
    const std::string body =
        (headerEnd == std::string::npos) ? std::string() : rawRequest.substr(headerEnd + 4);
    const std::string headLower = Lower(head);
    std::string method;
    std::string path;
    {
        const std::size_t firstEnd = head.find("\r\n");
        const std::string firstLine = head.substr(0, firstEnd);
        const std::size_t sp1 = firstLine.find(' ');
        const std::size_t sp2 = firstLine.rfind(' ');
        if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1) {
            method = firstLine.substr(0, sp1);
            path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }
    std::string route = path;
    const std::size_t qmark = route.find('?');
    const std::string query = (qmark == std::string::npos) ? std::string() : route.substr(qmark + 1);
    if (qmark != std::string::npos) route = route.substr(0, qmark);

    auto send = [&](const std::string& status, const std::string& extra, const std::string& content) {
        const std::string out = "HTTP/1.1 " + status +
                                "\r\nContent-Type: text/html; charset=utf-8\r\n"
                                "Content-Length: " + std::to_string(content.size()) + "\r\n" +
                                extra +
                                "X-Frame-Options: DENY\r\n"
                                "X-Content-Type-Options: nosniff\r\n"
                                "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n" +
                                content;
        SendAll(handle, out.data(), out.size());
    };
    auto redirect = [&](const std::string& location, const std::string& extra) {
        const std::string out = "HTTP/1.1 303 See Other\r\nLocation: " + location + "\r\n" + extra +
                                "Content-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(handle, out.data(), out.size());
    };
    auto field = [&](const std::string& name) -> std::string {
        std::size_t from = 0;
        while (from < body.size()) {
            const std::size_t amp = body.find('&', from);
            const std::string pair =
                body.substr(from, amp == std::string::npos ? std::string::npos : amp - from);
            from = (amp == std::string::npos) ? body.size() : amp + 1;
            const std::size_t eq = pair.find('=');
            if (eq == std::string::npos) continue;
            if (PercentDecode(pair.substr(0, eq)) == name) return PercentDecode(pair.substr(eq + 1));
        }
        return std::string();
    };

    // ---- 鉴权：只认"已用 op 账号登录过聊天"这件事 ----
    //
    // 没有口令登录这一步了。能进 /admin 的唯一途径：
    //   1. 在聊天页用 dchat 账号密码登录成功（服务器认证）
    //   2. 那个昵称在 dchat-admins.txt 里 → 登录时网关发了一个 op 会话号
    //   3. 浏览器把号存进 Cookie（由聊天页写），这里校验
    // 三条缺一不可。别人就算知道这个页面存在，也拿不到号。
    const std::string sessionId = OpSessionFromRequest(headLower, head);
    std::string opNick;
    if (!OpSessionNick(sessionId, &opNick)) {
        // /admin/login 这种老地址也不再提供口令表单，直接给"需要 op 权限"
        send("403 Forbidden", "", AdminNeedOpPage(sessionId.empty()
                                                       ? std::string()
                                                       : std::string("会话已过期，请回聊天页重新登录")));
        return;
    }

    if (route == "/admin/set") {
        if (method != "POST") {
            redirect("/admin", "");
            return;
        }
        if (field("csrf") != sessionId) {
            send("403 Forbidden", "", AdminNeedOpPage("会话校验失败，请回聊天页重新登录"));
            return;
        }
        int changed = 0;
        std::string firstError;
        for (const AdminRuleMeta& meta : kAdminRules) {
            std::string request;
            if (meta.isBool) {
                request = std::string("SET ") + meta.name +
                          ((field(meta.name) == "true") ? " true" : " false");
            } else {
                const std::string value = field(meta.name);
                if (value.empty()) continue;
                request = std::string("SET ") + meta.name + " " + value;
            }
            const std::string reply = AdminCall(request);
            if (reply.rfind("OK", 0) == 0) {
                ++changed;
            } else if (firstError.empty()) {
                firstError = std::string(meta.name) + "：" + Trim(reply);
            }
        }
        std::string message;
        if (firstError.empty()) {
            message = "✓ 已保存 " + std::to_string(changed) + " 项（已写盘并广播给在线客户端）";
        } else {
            message = "✗ 部分失败：" + firstError;
        }
        std::string encoded;
        for (char ch : message) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte >= 0x80) {
                encoded.push_back(ch);  // 中文原样带过去，浏览器能处理
            } else if ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') || byte == '-' || byte == '_' || byte == '.') {
                encoded.push_back(ch);
            } else {
                char hexText[8];
                std::snprintf(hexText, sizeof(hexText), "%%%02X", byte);
                encoded += hexText;
            }
        }
        redirect("/admin?m=" + encoded, "");
        return;
    }

    std::string message;
    const std::size_t m = query.find("m=");
    if (m != std::string::npos) message = PercentDecode(query.substr(m + 2));
    send("200 OK", "", AdminPage(sessionId, opNick, message));
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--port") {
            g_config.port = std::atoi(next().c_str());
        } else if (arg == "--server") {
            const std::string value = next();
            const std::size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                g_config.serverHost = value.substr(0, colon);
                g_config.serverPort = std::atoi(value.c_str() + colon + 1);
            } else {
                g_config.serverHost = value;
            }
        } else if (arg == "--dl") {
            g_config.dlDir = next();
        } else if (arg == "--admin") {
            // --admin 127.0.0.1:5556，指向服务器 --admin-port 开的那个回环端口
            const std::string value = next();
            const std::size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                g_config.adminHost = value.substr(0, colon);
                g_config.adminPort = std::atoi(value.c_str() + colon + 1);
            } else {
                g_config.adminPort = std::atoi(value.c_str());
            }
        } else if (arg == "--admin-token") {
            g_config.adminTokenFile = next();
        } else if (arg == "--admins") {
            // op 名单文件（服务端工作目录里的 dchat-admins.txt）。
            // 管理页的权限判定读它：昵称在里面 = op = 登录后能进 /admin。
            g_config.adminsFile = next();
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：dchat-webd [--port 8080] [--server 127.0.0.1:5555] [--dl 目录]\n"
                        "                 [--admin 127.0.0.1:5556] [--admin-token 口令文件]\n"
                        "                 [--admins dchat-admins.txt]\n");
            return 0;
        }
    }
    if (g_config.port <= 0 || g_config.port > 65535) g_config.port = 8080;

    if (!dchat::sock::Startup()) {
        std::fprintf(stderr, "socket 初始化失败\n");
        return 1;
    }
    const dchat::sock::Handle listener = dchat::sock::CreateTcp();
    if (listener == dchat::sock::kInvalid) {
        std::fprintf(stderr, "建监听 socket 失败\n");
        return 1;
    }
    dchat::sock::SetReuseAddress(listener);
    const dchat::sock::Address address = dchat::sock::MakeAddress("0.0.0.0", g_config.port);
    if (!dchat::sock::Bind(listener, address) || !dchat::sock::Listen(listener, 32)) {
        std::fprintf(stderr, "监听 %d 失败\n", g_config.port);
        return 1;
    }
    std::printf("dchat-webd 已启动：http://0.0.0.0:%d/  （上游 dchat %s:%d，下载目录 %s）\n",
                g_config.port, g_config.serverHost.c_str(), g_config.serverPort,
                g_config.dlDir.c_str());
    std::fflush(stdout);

    while (true) {
        dchat::sock::Address peer{};
        const dchat::sock::Handle client = dchat::sock::Accept(listener, &peer);
        if (client == dchat::sock::kInvalid) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::thread([client]() {
            ServeConnection(client);
            dchat::sock::Close(client);
        }).detach();
    }
}

