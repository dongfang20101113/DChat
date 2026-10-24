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
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
        std::fprintf(stderr, "[ws] 浏览器输入: %s\n", line.c_str());
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
            if (bytes.size() > 8ull * 1024 * 1024) {
                SendLine(std::string("ERROR\t网页版上传上限 8 MB，更大的文件请用桌面客户端"));
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
                SendLine("INFO\t附件 " + message.fileId + "：" + message.text +
                         "（在输入框里发 /get " + message.fileId + " 下载）");
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
        SendLine(std::string("ERROR\t网页版暂不支持录音"));
    }

    /** 发一帧 WebSocket 文本帧。**加锁**：接收线程和 WS 线程都会调。 */
    void SendLine(const std::string& text);

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
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                auto self = weak.lock();
                if (!self) return;
                if (!self->core_ || !self->core_->LoggedIn()) continue;
                const auto& rooms = self->core_->Rooms();
                const std::string current = self->core_->CurrentRoom();
                std::string signature = current + ";";
                for (const auto& room : rooms) {
                    signature += room.id + "|" + room.name + "|" + room.myRole + "|" +
                                 std::to_string(room.members) + "|" +
                                 std::to_string(room.maxMembers) + "|" + room.kind + ";";
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
  <a href="/download" style="font-size:13px">下载客户端</a>
</header>
<main><aside id="rooms"></aside><div id="log"></div></main>
<footer>
  <input id="input" placeholder="说点什么…（/help 看指令，/rooms 看会话，/get <id> 下载文件）" disabled>
  <button class="ghost" id="pick" disabled title="发文件（上限 8 MB）">📎</button>
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
  <button type="submit">连接</button>
  <div class="other">想装客户端？<a href="/download">下载 Windows / Android / Linux 版</a></div>
</form></div>
<script>
const logEl = document.getElementById('log');
const stateEl = document.getElementById('state');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
let ws = null;
let roomBuf = [];      // 正在收的一批会话
let currentRoom = '';  // 网关告诉我们"当前在哪个会话"

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

document.getElementById('loginForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const hostEl = document.getElementById('host');
  const parts = hostEl.value.split(':');
  // 服务器地址交给网关：网关按自己的默认值连，这里只把 host:port 透传过去
  document.getElementById('login').style.display = 'none';
  setState('连接中…', false);
  add('sys', '正在连接 ' + hostEl.value + ' …');
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?server=' + encodeURIComponent(hostEl.value));
  ws.onopen = () => {
    setState('已连接', true);
    const reg = document.getElementById('reg').checked ? '1' : '0';
    send('LOGIN\t' + document.getElementById('user').value + '\t' +
         document.getElementById('pass').value + '\t' + reg);
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
      case 'SYS':   add('sys', f[1]); break;
      case 'ERROR': add('err', f[1]); break;
      case 'INFO':  add('sys', f[1]); break;
      case 'FILE': {
        // 网关已经把附件下到本机了，给一条可以直接点的下载链接
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
        if (f[1] === '已登录') { inputEl.disabled = false; sendEl.disabled = false; pickEl.disabled = false; inputEl.focus(); }
        break;
    }
  };
  ws.onclose = () => { setState('已断开', false); inputEl.disabled = true; sendEl.disabled = true; pickEl.disabled = true; };
  ws.onerror = () => add('err', '连接出错');
});

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
      if (id !== currentRoom) send('INPUT\t/open ' + id);
      document.body.classList.remove('drawer');  // 手机上选完自动收起
    });
    roomsEl.appendChild(row);
    if (id === currentRoom) currentName = name || id;
  }
  whereEl.textContent = currentName ? '　· ' + currentName : '';
}

// 手机上的 ☰：展开/收起会话抽屉
document.getElementById('menu').addEventListener('click', () => {
  document.body.classList.toggle('drawer');
});

sendEl.addEventListener('click', submit);
inputEl.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });

// 发文件：读成 Base64 一帧发过去。网关那边解码写临时文件再走 /send。
// 上限 8 MB —— 网页版是把整个文件塞进一帧 WebSocket，再大就不合适了
// （桌面端没这个限制，它直接读盘分块发）。
const pickEl = document.getElementById('pick');
const fileEl = document.getElementById('file');
pickEl.addEventListener('click', () => fileEl.click());
fileEl.addEventListener('change', () => {
  const f = fileEl.files[0];
  if (!f) return;
  if (f.size > 8 * 1024 * 1024) {
    add('err', '网页版上传上限 8 MB，更大的文件请用桌面客户端');
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
</script></body></html>
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
};

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
        while (!session->closed()) {
            const std::ptrdiff_t got = dchat::sock::Receive(handle, buffer, sizeof(buffer));
            if (got <= 0) {
                // 一定要记下来：这个循环退出等于浏览器被踢，而"为什么退出"
                // 光看现象（浏览器那头连接断）是分不清的。
                std::fprintf(stderr, "[ws] 接收结束 got=%ld\n", static_cast<long>(got));
                std::fflush(stderr);
                break;
            }
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
        reply("200 OK", "text/html; charset=utf-8", kIndexHtml);
    } else if (route == "/download") {
        reply("200 OK", "text/html; charset=utf-8", kDownloadHtml);
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
        reply("404 Not Found", "text/plain; charset=utf-8", "not found");
    }
    (void)method;
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
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：dchat-webd [--port 8080] [--server 127.0.0.1:5555] [--dl 目录]\n");
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

