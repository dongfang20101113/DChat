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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <csignal>

#include "bot.h"
#include "bot_chunk.h"
#include "bot_config.h"
#include "bot_event.h"
#include "bot_policy.h"
#include "bot_process.h"
#include "crypto.h"
#include "file_transfer.h"
#include "protocol.h"
#include "render.h"

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
    const std::string line = NowText() + " [" + level + "] " + text + "\n";
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
    if (file_ != nullptr) {
        std::fputs(line.c_str(), static_cast<std::FILE*>(file_));
        std::fflush(static_cast<std::FILE*>(file_));
    }
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
    Bot(const Config& config, Logger& log) : cfg_(config), log_(log) {}

    void Run();

private:
    struct RoomState {
        std::string id;
        std::string kind;
        std::string name;
        bool member = false;
    };
    struct Outgoing {
        int generation = 0;
        std::string room;
        std::string text;
    };
    struct Job {
        int generation = 0;
        std::string room;
        std::string roomName;
        std::string nick;
        std::string prompt;
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
    void SenderLoop();
    void WorkerLoop();
    void EnqueueOut(int generation, const std::string& room, const std::string& text);
    void EmitText(int generation, const std::string& room, const std::string& text);
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

    Config cfg_;
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
    bool roomListDone_ = false;
    std::atomic<bool> loggedIn_{false};
    std::atomic<int> generation_{0};
    std::atomic<int> chatIntervalMs_{0};

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
};

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
    for (const std::string& configured : cfg_.rooms) {
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
    const std::string portText = std::to_string(cfg_.port);
    if (::getaddrinfo(cfg_.host.c_str(), portText.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        if (error != nullptr) *error = "解析不了主机：" + cfg_.host;
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
            *error = "连不上 " + cfg_.host + ":" + std::to_string(cfg_.port) + "（错误码 " +
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
        if (cfg_.encrypt == "require") {
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
    if (!cfg_.fingerprint.empty()) {
        if (!SameNick(cfg_.fingerprint, fingerprint)) {
            log_.Error("服务器公钥指纹和配置里的不一致：配置 " + cfg_.fingerprint + "，实际 " +
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
    if (cfg_.encrypt != "off") {
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
    const bool registering = cfg_.registerFirst;
    SendRaw(BuildLine(registering ? "REGISTER" : "LOGIN", cfg_.nick + " " + cfg_.password));
    log_.Info(std::string(registering ? "注册并登录" : "登录") + "：" + cfg_.nick);

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
                SendRaw(BuildLine("LOGIN", cfg_.nick + " " + cfg_.password));
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
    std::string roomList;
    for (const std::string& room : cfg_.rooms) {
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
    for (const std::string& room : cfg_.rooms) {
        if (SameRoomId(room, "hall")) continue;
        bool member = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const RoomState* state = FindRoomLocked(room);
            member = state != nullptr && state->member;
        }
        if (member) continue;
        if (!cfg_.autoJoin) {
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
        if (status == ReadStatus::Line) {
            if (!HandleIncoming(line)) break;
            continue;
        }
        if (status == ReadStatus::Timeout) continue;
        break;  // Closed / Failed
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

    const std::string text = UnescapeText(rawText);
    const TriggerMatch match = MatchTrigger(text, cfg_.nick, cfg_.trigger);
    if (!match.matched) return;

    if (!ReplyAllowed(cfg_.reply, nick)) {
        const char* why = (cfg_.reply.mode == ReplyMode::Whitelist) ? "不在白名单" : "在黑名单";
        log_.Info("跳过 " + nick + " 的 @" + "（" + why + "）");
        return;
    }

    const int generation = generation_.load();
    int pending = 0;
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        pending = jobsPending_;
    }
    if (pending >= cfg_.brain.maxQueue + std::max(1, cfg_.brain.maxConcurrent)) {
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
    job.prompt = BuildPrompt(cfg_.brain, job.roomName, room, nick, match.text);
    // ⚠️ 回执必须**在**投递任务之前进队列：反过来的话工作线程可能先把第一段回答
    // 塞进队列，群里的顺序就变成"第一句 / （正在想…）/ 第二句"，看起来像答非所问。
    if (!cfg_.ackText.empty()) EnqueueOut(generation, room, cfg_.ackText);
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
        outQueue_.push_back({generation, room, text});
    }
    outCv_.notify_one();
}

void Bot::EmitText(int generation, const std::string& room, const std::string& text) {
    for (const std::string& chunk : SplitForChat(text, cfg_.typing)) {
        EnqueueOut(generation, room, chunk);
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
        if (!SendRaw(BuildLine("MSG", EscapeText(item.text)))) return;

        int delay = ChunkDelayMs(item.text, cfg_.typing);
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
    ProcessSpec spec;
    spec.cwd = cfg_.brain.cwd;
    spec.timeoutMs = cfg_.brain.timeoutMs;
    spec.argv = BuildArgv(cfg_.brain.command, job.prompt, session);
    if (!cfg_.brain.patch.empty()) {
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
        spec.argv.insert(spec.argv.begin() + static_cast<std::ptrdiff_t>(insertAt),
                         cfg_.brain.patch);
        spec.argv.insert(spec.argv.begin() + static_cast<std::ptrdiff_t>(insertAt), "--patch");
    }
    if (!cfg_.brain.permissionMode.empty()) {
        spec.env.push_back({"DSH_PERMISSION_MODE", cfg_.brain.permissionMode});
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
    if (!answerSession.empty() && cfg_.brain.sessionPerRoom) {
        SaveSession(job.room, answerSession);
    }
    return attempt;
}

void Bot::RunJob(const Job& job) {
    const std::string session = cfg_.brain.sessionPerRoom ? SessionFor(job.room) : std::string();
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

void Bot::StartThreads() {
    auto self = shared_from_this();
    std::thread([self] { self->SenderLoop(); }).detach();
    const int workers = std::max(1, cfg_.brain.maxConcurrent);
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
    if (cfg_.sessionFile.empty()) return;
    std::FILE* file = std::fopen(cfg_.sessionFile.c_str(), "r");
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
    if (cfg_.sessionFile.empty()) return;
    std::lock_guard<std::mutex> lock(sessionMutex_);
    std::FILE* file = std::fopen(cfg_.sessionFile.c_str(), "w");
    if (file == nullptr) return;
    for (const auto& pair : sessions_) {
        std::fprintf(file, "%s\t%s\n", pair.first.c_str(), pair.second.c_str());
    }
    std::fclose(file);
}

void Bot::Run() {
    LoadSessions();
    StartThreads();
    int backoff = cfg_.reconnectMinMs;
    while (!g_requestStop.load()) {
        std::string error;
        if (ConnectAndLogin(&error)) {
            backoff = cfg_.reconnectMinMs;
            ReceiveLoop();
        } else {
            log_.Error("连接失败：" + error);
        }
        Disconnect();
        if (g_requestStop.load()) break;
        log_.Info(std::to_string(backoff / 1000) + " 秒后重连");
        if (!SleepMs(backoff)) break;
        backoff = std::min(cfg_.reconnectMaxMs, backoff * 2);
    }
    StopThreads();
    // 给工作线程一点收尾时间（它们可能在等子进程）。进程随后退出，
    // 还没结束的回答本来也没人收得到了。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    log_.Info("退出");
}

}  // namespace

int RunBot(const Config& config) {
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
    auto bot = std::make_shared<Bot>(config, *logger);
    bot->Run();
    sock::Cleanup();
    return 0;
}

}  // namespace dshbot
}  // namespace dchat
