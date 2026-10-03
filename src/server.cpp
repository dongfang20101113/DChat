// 聊天服务器：Winsock2 + 每客户端一个线程，收到消息后广播给所有人。
// 控制台日志保持纯 ASCII（避免代码页问题），协议里的用户可见文本是 UTF-8 中文。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "auth.h"
#include "crypto.h"
#include "file_transfer.h"
#include "server_rules.h"
#include "server_command.h"

namespace {

constexpr const char* kServerName = "dchat Server";

void Log(const std::string& text);  // 定义在下面，文件暂存的几个函数也要用它

// 文件传输用的传输 ID：客户端自己生成，只允许字母/数字/下划线/连字符
bool IsValidTransferId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// 解析文件大小（只允许十进制数字，最多 20 位，防止溢出）
bool ParseByteCount(const std::string& text, unsigned long long* out) {
    if (text.empty() || text.size() > 20) return false;
    unsigned long long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
    }
    if (out) *out = value;
    return true;
}

// ---- 文件暂存（QQ 式"点击下载"）----
// 发送方先把文件传给服务器，服务器在**内存**里暂存一段时间；其他人在卡片上点「下载」
// 时才把数据发给他（FILE_GET）。过期或超出容量会删掉，所以服务器不会无限膨胀。
struct PendingUpload {          // 正在上传的文件（每个连接最多一个）
    std::string id;             // 客户端自己的传输 ID（只用于日志）
    std::string nameB64;
    unsigned long long declared = 0;
    std::string data;
    std::string thumb;          // 缩略图（PNG，图片缩小图 / 视频第一帧），可能为空
    bool expectThumb = false;   // 发送方声明"会带缩略图"
    // 发送方声明的**附件种类**，原样透传给接收方。
    //   0 / file = 普通文件   1 / sticker = 贴纸   voice = 语音消息
    // 服务端不解释这个值——它只负责透传。三种附件走完全相同的传输，
    // 区别只在客户端收到后怎么显示、以及要不要自动下载。
    std::string kind = "0";
};

struct StoredFile {             // 已经传完、等人下载的文件
    std::string id;             // 服务器分配（F1、F2…）
    std::string owner;
    std::string nameB64;
    unsigned long long size = 0;
    std::string data;
    std::string thumb;          // 缩略图（PNG），可能为空
    std::chrono::steady_clock::time_point expires;
};

std::mutex g_filesMutex;
std::vector<std::shared_ptr<StoredFile>> g_files;
unsigned long long g_fileSeq = 0;

constexpr std::size_t kMaxStoredFiles = 16;                        // 最多同时保留 16 个
// ---- 服务器规则（/chatrule 改，只能控制台改）----
std::mutex g_rulesMutex;
dchat::ServerRules g_rules;

dchat::ServerRules CurrentRules() {
    std::lock_guard<std::mutex> lock(g_rulesMutex);
    return g_rules;
}

// 服务端暂存的总预算：文件 + 聊天记录都算在 maxservertemp 里
unsigned long long TempBudgetBytes() {
    return dchat::RuleMbToBytes(CurrentRules().maxServerTempMb);
}

std::string g_rulesPath = "dchat-rules.txt";  // 规则文件（可用 --rules 指定）

// 从规则文件读规则；文件不存在就用默认值并把默认值写出来
void LoadRules() {
    std::ifstream in(g_rulesPath, std::ios::binary);
    if (!in) {
        Log("no rules file yet, will create: " + g_rulesPath);
        std::ofstream out(g_rulesPath, std::ios::binary | std::ios::trunc);
        if (out) out << dchat::SerializeRules(CurrentRules());
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    dchat::ServerRules loaded;
    {
        std::lock_guard<std::mutex> lock(g_rulesMutex);
        loaded = g_rules;
    }
    const int count = dchat::ParseRules(text, &loaded);
    {
        std::lock_guard<std::mutex> lock(g_rulesMutex);
        g_rules = loaded;
    }
    Log("loaded " + std::to_string(count) + " rule(s) from " + g_rulesPath);
    const dchat::ServerRules now = CurrentRules();
    Log("rules: chatinterval=" + std::to_string(now.chatIntervalMs) + "ms documentsize=" +
        std::to_string(now.documentSizeMb) + "MB keepchathistory=" +
        (now.keepChatHistory ? "true" : "false") + " maxservertemp=" +
        std::to_string(now.maxServerTempMb) + "MB");
}

// 把当前规则写回文件（每次 /chatrule 改成功都会调用）
bool SaveRules() {
    std::ofstream out(g_rulesPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << dchat::SerializeRules(CurrentRules());
    return out.good();
}
constexpr int kFileTtlSeconds = 30 * 60;                           // 30 分钟后过期
constexpr std::size_t kMaxThumbBytes = 128 * 1024;                 // 缩略图上限

unsigned long long StoredBytesLocked() {
    unsigned long long total = 0;
    for (const auto& file : g_files) total += file->size;
    return total;
}

// 清掉过期文件；返回被清掉的描述（用于日志）
std::vector<std::string> ExpireFilesLocked() {
    std::vector<std::string> expired;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_files.begin(); it != g_files.end();) {
        if ((*it)->expires <= now) {
            expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
            it = g_files.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

// 存一个文件；成功返回服务器分配的 ID，失败返回空串
std::string StoreFile(const std::string& owner, const std::string& nameB64,
                      const std::string& data, const std::string& thumb) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    for (const std::string& line : ExpireFilesLocked()) Log("stored file expired: " + line);
    const unsigned long long budget = TempBudgetBytes();
    if (data.size() > budget) return std::string();  // 单个就超总量，存不下
    // 腾地方：先按最旧的删，直到数量和总量都满足
    while (!g_files.empty() && (g_files.size() >= kMaxStoredFiles ||
                                StoredBytesLocked() + data.size() > budget)) {
        Log("stored file evicted (no room): " + g_files.front()->id + "（" +
            g_files.front()->owner + " 上传）");
        g_files.erase(g_files.begin());
    }
    auto file = std::make_shared<StoredFile>();
    file->id = "F" + std::to_string(++g_fileSeq);
    file->owner = owner;
    file->nameB64 = nameB64;
    file->size = data.size();
    file->data = data;
    file->thumb = thumb;
    file->expires = std::chrono::steady_clock::now() + std::chrono::seconds(kFileTtlSeconds);
    g_files.push_back(file);
    return file->id;
}

// 按 ID 取文件（同时做过期清理）；找不到返回 nullptr
// ---- 聊天记录缓存：keepchathistory 打开时，新加入的人能看到之前的记录和文件 ----
std::mutex g_historyMutex;
std::deque<std::string> g_history;  // 已经广播出去的 SAY / ANNOUNCE 行（原样保存）
unsigned long long g_historyBytes = 0;
constexpr std::size_t kMaxHistoryLines = 2000;

unsigned long long StoredBytesTotal() {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    return StoredBytesLocked();
}

void RememberHistory(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    g_history.push_back(line);
    g_historyBytes += line.size() + 1;
    const unsigned long long budget = TempBudgetBytes();
    // 行数和总预算都要守（预算 = 文件 + 聊天记录，由 maxservertemp 决定）
    while (!g_history.empty() && (g_history.size() > kMaxHistoryLines ||
                                  g_historyBytes + StoredBytesTotal() > budget)) {
        g_historyBytes -= g_history.front().size() + 1;
        g_history.pop_front();
    }
}

std::vector<std::string> HistorySnapshot() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return std::vector<std::string>(g_history.begin(), g_history.end());
}

std::size_t HistoryCount() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return g_history.size();
}

// 把预算外的文件和聊天记录裁掉（maxservertemp 调小时立刻生效）
void EnforceTempBudget() {
    const unsigned long long budget = TempBudgetBytes();
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        while (!g_files.empty() && StoredBytesLocked() > budget) {
            Log("stored file evicted (over maxservertemp): " + g_files.front()->id);
            g_files.erase(g_files.begin());
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        while (!g_history.empty() && g_historyBytes + StoredBytesTotal() > budget) {
            g_historyBytes -= g_history.front().size() + 1;
            g_history.pop_front();
        }
    }
}

std::shared_ptr<StoredFile> FindStoredFile(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    for (const std::string& line : ExpireFilesLocked()) Log("stored file expired: " + line);
    for (const auto& file : g_files) {
        if (file->id == id) return file;
    }
    return nullptr;
}

std::mutex g_logMutex;

// 本机所有 IPv4 地址（启动时打印，方便告诉别人用哪个地址连）
std::vector<std::string> LocalAddresses() {
    std::vector<std::string> out;
    char name[256] = {0};
    if (::gethostname(name, sizeof(name)) != 0) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(name, nullptr, &hints, &result) != 0) return out;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        char text[64] = {0};
        const auto* address = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) continue;
        const std::string ip = text;
        if (ip == "127.0.0.1") continue;
        bool seen = false;
        for (const std::string& known : out) {
            if (known == ip) seen = true;
        }
        if (!seen) out.push_back(ip);
    }
    ::freeaddrinfo(result);
    return out;
}

// 打开 TCP keepalive：长时间没数据时由系统探测连接是否还活着
// （走公网 / NAT 时很有用，能及时发现"假连接"）
void EnableKeepAlive(SOCKET sock) {
    BOOL on = TRUE;
    ::setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
    tcp_keepalive settings{};
    settings.onoff = TRUE;
    settings.keepalivetime = 30000;    // 30 秒空闲后开始探测
    settings.keepaliveinterval = 5000; // 探测间隔 5 秒
    DWORD returned = 0;
    ::WSAIoctl(sock, SIO_KEEPALIVE_VALS, &settings, sizeof(settings), nullptr, 0, &returned, nullptr,
               nullptr);
}

void Log(const std::string& text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::printf("[%02d:%02d:%02d] %s\n", now.wHour, now.wMinute, now.wSecond, text.c_str());
    std::fflush(stdout);
}

// 服务器发出的每条用户可见消息都带上时间戳（hh:mm），客户端据此显示 [HH:MM]
std::string Timed(const std::string& command, const std::string& rest = std::string()) {
    const std::string time = dchat::NowTimeString();
    return dchat::BuildLine(command, rest.empty() ? time : time + " " + rest);
}

struct Client {
    SOCKET sock = INVALID_SOCKET;
    std::string nick;     // 空表示还没设置昵称
    std::string ip;       // 裸 IP（不含端口），公网加固按它做单 IP 限流
    std::string address;  // 例如 127.0.0.1:51234
    std::mutex sendMutex;
    // 正在上传的文件（收齐后由服务器暂存，别人点击下载时才发出去）
    std::shared_ptr<PendingUpload> upload;
    std::chrono::steady_clock::time_point lastMessage{};  // chatinterval 用：上一条消息的时间

    // 每个客户端各自的限速器（uploadrate / downloadrate）。0 = 不限制。
    // 用阻塞 Sleep 形成背压，而不是丢包或断连——发送方只是变慢，数据不会损坏。
    dchat::RateLimiter uploadLimiter;
    dchat::RateLimiter downloadLimiter;

    // 传输加密。握手完成后 active() 为真，之后收发都走密文。
    dchat::CryptoSession crypto;

    // 按限速器睡够时间。返回 false 表示 socket 已经不可用，调用方应当中止传输。
    void Throttle(dchat::RateLimiter* limiter, std::size_t bytes, int rateKbps) {
        if (!limiter) return;
        if (limiter->kbps != rateKbps) limiter->Configure(rateKbps);
        const int waitMs = limiter->Consume(bytes);
        if (waitMs > 0) {
            // 单次最多睡 1 秒，避免管理员把限速设得极低时整个线程卡死太久
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(waitMs, 1000)));
        }
    }

    bool SendLine(const std::string& line) {
        if (sock == INVALID_SOCKET) return false;

        // ⚠️ 加密必须在 sendMutex **锁内**做。
        // SendLine 会被广播线程和本连接自己的线程并发调用，而 CryptoSession 的
        // nonce 计数器不是线程安全的——两个线程同时加密就可能用同一个 nonce，
        // 在 GCM 下这是**致命**的（明文可被恢复、认证失效）。
        // 放在锁内之后，加密和发送成为一个原子操作，nonce 顺序也就确定了。
        std::lock_guard<std::mutex> lock(sendMutex);

        std::string data;
        if (crypto.active()) {
            std::string sealed;
            if (!crypto.Encrypt(line, &sealed)) return false;
            data = dchat::BuildLine(
                       "ENC", dchat::Base64Encode(reinterpret_cast<const unsigned char*>(sealed.data()),
                                                  sealed.size())) +
                   "\n";
        } else {
            data = line + "\n";
        }

        std::size_t sent = 0;
        while (sent < data.size()) {
            const int n = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }
};

std::mutex g_clientsMutex;
std::vector<std::shared_ptr<Client>> g_clients;

// 防爆破：按 IP 统计登录失败次数（规则 loginfails）。
// 放在全局是因为它要跨连接生效——攻击者换个连接接着试也得被拦住。
dchat::LoginFailTracker g_loginFails;

// ---------------------------------------------------------------------------
// 服务器身份密钥
//
// **必须持久化**。之前每次握手都现生成一对，结果指纹每次都变——
// 那样 TOFU（客户端记住服务器指纹）根本没有意义，也就完全挡不住主动中间人。
// 存文件里，首次运行生成，之后一直复用。
//
// 用的是"服务器静态 + 客户端临时"的 ECDH（TLS 里也常见）：
// 身份密钥长期不变，每次连接的安全性靠客户端的临时密钥和双方随机数保证。
// ---------------------------------------------------------------------------

dchat::EcdhKeyPair g_identityKey;

const char* const kIdentityKeyPath = "dchat-server-key.txt";

std::string BytesToHex(const unsigned char* data, std::size_t size) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

bool HexToBytes(const std::string& hex, std::vector<unsigned char>* out) {
    if (hex.empty() || hex.size() % 2 != 0) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out->clear();
    out->reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

/** ECCPRIVATE_BLOB 的布局：header(8) + X(32) + Y(32) + d(32)，标量在最后 32 字节。 */
constexpr std::size_t kEccScalarOffset = 8 + 32 + 32;

bool LoadOrCreateIdentityKey() {
    {
        std::ifstream in(kIdentityKeyPath);
        if (in) {
            // ⚠️ 必须**跳过注释行**。这个文件第一版就是被这个坑住了：
            // 文件里写了 '#' 开头的说明，读的时候却用 `in >> a >> b` 当两个裸 token 读，
            // 结果读到的是 "#" 和 "dchat"，判定"内容不对"又生成了一对新密钥——
            // 指纹照样每次都变，而且不报任何错。
            std::string scalarHex, publicHex, line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                std::size_t begin = 0;
                while (begin < line.size() && line[begin] == ' ') ++begin;
                const std::string trimmed = line.substr(begin);
                if (trimmed.empty() || trimmed[0] == '#') continue;
                if (scalarHex.empty()) {
                    scalarHex = trimmed;
                } else {
                    publicHex = trimmed;
                    break;
                }
            }

            if (!scalarHex.empty() && !publicHex.empty()) {
                std::vector<unsigned char> scalar, pub;
                if (HexToBytes(scalarHex, &scalar) && HexToBytes(publicHex, &pub) &&
                    dchat::ImportEcdhKeyPair(scalar, pub, &g_identityKey)) {
                    Log(std::string("身份密钥已加载：") + kIdentityKeyPath);
                    Log("★ 服务器指纹（客户端应当记住它）：" +
                        dchat::PublicKeyFingerprint(g_identityKey.publicKey));
                    return true;
                }
                Log("⚠ 身份密钥文件解析失败，将重新生成（客户端记下的指纹会失效）");
            }
        }
    }

    if (!dchat::GenerateEcdhKeyPair(&g_identityKey)) return false;
    std::vector<unsigned char> scalar;
    if (!dchat::EcdhPrivateScalar(g_identityKey, &scalar)) return false;

    const std::string scalarHex =
        BytesToHex(scalar.data(), scalar.size());
    const std::string publicHex =
        BytesToHex(g_identityKey.publicKey.data(), g_identityKey.publicKey.size());

    std::ofstream out(kIdentityKeyPath);
    if (!out) {
        Log("⚠ 无法写入身份密钥文件：服务器重启后指纹会变，TOFU 会失效");
    } else {
        out << "# dchat 服务器身份密钥。\n"
            << "# 删掉它服务器会生成新的一对，但**所有客户端记下的指纹都会对不上**——\n"
            << "# 除非你确实想换身份，否则别删。\n"
            << "# ⚠ 这个文件不要公开：拿到标量就能冒充这台服务器。\n"
            << scalarHex << "\n"
            << publicHex << "\n";
        Log(std::string("已生成并保存身份密钥：") + kIdentityKeyPath);
    }
    Log("★ 服务器指纹（客户端应当记住它）：" +
        dchat::PublicKeyFingerprint(g_identityKey.publicKey));
    return true;
}

// ---- 黑名单：昵称 -> 解封时间点 ----
struct BanEntry {
    std::string name;
    std::chrono::steady_clock::time_point until;
    bool permanent = false;
};

std::mutex g_banMutex;
std::vector<BanEntry> g_bans;

// ---- 账号（用户名 + 加盐哈希密码）----
std::mutex g_usersMutex;
std::vector<dchat::UserRecord> g_users;
std::string g_usersPath = "dchat-users.txt";

void LoadUsers() {
    std::ifstream in(g_usersPath, std::ios::binary);
    if (!in) {
        Log("no accounts file yet, will create: " + g_usersPath);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::lock_guard<std::mutex> lock(g_usersMutex);
    dchat::ParseUsers(text, &g_users);
    Log("loaded " + std::to_string(g_users.size()) + " account(s) from " + g_usersPath);
}

bool SaveUsers() {
    std::lock_guard<std::mutex> lock(g_usersMutex);
    std::ofstream out(g_usersPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << dchat::SerializeUsers(g_users);
    return out.good();
}

bool UserExists(const std::string& name, dchat::UserRecord* copy) {
    std::lock_guard<std::mutex> lock(g_usersMutex);
    const dchat::UserRecord* found = dchat::FindUser(g_users, name);
    if (!found) return false;
    if (copy) *copy = *found;  // 拷贝出来，避免解锁后指针失效
    return true;
}

// 改密码并写盘。返回 0=成功，1=没有这个账号，2=保存失败
int SetAccountPassword(const std::string& name, const std::string& newPassword) {
    {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        if (!dchat::ChangeUserPassword(&g_users, name, newPassword)) return 1;
    }
    return SaveUsers() ? 0 : 2;
}

// 清掉已到期的封禁（调用者需持有 g_banMutex）
void EraseExpiredBansLocked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_bans.begin(); it != g_bans.end();) {
        if (it->permanent) {  // 永久封禁不会自动到期
            ++it;
            continue;
        }
        if (it->until <= now) {
            Log("ban expired: " + it->name);
            it = g_bans.erase(it);
        } else {
            ++it;
        }
    }
}

// 如果 name 在封禁中，返回 true 并给出剩余秒数
bool IsBanned(const std::string& name, int* remainingSeconds = nullptr,
              bool* isPermanent = nullptr) {
    std::lock_guard<std::mutex> lock(g_banMutex);
    EraseExpiredBansLocked();
    const auto now = std::chrono::steady_clock::now();
    for (const BanEntry& entry : g_bans) {
        if (entry.name != name) continue;
        if (isPermanent) *isPermanent = entry.permanent;
        if (remainingSeconds && !entry.permanent) {
            *remainingSeconds = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(entry.until - now).count());
        }
        return true;
    }
    return false;
}

// 由后台线程每秒调用一次，把"封禁到期"写进日志
void ExpireBans() {
    std::lock_guard<std::mutex> lock(g_banMutex);
    EraseExpiredBansLocked();
}

// 加入黑名单（同一昵称重复封禁会覆盖原来的到期时间）
void AddBan(const std::string& name, int seconds, bool permanent) {
    std::lock_guard<std::mutex> lock(g_banMutex);
    const auto until = permanent ? std::chrono::steady_clock::time_point::max()
                                 : std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (BanEntry& entry : g_bans) {
        if (entry.name == name) {
            entry.until = until;
            entry.permanent = permanent;
            return;
        }
    }
    g_bans.push_back(BanEntry{name, until, permanent});
}

bool RemoveBan(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_banMutex);
    const auto before = g_bans.size();
    g_bans.erase(std::remove_if(g_bans.begin(), g_bans.end(),
                                [&](const BanEntry& entry) { return entry.name == name; }),
                 g_bans.end());
    return g_bans.size() != before;
}

std::string BanList() {
    std::lock_guard<std::mutex> lock(g_banMutex);
    const auto now = std::chrono::steady_clock::now();
    std::string text;
    for (const BanEntry& entry : g_bans) {
        if (!entry.permanent && entry.until <= now) continue;
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(entry.until - now).count());
        if (!text.empty()) text += "  ";
        text += entry.name + "(" +
                (entry.permanent ? std::string("永久") : (std::to_string(left) + "s")) + ")";
    }
    if (text.empty()) text = "(没有正在封禁的昵称)";
    return text;
}

// ---- 管理员（op）名单 ----
std::mutex g_opMutex;
std::vector<std::string> g_ops;

bool IsOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    return std::find(g_ops.begin(), g_ops.end(), name) != g_ops.end();
}

bool AddOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    if (std::find(g_ops.begin(), g_ops.end(), name) != g_ops.end()) return false;
    g_ops.push_back(name);
    return true;
}

bool RemoveOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    const auto before = g_ops.size();
    g_ops.erase(std::remove(g_ops.begin(), g_ops.end(), name), g_ops.end());
    return g_ops.size() != before;
}

std::string OpList() {
    std::lock_guard<std::mutex> lock(g_opMutex);
    if (g_ops.empty()) return "(没有管理员)";
    std::string text;
    for (const std::string& name : g_ops) {
        if (!text.empty()) text += "  ";
        text += name;
    }
    return text;
}

std::vector<std::shared_ptr<Client>> ClientSnapshot() {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    return g_clients;
}

void Broadcast(const std::string& line, const Client* except = nullptr) {
    for (const auto& client : ClientSnapshot()) {
        if (client.get() == except) continue;
        client->SendLine(line);  // 发送失败由该客户端的读线程负责清理
    }
}

std::string NickList() {
    std::string list;
    for (const auto& client : ClientSnapshot()) {
        if (client->nick.empty()) continue;
        if (!list.empty()) list += ", ";
        list += client->nick;
    }
    if (list.empty()) list = "(暂时没人设置昵称)";
    return list;
}

// 所有"服务器知道的名字"：已注册账号 + 管理员 + 黑名单里的名字。
// 发给客户端做 Tab 补全（/ban /op /unban /ip 要能补到不在线的人）。
// 单行有 4096 字节上限，太长就截断（只影响补全提示，不影响功能）。
std::string KnownNameList() {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        for (const dchat::UserRecord& user : g_users) names.push_back(user.name);
    }
    {
        std::lock_guard<std::mutex> lock(g_opMutex);
        for (const std::string& name : g_ops) names.push_back(name);
    }
    {
        std::lock_guard<std::mutex> lock(g_banMutex);
        for (const BanEntry& entry : g_bans) names.push_back(entry.name);
    }
    std::string list;
    std::vector<std::string> unique;
    for (const std::string& name : names) {
        if (name.empty()) continue;
        if (std::find(unique.begin(), unique.end(), name) != unique.end()) continue;  // 去重
        unique.push_back(name);
        const std::string piece = (list.empty() ? "" : ", ") + name;
        if (list.size() + piece.size() > 3500) break;  // 留出命令名和时间的余量
        list += piece;
    }
    return list;
}

// 把名单发给一个客户端（只在登录成功后发，避免未登录的连接看到账号名）
void SendKnownNames(const std::shared_ptr<Client>& client) {
    if (!client || client->nick.empty()) return;
    client->SendLine(Timed("KNOWN", KnownNameList()));
}

void BroadcastKnownNames() {
    for (const auto& client : ClientSnapshot()) {
        if (client->nick.empty()) continue;
        client->SendLine(Timed("KNOWN", KnownNameList()));
    }
}

bool NickTaken(const std::string& nick, const Client* self) {
    for (const auto& client : ClientSnapshot())
        if (client.get() != self && client->nick == nick) return true;
    return false;
}

void RemoveClient(Client* target) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                                   [target](const std::shared_ptr<Client>& c) {
                                       return c.get() == target;
                                   }),
                    g_clients.end());
}

// 按昵称找到在线客户端并踢下线（不封禁，对方可以立刻重新加入）
bool KickByName(const std::string& name) {
    std::shared_ptr<Client> target;
    for (const auto& client : ClientSnapshot()) {
        if (client->nick == name) {
            target = client;
            break;
        }
    }
    if (!target) return false;
    target->SendLine(Timed("ERROR", "你被管理员移出了房间（可以重新加入）"));
    ::shutdown(target->sock, SD_BOTH);  // 读线程的 recv 会立刻返回，连接随之关闭
    return true;
}

// 给在线的某个昵称发一条系统提示（不在线返回 false）
bool NotifyByName(const std::string& name, const std::string& text) {
    std::shared_ptr<Client> target;
    for (const auto& client : ClientSnapshot()) {
        if (client->nick == name) {
            target = client;
            break;
        }
    }
    if (!target) return false;
    target->SendLine(Timed("SYS", text));
    return true;
}

// 查在线客户端的地址（形如 192.168.1.5:51234）；不在线返回空串
std::string AddressOf(const std::string& name) {
    for (const auto& client : ClientSnapshot()) {
        if (client->nick == name) return client->address;
    }
    return std::string();
}

// 执行一条指令。source 为 nullptr 表示来自服务器控制台；否则把结果回给这个人（不广播）
void ExecuteCommand(const dchat::ServerCommand& command, Client* source) {
    auto reply = [&](const std::string& text, bool isError) {
        if (source) source->SendLine(Timed(isError ? "ERROR" : "SYS", text));
    };

    switch (command.kind) {
        case dchat::ServerCommand::Kind::Help: {
            const std::string help = dchat::ServerCommandHelp();
            if (source) {
                // 聊天框里逐行发，客户端会一行行显示
                std::size_t begin = 0;
                while (begin <= help.size()) {
                    const std::size_t end = help.find('\n', begin);
                    const std::string lineText =
                        help.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                    if (!lineText.empty()) source->SendLine(Timed("SYS", lineText));
                    if (end == std::string::npos) break;
                    begin = end + 1;
                }
            } else {
                std::cout << help << std::endl;
            }
            break;
        }
        case dchat::ServerCommand::Kind::Ban: {
            AddBan(command.name, command.seconds, command.permanent);
            const std::string duration =
                dchat::FormatBanDuration(command.seconds, command.permanent);
            Log("banned " + command.name + " for " + (command.permanent ? std::string("forever")
                                                                        : std::to_string(command.seconds) + "s") +
                " (" + duration + ")");
            reply("已封禁 " + command.name + "（" + duration + "）", false);
            Broadcast(Timed("SYS", command.name + " 已被管理员封禁（" + duration + "）"));
            if (KickByName(command.name)) Log("kicked banned user: " + command.name);
            BroadcastKnownNames();  // 黑名单变了，让客户端的 Tab 补全跟上
            break;
        }
        case dchat::ServerCommand::Kind::Kick: {
            const bool kicked = KickByName(command.name);
            Log(kicked ? ("kicked " + command.name) : ("kick ignored, not online: " + command.name));
            reply(kicked ? ("已把 " + command.name + " 移出房间") : (command.name + " 不在线"),
                  !kicked);
            if (kicked) Broadcast(Timed("SYS", command.name + " 被管理员移出房间"));
            break;
        }
        case dchat::ServerCommand::Kind::Unban: {
            const bool removed = RemoveBan(command.name);
            Log(removed ? ("unbanned " + command.name) : ("not in ban list: " + command.name));
            reply(removed ? ("已解除对 " + command.name + " 的封禁")
                          : (command.name + " 不在黑名单里"),
                  !removed);
            if (removed) Broadcast(Timed("SYS", command.name + " 的封禁已解除"));
            if (removed) BroadcastKnownNames();
            break;
        }
        case dchat::ServerCommand::Kind::Op: {
            const bool added = AddOp(command.name);
            Log(added ? ("op: " + command.name) : ("already op: " + command.name));
            if (added) Log("admins: " + OpList());
            reply(added ? (command.name + " 已获得管理员权限")
                        : (command.name + " 已经是管理员"),
                  !added);
            if (added) Broadcast(Timed("SYS", command.name + " 已成为管理员"));
            if (added) BroadcastKnownNames();
            break;
        }
        case dchat::ServerCommand::Kind::Deop: {
            const bool removed = RemoveOp(command.name);
            Log(removed ? ("deop: " + command.name) : ("not an op: " + command.name));
            if (removed) Log("admins: " + OpList());
            reply(removed ? (command.name + " 的管理员权限已取消")
                          : (command.name + " 不是管理员"),
                  !removed);
            if (removed) Broadcast(Timed("SYS", command.name + " 的管理员权限已取消"));
            if (removed) BroadcastKnownNames();
            break;
        }
        case dchat::ServerCommand::Kind::ListBans:
            Log("ban list: " + BanList());
            reply("黑名单：" + BanList(), false);
            break;
        case dchat::ServerCommand::Kind::ListOps:
            Log("op list: " + OpList());
            reply("管理员名单：" + OpList(), false);
            break;
        case dchat::ServerCommand::Kind::Say:
            Log("announce: " + command.text);
            {
                const std::string announce = Timed("ANNOUNCE", command.text);
                Broadcast(announce);  // 全服公告：客户端会大字居中显示
                RememberHistory(announce);  // 公告也进聊天记录缓存
            }
            reply("公告已发出：" + command.text, false);
            break;
        case dchat::ServerCommand::Kind::ChangePassword: {
            // 不带名字 = 改自己的密码（source 就是发起人）；带名字 = 控制台给别的账号改
            const std::string target = command.name.empty() ? (source ? source->nick : std::string())
                                                            : command.name;
            if (target.empty()) {  // 控制台里只给了密码：没有"自己"这个概念，给出用法
                const std::string usage =
                    "用法：/changepassword <昵称> <新密码>（控制台改别人的密码）；"
                    "玩家自己在聊天框里用 /changepassword <新密码>";
                Log(usage);
                break;
            }
            const bool self = source && source->nick == target;
            const int result = SetAccountPassword(target, command.password);
            if (result == 1) {
                Log("password change failed, no such account: " + target);
                reply("没有这个账号：" + target, true);
            } else if (result == 2) {
                Log("failed to save accounts file after password change: " + g_usersPath);
                reply("服务器无法保存账号文件：" + g_usersPath, true);
            } else {
                // 注意：日志里只记"谁改的、改了哪个账号"，绝不写密码
                Log("password changed for account: " + target +
                    (self ? " (by himself)" : " (by admin/console)"));
                reply(self ? "密码已修改，下次登录请用新密码" : ("已修改 " + target + " 的密码"), false);
                if (!self && NotifyByName(target, "你的密码已被管理员修改")) {
                    Log("notified " + target + " about the password change");
                }
            }
            break;
        }
        case dchat::ServerCommand::Kind::ChatRule: {
            // 查看 / 修改服务器规则（只有控制台能到这里，客户端那条路被 consoleOnly 拦掉了）
            if (command.rule.empty()) {
                for (const std::string& row : dchat::AllRuleNames()) {
                    Log("  " + dchat::DescribeRule(CurrentRules(), row));
                }
                Log("  用法：/chatrule <规则> [set|add|remove] <值>（布尔规则用 true/false）");
                break;
            }
            dchat::RuleAction action = dchat::RuleAction::Show;
            long long value = 0;
            bool boolValue = false;
            if (command.ruleAction == "set") action = dchat::RuleAction::Set;
            if (command.ruleAction == "add") action = dchat::RuleAction::Add;
            if (command.ruleAction == "remove") action = dchat::RuleAction::Remove;
            if (command.ruleAction == "setbool") {
                action = dchat::RuleAction::SetBool;
                boolValue = (command.ruleValue == "true");
            }
            if (action != dchat::RuleAction::Show && action != dchat::RuleAction::SetBool) {
                try {
                    value = std::stoll(command.ruleValue);
                } catch (...) {
                    Log("chatrule: 值不是整数：" + command.ruleValue);
                    break;
                }
            }
            dchat::RuleChange change;
            {
                std::lock_guard<std::mutex> lock(g_rulesMutex);
                change = dchat::ApplyRule(&g_rules, command.rule, action, value, boolValue);
            }
            Log("chatrule: " + change.message);
            if (change.ok && change.changed) {
                // 规则改动持久化：写回 dchat-rules.txt，重启后依然生效
                if (!SaveRules()) Log("！规则文件写入失败：" + g_rulesPath);
                Broadcast(RulesLineForClient(CurrentRules()));  // 让客户端更新本地检查
                if (change.rule == "maxservertemp") {
                    EnforceTempBudget();  // 调小了就立刻按新上限裁掉
                    Log("after maxservertemp: files=" + std::to_string(StoredBytesTotal()) +
                        " bytes, history lines=" + std::to_string(HistoryCount()));
                }
            }
            break;
        }
        case dchat::ServerCommand::Kind::ShowIp: {
            // 只在服务器控制台可用：查某个在线客户端的 IP 和端口
            const std::string address = AddressOf(command.name);
            if (address.empty()) {
                Log("ip lookup: " + command.name + " 不在线");
                reply(command.name + " 当前不在线（只有在线时才能查到地址）", true);
            } else {
                Log("ip of " + command.name + ": " + address);
                reply(command.name + " 的地址：" + address, false);
            }
            break;
        }
        case dchat::ServerCommand::Kind::Unknown:
        default:
            Log("unknown command, try /help");
            reply("未知指令，输入 /help 查看用法", true);
            break;
    }
}

// 处理一行服务端控制台指令
void HandleServerCommand(const std::string& line) {
    const dchat::ServerCommand command = dchat::ParseServerCommand(line);
    if (!command.error.empty()) {
        Log(command.error);
        return;
    }
    ExecuteCommand(command, nullptr);  // 控制台执行
}

// 从控制台读指令的线程（控制台输入已设为 UTF-8，中文昵称也能正确匹配）
void CommandLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        HandleServerCommand(line);
    }
}

// 每秒清理一次到期封禁
void BanMaintenanceLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ExpireBans();
        {  // 暂存的文件也会过期，顺手清一下
            std::lock_guard<std::mutex> lock(g_filesMutex);
            for (const std::string& line : ExpireFilesLocked()) {
                Log("stored file expired: " + line);
            }
        }
        // 登录失败记录也会过期，顺手清掉，免得长期运行时那张表无限增长
        g_loginFails.Sweep();
    }
}

// 认证通过后加入房间：登记昵称、打招呼、广播加入
// 新加入的人：把之前的聊天记录和还留着的文件卡片回放给他（keepchathistory 打开时）
void ReplayHistoryTo(const std::shared_ptr<Client>& client) {
    if (!CurrentRules().keepChatHistory) return;
    const std::vector<std::string> lines = HistorySnapshot();
    std::vector<std::string> offers;
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        for (const auto& file : g_files) {
            offers.push_back(Timed("FILE_OFFER", file->owner + " " + file->id + " " + file->nameB64 +
                                                     " " + std::to_string(file->size) + " " +
                                                     (file->thumb.empty() ? "0" : "1")));
        }
    }
    if (lines.empty() && offers.empty()) return;
    client->SendLine(Timed("SYS", "—— 以下是加入之前的聊天记录（keepchathistory 已打开）——"));
    for (const std::string& line : lines) client->SendLine(line);
    if (!offers.empty()) {
        client->SendLine(Timed("SYS", "房间里还留着这些文件，点卡片可以下载："));
        for (const std::string& offer : offers) client->SendLine(offer);
    }
}

bool CompleteLogin(const std::shared_ptr<Client>& client, const std::string& nick) {
    int remainingSeconds = 0;
    bool permanent = false;
    if (IsBanned(nick, &remainingSeconds, &permanent)) {  // 黑名单用户直接拒绝并断开
        const std::string duration = dchat::FormatBanDuration(remainingSeconds, permanent);
        client->SendLine(Timed("ERROR", permanent ? "你已被管理员永久封禁"
                                                  : ("你已被管理员封禁，剩余 " + duration)));
        Log("rejected banned user: " + nick);
        return false;
    }
    if (NickTaken(nick, client.get())) {
        client->SendLine(Timed("ERROR", dchat::NickErrorText(dchat::NickError::Taken)));
        return false;
    }
    client->nick = nick;
    // 明确通知客户端"认证已通过"：客户端据此结束"等待登录结果"的状态，
    // 不会再被 WELCOME 之类的普通提示干扰（否则密码错误时客户端不会报错、也不会断开）
    client->SendLine(Timed("LOGGEDIN", nick));
    Broadcast(Timed("JOINED", nick), client.get());
    client->SendLine(Timed("SYS", "你好，" + nick + "！直接输入内容按回车即可发言。"));
    client->SendLine(Timed("NAMES", NickList()));
    SendKnownNames(client);  // 已注册账号名单：Tab 补 /ban /op /unban /ip 用
    client->SendLine(RulesLineForClient(CurrentRules()));  // 把当前规则告诉客户端
    ReplayHistoryTo(client);                                // keepchathistory 打开时补历史
    Log(client->address + " joined as " + nick);
    return true;
}

// 返回 false 表示客户端要求断开
bool HandleLine(const std::shared_ptr<Client>& client, const std::string& line) {
    const dchat::Message msg = dchat::ParseLine(line);
    if (msg.command.empty()) return true;

    // ---- 登录 / 注册 ----
    // ---- 传输加密握手 ----
    //
    // HELLO 必须在**没有加密**的时候发（这是协商本身）。流程：
    //     C -> S  HELLO <版本> <客户端公钥B64> <客户端随机数B64>
    //     S -> C  HELLO_OK <服务器公钥B64> <服务器随机数B64>     ← 明文
    //     ---- 双方各自算出两把会话密钥，此后每一行都包成 ENC <base64> ----
    //
    // 老客户端永远不发 HELLO，服务器就一直用明文——**不会因为它们不支持加密就连不上**。
    if (msg.command == "HELLO") {
        if (client->crypto.active()) {
            client->SendLine(Timed("ERROR", "这条连接已经启用加密了"));
            return true;
        }
        const std::vector<std::string> words = msg.Words();
        if (words.size() < 3) {
            client->SendLine(Timed("ERROR", "用法：HELLO <版本> <公钥> <随机数>"));
            return true;
        }
        if (std::atoi(words[0].c_str()) != dchat::kCryptoVersion) {
            client->SendLine(Timed("ERROR", "不支持的加密版本：" + words[0]));
            return true;
        }

        std::vector<unsigned char> peerPublic;
        std::vector<unsigned char> peerNonce;
        if (!dchat::Base64Decode(words[1], &peerPublic) ||
            peerPublic.size() != dchat::kP256PublicKeyBytes) {
            client->SendLine(Timed("ERROR", "客户端公钥格式不对"));
            return true;
        }
        if (!dchat::Base64Decode(words[2], &peerNonce) ||
            peerNonce.size() != dchat::kHandshakeNonceBytes) {
            client->SendLine(Timed("ERROR", "客户端随机数格式不对"));
            return true;
        }

        // 用**持久化的身份密钥**，不是每次现生成——
        // 后者会让指纹每次都变，客户端的 TOFU 就形同虚设。
        if (g_identityKey.publicKey.empty()) {
            client->SendLine(Timed("ERROR", "服务器身份密钥不可用"));
            return true;
        }
        const dchat::EcdhKeyPair& serverKey = g_identityKey;
        std::vector<unsigned char> shared;
        if (!dchat::ComputeSharedSecret(serverKey, peerPublic, &shared)) {
            // 公钥不是曲线上的合法点（可能有人在瞎试），拒绝但不断开
            client->SendLine(Timed("ERROR", "客户端公钥不是合法的 P-256 点"));
            Log("crypto handshake rejected (bad public key): " + client->address);
            return true;
        }

        std::vector<unsigned char> serverNonce;
        if (!dchat::RandomBytes(dchat::kHandshakeNonceBytes, &serverNonce)) {
            client->SendLine(Timed("ERROR", "服务器随机数生成失败"));
            return true;
        }
        const dchat::SessionKeys keys =
            dchat::DeriveSessionKeys(shared, peerNonce, serverNonce);
        if (!keys.valid()) {
            client->SendLine(Timed("ERROR", "会话密钥派生失败"));
            return true;
        }

        // HELLO_OK 必须**明文**发出去——此时加密还没启用
        const std::string body =
            dchat::Base64Encode(serverKey.publicKey.data(), serverKey.publicKey.size()) + " " +
            dchat::Base64Encode(serverNonce.data(), serverNonce.size());
        if (!client->SendLine(dchat::BuildLine("HELLO_OK", body))) return false;

        // 服务端发用 s2c、收用 c2s（客户端正好相反）
        if (!client->crypto.Start(keys.serverToClient, keys.clientToServer)) {
            client->SendLine(Timed("ERROR", "会话启动失败"));
            return true;
        }
        Log("encrypted channel established with " + client->address +
            "（指纹 " + dchat::PublicKeyFingerprint(serverKey.publicKey) + "）");
        return true;
    }

    if (msg.command == "LOGIN" || msg.command == "REGISTER") {
        const std::vector<std::string> words = msg.Words();
        if (!client->nick.empty()) {  // 已经登录过：不重复加入、不重复广播
            client->SendLine(Timed("ERROR", "你已经登录为 " + client->nick + "，无需再次登录"));
            return true;
        }
        // 注意：Words() 只切分命令之后的文本，所以这里是 用户名 + 密码 两个词
        if (words.size() != 2) {
            client->SendLine(Timed("ERROR", "用法：" + msg.command + " <用户名> <密码>"));
            return true;
        }
        dchat::NickError error = dchat::NickError::None;
        const std::string name = dchat::NormalizeNick(words[0], &error);
        if (name.empty()) {
            client->SendLine(Timed("ERROR", dchat::NickErrorText(error)));
            return true;
        }
        dchat::PasswordError passwordError = dchat::PasswordError::None;
        if (!dchat::ValidatePassword(words[1], &passwordError)) {
            client->SendLine(Timed("ERROR", dchat::PasswordErrorText(passwordError)));
            return true;
        }

        // ---- 防爆破（规则 loginfails）----
        // 放在密码比对**之前**：被锁的 IP 连"用户名存不存在"都问不出来，
        // 否则攻击者仍能靠错误信息的差别枚举账号。
        if (msg.command == "LOGIN") {
            const int failLimit = CurrentRules().loginFailLimit;
            if (g_loginFails.IsBlocked(client->ip, failLimit)) {
                client->SendLine(Timed("ERROR", "登录失败次数过多，请过几分钟再试"));
                Log("login blocked (too many failures from " + client->ip + ")");
                return true;
            }
        }

        if (msg.command == "REGISTER") {
            if (UserExists(name, nullptr)) {
                client->SendLine(Timed("ERROR", "用户名已存在，请到「登录」界面直接登录"));
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(g_usersMutex);
                g_users.push_back(dchat::MakeUser(name, words[1]));
            }
            if (!SaveUsers()) {
                client->SendLine(Timed("ERROR", "服务器无法保存账号文件：" + g_usersPath));
                Log("failed to save accounts file: " + g_usersPath);
                return true;
            }
            Log("registered account: " + name);
            client->SendLine(Timed("SYS", "注册成功，账号已保存"));
            BroadcastKnownNames();  // 新账号也进别人的 Tab 补全名单
        } else {
            dchat::UserRecord user;
            if (!UserExists(name, &user)) {
                client->SendLine(Timed("ERROR", "用户名不存在，请先注册（点「没有账号？注册新账号」）"));
                Log("login failed (no such user): " + name);
                g_loginFails.NoteFailure(client->ip);  // 探测账号也算一次失败
                return true;
            }
            if (!dchat::CheckPassword(user, words[1])) {
                client->SendLine(Timed("ERROR", "密码错误"));
                Log("login failed (wrong password): " + name);
                g_loginFails.NoteFailure(client->ip);
                return true;
            }
            Log("login ok: " + name);
            g_loginFails.Clear(client->ip);  // 登录成功立刻清零，免得本人打错几次把自己锁了
        }
        return CompleteLogin(client, name);
    }

    if (msg.command == "NICK") {  // 现在必须登录，昵称就是用户名
        client->SendLine(Timed("ERROR", "请先登录：LOGIN <用户名> <密码>，首次使用用 REGISTER <用户名> <密码>"));
        return true;
    }

    if (msg.command == "MSG") {
        if (client->nick.empty()) {
            client->SendLine(Timed("ERROR", "请先登录后再发言"));
            return true;
        }
        if (msg.rest.empty()) {
            client->SendLine(Timed("ERROR", "消息内容不能为空"));
            return true;
        }
        // 只要以 '/' 开头就当成命令尝试（和 Minecraft 一样）：打错了也只回给本人，不广播
        if (dchat::IsCommandAttempt(msg.rest)) {
            const dchat::ServerCommand command = dchat::ParseServerCommand(msg.rest);
            if (!command.knownCommand) {  // 指令名打错/不认识：只有发送者看到提示
                client->SendLine(Timed("ERROR", command.error));
                Log("unknown command from " + client->nick + ": " + msg.rest);
                return true;
            }
            if (command.consoleOnly) {  // 例如给别的账号改密码：聊天框里一律不行，管理员也不行
                client->SendLine(Timed("ERROR",
                                       "这条指令只能在服务器控制台使用（/" + command.keyword +
                                           " 的这个用法）"));
                Log("console-only command from " + client->nick + " ignored: /" + command.keyword);
                return true;
            }
            if (command.opRequired && !IsOp(client->nick)) {  // 认识这条指令但没权限
                client->SendLine(Timed("ERROR", "你没有管理员权限（指令只有管理员能用）"));
                Log("command denied for non-op: " + client->nick + " -> " + msg.rest);
                return true;
            }
            if (command.opRequired) {
                Log("op command from " + client->nick + ": " + msg.rest);
            } else {
                // 自助指令的参数可能是密码，日志里只记录指令名
                Log("command from " + client->nick + ": /" + command.keyword);
            }
            if (!command.error.empty()) {
                client->SendLine(Timed("ERROR", command.error));
                return true;
            }
            ExecuteCommand(command, client.get());
            return true;
        }
        // ---- 文本长度 / 行数限制（2026-10 新增，公网接入用）----
        // 消息里的换行是**转义**过的（\n 两个字符），所以这里要用协议层的函数来判断，
        // 不能直接数字符——转义后 "\n" 是 2 个字符但只代表 1 个换行。
        const dchat::ServerRules& rules = CurrentRules();
        if (rules.maxTextLength > 0 &&
            dchat::Utf8CharCount(dchat::UnescapeText(msg.rest)) >
                static_cast<std::size_t>(rules.maxTextLength)) {
            client->SendLine(Timed("ERROR", "消息太长了：当前规则 maxtextlen = " +
                                                std::to_string(rules.maxTextLength) +
                                                " 字符，你这条有 " +
                                                std::to_string(dchat::Utf8CharCount(
                                                    dchat::UnescapeText(msg.rest))) +
                                                " 字符"));
            return true;
        }
        if (rules.maxTextLines > 0 &&
            dchat::CountTextLines(msg.rest) > static_cast<std::size_t>(rules.maxTextLines)) {
            client->SendLine(Timed("ERROR", "消息行数太多：当前规则 maxtextlines = " +
                                                std::to_string(rules.maxTextLines) + " 行，你这条有 " +
                                                std::to_string(dchat::CountTextLines(msg.rest)) +
                                                " 行"));
            return true;
        }
        // chatinterval：两条消息之间的最小间隔（0 = 不限制）
        const int interval = CurrentRules().chatIntervalMs;
        const auto now = std::chrono::steady_clock::now();
        if (interval > 0 && client->lastMessage.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - client->lastMessage).count() <
                interval) {
            client->SendLine(Timed("ERROR", "发言太快了：当前规则 chatinterval = " +
                                                std::to_string(interval) + " ms"));
            return true;
        }
        client->lastMessage = now;
        const std::string sayLine = Timed("SAY", client->nick + " " + msg.rest);
        Broadcast(sayLine);  // 连发送者一起发，便于回显
        RememberHistory(sayLine);
        Log("MSG " + client->nick + ": " + msg.rest);
        return true;
    }

    // ---- 文件：上传到服务器暂存 / 点击时下载 ----
    // 发送方：FILE_SEND <本地ID> <文件名(Base64)> <字节数>、FILE_CHUNK <本地ID> <Base64>、
    //         FILE_END <本地ID>、FILE_CANCEL <本地ID>
    // 收文件：FILE_GET <服务器文件ID>
    // 服务器主动发：FILE_OFFER（有人上传好了，带服务器文件ID）、FILE_BEGIN/FILE_DATA/FILE_END
    //              （下载数据流）、FILE_FAIL（下载失败）
    if (msg.command == "FILE_SEND" || msg.command == "FILE_CHUNK" ||
        msg.command == "FILE_END" || msg.command == "FILE_CANCEL" || msg.command == "FILE_GET" ||
        msg.command == "FILE_THUMB" || msg.command == "FILE_THUMB_GET") {
        if (client->nick.empty()) {
            client->SendLine(Timed("ERROR", "请先登录后再传文件"));
            return true;
        }
        const std::vector<std::string> words = msg.Words();

        if (msg.command == "FILE_SEND") {
            if (words.size() != 3 && words.size() != 4) {
                client->SendLine(Timed("ERROR",
                                       "用法：FILE_SEND <传输ID> <文件名> <字节数> [有缩略图]"));
                return true;
            }
            if (!IsValidTransferId(words[0])) {
                client->SendLine(Timed("ERROR", "传输 ID 只能包含字母、数字、下划线和连字符（最多 64 位）"));
                return true;
            }
            if (words[1].size() > 1024) {
                client->SendLine(Timed("ERROR", "文件名太长"));
                return true;
            }
            unsigned long long bytes = 0;
            if (!ParseByteCount(words[2], &bytes) || bytes == 0) {
                client->SendLine(Timed("ERROR", "文件大小无法识别：" + words[2]));
                return true;
            }
            const unsigned long long sizeLimit = dchat::RuleMbToBytes(CurrentRules().documentSizeMb);
            if (bytes > sizeLimit) {  // documentsize 规则决定单文件上限
                client->SendLine(Timed("ERROR", "文件太大了：当前规则 documentsize = " +
                                                  std::to_string(CurrentRules().documentSizeMb) +
                                                  " MB（" + dchat::FormatBytes(sizeLimit) + "）"));
                return true;
            }
            if (client->upload) {
                client->SendLine(Timed("ERROR", "上一个文件还没传完，等它传完再发"));
                return true;
            }
            std::vector<unsigned char> nameBytes;
            const std::string showName = dchat::Base64Decode(words[1], &nameBytes)
                                             ? dchat::SanitizeFileName(std::string(
                                                   nameBytes.begin(), nameBytes.end()))
                                             : std::string("(文件名无法解析)");
            client->upload = std::make_shared<PendingUpload>();
            client->upload->id = words[0];
            client->upload->nameB64 = words[1];
            client->upload->declared = bytes;
            // 可选的第四个字：附件种类。**只追加在末尾**，老客户端不发它就当普通文件。
            // 服务端**不校验**这个值合不合理（比如"既贴纸又语音"），
            // 因为取值是单值的、结构上就不可能出现组合；真正要在意的是
            // 客户端收到不认识的取值时按普通文件处理。
            if (words.size() >= 4 && !words[3].empty()) {
                client->upload->kind = words[3];
            }
            client->upload->thumb.clear();
            // 第 4 个字段是 1 时表示"接着会发一张缩略图"（图片缩小图 / 视频第一帧）
            client->upload->expectThumb = (words.size() == 4 && words[3] == "1");
            Log("upload started: " + client->nick + " " + showName + "（" +
                dchat::FormatBytes(bytes) + "）");
            return true;
        }

        if (msg.command == "FILE_THUMB") {  // 缩略图数据（可能分几行）
            if (words.size() != 2) {
                client->SendLine(Timed("ERROR", "用法：FILE_THUMB <传输ID> <数据>"));
                return true;
            }
            if (!client->upload) {
                client->SendLine(Timed("ERROR", "没有正在进行的上传"));
                return true;
            }
            std::vector<unsigned char> chunk;
            if (!dchat::Base64Decode(words[1], &chunk)) {
                client->SendLine(Timed("ERROR", "缩略图数据损坏，已忽略"));
                return true;
            }
            if (client->upload->thumb.size() + chunk.size() > kMaxThumbBytes) {
                client->SendLine(Timed("ERROR", "缩略图太大，已忽略"));
                client->upload->thumb.clear();
                client->upload->expectThumb = false;
                return true;
            }
            client->upload->thumb.append(chunk.begin(), chunk.end());
            return true;
        }

        if (msg.command == "FILE_CHUNK") {
            if (words.size() != 2) {
                client->SendLine(Timed("ERROR", "用法：FILE_CHUNK <传输ID> <数据>"));
                return true;
            }
            if (!client->upload) {
                client->SendLine(Timed("ERROR", "没有正在进行的上传（先发 FILE_SEND）"));
                return true;
            }
            std::vector<unsigned char> chunk;
            if (!dchat::Base64Decode(words[1], &chunk)) {
                client->SendLine(Timed("ERROR", "文件数据损坏，上传已中止"));
                Log("upload aborted (bad base64): " + client->nick);
                client->upload.reset();
                return true;
            }
            if (client->upload->data.size() + chunk.size() > client->upload->declared) {
                client->SendLine(Timed("ERROR", "收到的数据超过了声明的大小，上传已中止"));
                Log("upload aborted (too much data): " + client->nick);
                client->upload.reset();
                return true;
            }
            client->upload->data.append(chunk.begin(), chunk.end());
            // uploadrate：按限速睡够再收下一块。客户端会自然被 TCP 反压拖慢，
            // 不需要额外通知——这比"超速就断开"友好得多。
            client->Throttle(&client->uploadLimiter, chunk.size(), CurrentRules().uploadRateKbps);
            return true;
        }

        if (msg.command == "FILE_END") {
            if (words.size() != 1) {
                client->SendLine(Timed("ERROR", "用法：FILE_END <传输ID>"));
                return true;
            }
            if (!client->upload) {
                client->SendLine(Timed("ERROR", "没有正在进行的上传"));
                return true;
            }
            const std::shared_ptr<PendingUpload> upload = client->upload;
            client->upload.reset();
            if (upload->data.size() != upload->declared) {
                client->SendLine(Timed("ERROR", "文件不完整，上传取消（收到 " +
                                                  dchat::FormatBytes(upload->data.size()) +
                                                  "，声明 " +
                                                  dchat::FormatBytes(upload->declared) + "）"));
                Log("upload aborted (incomplete): " + client->nick);
                return true;
            }
            const std::string fileId = StoreFile(client->nick, upload->nameB64, upload->data,
                                                 upload->thumb);
            if (fileId.empty()) {
                client->SendLine(Timed("ERROR", "服务器暂时存不下这个文件，稍后再试"));
                return true;
            }
            std::vector<unsigned char> nameBytes;
            const std::string showName = dchat::Base64Decode(upload->nameB64, &nameBytes)
                                             ? dchat::SanitizeFileName(std::string(
                                                   nameBytes.begin(), nameBytes.end()))
                                             : std::string("(文件名无法解析)");
            std::size_t others = 0;
            for (const auto& other : ClientSnapshot()) {
                if (other.get() != client.get() && !other->nick.empty()) ++others;
            }
            client->SendLine(Timed("SYS", others ? ("文件已上传，房间里的 " +
                                                    std::to_string(others) +
                                                    " 位成员点一下卡片就能下载")
                                                 : "文件已上传，但房间里目前没有其他人"));
            // 通知别人"有文件可以下载了"（上传者自己不用再下自己发的文件）
            //
            // 种类标记是**追加在末尾的第 7 位**，绝不能插到中间：老客户端按位置
            // 读到第 5 个字段就停了，插进去会让它们把标记当成字节数解析。
            // 这和 RULES 行当初的扩法一样——只追加、不重排。
            Broadcast(Timed("FILE_OFFER", client->nick + " " + fileId + " " + upload->nameB64 + " " +
                                             std::to_string(upload->data.size()) + " " +
                                             (upload->thumb.empty() ? "0" : "1") + " " +
                                             upload->kind),
                      client.get());
            Log("file stored: id=" + fileId + " owner=" + client->nick + " " + showName + "（" +
                dchat::FormatBytes(upload->data.size()) + "）");
            return true;
        }

        if (msg.command == "FILE_CANCEL") {
            if (client->upload) {
                Log("upload cancelled: " + client->nick);
                client->upload.reset();
            }
            return true;
        }

        // FILE_GET <服务器文件ID>：把暂存的文件发给请求者
        if (msg.command == "FILE_THUMB_GET") {  // 只要缩略图（视频预览用）
            if (words.size() != 1 || !IsValidTransferId(words[0])) {
                client->SendLine(Timed("ERROR", "用法：FILE_THUMB_GET <文件ID>"));
                return true;
            }
            const std::shared_ptr<StoredFile> thumbFile = FindStoredFile(words[0]);
            if (!thumbFile || thumbFile->thumb.empty()) {
                client->SendLine(dchat::BuildLine("FILE_FAIL",
                                                 words[0] + " 这个文件没有缩略图"));
                return true;
            }
            Log("download thumb: " + client->nick + " " + thumbFile->id + "（" +
                dchat::FormatBytes(thumbFile->thumb.size()) + "）");
            bool thumbOk = true;
            for (std::size_t offset = 0; thumbOk && offset < thumbFile->thumb.size();
                 offset += dchat::kFileChunkBytes) {
                const std::size_t remain = thumbFile->thumb.size() - offset;
                const std::size_t length =
                    remain < dchat::kFileChunkBytes ? remain : dchat::kFileChunkBytes;
                const std::string piece = dchat::Base64Encode(
                    reinterpret_cast<const unsigned char*>(thumbFile->thumb.data() + offset),
                    length);
                thumbOk = client->SendLine(dchat::BuildLine("FILE_THUMB_DATA",
                                                           thumbFile->id + " " + piece));
            }
            if (thumbOk) client->SendLine(dchat::BuildLine("FILE_THUMB_END", thumbFile->id));
            return true;
        }

        if (words.size() != 1 || !IsValidTransferId(words[0])) {
            client->SendLine(Timed("ERROR", "用法：FILE_GET <文件ID>"));
            return true;
        }
        const std::shared_ptr<StoredFile> file = FindStoredFile(words[0]);
        if (!file) {
            // 控制消息不带时间戳，和其他文件消息一致，客户端解析更简单
            client->SendLine(dchat::BuildLine("FILE_FAIL", words[0] + " 文件不存在或已经过期"));
            Log("download miss: " + client->nick + " -> " + words[0]);
            return true;
        }
        Log("download: " + client->nick + " 下载 " + file->id + "（由 " + file->owner +
            " 上传，" + dchat::FormatBytes(file->size) + "）");
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size)));
        for (unsigned long long offset = 0; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {
            const unsigned long long remain = file->size - offset;
            const std::size_t length = remain < dchat::kFileChunkBytes
                                           ? static_cast<std::size_t>(remain)
                                           : dchat::kFileChunkBytes;
            const std::string chunk = dchat::Base64Encode(
                reinterpret_cast<const unsigned char*>(file->data.data() + offset), length);
            // downloadrate：按限速睡够再发下一块（阻塞形成背压，不丢数据）
            client->Throttle(&client->downloadLimiter, length, CurrentRules().downloadRateKbps);
            ok = client->SendLine(dchat::BuildLine("FILE_DATA", file->id + " " + chunk));
        }
        if (ok) client->SendLine(dchat::BuildLine("FILE_END", file->id));
        return true;
    }

    if (msg.command == "LIST") {
        client->SendLine(Timed("NAMES", NickList()));
        return true;
    }
    if (msg.command == "PING") {
        client->SendLine(Timed("PONG"));
        return true;
    }
    if (msg.command == "QUIT") {
        return false;
    }

    client->SendLine(Timed("ERROR", "未知命令: " + msg.command));
    return true;
}

void ClientLoop(std::shared_ptr<Client> client) {
    client->SendLine(Timed("WELCOME", kServerName));

    // ---- 握手超时（规则 handshaketimeout）----
    // 连上以后迟迟不登录的连接最讨厌：白占一个线程和一份内存，开几千个就能把服务器拖垮。
    // 用 SO_RCVTIMEO 让 recv 超时返回；**登录成功后立刻撤掉这个限制**，
    // 否则正常用户挂机不说话也会被踢。
    bool handshakeTimeoutActive = false;
    const int handshakeSec = CurrentRules().handshakeTimeoutSec;
    if (handshakeSec > 0) {
        DWORD timeoutMs = static_cast<DWORD>(handshakeSec) * 1000;
        if (::setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == 0) {
            handshakeTimeoutActive = true;
        }
    }

    dchat::LineBuffer buffer;
    std::vector<char> chunk(2048);
    bool keepGoing = true;
    while (keepGoing) {
        const int received = ::recv(client->sock, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received == SOCKET_ERROR) {
            if (::WSAGetLastError() == WSAETIMEDOUT) {
                if (client->nick.empty()) {
                    client->SendLine(Timed("ERROR", "太久没有登录，连接已关闭"));
                    Log("handshake timeout: " + client->address);
                    break;
                }
                continue;  // 已经登录了：这次只是没数据，继续等
            }
            break;
        }
        if (received == 0) break;  // 对端正常关闭

        buffer.Append(chunk.data(), static_cast<std::size_t>(received));
        if (buffer.bad()) {
            client->SendLine(Timed("ERROR", "单行数据过长，连接已关闭"));
            break;
        }
        std::string line;
        while (buffer.PopLine(&line)) {
            // 加密启用后，收到的每一行都必须是 `ENC <base64>`，先解密再交给 HandleLine。
            std::string effective = line;
            if (client->crypto.active()) {
                const dchat::Message outer = dchat::ParseLine(line);
                if (outer.command != "ENC") {
                    // ⚠️ 这里必须**直接断开**，不能"宽容地当明文处理"：
                    // 否则攻击者只要在加密通道里发明文指令就能绕过加密
                    // （这叫降级攻击，是加密协议最经典的坑之一）。
                    client->SendLine(Timed("ERROR", "加密已启用，拒绝明文指令"));
                    Log("plaintext after handshake, disconnecting: " + client->address + " -> " +
                        outer.command);
                    keepGoing = false;
                    break;
                }
                std::vector<unsigned char> sealed;
                if (!dchat::Base64Decode(outer.rest, &sealed)) {
                    client->SendLine(Timed("ERROR", "ENC 行不是合法的 Base64"));
                    keepGoing = false;
                    break;
                }
                const std::string sealedText(sealed.begin(), sealed.end());
                if (!client->crypto.Decrypt(sealedText, &effective)) {
                    // 认证失败 = 被篡改 / 密钥不对 / 计数器错乱。绝不能继续用这条连接。
                    Log("decrypt failed, disconnecting: " + client->address);
                    keepGoing = false;
                    break;
                }
            }
            if (!HandleLine(client, effective)) {
                keepGoing = false;
                break;
            }
        }

        // 登录成功就撤掉握手超时（设回 0 = 永不超时，恢复原来的行为）
        if (handshakeTimeoutActive && !client->nick.empty()) {
            DWORD zero = 0;
            ::setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&zero), sizeof(zero));
            handshakeTimeoutActive = false;
        }
    }

    RemoveClient(client.get());
    if (!client->nick.empty()) {
        Broadcast(Timed("LEFT", client->nick));
        Log(client->address + " left (" + client->nick + ")");
    } else {
        Log(client->address + " disconnected");
    }
    ::shutdown(client->sock, SD_BOTH);
    ::closesocket(client->sock);
    client->sock = INVALID_SOCKET;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);  // 控制台输入也按 UTF-8，中文昵称才能匹配

    int port = dchat::kDefaultPort;
    std::string bindAddress;  // 空 = 监听所有网卡
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) port = std::atoi(argv[++i]);
        if ((arg == "--users" || arg == "-u") && i + 1 < argc) g_usersPath = argv[++i];
        if ((arg == "--bind" || arg == "-b") && i + 1 < argc) bindAddress = argv[++i];
        if ((arg == "--rules" || arg == "-r") && i + 1 < argc) g_rulesPath = argv[++i];
    }
    if (port <= 0 || port > 65535) {
        std::printf("invalid port: %d\n", port);
        return 1;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 1;
    }

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (bindAddress.empty()) {
        address.sin_addr.s_addr = htonl(INADDR_ANY);  // 默认：所有网卡都监听
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
        std::printf("--bind 只支持 IPv4 地址，例如 --bind 0.0.0.0 或 --bind 192.168.31.251\n");
        ::closesocket(listener);
        WSACleanup();
        return 1;
    }
    address.sin_port = htons(static_cast<u_short>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::printf("bind() failed on port %d: %d\n", port, WSAGetLastError());
        ::closesocket(listener);
        WSACleanup();
        return 1;
    }
    if (::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        std::printf("listen() failed: %d\n", WSAGetLastError());
        ::closesocket(listener);
        WSACleanup();
        return 1;
    }

    Log("dchat server listening on port " + std::to_string(port) + " (Ctrl+C to stop)");
    LoadUsers();
    Log("accounts file: " + g_usersPath + "（密码以加盐哈希保存，不存明文）");
    LoadRules();
    Log("rules file: " + g_rulesPath + "（/chatrule 改完会自动写回）");
    if (!LoadOrCreateIdentityKey()) {
        Log("⚠ 身份密钥初始化失败：传输加密将不可用（客户端会退回明文）");
    }
    Log("管理员指令：/ban <昵称> <时长>  /kick <昵称>  /unban <昵称>  /bans  /op <昵称>  /say <公告>");
    Log("查在线地址：/ip <昵称>（仅控制台）    改密码：/changepassword 或简写 /cp");
    Log("服务器规则：/chatrule（仅控制台）—— 聊天类：chatinterval / documentsize / keepchathistory / maxservertemp");
    Log("                                        限速与文本：uploadrate / downloadrate / maxtextlen / maxtextlines");
    Log("                                        公网加固：maxconns / maxconnsperip / loginfails / handshaketimeout");
    Log("给别的账号改密码：/changepassword <昵称> <新密码>（聊天框里玩家只能改自己的）");
    Log("完整帮助：/help");
    // 启动时把本机地址列出来，方便告诉别人用哪个地址连
    for (const std::string& ip : LocalAddresses()) {
        Log("本机地址（局域网里可用）：" + ip + ":" + std::to_string(port));
    }
    Log("公网使用：需要在路由器上把这个端口映射到本机，并在防火墙放行；"
        "如果运营商是 CGNAT / 没有公网 IP，直接用内网穿透工具（Tailscale、ZeroTier 等）更省事");

    std::thread(CommandLoop).detach();     // 从控制台读指令
    std::thread(BanMaintenanceLoop).detach();  // 每秒清理到期封禁

    for (;;) {
        sockaddr_in peer{};
        int peerLength = sizeof(peer);
        const SOCKET sock = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (sock == INVALID_SOCKET) break;

        // 关掉 Nagle：聊天都是小消息，等合并会让人感觉卡顿
        BOOL noDelay = TRUE;
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                     sizeof(noDelay));
        EnableKeepAlive(sock);  // 公网 / NAT 环境下尽早发现断掉的连接

        char host[64] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
        const std::string ip(host);

        // ---- 公网加固：连接数限制（规则 maxconns / maxconnsperip）----
        // 必须在 accept 之后判断——accept 之前看不到对端地址。
        // 超限时**先告诉对方原因再关**，否则客户端只会看到"莫名其妙连不上"，
        // 既难排查也容易被当成服务器坏了。
        {
            const dchat::ServerRules limits = CurrentRules();
            std::string reason;

            if (limits.maxConnections > 0) {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                if (static_cast<int>(g_clients.size()) >= limits.maxConnections) {
                    reason = "服务器连接数已满（上限 " +
                             std::to_string(limits.maxConnections) + "），请稍后再试";
                }
            }
            if (reason.empty() && limits.maxConnectionsPerIp > 0) {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                int sameIp = 0;
                for (const std::shared_ptr<Client>& existing : g_clients) {
                    if (existing->ip == ip) ++sameIp;
                }
                if (sameIp >= limits.maxConnectionsPerIp) {
                    reason = "同一地址的连接数已达上限（" +
                             std::to_string(limits.maxConnectionsPerIp) + "）";
                }
            }

            if (!reason.empty()) {
                // 直接发一行 ERROR 再关，不走 Client 对象（它还没进列表）
                const std::string line =
                    dchat::BuildLine("ERROR", dchat::NowTimeString() + " " + reason);
                const std::string payload = line + "\n";
                ::send(sock, payload.data(), static_cast<int>(payload.size()), 0);
                ::shutdown(sock, SD_BOTH);
                ::closesocket(sock);
                Log("rejected connection from " + ip + ": " + reason);
                continue;
            }
        }

        auto client = std::make_shared<Client>();
        client->sock = sock;
        client->ip = ip;
        client->address = ip + ":" + std::to_string(ntohs(peer.sin_port));
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients.push_back(client);
        }
        Log("connection from " + client->address);
        std::thread(ClientLoop, client).detach();
    }

    ::closesocket(listener);
    WSACleanup();
    return 0;
}
