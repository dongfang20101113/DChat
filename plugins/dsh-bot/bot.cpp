// dchat × DeepSeek Harness 机器人（实现）
//
// 三条线程，各管一件事：
//
//   接收线程（主线程）  读行、解析、决定"这条要不要回"
//   工作线程池          跑 dsh 子进程，把回答切成一条条塞进发送队列
//   发送线程            按房间切换 + 打字节奏，一条条真正发出去
//
// 写 socket 的只有发送线程（登录阶段是主线程，那时发送线程还没开始消费），
// 并且所有写都过 sendMutex_ —— dchat 协议是行式的，两条线程同时写只要一次
// write 被切开，服务器就会收到半行拼接，表现为"消息莫名其妙变成乱码"，
// 而且只在忙的时候偶发。
#include "socket_util.h"  // 必须第一个：winsock2.h 要排在 windows.h 之前

#ifdef _WIN32
#include <windows.h>  // GetModuleFileNameW：找和程序放一起的补丁文件
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <csignal>

#include "bot.h"
#include "bot_api.h"
#include "bot_auth.h"
#include "bot_chunk.h"
#include "bot_config.h"
#include "bot_event.h"
#include "bot_file.h"
#include "bot_http.h"
#include "bot_json.h"
#include "bot_api.h"
#include "bot_policy.h"
#include "bot_process.h"
#include "crypto.h"
#include "file_transfer.h"
#include "protocol.h"
#include "render.h"
#include "ui_page.h"

namespace dchat {
namespace dshbot {

namespace {

std::atomic<bool> g_requestStop{false};

void OnSignal(int) { g_requestStop = true; }

std::string NowText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

std::string JoinFrom(const std::vector<std::string>& words, std::size_t start) {    std::string out;
    for (std::size_t i = start; i < words.size(); ++i) {
        if (!out.empty()) out.push_back(' ');
        out += words[i];
    }
    return out;
}

std::string DecodeBase64Text(const std::string& text) {
    std::vector<unsigned char> bytes;
    if (!Base64Decode(text, &bytes)) return std::string();
    return std::string(bytes.begin(), bytes.end());
}

bool SameRoomId(const std::string& a, const std::string& b) { return SameNick(a, b); }

/**
 * 可执行文件所在目录。用来找"和程序放一起"的补丁文件
 * （dsh-chat-only.yml / dsh-local-ops.yml），这样部署时不用在配置里写死绝对路径。
 */
std::string ExecutableDir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::string();
    const std::wstring wide(buffer, buffer + length);
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return std::string();
    std::string utf8(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    const std::size_t slash = utf8.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : utf8.substr(0, slash);
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return std::string();
    const std::string path(buffer, static_cast<std::size_t>(length));
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
#endif
}

bool FileExists(const std::string& path) {
    std::error_code code;
    return std::filesystem::is_regular_file(std::filesystem::path(path), code) && !code;
}

/** 在可执行文件附近找随程序附带的补丁（部署时不用写死绝对路径）。 */
std::string FindBundledPatch(const std::string& name) {
    const std::string dir = ExecutableDir();
    if (dir.empty()) return std::string();
    const std::string candidates[] = {
        dir + "/" + name,
        dir + "/dsh-bot/" + name,
        dir + "/../plugins/dsh-bot/" + name,  // 仓库里直接跑 build/dchat-dsh-bot.exe 的情况
    };
    for (const std::string& candidate : candidates) {
        if (FileExists(candidate)) return candidate;
    }
    return std::string();
}

}  // namespace

// ===========================================================================
// Logger
// ===========================================================================

Logger::~Logger() {
    if (file_ != nullptr) std::fclose(static_cast<std::FILE*>(file_));
}

bool Logger::Open(const std::string& path, bool verbose, std::string* error) {
    verbose_ = verbose;
    if (path.empty()) return true;
    std::FILE* file = std::fopen(path.c_str(), "a");
    if (file == nullptr) {
        if (error != nullptr) *error = "打不开日志文件：" + path;
        return false;
    }
    file_ = file;
    return true;
}

void Logger::Write(const char* level, const std::string& text) {
    const std::string line = NowText() + " [" + level + "] " + text;
    {
        // 多线程同时写（接收线程/工作线程/发送线程/HTTP 线程）：
        // 不加锁的话两行会交错成半行，日志本来就是拿来查问题的，交错等于白记。
        std::lock_guard<std::mutex> lock(mutex_);
        std::fputs((line + "\n").c_str(), stdout);
        std::fflush(stdout);
        if (file_ != nullptr) {
            std::fputs((line + "\n").c_str(), static_cast<std::FILE*>(file_));
            std::fflush(static_cast<std::FILE*>(file_));
        }
        tail_.push_back(line);
        if (tail_.size() > 300) tail_.pop_front();
    }
}

std::vector<std::string> Logger::Tail(std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t begin = tail_.size() > count ? tail_.size() - count : 0;
    return std::vector<std::string>(tail_.begin() + static_cast<std::ptrdiff_t>(begin), tail_.end());
}

void Logger::Info(const std::string& text) { Write("INFO", text); }
void Logger::Warn(const std::string& text) { Write("WARN", text); }
void Logger::Error(const std::string& text) { Write("ERROR", text); }
void Logger::Debug(const std::string& text) {
    if (verbose_) Write("DEBUG", text);
}

// ===========================================================================
// Bot
// ===========================================================================

namespace {

enum class ReadStatus { Line, Timeout, Closed, Failed };

class Bot : public std::enable_shared_from_this<Bot> {
public:
    Bot(const Config& config, const std::string& configPath, Logger& log)
        : config_(std::make_shared<const Config>(config)), configPath_(configPath), log_(log) {}

    void Run();

private:
    struct RoomState {
        std::string id;
        std::string kind;
        std::string name;
        bool member = false;
    };
    /** 管理界面的登录会话：谁登的、是不是 admin、是哪些群的群主/管理员。 */
    struct UiSession {
        std::string nick;
        /**
         * 全局管理员（= 服务端 dchat-admins.txt 里那些人）。
         * **这是权限，不是身份标签**：界面上不显示这个词，只用来决定能不能改。
         */
        bool admin = false;
        /** 这个人在这些群里是群主或群管理员 —— 也就是"这些群的 op"。 */
        std::set<std::string> opRooms;
        std::chrono::steady_clock::time_point expires{};
    };
    struct Outgoing {
        int generation = 0;
        std::string room;
        std::string text;
        /** 非空 = 这条不是文字，是把 path 这个文件传到当前会话。 */
        std::string path;
    };
    struct Job {
        int generation = 0;
        std::string room;
        std::string roomName;
        std::string nick;
        std::string prompt;
        /** 去掉 @ 之后的原始内容（api 模式直接当用户消息用，比塞整段模板干净）。 */
        std::string userText;
    };
    struct Attempt {
        bool started = false;
        bool timedOut = false;
        bool producedText = false;
        int exitCode = -1;
        std::string stderrTail;
    };

    // ---- 连接 ----
    bool ConnectSocket(std::string* error);
    bool DoHandshake();
    bool ConnectAndLogin(std::string* error);
    void JoinRooms();
    void ReceiveLoop();
    void Disconnect();
    ReadStatus ReadRawLine(std::string* line, int timeoutMs);
    bool DecodeIncoming(const std::string& rawLine, std::string* plain);
    bool HandleIncoming(const std::string& rawLine);
    bool SendRaw(const std::string& line);

    // ---- 解析 ----
    void HandleLine(const std::string& line);
    void HandleRoomList(const std::vector<std::string>& words);
    void HandleChat(const std::string& room, const std::string& nick, const std::string& rawText);
    void HandleRules(const std::vector<std::string>& words);
    RoomState* FindRoomLocked(const std::string& id);

    // ---- 线程 ----
    void StartThreads();
    void StopThreads();
    /** api 模式的一轮回答（HTTP + 工具都在 bot_api.cpp 里）。 */
    void RunApiJob(const Job& job);
    void SenderLoop();
    void WorkerLoop();
    void EnqueueOut(int generation, const std::string& room, const std::string& text);
    void EnqueueFile(int generation, const std::string& room, const std::string& path);
    void EmitText(int generation, const std::string& room, const std::string& text);
    /** 把文件传上去。在发送线程里跑（它负责切房间 + 独占 socket 写）。 */
    bool UploadFile(const std::string& room, const std::string& path);
    int FileCapMb() const;
    /** 机器人被授权动手的目录（= 子进程工作目录；没配就是进程当前目录）。 */
    std::string WorkspaceDir() const;
    /** 实际生效的 DSH 补丁文件（配置里没写就按权限模式找随程序附带的那份）。 */
    std::string ResolvePatch() const;
    /**
     * 给工作目录补上"当前用户完全控制"。
     *
     * 为什么需要：DSH 的 shell 工具在跑命令前会 SetNamedSecurityInfoW 给沙箱
     * 补写权限，而**普通目录从父目录继承下来的只有 Modify（不含改 ACL 的
     * WRITE_DAC）**，于是一跑命令就报
     *   `SetNamedSecurityInfoW failed (Win32 5): grantWrite(<目录>)`
     * 表现是"能写文件、但一执行命令就失败"。启动时补一次（幂等），
     * 只针对它自己的工作目录。Windows 之外是空操作。
     */
    void EnsureWorkspaceAcl();
    void RunJob(const Job& job);
    Attempt RunAttempt(const Job& job, const std::string& session);

    // ---- 小工具 ----
    bool RoomConfigured(const std::string& room) const;
    bool IsMember(const std::string& room);
    std::string RoomDisplayName(const std::string& room);
    std::string SessionFor(const std::string& room);
    void SaveSession(const std::string& room, const std::string& session);
    void LoadSessions();
    void StoreSessions();
    bool SleepMs(int ms);

    // ---- 本机管理界面 ----
    void StartUi();
    HttpResponse HandleHttp(const HttpRequest& request);
    std::string StateJson(const UiSession& session);
    HttpResponse HandleSave(const std::string& body, const UiSession& session);
    HttpResponse HandleLogin(const std::string& body);
    /** 从 Cookie 里取会话 token。 */
    std::string CookieToken(const HttpRequest& request) const;
    bool LoadSession(const HttpRequest& request, UiSession* out);
    void DropSession(const HttpRequest& request);
    /**
     * 向服务器要一次最新的群成员表（发 ROOM <群>，服务器会回 RMEMBERS）。
     *
     * 为什么登录时要拉一次：成员角色是会变的，而机器人只在"自己在群里"的时候
     * 才收得到 RMEMBERS 推送。你刚被设成群管理员、而机器人在那之后才入群时，
     * 缓存里就没有这条 —— 表现是"我明明是群管理却进不去"。
     */
    void RefreshRoomRoles();
    HttpResponse HandleSearch(const std::string& body);
    HttpResponse HandleJoin(const std::string& body, const UiSession& session);
    bool ApplyConfig(const Config& next);
    /** 把界面管的那几个键写回配置文件（保留注释），失败只记日志 —— 内存里已经生效了。 */
    void WriteConfigFile(const Config& next);

    struct SearchHit {
        std::string id;
        std::string name;
        std::string joinMode;
        int members = 0;
        int maxMembers = 0;
    };

    /**
     * 配置在运行期是可改的（本地管理界面保存后立刻生效），所以存成**不可变快照**：
     * 读的人拿一个 shared_ptr 走，写的人整份换掉。这样既不用给每条读加锁，
     * 也不会读到"改了一半"的配置（比如 mode 已经是 blacklist 但名单还没写进去）。
     */
    std::shared_ptr<const Config> Cfg() const;

    // api 模式下每个房间的最近几条纯文本对话（工具消息不入历史，省上下文）
    std::mutex historyMutex_;
    std::map<std::string, std::vector<ApiMessage>> histories_;

    mutable std::mutex configMutex_;
    std::shared_ptr<const Config> config_;
    std::string configPath_;
    Logger& log_;

    sock::Handle sock_ = sock::kInvalid;
    CryptoSession crypto_;
    bool encrypted_ = false;
    LineBuffer readBuffer_;
    std::mutex sendMutex_;

    std::mutex stateMutex_;
    std::string selfNick_;
    std::string currentRoom_ = "hall";
    std::map<std::string, RoomState> rooms_;
    /** 每个群的成员 -> 角色（owner/admin/member），RMEMBERS 推来的。权限判断要用。 */
    std::map<std::string, std::map<std::string, std::string>> roomRoles_;
    // ---- 管理界面的登录会话 ----
    std::mutex uiSessionMutex_;
    std::map<std::string, UiSession> uiSessions_;
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, int>> loginAttempts_;
    bool roomListDone_ = false;
    std::atomic<bool> loggedIn_{false};
    std::atomic<int> generation_{0};
    std::atomic<int> chatIntervalMs_{0};
    std::atomic<int> serverFileLimitMb_{0};  // 服务器 rules 里的 documentsize
    int nextUploadId_ = 0;                   // 只在发送线程里用
    // 心跳时间戳（只在接收线程里读写）
    std::chrono::steady_clock::time_point lastPingAt_{};
    std::chrono::steady_clock::time_point lastPongAt_{};
    // 登录时的历史回放静默期（只在接收线程里读写）
    bool historyMode_ = false;
    std::chrono::steady_clock::time_point historyQuietUntil_{};
    int droppedHistory_ = 0;

    std::mutex outMutex_;
    std::condition_variable outCv_;
    std::deque<Outgoing> outQueue_;
    bool outStop_ = false;

    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    int jobsPending_ = 0;
    bool jobStop_ = false;

    std::mutex sessionMutex_;
    std::map<std::string, std::string> sessions_;

    // ---- 管理界面 ----
    HttpServer ui_;
    mutable std::mutex searchMutex_;
    std::condition_variable searchCv_;
    bool searchPending_ = false;
    bool searchDone_ = false;
    std::vector<SearchHit> searchHits_;
};

std::shared_ptr<const Config> Bot::Cfg() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

std::string Bot::RoomDisplayName(const std::string& room) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto it = rooms_.find(room);
    if (it != rooms_.end() && !it->second.name.empty()) return it->second.name;
    return room == "hall" ? std::string("大厅") : room;
}

Bot::RoomState* Bot::FindRoomLocked(const std::string& id) {
    const auto it = rooms_.find(id);
    return it == rooms_.end() ? nullptr : &it->second;
}

bool Bot::RoomConfigured(const std::string& room) const {
    for (const std::string& configured : Cfg()->rooms) {
        if (SameRoomId(configured, room)) return true;
    }
    return false;
}

bool Bot::IsMember(const std::string& room) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (room == "hall") return true;
    const RoomState* state = FindRoomLocked(room);
    return state != nullptr && state->member;
}

bool Bot::SleepMs(int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_requestStop.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------

bool Bot::ConnectSocket(std::string* error) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;  // 服务端只监听 IPv4
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const std::string portText = std::to_string(Cfg()->port);
    if (::getaddrinfo(Cfg()->host.c_str(), portText.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        if (error != nullptr) *error = "解析不了主机：" + Cfg()->host;
        return false;
    }
    sockaddr_in address{};
    std::memcpy(&address, result->ai_addr, sizeof(address));
    ::freeaddrinfo(result);

    sock_ = sock::CreateTcp();
    if (sock_ == sock::kInvalid) {
        if (error != nullptr) *error = "建 socket 失败";
        return false;
    }
    sock::SetNoSigpipe(sock_);
    sock::SetNoDelay(sock_);
    // 接收超时不是为了"超时就断"，而是让接收循环每隔一会儿能看一眼退出标志。
    // 聊天服务器可能几个小时没有消息，卡在 recv 里就接不住 systemd stop。
    sock::SetReceiveTimeout(sock_, 200);
    if (::connect(sock_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) {
            *error = "连不上 " + Cfg()->host + ":" + std::to_string(Cfg()->port) + "（错误码 " +
                     std::to_string(sock::LastError()) + "）";
        }
        sock::Close(sock_);
        sock_ = sock::kInvalid;
        return false;
    }
    readBuffer_.Reset();
    crypto_.Reset();
    encrypted_ = false;
    return true;
}

ReadStatus Bot::ReadRawLine(std::string* line, int timeoutMs) {
    if (readBuffer_.PopLine(line)) return ReadStatus::Line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_requestStop.load()) return ReadStatus::Failed;
        char chunk[4096];
        const std::ptrdiff_t got = sock::Receive(sock_, chunk, sizeof(chunk));
        if (got > 0) {
            readBuffer_.Append(chunk, static_cast<std::size_t>(got));
            if (readBuffer_.bad()) return ReadStatus::Failed;  // 单行太长
            if (readBuffer_.PopLine(line)) return ReadStatus::Line;
            continue;
        }
        if (got == 0) return ReadStatus::Closed;
        if (!sock::WouldBlock()) return ReadStatus::Failed;
        // 接收超时：没数据，但连接还活着
    }
    return ReadStatus::Timeout;
}

bool Bot::SendRaw(const std::string& line) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (sock_ == sock::kInvalid) return false;
    std::string payload = line;
    if (crypto_.active()) {
        std::string sealed;
        if (!crypto_.Encrypt(line, &sealed)) return false;
        payload = "ENC " + Base64Encode(sealed);
    }
    return sock::SendAll(sock_, (payload + "\n").data(), payload.size() + 1);
}

/**
 * 把一行原始数据解成明文协议行。
 *
 * ⚠️ 加密启用后收到的是 `ENC <base64>`，**所有**要看命令名的判断都必须在这一步
 * 之后做。踩过一次：注册回退的判断直接拿原始行 ParseLine，命令行永远是 "ENC"，
 * 于是"账号已存在就改登录"这条分支从来没进过 —— 表现是机器人一直重连不上。
 */
bool Bot::DecodeIncoming(const std::string& rawLine, std::string* plain) {
    *plain = rawLine;
    if (!crypto_.active()) return true;
    const Message outer = ParseLine(rawLine);
    if (outer.command != "ENC") return false;
    std::vector<unsigned char> bytes;
    if (!Base64Decode(outer.rest, &bytes)) return false;
    const std::string sealed(bytes.begin(), bytes.end());
    return crypto_.Decrypt(sealed, plain);
}

bool Bot::HandleIncoming(const std::string& rawLine) {
    std::string plain;
    if (!DecodeIncoming(rawLine, &plain)) {
        log_.Error("解密失败（数据被篡改或状态错乱），断开");
        return false;
    }
    HandleLine(plain);
    return true;
}

bool Bot::DoHandshake() {
    std::vector<unsigned char> clientNonce;
    EcdhKeyPair clientKey;
    if (!RandomBytes(kHandshakeNonceBytes, &clientNonce) || !GenerateEcdhKeyPair(&clientKey)) {
        return false;
    }
    const std::string hello =
        BuildLine("HELLO", std::to_string(kCryptoVersion) + " " +
                               Base64Encode(clientKey.publicKey.data(), clientKey.publicKey.size()) +
                               " " + Base64Encode(clientNonce.data(), clientNonce.size()));
    if (!SendRaw(hello)) return false;

    std::vector<std::string> skipped;  // 握手期间收到的非响应行（WELCOME 之类）
    bool gotHelloOk = false;
    std::string helloLine;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string line;
        const ReadStatus status = ReadRawLine(&line, 200);
        if (status == ReadStatus::Closed || status == ReadStatus::Failed) return false;
        if (status != ReadStatus::Line) continue;
        if (ParseLine(line).command == "HELLO_OK") {
            gotHelloOk = true;
            helloLine = line;
            break;
        }
        skipped.push_back(line);
    }
    if (!gotHelloOk) {
        if (Cfg()->encrypt == "require") {
            log_.Error("服务器没有回应加密握手，但 server.encrypt = require，不连了");
            return false;
        }
        log_.Warn("服务器不支持加密握手，退回明文（机器人密码会在网络上明文传输）");
        // 这些行是**明文**，不能走 HandleIncoming（它会拿当前密钥去解密）
        for (const std::string& line : skipped) HandleLine(line);
        return true;
    }

    const Message raw = ParseLine(helloLine);
    std::vector<std::string> words = raw.Words();
    if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
    if (words.size() < 2) {
        log_.Error("HELLO_OK 字段不够，握手失败");
        return false;
    }
    std::vector<unsigned char> serverPublic;
    std::vector<unsigned char> serverNonce;
    if (!Base64Decode(words[0], &serverPublic) || !Base64Decode(words[1], &serverNonce) ||
        serverPublic.size() != kP256PublicKeyBytes || serverNonce.size() != kHandshakeNonceBytes) {
        log_.Error("HELLO_OK 里的公钥/随机数不合法，握手失败");
        return false;
    }
    std::vector<unsigned char> shared;
    if (!ComputeSharedSecret(clientKey, serverPublic, &shared)) {
        log_.Error("ECDH 计算失败，握手失败");
        return false;
    }
    const SessionKeys keys = DeriveSessionKeys(shared, clientNonce, serverNonce);
    if (!crypto_.Start(keys.clientToServer, keys.serverToClient)) {
        log_.Error("会话密钥初始化失败");
        return false;
    }
    encrypted_ = true;

    const std::string fingerprint = PublicKeyFingerprint(serverPublic);
    if (!Cfg()->fingerprint.empty()) {
        if (!SameNick(Cfg()->fingerprint, fingerprint)) {
            log_.Error("服务器公钥指纹和配置里的不一致：配置 " + Cfg()->fingerprint + "，实际 " +
                       fingerprint + "（可能是换了服务器，也可能是中间人）");
            return false;
        }
        log_.Debug("服务器指纹校验通过：" + fingerprint);
    } else {
        log_.Info("已加密连接（服务器指纹 " + fingerprint + "，可写进 server.fingerprint 钉住）");
    }
    for (const std::string& line : skipped) HandleLine(line);
    return true;
}

bool Bot::ConnectAndLogin(std::string* error) {
    if (!ConnectSocket(error)) return false;
    if (Cfg()->encrypt != "off") {
        if (!DoHandshake()) {
            if (error != nullptr) *error = "加密握手失败";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        rooms_.clear();
        roomListDone_ = false;
        selfNick_.clear();
    }
    currentRoom_ = "hall";
    loggedIn_ = false;

    SendRaw(BuildLine("FEATURES", "rooms1"));
    const bool registering = Cfg()->registerFirst;
    SendRaw(BuildLine(registering ? "REGISTER" : "LOGIN", Cfg()->nick + " " + Cfg()->password));
    log_.Info(std::string(registering ? "注册并登录" : "登录") + "：" + Cfg()->nick);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    bool retriedLogin = false;
    std::string lastError;
    while (std::chrono::steady_clock::now() < deadline && !loggedIn_.load()) {
        std::string line;
        const ReadStatus status = ReadRawLine(&line, 200);
        if (status == ReadStatus::Closed || status == ReadStatus::Failed) {
            if (error != nullptr) {
                *error = g_requestStop.load() ? "被要求退出" : "连接在登录期间断开";
            }
            return false;
        }
        if (status != ReadStatus::Line) continue;
        std::string plain;
        if (!DecodeIncoming(line, &plain)) {
            if (error != nullptr) *error = "登录期间解密失败（密钥状态不对）";
            return false;
        }
        // 自动注册模式下账号已存在：改走登录，别让"首次部署之后第二次启动"失败
        if (registering && !retriedLogin) {
            const Message message = ParseLine(plain);
            if (message.command == "ERROR" && message.rest.find("已存在") != std::string::npos) {
                retriedLogin = true;
                SendRaw(BuildLine("LOGIN", Cfg()->nick + " " + Cfg()->password));
                continue;
            }
        }
        const Message message = ParseLine(plain);
        if (message.command == "ERROR") lastError = message.rest;
        HandleLine(plain);
    }
    if (!loggedIn_.load()) {
        if (error != nullptr) {
            *error = "登录失败：" + (lastError.empty() ? "没有收到 LOGGEDIN" : lastError);
        }
        return false;
    }

    // 等会话列表到齐（服务端紧跟着 LOGGEDIN 发，正常几十毫秒）
    const auto listDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (!roomListDone_ && std::chrono::steady_clock::now() < listDeadline) {
        std::string line;
        const ReadStatus status = ReadRawLine(&line, 200);
        if (status != ReadStatus::Line) continue;
        if (!HandleIncoming(line)) break;
    }

    JoinRooms();
    historyMode_ = false;
    droppedHistory_ = 0;
    // 心跳基线：从现在开始算，别让上一次连接的旧时间戳把新连接判成超时
    lastPongAt_ = std::chrono::steady_clock::now();
    lastPingAt_ = lastPongAt_;
    std::string roomList;
    for (const std::string& room : Cfg()->rooms) {
        if (!roomList.empty()) roomList += ", ";
        roomList += room;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        log_.Info("已登录为 " + selfNick_ + "，接入会话：" + roomList);
    }
    return true;
}

void Bot::JoinRooms() {
    for (const std::string& room : Cfg()->rooms) {
        if (SameRoomId(room, "hall")) continue;
        bool member = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const RoomState* state = FindRoomLocked(room);
            member = state != nullptr && state->member;
        }
        if (member) continue;
        if (!Cfg()->autoJoin) {
            log_.Warn("不在会话 " + room + " 里，且 rooms.autojoin = false，不会回复该会话");
            continue;
        }
        log_.Info("申请加入会话：" + room);
        SendRaw(BuildLine("GJOIN", room));
    }
}

void Bot::ReceiveLoop() {
    while (!g_requestStop.load()) {
        std::string line;
        const ReadStatus status = ReadRawLine(&line, 200);
        const auto cfg = Cfg();
        if (status == ReadStatus::Line) {
            if (!HandleIncoming(line)) break;
        } else if (status != ReadStatus::Timeout) {
            break;  // Closed / Failed
        }
        // ---- 心跳 ----
        // 空闲连接可能被中间设备悄悄清掉：recv 既不返回 0 也不报错，不主动探活
        // 就一直"以为自己在线"。这里定期发 PING，迟迟收不到 PONG 就断开重连。
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPingAt_ >= std::chrono::milliseconds(cfg->pingIntervalMs)) {
            lastPingAt_ = now;
            if (!SendRaw(BuildLine("PING"))) break;
            log_.Debug("心跳 PING");
        }
        if (now - lastPongAt_ > std::chrono::milliseconds(cfg->pingTimeoutMs)) {
            log_.Warn("心跳超时（" + std::to_string(cfg->pingTimeoutMs) +
                      " ms 没有 PONG），连接大概已经被中间设备掐了，重连");
            break;
        }
    }
    if (!g_requestStop.load()) log_.Warn("连接已断开");
}

void Bot::Disconnect() {
    generation_.fetch_add(1);  // 让还在跑的回答作废
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outQueue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        jobs_.clear();
        jobsPending_ = 0;
    }
    if (sock_ != sock::kInvalid) {
        sock::Close(sock_);
        sock_ = sock::kInvalid;
    }
    crypto_.Reset();
    encrypted_ = false;
    loggedIn_ = false;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

void Bot::HandleLine(const std::string& line) {
    const Message message = ParseLine(line);
    if (message.command.empty()) return;
    // 回放模式下要不要丢掉某条消息，判断在 HandleChat 里（那里顺便把窗口往后延）。
    if (message.command.empty()) return;
    std::vector<std::string> words = message.Words();
    // ⚠️ 服务端所有带时间的行都是 `命令 hh:mm 其余…`（Timed() 把时间拼在**最前面**）。
    // 不先摘掉的话，`LOGGEDIN 09:37 alice` 会被读成"我叫 09:37"，后面所有
    // "这是不是我自己的回声"的判断全部失效 —— 机器人会跟自己的话聊起来。
    std::size_t first = 0;
    if (!words.empty() && LooksLikeTime(words[0])) first = 1;

    if (message.command == "LOGGEDIN") {
        if (words.size() > first) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            selfNick_ = words[first];
        }
        loggedIn_ = true;
        return;
    }
    if (message.command == "ROOMLIST") {
        HandleRoomList(words);
        return;
    }
    if (message.command == "ROOMLIST_END") {
        roomListDone_ = true;
        return;
    }
    if (message.command == "RMEMBERS") {
        // RMEMBERS <房间> <昵称> <角色>：管理界面判断"群主"就靠它
        if (words.size() >= 3) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            roomRoles_[words[0]][words[1]] = words[2];
        }
        return;
    }
    if (message.command == "RMEMBERS_END") return;
    if (message.command == "RINFO") {
        if (words.size() >= 3) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            RoomState& state = rooms_[words[0]];
            state.id = words[0];
            state.kind = words[1];
            state.name = DecodeBase64Text(words[2]);
            state.member = true;
        }
        return;
    }
    if (message.command == "SYS") {
        // 服务端开始回放历史了：进入"回放模式"，收到的聊天行先不算实时。
        // 结束不靠固定计时，而是靠"安静了多久"——回放是服务端一口气写完的，
        // 中间不会有几百毫秒的空档；固定 2 秒会让刚上线的机器人头两秒完全不理人。
        const std::string text = JoinFrom(words, first);
        if (text.find("以下是加入之前的聊天记录") != std::string::npos ||
            text.find("你不在的时候有") != std::string::npos) {
            historyMode_ = true;
            historyQuietUntil_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(Cfg()->historySettleMs);
            log_.Debug("服务端开始回放历史");
        }
        return;
    }
    if (message.command == "RMSG") {
        // RMSG <时间> <房间ID> <昵称> <文本>
        if (words.size() > first + 1) {
            HandleChat(words[first], words[first + 1], JoinFrom(words, first + 2));
        }
        return;
    }
    if (message.command == "SAY") {
        // SAY <时间> <昵称> <文本>（大厅，老格式）
        if (words.size() > first) {
            HandleChat("hall", words[first], JoinFrom(words, first + 1));
        }
        return;
    }
    if (message.command == "GHIT") {
        // GHIT <房间ID> <群名B64> <成员数> <最大人数> <加群方式>
        if (words.size() >= 5) {
            std::lock_guard<std::mutex> lock(searchMutex_);
            if (searchPending_) {
                SearchHit hit;
                hit.id = words[0];
                hit.name = DecodeBase64Text(words[1]);
                hit.members = std::atoi(words[2].c_str());
                hit.maxMembers = std::atoi(words[3].c_str());
                hit.joinMode = words[4];
                searchHits_.push_back(hit);
            }
        }
        return;
    }
    if (message.command == "GHIT_END") {
        {
            std::lock_guard<std::mutex> lock(searchMutex_);
            searchDone_ = true;
        }
        searchCv_.notify_all();
        return;
    }
    if (message.command == "PONG") {
        lastPongAt_ = std::chrono::steady_clock::now();
        return;
    }
    if (message.command == "RULES") {
        HandleRules(words);
        return;
    }
    if (message.command == "ERROR") {
        log_.Warn("服务器：" + JoinFrom(words, first));
        // 服务端把 chatinterval 的当前值写在提示里，顺手学过来，免得每发一条就被骂一次
        const std::size_t at = message.rest.find("chatinterval = ");
        if (at != std::string::npos) {
            const int value = std::atoi(message.rest.c_str() + at + 15);
            if (value > 0) {
                chatIntervalMs_ = value;
                log_.Info("按服务器要求放慢发言：每条间隔至少 " + std::to_string(value) + " ms");
            }
        }
        return;
    }
    log_.Debug("忽略：" + message.command);
}

void Bot::HandleRules(const std::vector<std::string>& words) {
    // RULES <documentsize> <chatinterval> ...
    if (words.size() >= 2) {
        const int interval = std::atoi(words[1].c_str());
        if (interval > 0) chatIntervalMs_ = interval;
    }
    if (!words.empty()) {
        const int maxMb = std::atoi(words[0].c_str());
        if (maxMb > 0) serverFileLimitMb_ = maxMb;
    }
}

void Bot::HandleRoomList(const std::vector<std::string>& words) {
    // ROOMLIST <id> <kind> <名字B64> <成员数> <最大人数> <我的角色>
    if (words.size() < 3) return;
    std::lock_guard<std::mutex> lock(stateMutex_);
    RoomState& state = rooms_[words[0]];
    state.id = words[0];
    state.kind = words[1];
    state.name = DecodeBase64Text(words[2]);
    state.member = true;  // 出现在 ROOMLIST 里 = 我是成员
}

void Bot::HandleChat(const std::string& room, const std::string& nick, const std::string& rawText) {
    std::string self;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        self = selfNick_;
    }
    if (nick.empty() || (!self.empty() && SameNick(nick, self))) return;  // 自己的回声
    if (!RoomConfigured(room)) return;
    if (!IsMember(room)) {
        log_.Debug("收到 " + room + " 的消息但我不在里面，忽略");
        return;
    }
    // 登录瞬间服务端会用**原始 SAY/RMSG 行**重放错过的历史（keepchathistory /
    // offlinemessages）。不丢掉的话，机器人一上线就把几小时前 @ 过它的消息
    // 逐条回答一遍 —— 群里看到的是一串答非所问。历史行没有序号，协议上分不出来，
    // 所以用"服务端发回放提示之后、直到它安静下来"这段时间来界定，见 HandleLine 的 SYS。
    if (historyMode_) {
        const auto now = std::chrono::steady_clock::now();
        if (now < historyQuietUntil_) {
            // 丢掉的同时**把窗口往后推**：服务端是一口气写完历史的，
            // 只要它还在出聊天行，就还在回放。等它安静够久，下一条就是实时的。
            historyQuietUntil_ = now + std::chrono::milliseconds(Cfg()->historySettleMs);
            ++droppedHistory_;
            log_.Debug("静默期内丢掉一条回放消息（" + nick + "）：" + UnescapeText(rawText));
            return;
        }
        historyMode_ = false;
        if (droppedHistory_ > 0) {
            log_.Info("登录时忽略了 " + std::to_string(droppedHistory_) + " 条历史回放消息");
            droppedHistory_ = 0;
        }
    }

    const std::string text = UnescapeText(rawText);
    const TriggerMatch match = MatchTrigger(text, Cfg()->nick, Cfg()->trigger);
    if (!match.matched) return;

    if (!ReplyAllowed(Cfg()->reply, nick)) {
        const char* why = (Cfg()->reply.mode == ReplyMode::Whitelist) ? "不在白名单" : "在黑名单";
        log_.Info("跳过 " + nick + " 的 @" + "（" + why + "）");
        return;
    }

    const int generation = generation_.load();
    int pending = 0;
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        pending = jobsPending_;
    }
    if (pending >= Cfg()->brain.maxQueue + std::max(1, Cfg()->brain.maxConcurrent)) {
        EnqueueOut(generation, room,
                   "（手头还有 " + std::to_string(pending) + " 个问题在排队，等我一下）");
        log_.Warn("队列已满（" + std::to_string(pending) + "），拒绝 " + nick);
        return;
    }

    Job job;
    job.generation = generation;
    job.room = room;
    job.roomName = RoomDisplayName(room);
    job.nick = nick;
    job.prompt = BuildPrompt(Cfg()->brain, Cfg()->fileRoots, job.roomName, room, nick, match.text);
    job.userText = match.text;
    // ⚠️ 回执必须**在**投递任务之前进队列：反过来的话工作线程可能先把第一段回答
    // 塞进队列，群里的顺序就变成"第一句 / （正在想…）/ 第二句"，看起来像答非所问。
    if (!Cfg()->ackText.empty()) EnqueueOut(generation, room, Cfg()->ackText);
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        jobs_.push_back(job);
        ++jobsPending_;
    }
    log_.Info("收到 @" + nick + "（" + room + "）：" + match.text);
    jobCv_.notify_one();
}

// ---------------------------------------------------------------------------
// 发送线程 / 工作线程
// ---------------------------------------------------------------------------

void Bot::EnqueueOut(int generation, const std::string& room, const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        if (outStop_) return;
        if (outQueue_.size() > 512) return;  // 兜底：不能无限堆
        outQueue_.push_back({generation, room, text, std::string()});
    }
    outCv_.notify_one();
}

void Bot::EnqueueFile(int generation, const std::string& room, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        if (outStop_) return;
        if (outQueue_.size() > 512) return;
        Outgoing item;
        item.generation = generation;
        item.room = room;
        item.path = path;
        outQueue_.push_back(item);
    }
    outCv_.notify_one();
}

std::string Bot::WorkspaceDir() const {
    const auto cfg = Cfg();
    if (!cfg->brain.cwd.empty()) return cfg->brain.cwd;
    std::error_code code;
    const auto current = std::filesystem::current_path(code);
    return code ? std::string() : current.string();
}

int Bot::FileCapMb() const {
    const int configured = Cfg()->fileMaxMb;
    if (configured <= 0) return 0;
    const int server = serverFileLimitMb_.load();
    if (server <= 0) return configured;
    return std::min(configured, server);
}

void Bot::EnsureWorkspaceAcl() {
#ifdef _WIN32
    const std::string dir = WorkspaceDir();
    if (dir.empty()) return;
    std::error_code code;
    if (!std::filesystem::is_directory(std::filesystem::path(dir), code) || code) return;
    wchar_t user[256] = {0};
    DWORD length = 256;
    if (!GetUserNameW(user, &length)) return;
    char utf8[512] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, user, -1, utf8, sizeof(utf8), nullptr, nullptr) <= 0) {
        return;
    }
    ProcessSpec spec;
    spec.argv = {"icacls", dir, "/grant", std::string(utf8) + ":(OI)(CI)F"};
    spec.timeoutMs = 15000;
    BrainRunResult result;
    std::string error;
    if (!RunBrain(spec, std::string(), [](const std::string&) { return true; }, &result, &error)) {
        log_.Debug("补工作目录权限失败：" + error);
        return;
    }
    if (result.exitCode == 0) {
        log_.Debug("工作目录已确认给当前用户完全控制（DSH 跑命令要用）");
    } else {
        log_.Warn("工作目录权限没补上，DSH 的 shell 工具可能报 grantWrite 失败：" +
                  result.stderrTail.substr(0, 200));
    }
#else
    // Linux 没有这个问题：沙箱用 namespace/挂载，不需要改 ACL
#endif
}

std::string Bot::ResolvePatch() const {
    const auto cfg = Cfg();
    if (!cfg->brain.patch.empty()) return cfg->brain.patch;
    // 没写就找随程序附带的那份：
    //   workspace-write / danger-full-access → dsh-local-ops.yml（允许动手）
    //   其它（默认 read-only）              → dsh-chat-only.yml（只聊天）
    const bool canTouch = cfg->brain.permissionMode == "workspace-write" ||
                          cfg->brain.permissionMode == "danger-full-access";
    return FindBundledPatch(canTouch ? "dsh-local-ops.yml" : "dsh-chat-only.yml");
}

bool Bot::UploadFile(const std::string& room, const std::string& path) {
    const int capMb = FileCapMb();
    if (capMb <= 0) {
        EnqueueOut(generation_.load(), room, "（我这边没开「发文件」）");
        return false;
    }
    std::error_code code;
    const auto bytes = std::filesystem::file_size(path, code);
    if (code) {
        log_.Warn("发文件失败：读不到 " + path);
        EnqueueOut(generation_.load(), room, "（这个文件我读不到）");
        return false;
    }
    if (bytes == 0) {
        EnqueueOut(generation_.load(), room, "（空文件发不了）");
        return false;
    }
    if (bytes > static_cast<unsigned long long>(capMb) * 1024ull * 1024ull) {
        EnqueueOut(generation_.load(), room,
                   "（文件太大：我这边最多发 " + std::to_string(capMb) + " MB）");
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        EnqueueOut(generation_.load(), room, "（这个文件我打不开）");
        return false;
    }
    const std::string name = SanitizeFileName(BaseNameOf(path));
    if (name.empty()) {
        EnqueueOut(generation_.load(), room, "（文件名不合法）");
        return false;
    }
    const std::string transferId = "B" + std::to_string(nextUploadId_++);
    if (!SendRaw(BuildLine(
            "FILE_SEND",
            BuildFileSendRest(transferId, Base64Encode(name), bytes, std::string(), false)))) {
        return false;
    }
    std::vector<char> buffer(kFileChunkBytes);
    unsigned long long sent = 0;
    while (sent < bytes) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<unsigned long long>(kFileChunkBytes, bytes - sent));
        in.read(buffer.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            SendRaw(BuildLine("FILE_CANCEL", transferId));
            log_.Warn("发文件失败：文件在读的过程中变短了 " + path);
            return false;
        }
        const std::string piece =
            Base64Encode(reinterpret_cast<const unsigned char*>(buffer.data()),
                         static_cast<std::size_t>(got));
        if (!SendRaw(BuildLine("FILE_CHUNK", transferId + " " + piece))) {
            log_.Warn("发文件失败：连接断了（已发 " + std::to_string(sent) + " 字节）");
            return false;
        }
        sent += static_cast<unsigned long long>(got);
    }
    if (!SendRaw(BuildLine("FILE_END", transferId))) return false;
    log_.Info("已发送文件 " + name + "（" + std::to_string(bytes) + " 字节）到 " + room);
    return true;
}

void Bot::EmitText(int generation, const std::string& room, const std::string& text) {
    // 先把"发文件"指令抠出来：它是给机器人看的，不能原样显示在群里
    const FileDirective directive = ExtractFileDirectives(text);
    for (const std::string& chunk : SplitForChat(directive.text, Cfg()->typing)) {
        EnqueueOut(generation, room, chunk);
    }
    const std::string workspace = WorkspaceDir();
    // 可发范围 = 工作目录 + 额外的只读目录（发文件是只读操作，改文件才是写操作）
    std::vector<std::string> roots;
    roots.push_back(workspace);
    for (const std::string& extra : Cfg()->fileRoots) roots.push_back(extra);
    for (const std::string& raw : directive.files) {
        const std::string resolved = ResolveSendablePath(roots, raw);
        if (resolved.empty()) {
            // 越界/不存在/是目录：明确说一句，别让群里以为"发了但没收到"
            log_.Warn("拒绝发文件（不在允许的目录里或不是普通文件）：" + raw);
            EnqueueOut(generation, room, "（这个文件我发不了：只能发工作目录里的文件）");
            continue;
        }
        // 先传文件再传文字？不行：文字块可能已经入队并开始发了。
        // 发文件的意图是"把东西给你"，放在文字后面更自然，顺序由队列保证。
        EnqueueFile(generation, room, resolved);
    }
}

void Bot::SenderLoop() {
    while (true) {
        Outgoing item;
        {
            std::unique_lock<std::mutex> lock(outMutex_);
            outCv_.wait(lock, [&] { return outStop_ || !outQueue_.empty(); });
            if (outStop_ && outQueue_.empty()) return;
            // **严格按队列顺序发**。曾经写过"优先挑同一房间"：本意是少切几次房间，
            // 结果把同一房间的第二条提到了第一条前面，群里看到的是
            // "第二句！/ 第三句？/ 第一句。" —— 顺序错乱比多切一次房间严重得多。
            item = outQueue_.front();
            outQueue_.pop_front();
        }
        if (item.generation != generation_.load()) continue;  // 上一条连接遗留的回答
        if (!IsMember(item.room)) continue;

        if (currentRoom_ != item.room) {
            if (!SendRaw(BuildLine("ROOM", item.room))) return;
            currentRoom_ = item.room;
        }
        if (!item.path.empty()) {
            // 文件：协议是"上传到当前会话"，所以必须在切完房间之后再传
            UploadFile(item.room, item.path);
            continue;  // 文件不分条，也不需要打字间隔
        }
        if (!SendRaw(BuildLine("MSG", EscapeText(item.text)))) return;

        int delay = ChunkDelayMs(item.text, Cfg()->typing);
        const int interval = chatIntervalMs_.load();
        if (interval > 0 && delay < interval + 40) delay = interval + 40;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
        std::unique_lock<std::mutex> lock(outMutex_);
        outCv_.wait_until(lock, deadline, [&] { return outStop_; });
        if (outStop_) return;
    }
}

void Bot::WorkerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            jobCv_.wait(lock, [&] { return jobStop_ || !jobs_.empty(); });
            if (jobStop_ && jobs_.empty()) return;
            job = jobs_.front();
            jobs_.pop_front();
        }
        RunJob(job);
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            if (jobsPending_ > 0) --jobsPending_;
        }
    }
}

Bot::Attempt Bot::RunAttempt(const Job& job, const std::string& session) {
    Attempt attempt;
    const auto cfg = Cfg();
    ProcessSpec spec;
    spec.cwd = cfg->brain.cwd;
    spec.timeoutMs = cfg->brain.timeoutMs;
    spec.argv = BuildArgv(cfg->brain.command, job.prompt, session);
    const std::string patch = ResolvePatch();
    if (!patch.empty()) {
        if (!FileExists(patch)) {
            log_.Warn("补丁文件不存在：" + patch + "（按没有补丁继续跑，行为可能不符合预期）");
        } else {
            // dsh 的启动器要求 `--patch` 排在 **profile 名之后**：
            //   dsh headless --patch ./x.yml --json -      ✅
            //   dsh --patch ./x.yml headless --json -      ❌ "--profile <name> is required"
            // 所以不能死板地插在第 2 个位置，得先找到那个 profile 名（第一个不以 '-' 开头的参数）。
            std::size_t insertAt = spec.argv.size();
            for (std::size_t i = 1; i < spec.argv.size(); ++i) {
                if (!spec.argv[i].empty() && spec.argv[i][0] != '-') {
                    insertAt = i + 1;
                    break;
                }
            }
            spec.argv.insert(spec.argv.begin() + static_cast<std::ptrdiff_t>(insertAt), patch);
            spec.argv.insert(spec.argv.begin() + static_cast<std::ptrdiff_t>(insertAt), "--patch");
        }
    }
    if (!cfg->brain.permissionMode.empty()) {
        spec.env.push_back({"DSH_PERMISSION_MODE", cfg->brain.permissionMode});
    }

    std::string answerSession;
    std::string finalText;
    auto onLine = [&](const std::string& line) -> bool {
        DshEvent event;
        if (!ParseDshEvent(line, &event)) {
            log_.Debug("非事件行：" + line);
            return true;
        }
        if (!event.sessionId.empty()) answerSession = event.sessionId;
        if (event.type == "text" && !event.text.empty()) {
            attempt.producedText = true;
            EmitText(job.generation, job.room, event.text);
        } else if (event.type == "final" && !event.text.empty()) {
            finalText = event.text;
        } else if (event.type == "error" && !event.error.empty()) {
            log_.Warn("DSH 报错：" + event.error);
        }
        return true;
    };

    const auto started = std::chrono::steady_clock::now();
    BrainRunResult result;
    std::string error;
    if (!RunBrain(spec, job.prompt, onLine, &result, &error)) {
        log_.Error("起不了大脑进程：" + error);
        attempt.started = false;
        return attempt;
    }
    attempt.started = true;
    attempt.exitCode = result.exitCode;
    attempt.timedOut = result.timedOut;
    attempt.stderrTail = result.stderrTail;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    log_.Debug("大脑结束：退出码 " + std::to_string(result.exitCode) + "，耗时 " +
               std::to_string(elapsed) + " ms");

    // 没有 text 事件但给了 final（不同版本的 DSH 可能只发 final）：用 final 兜底
    if (!attempt.producedText && !finalText.empty()) {
        EmitText(job.generation, job.room, finalText);
        attempt.producedText = true;
    }
    if (!answerSession.empty() && Cfg()->brain.sessionPerRoom) {
        SaveSession(job.room, answerSession);
    }
    return attempt;
}

void Bot::RunJob(const Job& job) {    if (Cfg()->brain.mode == "api") {
        RunApiJob(job);
        return;
    }
    const std::string session = Cfg()->brain.sessionPerRoom ? SessionFor(job.room) : std::string();
    Attempt attempt = RunAttempt(job, session);
    // 会话 id 失效（会话文件被清、HOME 换了）时 dsh 直接报错、一个字都不出。
    // 这种情况重来一次（不带会话）比回一句"出错了"有用得多。
    if (!attempt.producedText && !attempt.timedOut && attempt.started &&
        !session.empty()) {
        log_.Warn("带会话 id 的回答失败，去掉会话重试一次");
        attempt = RunAttempt(job, std::string());
    }
    if (!attempt.started) {
        EnqueueOut(job.generation, job.room, "（我这边起不来，稍后再试）");
        return;
    }
    if (attempt.timedOut) {
        EnqueueOut(job.generation, job.room, "（想太久了，这次先不说了）");
        return;
    }
    if (!attempt.producedText) {
        log_.Warn("大脑没有给出任何文本（退出码 " + std::to_string(attempt.exitCode) + "）：" +
                  attempt.stderrTail.substr(0, 400));
        EnqueueOut(job.generation, job.room, "（我这边出错了，没答上来）");
        return;
    }
    if (attempt.exitCode != 0) {
        log_.Warn("大脑退出码 " + std::to_string(attempt.exitCode) + "：" +
                  attempt.stderrTail.substr(0, 400));
    }
}

void Bot::RunApiJob(const Job& job) {
    const auto cfg = Cfg();
    ApiRunOptions options;
    options.url = cfg->brain.apiUrl;
    options.model = cfg->brain.apiModel;
    // 每个群可以有自己的 key（用户在网页里给这个群单独填过就用它）
    const auto roomKey = cfg->brain.roomApiKeys.find(job.room);
    options.key = (roomKey != cfg->brain.roomApiKeys.end() && !roomKey->second.empty())
                      ? roomKey->second
                      : cfg->brain.apiKey;
    options.maxRounds = cfg->brain.apiMaxRounds;
    options.timeoutMs = cfg->brain.apiTimeoutMs;
    options.workspace = WorkspaceDir();
    options.sendRoots = cfg->fileRoots;
    options.systemPrompt = cfg->brain.apiSystemPrompt;

    std::vector<ApiMessage> history;
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        const auto it = histories_.find(job.room);
        if (it != histories_.end()) history = it->second;
    }
    const std::string userText =
        "「" + job.roomName + "」里的 " + job.nick + " 说：" + job.userText;
    const ApiTurnResult turn = RunApiTurn(options, history, userText,
                                          [&](const std::string& line) { log_.Debug(line); });
    if (!turn.ok) {
        log_.Warn("api 大脑失败：" + turn.error);
        EnqueueOut(job.generation, job.room, "（" + turn.error + "）");
        return;
    }
    if (!turn.answer.empty()) {
        EmitText(job.generation, job.room, turn.answer);
        std::lock_guard<std::mutex> lock(historyMutex_);
        std::vector<ApiMessage>& list = histories_[job.room];
        list.push_back({"user", userText});
        list.push_back({"assistant", turn.answer});
        while (list.size() > 12) list.erase(list.begin());
    }
    for (const std::string& file : turn.files) {
        EnqueueFile(job.generation, job.room, file);
    }
}

void Bot::StartThreads() {    auto self = shared_from_this();
    std::thread([self] { self->SenderLoop(); }).detach();
    const int workers = std::max(1, Cfg()->brain.maxConcurrent);
    for (int i = 0; i < workers; ++i) {
        std::thread([self] { self->WorkerLoop(); }).detach();
    }
}

void Bot::StopThreads() {
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outStop_ = true;
    }
    outCv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        jobStop_ = true;
    }
    jobCv_.notify_all();
}

// ---------------------------------------------------------------------------
// 会话 id 落盘
// ---------------------------------------------------------------------------

std::string Bot::SessionFor(const std::string& room) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    const auto it = sessions_.find(room);
    return it == sessions_.end() ? std::string() : it->second;
}

void Bot::SaveSession(const std::string& room, const std::string& session) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessions_[room] = session;
    }
    StoreSessions();
}

void Bot::LoadSessions() {
    if (Cfg()->sessionFile.empty()) return;
    std::FILE* file = std::fopen(Cfg()->sessionFile.c_str(), "r");
    if (file == nullptr) return;
    char buffer[1024];
    std::lock_guard<std::mutex> lock(sessionMutex_);
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        std::string line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        sessions_[line.substr(0, tab)] = line.substr(tab + 1);
    }
    std::fclose(file);
}

void Bot::StoreSessions() {
    if (Cfg()->sessionFile.empty()) return;
    std::lock_guard<std::mutex> lock(sessionMutex_);
    std::FILE* file = std::fopen(Cfg()->sessionFile.c_str(), "w");
    if (file == nullptr) return;
    for (const auto& pair : sessions_) {
        std::fprintf(file, "%s\t%s\n", pair.first.c_str(), pair.second.c_str());
    }
    std::fclose(file);
}

// ---------------------------------------------------------------------------
// 本机管理界面
// ---------------------------------------------------------------------------

namespace {

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ",";
        out += item;
    }
    return out;
}

/** 路径列表用分号：Windows 路径里几乎不会出现 ';'，而 ',' 在文件名里更常见。 */
std::string JoinPaths(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ";";
        out += item;
    }
    return out;
}

HttpResponse JsonError(int status, const std::string& message) {
    HttpResponse response;
    response.status = status;
    response.body = "{\"ok\":false,\"error\":" + JsonQuote(message) + "}";
    return response;
}

}  // namespace

void Bot::StartUi() {
    const int port = Cfg()->uiPort;
    if (port <= 0) return;
    const std::string host = Cfg()->uiBind.empty() ? std::string("127.0.0.1") : Cfg()->uiBind;
    auto self = shared_from_this();
    std::string error;
    if (!ui_.Start(
            host, port,
            [self](const HttpRequest& request) { return self->HandleHttp(request); }, &error)) {
        // 端口被占用不该让机器人起不来：它照样能聊天，只是没界面
        log_.Warn("管理界面没起来：" + error + "（机器人照常工作）");
        return;
    }
    log_.Info("管理界面：http://" + host + ":" + std::to_string(ui_.BoundPort()) + "/" +
              (host == "127.0.0.1" ? "" : "（公网可达，需要登录）"));
}

HttpResponse Bot::HandleHttp(const HttpRequest& request) {
    // POST 必须带自定义头：跨站页面发不出这个头
    // （带自定义头的跨域请求会先进 OPTIONS 预检，而我们对 OPTIONS 一律 405）。
    if (request.method == "POST" &&
        request.headers.find("x-dchat-bot") == request.headers.end()) {
        return JsonError(403, "forbidden");
    }
    // 网关把管理界面挂在 /bot/ 下转发过来，页面里的接口地址也写的是 /bot/api/...
    // 这里统一剥掉这层前缀，机器人自己直接访问（127.0.0.1:8731）也照样能用。
    // ⚠️ /room/<群>/bot 不能被剥（那是单群页自己的地址）。
    std::string path = request.path;
    if (path.rfind("/bot/", 0) == 0) {
        path = path.substr(4);
    } else if (path == "/bot") {
        path = "/";
    }
    // ---- 登录（唯一不需要会话的两个入口）----
    if (request.method == "GET" && (path == "/login" || path == "/")) {
        // 没登录时首页就是登录页；登录页本身也走这里
        UiSession probe;
        if (path == "/" && LoadSession(request, &probe)) {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.body = kUiPage;
            return response;
        }
        HttpResponse response;
        response.contentType = "text/html; charset=utf-8";
        response.body = kLoginPage;
        return response;
    }
    if (request.method == "POST" && path == "/api/login") {
        return HandleLogin(request.body);
    }
    // ---- 其余全部要登录 ----
    UiSession session;
    if (!LoadSession(request, &session)) {
        // 没登录：页面请求给登录页，接口请求 401
        const bool wantsPage =
            request.method == "GET" &&
            (path == "/index.html" ||
             (path.rfind("/room/", 0) == 0 && path.find("/bot") != std::string::npos));
        if (wantsPage) {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.body = kLoginPage;
            return response;
        }
        return JsonError(401, "请先登录");
    }
    if (path == "/api/logout") {
        DropSession(request);
        HttpResponse response;
        response.body = "{\"ok\":true}";
        return response;
    }
    if (request.method == "GET" && path == "/index.html") {
        HttpResponse response;
        response.contentType = "text/html; charset=utf-8";
        response.body = kUiPage;
        return response;
    }
    // 单群管理页：/room/<群>/bot（由 webd 把 /room/<群>/bot 转到这里）
    if (request.method == "GET" && path.rfind("/room/", 0) == 0) {
        const std::size_t end = path.find("/bot");
        const std::string room =
            path.substr(6, end == std::string::npos ? std::string::npos : end - 6);
        if (!room.empty()) {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            // 群名标记必须插在 <head> 里、**主脚本之前**：页面脚本一加载就会 render()，
            // 那时如果 __ROOM__ 还没定义，单群页会当成总览页显示（"接入的群"又冒出来）。
            std::string page = kUiPage;
            const std::size_t headAt = page.find("<head>");
            const std::string mark = "<script>window.__ROOM__ = " + JsonQuote(room) + ";</script>";
            if (headAt != std::string::npos) {
                page.insert(headAt + 6, mark);
            } else {
                page = mark + page;
            }
            response.body = page;
            return response;
        }
    }
    if (request.method == "GET" && path == "/api/state") {
        HttpResponse response;
        response.body = StateJson(session);
        return response;
    }
    if (request.method == "POST" && path == "/api/config") {
        return HandleSave(request.body, session);
    }
    if (request.method == "POST" && path == "/api/search") {
        if (!session.admin) return JsonError(403, "只有管理员能用搜索");
        return HandleSearch(request.body);
    }
    if (request.method == "POST" && path == "/api/join") {
        if (!session.admin) return JsonError(403, "只有管理员能让它加群");
        return HandleJoin(request.body, session);
    }
    return JsonError(404, "not found");
}

/** 从 Cookie 里取会话 token。 */
void Bot::RefreshRoomRoles() {
    if (sock_ == sock::kInvalid || !loggedIn_.load()) return;
    std::vector<std::string> rooms;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& pair : rooms_) {
            if (pair.second.member) rooms.push_back(pair.first);
        }
    }
    if (rooms.empty()) return;
    for (const std::string& room : rooms) SendRaw(BuildLine("ROOM", room));
    // 给服务器一点时间把 RMEMBERS 推回来（接收线程会顺手更新 roomRoles_）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::string Bot::CookieToken(const HttpRequest& request) const {
    const auto it = request.headers.find("cookie");
    if (it == request.headers.end()) return std::string();
    const std::string& text = it->second;
    const std::string name = "dchatbot_session=";
    std::size_t pos = text.find(name);
    while (pos != std::string::npos) {
        const std::size_t start = pos + name.size();
        const std::size_t end = text.find(';', start);
        const std::string token =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) return token;
        pos = text.find(name, pos + 1);
    }
    return std::string();
}

bool Bot::LoadSession(const HttpRequest& request, UiSession* out) {
    const std::string token = CookieToken(request);
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lock(uiSessionMutex_);
    const auto it = uiSessions_.find(token);
    if (it == uiSessions_.end()) return false;
    if (std::chrono::steady_clock::now() > it->second.expires) {
        uiSessions_.erase(it);
        return false;
    }
    *out = it->second;
    return true;
}

void Bot::DropSession(const HttpRequest& request) {
    const std::string token = CookieToken(request);
    if (token.empty()) return;
    std::lock_guard<std::mutex> lock(uiSessionMutex_);
    uiSessions_.erase(token);
}

HttpResponse Bot::HandleLogin(const std::string& body) {
    FlatJson input;
    std::string error;
    if (!ParseFlatJson(body, &input, &error)) return JsonError(400, error);
    const std::string nick = input.GetString("nick", std::string());
    const std::string password = input.GetString("password", std::string());
    if (nick.empty() || password.empty()) return JsonError(400, "昵称和密码都要填");

    // 简单的暴力破解闸门：每个 IP 每分钟最多 5 次
    {
        std::lock_guard<std::mutex> lock(uiSessionMutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = loginAttempts_.begin(); it != loginAttempts_.end();) {
            if (now - it->second.first > std::chrono::minutes(1)) {
                it = loginAttempts_.erase(it);
            } else {
                ++it;
            }
        }
        if (loginAttempts_.size() > 200) return JsonError(429, "尝试太频繁，过一会儿再试");
    }

    const auto cfg = Cfg();
    const PasswordCheck check =
        VerifyDchatPassword(cfg->host, cfg->port, nick, password, 6000);
    if (!check.ok) {
        log_.Warn("管理界面登录失败：" + nick + "（" + check.error + "）");
        return JsonError(403, check.error.empty() ? "昵称或密码不对" : check.error);
    }
    // op 名单：和服务器同一个文件（机器人就跑在这台机器上）
    // 全局管理员名单（= 服务端 dchat-admins.txt）。这是"所有群的群主"那档权限。
    bool admin = false;
    {
        std::ifstream admins(cfg->uiOpFile.c_str());
        std::string line;
        while (std::getline(admins, line)) {
            // 文件可能是 UTF-8 BOM 开头（很多编辑器/Set-Content 默认带），
            // 不剥掉的话第一个昵称永远匹配不上 —— 这是踩过的坑。
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line[0] == '#') continue;
            if (SameNick(line, nick)) admin = true;
        }
    }
    std::set<std::string> opRooms;
    {
        // 先向服务器要一次最新的成员表：缓存里的角色可能已经过期
        //（比如你刚被设成群管理员，但机器人入群时还没这回事）。
        RefreshRoomRoles();
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& room : roomRoles_) {
            for (const auto& member : room.second) {
                // owner = 群主；admin = 群管理员。这两个都算"这个群的 op"。
                if (SameNick(member.first, nick) &&
                    (member.second == "owner" || member.second == "admin")) {
                    opRooms.insert(room.first);
                }
            }
        }
    }
    if (!admin && opRooms.empty()) {
        log_.Warn("管理界面登录被拒（既不是管理员也不是任何群的群主/管理员）：" + nick);
        return JsonError(403, "你不是管理员，也不是这个机器人在的群的群主/群管理员");
    }

    std::string token;
    {
        std::vector<unsigned char> bytes;
        if (!dchat::RandomBytes(24, &bytes)) return JsonError(500, "生成会话失败");
        static constexpr char kHex[] = "0123456789abcdef";
        for (unsigned char byte : bytes) {
            token.push_back(kHex[byte >> 4]);
            token.push_back(kHex[byte & 0x0F]);
        }
    }
    UiSession session;
    session.nick = nick;
    session.admin = admin;
    session.opRooms = opRooms;
    session.expires = std::chrono::steady_clock::now() + std::chrono::hours(8);
    {
        std::lock_guard<std::mutex> lock(uiSessionMutex_);
        uiSessions_[token] = session;
    }
    log_.Info("管理界面登录：" + nick + "（" +
              (admin ? std::string("管理员")
                     : "群管理员：" + [&] {
                           std::string list;
                           for (const std::string& room : opRooms) {
                               if (!list.empty()) list += ",";
                               list += room;
                           }
                           return list;
                       }()) +
              "）");

    std::string rooms;
    for (const std::string& room : opRooms) {
        if (!rooms.empty()) rooms += ",";
        rooms += JsonQuote(room);
    }
    HttpResponse response;
    response.headers.push_back(
        {"Set-Cookie",
         "dchatbot_session=" + token + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=28800"});
    // 注意：**不把 admin 这个身份回给页面**（用户要求"只是权限不要显示"）。
    // 页面只需要知道"能不能改"，由后端每次请求自己判。
    response.body = "{\"ok\":true,\"nick\":" + JsonQuote(nick) + ",\"opRooms\":[" + rooms + "]}";
    return response;
}

std::string Bot::StateJson(const UiSession& session) {
    const auto cfg = Cfg();
    std::string json = "{\"ok\":true";
    json += ",\"online\":";
    json += (loggedIn_.load() && sock_ != sock::kInvalid) ? "true" : "false";
    json += ",\"nick\":" + JsonQuote(cfg->nick);
    json += ",\"server\":" + JsonQuote(cfg->host + ":" + std::to_string(cfg->port));
    json += ",\"rooms\":" + JsonQuoteArray(cfg->rooms);
    // 页面用这两个决定"显示哪一页"：能不能管所有群、能管哪些群。
    // 这不是身份标签 —— 界面上不会出现"管理员/群管理员"字样。
    json += ",\"canManageAll\":";
    json += session.admin ? "true" : "false";
    {
        std::string opRooms;
        for (const std::string& room : session.opRooms) {
            if (!opRooms.empty()) opRooms += ",";
            opRooms += JsonQuote(room);
        }
        json += ",\"opRooms\":[" + opRooms + "]";
    }

    // known = 我已经加入的群 + 配置里写了但还没加入的（后者标 joined:false，
    // 这样界面上仍然能看到它、也能把它关掉 —— 否则配错一个群 ID 就再也删不掉了）
    std::vector<RoomState> known;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& pair : rooms_) known.push_back(pair.second);
    }
    for (const std::string& configured : cfg->rooms) {
        bool present = false;
        for (const RoomState& room : known) {
            if (SameRoomId(room.id, configured)) {
                present = true;
                break;
            }
        }
        if (!present) {
            RoomState ghost;
            ghost.id = configured;
            ghost.name = configured;
            ghost.kind = "unknown";
            ghost.member = false;
            known.push_back(ghost);
        }
    }
    // 大厅排最前，其余按 ID 排 —— 界面上顺序稳定，找起来快
    std::sort(known.begin(), known.end(), [](const RoomState& a, const RoomState& b) {
        if (a.id == "hall") return b.id != "hall";
        if (b.id == "hall") return false;
        return a.id < b.id;
    });
    json += ",\"known\":[";
    for (std::size_t i = 0; i < known.size(); ++i) {
        if (i > 0) json += ",";
        json += "{\"id\":" + JsonQuote(known[i].id);
        json += ",\"name\":" + JsonQuote(known[i].name.empty() ? known[i].id : known[i].name);
        json += ",\"kind\":" + JsonQuote(known[i].kind);
        json += ",\"joined\":";
        json += known[i].member ? "true" : "false";
        json += "}";
    }
    json += "]";

    const char* mode = "all";
    if (cfg->reply.mode == ReplyMode::Whitelist) mode = "whitelist";
    if (cfg->reply.mode == ReplyMode::Blacklist) mode = "blacklist";
    json += ",\"replyMode\":" + JsonQuote(mode);
    json += ",\"replyList\":" + JsonQuoteArray(cfg->reply.nicks);
    json += ",\"mentionOnly\":";
    json += cfg->trigger.mentionOnly ? "true" : "false";
    json += ",\"aliases\":" + JsonQuoteArray(cfg->trigger.aliases);
    json += ",\"ackText\":" + JsonQuote(cfg->ackText);
    const bool canTouch = cfg->brain.permissionMode == "workspace-write" ||
                          cfg->brain.permissionMode == "danger-full-access";
    json += ",\"brainMode\":" + JsonQuote(canTouch ? "local" : "chat");
    // ---- 大脑类型与 api 配置 ----
    // **key 是只写的**：页面只能"设置/替换/清空"，永远拿不回任何片段
    //（连前 5 位后 4 位都不给 —— 用户要求"除了服务器端，任何人都不能查看"）。
    // 这里只告诉界面"有没有设置"。
    json += ",\"brainType\":" + JsonQuote(cfg->brain.mode);
    json += ",\"apiUrl\":" + JsonQuote(cfg->brain.apiUrl);
    json += ",\"apiModel\":" + JsonQuote(cfg->brain.apiModel);
    json += ",\"apiKeySet\":" + std::string(cfg->brain.apiKey.empty() ? "false" : "true");
    {
        std::string roomKeys;
        for (const auto& entry : cfg->brain.roomApiKeys) {
            if (!roomKeys.empty()) roomKeys += ",";
            roomKeys += "{\"room\":" + JsonQuote(entry.first) + "}";
        }
        json += ",\"apiRoomKeys\":[" + roomKeys + "]";
    }
    json += ",\"workDir\":" + JsonQuote(cfg->brain.cwd);
    json += ",\"fileMaxMb\":" + std::to_string(cfg->fileMaxMb);
    json += ",\"fileRoots\":" + JsonQuoteArray(cfg->fileRoots);
    json += ",\"patch\":" + JsonQuote(ResolvePatch());
    json += ",\"typing\":{";
    json += "\"maxChars\":" + std::to_string(cfg->typing.maxChars);
    json += ",\"minChars\":" + std::to_string(cfg->typing.minChars);
    json += ",\"minGapMs\":" + std::to_string(cfg->typing.minGapMs);
    json += ",\"perCharMs\":" + std::to_string(cfg->typing.perCharMs);
    json += ",\"maxGapMs\":" + std::to_string(cfg->typing.maxGapMs);
    json += "}";

    const std::vector<std::string> tail = log_.Tail(80);
    json += ",\"log\":[";
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if (i > 0) json += ",";
        json += JsonQuote(tail[i]);
    }
    json += "]}";
    return json;
}

namespace {

/**
 * 解析"房间ID = sk-..."这种行，写进 keys。
 *
 * `onlyRoom` 非空时**只接受这一行**（单群页/群管理员用）：页面只有这个群的输入框，
 * 别的群的行即使被构造出来也不认，否则群管理员改一个页面就能改别的群。
 * 值写 `-` 表示清空该群的 key。
 */
void ApplyRoomKeyLines(const std::string& text, std::map<std::string, std::string>* keys,
                       const std::string& onlyRoom) {
    const auto trim = [](std::string* value) {
        while (!value->empty() && (value->back() == ' ' || value->back() == '\t')) value->pop_back();
        while (!value->empty() && (value->front() == ' ' || value->front() == '\t')) {
            value->erase(0, 1);
        }
    };
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string room = line.substr(0, equals);
        std::string key = line.substr(equals + 1);
        trim(&room);
        trim(&key);
        if (room.empty()) continue;
        if (!onlyRoom.empty() && !SameRoomId(room, onlyRoom)) continue;
        if (key == "-") {
            keys->erase(room);
        } else if (!key.empty()) {
            (*keys)[room] = key;
        }
    }
}

}  // namespace

HttpResponse Bot::HandleSave(const std::string& body, const UiSession& session) {
    FlatJson input;
    std::string error;
    if (!ParseFlatJson(body, &input, &error)) return JsonError(400, error);

    auto current = Cfg();

    // 群主/群管理员（非 admin）只能改**自己那个群**的东西。
    // 这里的 scopeRoom 就是"这一页管的是哪个群"：单群页 /room/<群>/bot 带群名，
    // 总览页不带（非 admin 只能改自己有权限的群，见下面逐个字段的判断）。
    if (!session.admin) {
        Config next = *current;
        if (input.Has("room")) {
            const std::string room = input.GetString("room", std::string());
            if (room.empty() || session.opRooms.count(room) == 0) {
                log_.Warn("群管理员 " + session.nick + " 想改 " + room + "，但 TA 不是那个群的管理");
                return JsonError(403, "你不是这个群的管理员");
            }
            if (input.Has("apiRoomKeyText")) {
                const std::string text = input.GetString("apiRoomKeyText", std::string());
                std::map<std::string, std::string> keys = next.brain.roomApiKeys;
                ApplyRoomKeyLines(text, &keys, room);
                next.brain.roomApiKeys = keys;
            }
            ApplyConfig(next);
            WriteConfigFile(next);
            HttpResponse response;
            response.body = "{\"ok\":true,\"note\":" + JsonQuote("已保存「" + room + "」的设置") +
                            ",\"state\":" + StateJson(session) + "}";
            return response;
        }
        // 没带 room：只允许改自己有权限的群的那些 key，其余字段忽略
        if (input.Has("apiRoomKeyText")) {
            const std::string text = input.GetString("apiRoomKeyText", std::string());
            std::map<std::string, std::string> keys = next.brain.roomApiKeys;
            ApplyRoomKeyLines(text, &keys, std::string());
            next.brain.roomApiKeys = keys;
        }
        ApplyConfig(next);
        WriteConfigFile(next);
        HttpResponse response;
        response.body = "{\"ok\":true,\"note\":" +
                        JsonQuote("已保存（你只能改自己管理的群的设置，其他设置需要管理员）") +
                        ",\"state\":" + StateJson(session) + "}";
        return response;
    }

    Config next = *current;
    std::vector<std::string> values;

    if (input.GetArray("rooms", &values)) {
        next.rooms.clear();
        for (const std::string& raw : values) {
            std::string room = raw;
            for (char& ch : room) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (room.empty()) continue;
            bool duplicate = false;
            for (const std::string& existing : next.rooms) {
                if (SameRoomId(existing, room)) duplicate = true;
            }
            if (!duplicate) next.rooms.push_back(room);
        }
    }
    if (input.Has("mentionOnly")) next.trigger.mentionOnly = input.GetBool("mentionOnly", true);
    if (input.GetArray("aliases", &values)) next.trigger.aliases = values;
    if (input.Has("replyMode")) {
        const std::string mode = input.GetString("replyMode", "all");
        if (mode == "all") {
            next.reply.mode = ReplyMode::All;
        } else if (mode == "whitelist") {
            next.reply.mode = ReplyMode::Whitelist;
        } else if (mode == "blacklist") {
            next.reply.mode = ReplyMode::Blacklist;
        } else {
            return JsonError(400, "replyMode 只能是 all / whitelist / blacklist");
        }
    }
    if (input.GetArray("replyList", &values)) next.reply.nicks = values;
    if (input.Has("ackText")) next.ackText = input.GetString("ackText", std::string());
    if (input.Has("brainType")) {
        const std::string type = input.GetString("brainType", "dsh");
        if (type != "dsh" && type != "api") return JsonError(400, "brainType 只能是 dsh / api");
        next.brain.mode = type;
    }
    if (input.Has("apiUrl")) {
        const std::string url = input.GetString("apiUrl", std::string());
        if (!url.empty()) next.brain.apiUrl = url;
    }
    if (input.Has("apiModel")) {
        const std::string model = input.GetString("apiModel", std::string());
        if (!model.empty()) next.brain.apiModel = model;
    }
    // key 的处理约定：**留空 = 不改**（页面里只显示掩码，提交空值不会把 key 抹掉），
    // 单独填一个 "-" = 清空。这样"每对话一个 key"的编辑才不会互相覆盖。
    if (input.Has("apiKey")) {
        const std::string key = input.GetString("apiKey", std::string());
        if (key == "-") {
            next.brain.apiKey.clear();
        } else if (!key.empty()) {
            next.brain.apiKey = key;
        }
    }
    if (input.Has("apiRoomKeyText")) {
        // 表单里只提交"新写的"那几行：`房间 = sk-...` 表示设置，`房间 = -` 表示清空。
        // 没出现在表单里的群保持原样 —— key 是只写的，页面本来就拿不回旧值。
        std::map<std::string, std::string> keys = next.brain.roomApiKeys;
        ApplyRoomKeyLines(input.GetString("apiRoomKeyText", std::string()), &keys, std::string());
        next.brain.roomApiKeys = keys;
    }
    if (input.Has("brainMode")) {
        const std::string mode = input.GetString("brainMode", "chat");
        if (mode == "local") {
            next.brain.permissionMode = "workspace-write";
            next.brain.patch = FindBundledPatch("dsh-local-ops.yml");
        } else if (mode == "chat") {
            next.brain.permissionMode = "read-only";
            next.brain.patch = FindBundledPatch("dsh-chat-only.yml");
        } else {
            return JsonError(400, "brainMode 只能是 chat / local");
        }
    }
    if (input.Has("workDir")) {
        std::string dir = input.GetString("workDir", std::string());
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        next.brain.cwd = dir;
    }
    if (input.Has("fileMaxMb")) next.fileMaxMb = input.GetInt("fileMaxMb", next.fileMaxMb);
    if (input.GetArray("fileRoots", &values)) {
        next.fileRoots.clear();
        for (const std::string& raw : values) {
            std::string dir = raw;
            while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
            if (!dir.empty()) next.fileRoots.push_back(dir);
        }
    }
    if (input.Has("maxChars")) next.typing.maxChars = input.GetInt("maxChars", next.typing.maxChars);
    if (input.Has("minChars")) next.typing.minChars = input.GetInt("minChars", next.typing.minChars);
    if (input.Has("minGapMs")) next.typing.minGapMs = input.GetInt("minGapMs", next.typing.minGapMs);
    if (input.Has("perCharMs")) next.typing.perCharMs = input.GetInt("perCharMs", next.typing.perCharMs);
    if (input.Has("maxGapMs")) next.typing.maxGapMs = input.GetInt("maxGapMs", next.typing.maxGapMs);

    if (!ValidateConfig(&next, &error)) return JsonError(400, error);
    ApplyConfig(next);
    WriteConfigFile(next);

    HttpResponse response;
    response.body = "{\"ok\":true,\"state\":" + StateJson(session) + "}";
    return response;
}

HttpResponse Bot::HandleSearch(const std::string& body) {
    FlatJson input;
    std::string error;
    if (!ParseFlatJson(body, &input, &error)) return JsonError(400, error);
    const std::string query = input.GetString("q", std::string());
    if (query.empty()) return JsonError(400, "搜索关键词不能为空");
    if (sock_ == sock::kInvalid || !loggedIn_.load()) return JsonError(409, "机器人还没连上服务器");

    {
        std::lock_guard<std::mutex> lock(searchMutex_);
        searchHits_.clear();
        searchDone_ = false;
        searchPending_ = true;
    }
    if (!SendRaw(BuildLine("GSEARCH", Base64Encode(query)))) {
        return JsonError(502, "发给服务器失败");
    }
    std::vector<SearchHit> hits;
    {
        std::unique_lock<std::mutex> lock(searchMutex_);
        // 服务端是同步回一屏结果，正常几十毫秒；3 秒还没收全就当没有
        searchCv_.wait_for(lock, std::chrono::seconds(3), [&] { return searchDone_; });
        hits = searchHits_;
        searchPending_ = false;
    }
    std::string json = "{\"ok\":true,\"hits\":[";
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) json += ",";
        json += "{\"id\":" + JsonQuote(hits[i].id);
        json += ",\"name\":" + JsonQuote(hits[i].name);
        json += ",\"members\":" + std::to_string(hits[i].members);
        json += ",\"maxMembers\":" + std::to_string(hits[i].maxMembers);
        json += ",\"joinMode\":" + JsonQuote(hits[i].joinMode) + "}";
    }
    json += "]}";
    HttpResponse response;
    response.body = json;
    return response;
}

HttpResponse Bot::HandleJoin(const std::string& body, const UiSession& session) {
    FlatJson input;
    std::string error;
    if (!ParseFlatJson(body, &input, &error)) return JsonError(400, error);
    std::string room = input.GetString("room", std::string());
    for (char& ch : room) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (room.empty()) return JsonError(400, "群 ID 不能为空");
    for (char ch : room) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) return JsonError(400, "群 ID 只能用字母、数字、下划线和减号");
    }

    Config next = *Cfg();
    bool present = false;
    for (const std::string& existing : next.rooms) {
        if (SameRoomId(existing, room)) present = true;
    }
    if (!present) next.rooms.push_back(room);
    ApplyConfig(next);  // 里面会给新群发 GJOIN
    WriteConfigFile(next);

    HttpResponse response;
    response.body = "{\"ok\":true,\"note\":" +
                    JsonQuote("已把 " + room + " 加进接入列表；需要审核的群要等群主同意后才会回复") +
                    ",\"state\":" + StateJson(session) + "}";
    return response;
}

bool Bot::ApplyConfig(const Config& next) {
    std::vector<std::string> added;
    {
        const auto current = Cfg();
        for (const std::string& room : next.rooms) {
            bool present = false;
            for (const std::string& old : current->rooms) {
                if (SameRoomId(old, room)) present = true;
            }
            if (!present) added.push_back(room);
        }
    }
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = std::make_shared<const Config>(next);
    }
    std::string rooms;
    for (const std::string& room : next.rooms) {
        if (!rooms.empty()) rooms += ",";
        rooms += room;
    }
    log_.Info("配置已更新：接入 " + rooms + "；回复范围 " +
              std::string(next.reply.mode == ReplyMode::All
                              ? "all"
                              : (next.reply.mode == ReplyMode::Whitelist ? "whitelist" : "blacklist")));
    // 新加进来的群立刻申请加入（已在群里的不会重复申请）
    for (const std::string& room : added) {
        if (SameRoomId(room, "hall")) continue;
        if (sock_ == sock::kInvalid || !loggedIn_.load()) continue;
        log_.Info("申请加入会话：" + room);
        SendRaw(BuildLine("GJOIN", room));
    }
    // 工作目录或权限模式刚改过：把 ACL 也补一下（DSH 跑命令要用）
    const bool canTouch = next.brain.permissionMode == "workspace-write" ||
                          next.brain.permissionMode == "danger-full-access";
    if (canTouch) EnsureWorkspaceAcl();
    return true;
}

void Bot::WriteConfigFile(const Config& next) {
    if (configPath_.empty()) return;
    std::string text;
    {
        std::ifstream file(configPath_, std::ios::binary);
        if (file) {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            text = buffer.str();
        }
    }
    const char* mode = "all";
    if (next.reply.mode == ReplyMode::Whitelist) mode = "whitelist";
    if (next.reply.mode == ReplyMode::Blacklist) mode = "blacklist";
    const std::vector<std::pair<std::string, std::string>> changes = {
        {"rooms", JoinList(next.rooms)},
        {"rooms.autojoin", next.autoJoin ? "true" : "false"},
        {"trigger.mentiononly", next.trigger.mentionOnly ? "true" : "false"},
        {"trigger.aliases", JoinList(next.trigger.aliases)},
        {"reply.mode", mode},
        {"reply.list", JoinList(next.reply.nicks)},
        {"reply.acktext", EscapeConfigValue(next.ackText)},
        {"brain.permissionmode", next.brain.permissionMode},
        {"brain.patch", next.brain.patch},
        {"brain.cwd", next.brain.cwd},
        {"file.maxmb", std::to_string(next.fileMaxMb)},
        {"file.roots", JoinPaths(next.fileRoots)},
        {"typing.maxchars", std::to_string(next.typing.maxChars)},
        {"typing.minchars", std::to_string(next.typing.minChars)},
        {"typing.mingapms", std::to_string(next.typing.minGapMs)},
        {"typing.percharms", std::to_string(next.typing.perCharMs)},
        {"typing.maxgapms", std::to_string(next.typing.maxGapMs)},
        {"brain.mode", next.brain.mode},
        {"brain.api.url", EscapeConfigValue(next.brain.apiUrl)},
        {"brain.api.model", EscapeConfigValue(next.brain.apiModel)},
        {"brain.api.key", EscapeConfigValue(next.brain.apiKey)},
    };
    const std::string updated = UpdateConfigText(text, changes);
    // 每个群的 key 单独写（包含"被删掉的"：写成空值，加载时会被当成没有）
    std::string withRoomKeys = updated;
    {
        std::vector<std::pair<std::string, std::string>> roomChanges;
        for (const auto& entry : next.brain.roomApiKeys) {
            roomChanges.emplace_back("brain.api.key." + entry.first,
                                     EscapeConfigValue(entry.second));
        }
        for (const auto& entry : Cfg()->brain.roomApiKeys) {
            if (next.brain.roomApiKeys.find(entry.first) == next.brain.roomApiKeys.end()) {
                roomChanges.emplace_back("brain.api.key." + entry.first, std::string());
            }
        }
        if (!roomChanges.empty()) withRoomKeys = UpdateConfigText(withRoomKeys, roomChanges);
    }
    // 先写临时文件再改名：直接覆盖写，写一半断电会留下半份配置，下次启动直接报错。
    const std::string temp = configPath_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            log_.Warn("保存配置失败：写不了 " + temp + "（改动已在内存里生效，重启会丢）");
            return;
        }
        out.write(withRoomKeys.data(), static_cast<std::streamsize>(withRoomKeys.size()));
        if (!out) {
            log_.Warn("保存配置失败：写 " + temp + " 出错");
            return;
        }
    }
    std::remove(configPath_.c_str());
    if (std::rename(temp.c_str(), configPath_.c_str()) != 0) {
        log_.Warn("保存配置失败：改名到 " + configPath_ + " 失败");
        return;
    }
    log_.Info("配置已写回 " + configPath_);
}

void Bot::Run() {
    LoadSessions();
    {
        const auto cfg = Cfg();
        const bool canTouch = cfg->brain.permissionMode == "workspace-write" ||
                              cfg->brain.permissionMode == "danger-full-access";
        if (canTouch) EnsureWorkspaceAcl();
    }
    StartUi();
    StartThreads();
    int backoff = Cfg()->reconnectMinMs;
    while (!g_requestStop.load()) {
        std::string error;
        if (ConnectAndLogin(&error)) {
            backoff = Cfg()->reconnectMinMs;
            ReceiveLoop();
        } else {
            log_.Error("连接失败：" + error);
        }
        Disconnect();
        if (g_requestStop.load()) break;
        log_.Info(std::to_string(backoff / 1000) + " 秒后重连");
        if (!SleepMs(backoff)) break;
        backoff = std::min(Cfg()->reconnectMaxMs, backoff * 2);
    }
    StopThreads();
    ui_.Stop();
    // 给工作线程一点收尾时间（它们可能在等子进程）。进程随后退出，
    // 还没结束的回答本来也没人收得到了。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    log_.Info("退出");
}

}  // namespace

int RunBot(const Config& config, const std::string& configPath) {
    // 故意 new 出来不释放：工作线程是 detach 的，进程退出前最后一刻它可能还在写日志。
    // 放在栈上会在 Run 返回时析构，然后被那条线程用到 —— 这是典型的退出期 use-after-free
    // （平时看不出来，偶尔崩在退出路径上，还查不到是谁）。
    Logger* logger = new Logger();
    std::string error;
    if (!logger->Open(config.logPath, config.verbose, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        // 日志文件打不开不致命：还能写 stdout / systemd journal
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifndef _WIN32
    // 写 stdout 用的不是 socket：管道另一端一关就是 SIGPIPE，默认直接杀进程。
    // 机器人不该因为"日志管道断了"就整个挂掉。
    std::signal(SIGPIPE, SIG_IGN);
#endif

    sock::Startup();
    auto bot = std::make_shared<Bot>(config, configPath, *logger);
    bot->Run();
    sock::Cleanup();
    return 0;
}

}  // namespace dshbot
}  // namespace dchat
