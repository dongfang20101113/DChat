// 聊天服务器：Winsock2 + 每客户端一个线程，收到消息后广播给所有人。
// 控制台日志保持纯 ASCII（避免代码页问题），协议里的用户可见文本是 UTF-8 中文。
#include "socket_util.h"  // 跨平台 socket（Windows Winsock / Linux POSIX）

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "history_store.h"
#include "register_guard.h"
#include "auth.h"
#include "crypto.h"
#include "file_transfer.h"
#include "server_rules.h"
#include "server_command.h"
#include "room_registry.h"

// 下面这些 using 让业务代码一个字都不用动就能两端编译。
// 名字像 Winsock 是刻意的——大面积改名会让 diff 淹没真正的移植点。
using dchat::sock::Accept;
using dchat::sock::Bind;
using dchat::sock::Handle;
using dchat::sock::kInvalid;
using dchat::sock::Listen;
using dchat::sock::Receive;
using dchat::sock::Send;

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
// 发送方先把文件传给服务器，服务器**在磁盘上**暂存一段时间；其他人在卡片上点「下载」
// 时才把数据发给他（FILE_GET）。过期或超出容量会连磁盘文件一起删掉。
//
// 为什么是磁盘而不是内存：原来 StoredFile::data 是 std::string，整个文件内容
// 都驻留内存。这台服务器只有 1.6GB 内存，传几个大文件就会被 OOM 杀掉
// （而且当时 maxservertemp 设成了 32GB，是物理内存的 20 倍，那道闸门形同虚设）。
// 语音消息也是走这条路径（kind=voice），所以一起落盘。
struct PendingUpload {          // 正在上传的文件（每个连接最多一个）
    std::string id;             // 客户端自己的传输 ID（只用于日志）
    // 这个上传属于哪个房间。**在 FILE_SEND 的时候钉死**，之后不再看 currentRoom：
    // 上传要跨越 FILE_SEND -> 一串 FILE_CHUNK -> FILE_END 好几条命令，用户完全
    // 可能中途切到别的会话去。不钉死的话，文件的前半段会进 A 群、后半段进 B 群，
    // 而 .part 文件只有一个 —— 结果是两个群里都出现一个内容错乱的附件。
    std::string roomId = dchat::kHallId;
    std::string nameB64;
    unsigned long long declared = 0;
    // ---- 磁盘暂存 ----
    // FILE_SEND 时建 .part 文件，FILE_CHUNK 直接往里追加。收齐后改名去掉 .part
    // 并把路径登记进 StoredFile；**半途中断（断线、取消、校验失败）由析构删掉**，
    // 这样无论从哪条路径退出，都不会在磁盘上留下垃圾。
    std::string partPath;
    std::ofstream out;
    unsigned long long received = 0;  // 已落盘字节数，用来校验完整性
    bool committed = false;           // 改名成功、已交给 StoredFile 管，析构不再删
    std::string thumb;          // 缩略图（PNG，图片缩小图 / 视频第一帧），可能为空
    bool expectThumb = false;   // 发送方声明"会带缩略图"
    // 发送方声明的**附件种类**，原样透传给接收方。
    //   0 / file = 普通文件   1 / sticker = 贴纸   voice = 语音消息
    // 服务端不解释这个值——它只负责透传。三种附件走完全相同的传输，
    // 区别只在客户端收到后怎么显示、以及要不要自动下载。
    std::string kind = "0";

    PendingUpload() = default;
    PendingUpload(const PendingUpload&) = delete;
    PendingUpload& operator=(const PendingUpload&) = delete;
    ~PendingUpload() {
        if (out.is_open()) out.close();
        // committed 为假 = 这份数据没有变成正式附件，磁盘上那份必须清掉
        if (!committed && !partPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(partPath, ec);
        }
    }
};

struct StoredFile {             // 已经传完、等人下载的文件
    std::string id;             // 服务器分配（F1、F2…）
    std::string owner;
    // 这个文件属于哪个房间。**必须记**：不记的话，群 A 里的文件能被群 B 的人
    // 拿着文件 ID 直接下载（FILE_GET 只认 ID），等于把私聊附件暴露给所有人。
    // 同时也用来算"这个房间的缓存预算被文件占了多少"。
    std::string roomId;
    std::string nameB64;
    unsigned long long size = 0;
    std::string path;           // 文件内容在磁盘上的位置（dchat-files[/房间ID]/upload-N）
    std::string thumb;          // 缩略图（PNG，仍放内存：只有几 KB，且预览路径到处在用）
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

// ---- 暂存目录 ----
// 每个房间一个目录：dchat-files/<房间ID>/。**大厅沿用原来的 dchat-files 根目录**
// （见 RoomRegistry::FilesDirFor），所以升级前已经传上去的文件照样下载得到。
// 分目录不只是为了整齐：群被解散时删掉整个目录就能把它的文件清干净，
// 不会误删别的群的东西。
std::string g_filesRoot = "dchat-files";
// 上传临时文件的序号，只用来保证 .part 名字唯一（正式附件的名字由 StoredFile::id 决定）
unsigned long long g_uploadSeq = 0;
constexpr const char* kPartSuffix = ".part";

bool EnsureFilesDir(const std::string& roomId) {
    std::error_code ec;
    const std::string dir = dchat::RoomRegistry::FilesDirFor(roomId);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        Log("cannot create file staging dir " + dir + "：" + ec.message());
        return false;
    }
    return true;
}

// 删掉一个暂存文件；删不掉只记日志 —— 清理失败不该影响主流程。
void RemoveStoredFileQuiet(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) Log("failed to remove staged file " + path + "：" + ec.message());
}

// 启动时清空暂存根目录（连同各房间子目录一起）。
// 文件本来就不跨重启（和以前"内存暂存"的行为一致：重启后旧附件不可下载），
// 顺带把上次崩溃留下的 .part 残留一起清掉。
void ClearFilesDir() {
    std::error_code ec;
    std::size_t removed = 0;
    if (std::filesystem::exists(g_filesRoot, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(g_filesRoot, ec)) {
            std::error_code rm;
            if (std::filesystem::remove_all(entry.path(), rm)) ++removed;
        }
    }
    if (removed > 0) {
        Log("cleared " + std::to_string(removed) + " leftover file(s) in " + g_filesRoot +
            "（暂存不跨重启）");
    }
}

/** 群被解散时把它的暂存目录整个删掉。大厅不走这里（它没有独立的子目录）。 */
void ClearRoomFilesDir(const std::string& roomId) {
    if (roomId == dchat::kHallId) return;
    std::error_code ec;
    const std::string dir = dchat::RoomRegistry::FilesDirFor(roomId);
    if (!std::filesystem::exists(dir, ec)) return;
    const std::uintmax_t removed = std::filesystem::remove_all(dir, ec);
    if (removed > 0) {
        Log("removed " + std::to_string(removed) + " item(s) in " + dir + "（房间已删除）");
    }
}

// 清掉过期文件；返回被清掉的描述（用于日志）
std::vector<std::string> ExpireFilesLocked() {
    std::vector<std::string> expired;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_files.begin(); it != g_files.end();) {
        if ((*it)->expires <= now) {
            expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
            // 磁盘上那份也要删，否则"过期"只清内存、文件会一直堆在磁盘上
            RemoveStoredFileQuiet((*it)->path);
            it = g_files.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

/** 某个房间当前的文件占用（调用方已持有 g_filesMutex）。 */
unsigned long long RoomStoredBytesLocked(const std::string& roomId) {
    unsigned long long total = 0;
    for (const auto& file : g_files) {
        if (file->roomId == roomId) total += file->size;
    }
    return total;
}

// 存一个文件；成功返回服务器分配的 ID，失败返回空串。
// roomBudget 为 0 表示"这个房间没有单独预算，只受全服 maxservertemp 约束"。
std::string StoreFile(const std::string& roomId, const std::string& owner,
                      const std::string& nameB64, unsigned long long size,
                      const std::string& path, const std::string& thumb,
                      unsigned long long roomBudget) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    for (const std::string& line : ExpireFilesLocked()) Log("stored file expired: " + line);
    const unsigned long long global = TempBudgetBytes();
    const unsigned long long roomLimit =
        (roomBudget > 0 && roomBudget < global) ? roomBudget : global;
    if (size > roomLimit) return std::string();  // 单个就超预算，存不下

    // 腾地方分两步，顺序很重要：
    //   1) 先删**本房间**最旧的。群文件超预算是这个群自己的事，不该把别的房间的
    //      东西挤掉 —— 否则会变成"隔壁群传了个大文件，我的文件凭空没了"。
    //   2) 再按全服总量删最旧的。maxservertemp 是所有房间共享的那道总闸。
    while (!g_files.empty() && RoomStoredBytesLocked(roomId) + size > roomLimit) {
        auto victim = std::find_if(g_files.begin(), g_files.end(),
                                   [&](const std::shared_ptr<StoredFile>& file) {
                                       return file->roomId == roomId;
                                   });
        if (victim == g_files.end()) break;  // 本房间已经没有可删的了
        Log("stored file evicted (room budget): " + (*victim)->id + "（" + (*victim)->owner +
            " 上传）");
        RemoveStoredFileQuiet((*victim)->path);
        g_files.erase(victim);
    }
    while (!g_files.empty() && (g_files.size() >= kMaxStoredFiles ||
                                StoredBytesLocked() + size > global)) {
        Log("stored file evicted (no room): " + g_files.front()->id + "（" +
            g_files.front()->owner + " 上传）");
        // 淘汰时把磁盘文件一起删掉，否则"腾地方"只腾了内存、磁盘越攒越多
        RemoveStoredFileQuiet(g_files.front()->path);
        g_files.erase(g_files.begin());
    }
    auto file = std::make_shared<StoredFile>();
    file->id = "F" + std::to_string(++g_fileSeq);
    file->owner = owner;
    file->roomId = roomId;
    file->nameB64 = nameB64;
    file->size = size;
    file->path = path;  // 内容已经在磁盘上了，这里只登记位置
    file->thumb = thumb;
    file->expires = std::chrono::steady_clock::now() + std::chrono::seconds(kFileTtlSeconds);
    g_files.push_back(file);
    return file->id;
}

// 按 ID 取文件（同时做过期清理）；找不到返回 nullptr
// ---- 聊天记录：每个房间各自一份 ----
// keepchathistory 打开时，新加入的人能看到之前的记录，而且**服务端重启后还在**。
//
// 从"一个全局记录"改成"每房间一份"是这次多会话改造的核心之一：私聊内容如果和
// 群聊共用一份历史，回放时就会串会话 —— 那是最严重的一类 bug（串会话等于泄露）。
//
// 房间表、每房间历史、每房间落盘流全部由 g_roomsMutex 罩住。用同一把锁的理由是
// "查房间 -> 改历史 -> 取广播目标"必须是一个原子操作，分层加锁迟早会出现
// "广播到一半房间被删了"。锁顺序固定为 g_roomsMutex -> g_filesMutex
// （算房间预算时要知道文件占了多大），绝不反过来。
std::mutex g_roomsMutex;
std::unique_ptr<dchat::RoomRegistry> g_rooms;

// 房间名单（群聊 + 私聊）。大厅**不在**这个文件里 —— 它由代码建，
// 写进去只会多一份可能与代码不一致的状态。和 dchat-admins.txt 同目录（服务工作目录）。
constexpr const char* kRoomsPath = "dchat-rooms.txt";

struct RoomHistoryFile {
    std::ofstream out;
    bool dirty = false;
    bool compactDue = false;
    unsigned long long sinceCompact = 0;
    std::chrono::steady_clock::time_point flushedAt{};
};
// 只有真正写过盘的房间才会在这里留下条目。
std::map<std::string, std::unique_ptr<RoomHistoryFile>> g_historyFiles;

constexpr std::size_t kMaxHistoryLines = 2000;
constexpr int kHistoryFlushMs = 1000;
constexpr unsigned long long kHistoryCompactEvery = 512;

dchat::RoomRegistry& RoomsLocked() {
    if (!g_rooms) {
        g_rooms = std::make_unique<dchat::RoomRegistry>(
            kMaxHistoryLines, dchat::RuleMbToBytes(CurrentRules().maxServerTempMb));
    }
    return *g_rooms;
}

RoomHistoryFile& HistoryFileLocked(const std::string& roomId) {
    std::unique_ptr<RoomHistoryFile>& slot = g_historyFiles[roomId];
    if (!slot) slot = std::make_unique<RoomHistoryFile>();
    return *slot;
}

/** 某个房间当前被群文件占掉的字节数（算历史能分到多少预算时要减掉）。 */
unsigned long long RoomFilesBytes(const std::string& roomId) {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    unsigned long long total = 0;
    for (const auto& file : g_files) {
        if (file->roomId == roomId) total += file->size;
    }
    return total;
}

/** 房间实际可用的缓存预算：群自己设的 cachemaxmb 和全局 maxservertemp 取小的那个。 */
unsigned long long EffectiveRoomBudget(const dchat::Room& room) {
    const unsigned long long own = dchat::RoomRegistry::BudgetBytes(room);
    const unsigned long long global = dchat::RuleMbToBytes(CurrentRules().maxServerTempMb);
    return own < global ? own : global;
}

/** 把一条记录追加到该房间的历史文件（调用方已持有 g_roomsMutex 且确认规则为真）。 */
void AppendHistoryFileLocked(const std::string& roomId, const dchat::HistoryEntry& entry) {
    RoomHistoryFile& file = HistoryFileLocked(roomId);
    if (!file.out.is_open()) {
        const std::string path = dchat::RoomRegistry::HistoryPathFor(roomId);
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        file.out.open(path, std::ios::binary | std::ios::app);
        if (!file.out) {
            Log("failed to open history file for append: " + path);
            return;
        }
    }
    file.out << dchat::FormatHistoryFileLine(entry);
    file.dirty = true;
}

// ---- 每用户的阅读进度（离线补发用）----
// 单独一个文件，不塞进账号文件：那是"用户凭据"，格式改动会牵连四端登录解析；
// 而这是服务端自己的阅读进度，客户端根本不需要知道。
std::mutex g_seenMutex;
std::map<std::string, unsigned long long> g_lastSeen;
std::string g_seenPath = "dchat-seen.txt";
std::ofstream g_seenOut;
bool g_seenDirty = false;
bool g_seenCompactDue = false;
unsigned long long g_seenSinceCompact = 0;
constexpr unsigned long long kSeenCompactEvery = 512;

/**
 * 记下某个人"已经读到第几条"，并**当场追加一行到进度文件**。
 *
 * 为什么追加放在这里、而不是交给调用方：谁改进度谁写盘，就不会出现
 * "两个用户几乎同时断开、后一个把标志清掉、前一个的进度漏写"这种情况。
 * 真正耗时的 flush 仍然由周期任务节流（最多每秒一次）。
 */
void RecordSeen(const std::string& nick) {
    if (nick.empty()) return;
    // **先取序号、再拿 g_seenMutex**：两把锁不嵌套，避免锁顺序问题。
    // 阅读进度这条路径只服务**大厅**的离线补发（群聊/私聊第一版不做离线补发，
    // 重新登录后切到那个会话就能拉到最近历史），所以这里看的是大厅的序号。
    unsigned long long seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        dchat::HistoryStore* store = RoomsLocked().HistoryOf(dchat::kHallId);
        seq = store ? store->LastSeq() : 0;
    }
    std::lock_guard<std::mutex> lock(g_seenMutex);
    unsigned long long& slot = g_lastSeen[nick];
    if (seq <= slot) return;  // 进度只前进不后退；没变化就不用写盘
    slot = seq;
    if (!g_seenOut.is_open()) {
        g_seenOut.open(g_seenPath, std::ios::binary | std::ios::app);
        if (!g_seenOut) return;
    }
    g_seenOut << dchat::FormatSeenFileLine({nick, seq});
    ++g_seenSinceCompact;
    g_seenDirty = true;
}

/** 取某人的进度；返回 false 表示没有记录（新用户）。 */
bool LookupSeen(const std::string& nick, unsigned long long* out) {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    const auto it = g_lastSeen.find(nick);
    if (it == g_lastSeen.end()) return false;
    if (out) *out = it->second;
    return true;
}

void FlushSeenIfDirty() {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    if (!g_seenDirty || !g_seenOut.is_open()) return;
    g_seenOut.flush();
    g_seenDirty = false;
}

/** 整份重写进度文件（追加写的文件里同一用户会攒下很多行）。 */
void CompactSeen() {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    if (g_seenOut.is_open()) {
        g_seenOut.flush();
        g_seenOut.close();
    }
    std::ofstream out(g_seenPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Log("failed to compact seen file: " + g_seenPath);
        return;
    }
    for (const auto& pair : g_lastSeen) {
        out << dchat::FormatSeenFileLine({pair.first, pair.second});
    }
    g_seenSinceCompact = 0;
    g_seenDirty = false;
    Log("seen progress compacted: " + std::to_string(g_lastSeen.size()) + " user(s)");
}

void LoadSeen() {
    std::ifstream in(g_seenPath, std::ios::binary);
    if (!in) return;  // 还没有进度文件：所有人都是"新用户"
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::vector<dchat::SeenRecord> records = dchat::ParseSeenFile(text);
    std::lock_guard<std::mutex> lock(g_seenMutex);
    for (const dchat::SeenRecord& record : records) g_lastSeen[record.nick] = record.seq;
    if (!records.empty()) {
        Log("loaded reading progress for " + std::to_string(records.size()) + " account(s)");
    }
}

/** 把攒着的记录刷到磁盘（周期任务与退出前调用）。 */
void FlushHistoryIfDirty() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto& pair : g_historyFiles) {
        RoomHistoryFile& file = *pair.second;
        if (!file.dirty || !file.out.is_open()) continue;
        file.out.flush();
        file.dirty = false;
        file.flushedAt = now;
    }
}

/** 整份重写历史文件，只留内存里还保着的那些（压实）。 */
void CompactHistory() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    for (auto it = g_historyFiles.begin(); it != g_historyFiles.end();) {
        const std::string roomId = it->first;
        RoomHistoryFile& file = *it->second;

        // 房间已经没了（群被解散）：把落盘流和这条记录一起清掉。
        // 不清的话 g_historyFiles 会随着"建群 -> 解散"无限增长，而且下一次
        // 建一个同名群时会**继承上一个群的历史** —— 那是最难查的一类串会话。
        dchat::HistoryStore* store = rooms.HistoryOf(roomId);
        if (!store) {
            if (file.out.is_open()) file.out.close();
            it = g_historyFiles.erase(it);
            continue;
        }
        if (!file.compactDue) {
            ++it;
            continue;
        }
        // 先关掉追加流，否则重写和追加会互相踩
        if (file.out.is_open()) {
            file.out.flush();
            file.out.close();
        }
        const std::string path = dchat::RoomRegistry::HistoryPathFor(roomId);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            Log("failed to compact history file: " + path);
            ++it;
            continue;
        }
        out << store->Serialize();
        file.sinceCompact = 0;
        file.dirty = false;
        file.compactDue = false;
        Log("history compacted [" + roomId + "]: " + std::to_string(store->Size()) + " line(s) kept");
        ++it;
    }
}

/** 启动时读一次房间名单（dchat-rooms.txt），恢复群聊与私聊。 */
void LoadRooms() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    rooms.Reset();  // 先建大厅；它在文件里不存在，永远由代码建
    std::ifstream in(kRoomsPath, std::ios::binary);
    if (!in) {
        Log(std::string("no rooms file yet; only the hall exists: ") + kRoomsPath);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t loaded = rooms.LoadFromText(text);
    Log("loaded " + std::to_string(loaded) + " room(s) from " + kRoomsPath);
}

/** 写回房间名单。**调用前必须已经持有 g_roomsMutex**（和 SaveOpsLocked 同一个约定）。 */
void SaveRoomsLocked() {
    std::ofstream out(kRoomsPath, std::ios::binary | std::ios::trunc);
    if (!out) return;  // 写不进去也不影响运行，内存里的房间照常生效
    out << RoomsLocked().Serialize();
}

void SaveRooms() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    SaveRoomsLocked();
}

/**
 * 启动时把每个房间的历史文件各读一遍。
 *
 * **无论 keepchathistory 是开还是关都要读**：读的目的不只是回放，
 * 更重要的是**继承文件里的最大序号**。否则：规则关着跑一阵（不写盘，
 * 但内存序号在涨），中途打开规则开始追加，序号是对的；可如果反过来 ——
 * 没读文件就从 1 开始追加，文件里已有的 N 条会让新行全部被判为"序号未递增"
 * 而在**下次启动时被丢弃**。
 *
 * 大厅沿用原来的 dchat-history.txt，所以升级后线上已有的记录照样读得回来。
 */
void LoadHistory() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    bool anyHall = false;
    for (const dchat::Room& room : rooms.Snapshot()) {
        const std::string path = dchat::RoomRegistry::HistoryPathFor(room.id);
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dchat::HistoryStore* store = rooms.HistoryOf(room.id);
        if (!store) continue;
        const std::size_t loaded = store->LoadFromText(text);
        if (room.id == dchat::kHallId) anyHall = true;
        if (loaded > 0) {
            Log("loaded " + std::to_string(loaded) + " history line(s) from " + path + "（末序号 " +
                std::to_string(store->LastSeq()) + "）");
        }
    }
    if (!anyHall) {
        Log(std::string("no history file yet, will create when keepchathistory is on: ") +
            dchat::RoomRegistry::HistoryPathFor(dchat::kHallId));
    }
}

unsigned long long StoredBytesTotal() {
    std::lock_guard<std::mutex> lock(g_filesMutex);
    return StoredBytesLocked();
}

void RememberHistory(const std::string& roomId, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    const dchat::Room* room = rooms.Find(roomId);
    if (!room) return;
    dchat::HistoryStore* store = rooms.HistoryOf(roomId);
    if (!store) return;

    const unsigned long long seq = store->Append(line);

    // 预算 = 这个房间的「总缓存最大大小」（历史和群文件**共享**这一份），
    // 同时受全局 maxservertemp 约束。文件先占，剩下的才是历史的：
    // 文件大、价值低，磁盘要满的时候先丢它才合理，所以历史是"用剩下的"。
    const unsigned long long budget = EffectiveRoomBudget(*room);
    const unsigned long long usedByFiles = RoomFilesBytes(roomId);
    store->TrimToBytes(budget > usedByFiles ? budget - usedByFiles : 0);

    if (!CurrentRules().keepChatHistory) return;  // 规则关着：不写盘

    dchat::HistoryEntry entry;
    entry.seq = seq;
    entry.line = line;
    AppendHistoryFileLocked(roomId, entry);

    RoomHistoryFile& file = HistoryFileLocked(roomId);
    // 节流 flush：距上次超过 1 秒才真正落盘
    const auto now = std::chrono::steady_clock::now();
    const auto since =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - file.flushedAt).count();
    if (file.dirty && since >= kHistoryFlushMs) {
        file.out.flush();
        file.dirty = false;
        file.flushedAt = now;
    }

    if (++file.sinceCompact >= kHistoryCompactEvery) {
        // 只有在"确实有旧行被裁掉"时压实才有意义：
        // 否则重写一遍文件一个字节都省不下来，纯属白费 I/O。
        if (store->TotalAppended() > store->Size()) file.compactDue = true;
        // 这一行是**诊断用**的，每 512 条才打一次，开销可以忽略。
        // 为什么值得留着：预算到底有没有生效，光看磁盘文件是分不清三种情况的 ——
        // ①内存按预算裁了、只是文件还没压实；②内存压根没裁；③裁了但压实没跑。
        // 这三个数字（追加过多少 / 现在还留多少 / 实际占多少字节）一眼就能区分。
        Log("history budget [" + roomId + "]: appended=" + std::to_string(store->TotalAppended()) +
            " kept=" + std::to_string(store->Size()) + " bytes=" + std::to_string(store->Bytes()) +
            " budget=" + std::to_string(budget) + " compactDue=" +
            (file.compactDue ? "1" : "0"));
        file.sinceCompact = 0;
    }
}

/** 全部房间的历史条数合计（状态日志用）。 */
std::size_t HistoryCount() {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    std::size_t total = 0;
    for (const dchat::Room& room : rooms.Snapshot()) {
        dchat::HistoryStore* store = rooms.HistoryOf(room.id);
        if (store) total += store->Size();
    }
    return total;
}

/**
 * 取某个房间里序号大于 afterSeq 的记录（离线补发用）。
 * 调用方不能持有 g_roomsMutex。
 */
std::vector<dchat::HistoryEntry> HistorySince(const std::string& roomId, unsigned long long afterSeq,
                                              std::size_t limit) {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    return RoomsLocked().HistorySince(roomId, afterSeq, limit);
}

/**
 * 取某个房间**最近** limit 条历史（旧 -> 新）。
 * 调用方不能持有 g_roomsMutex。
 */
std::vector<dchat::HistoryEntry> RoomRecentHistory(const std::string& roomId, std::size_t limit) {
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    return RoomsLocked().RecentHistory(roomId, limit);
}

// 把预算外的文件和聊天记录裁掉（maxservertemp 或某个群的 cachemaxmb 调小时立刻生效）
void EnforceTempBudget() {
    const unsigned long long budget = TempBudgetBytes();
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        while (!g_files.empty() && StoredBytesLocked() > budget) {
            Log("stored file evicted (over maxservertemp): " + g_files.front()->id);
            RemoveStoredFileQuiet(g_files.front()->path);
            g_files.erase(g_files.begin());
        }
    }
    // 逐个房间把「该群预算」重新应用一遍。管理员把 cachemaxmb 调小之后，
    // 必须真的有东西被裁掉，否则那个设置项就只是个好看的数字。
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    dchat::RoomRegistry& rooms = RoomsLocked();
    for (const dchat::Room& room : rooms.Snapshot()) {
        rooms.SetHistoryBudget(room.id, EffectiveRoomBudget(room), RoomFilesBytes(room.id));
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
        const auto* address = reinterpret_cast<const dchat::sock::Address*>(it->ai_addr);
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
//
// 两端设置方式不同：Windows 用 WSAIoctl(SIO_KEEPALIVE_VALS) 一次设好空闲时间和间隔，
// Linux 是三个独立 setsockopt（TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT）。
// 数值定成同一组（30 秒空闲、5 秒间隔、探 3 次算断），两端发现死连接的时间才一致。
void EnableKeepAlive(dchat::sock::Handle handle) {
#ifdef _WIN32
    BOOL on = TRUE;
    ::setsockopt(handle, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
    tcp_keepalive settings{};
    settings.onoff = TRUE;
    settings.keepalivetime = 30000;    // 30 秒空闲后开始探测
    settings.keepaliveinterval = 5000; // 探测间隔 5 秒
    DWORD returned = 0;
    ::WSAIoctl(handle, SIO_KEEPALIVE_VALS, &settings, sizeof(settings), nullptr, 0, &returned,
               nullptr, nullptr);
#else
    const int on = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    const int idle = 30;     // 秒
    const int interval = 5;  // 秒
    const int count = 3;     // 探测几次算断
    // 「空闲多久开始探测」这个选项三端三个名字：
    //   Windows 走上面的 WSAIoctl 分支
    //   Linux   TCP_KEEPIDLE
    //   macOS   TCP_KEEPALIVE（含义相同，宏名不同）
#ifdef __APPLE__
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#else
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    // macOS 没有 TCP_KEEPCNT（探测次数由系统定），有就设、没有就跳过
#ifndef __APPLE__
    ::setsockopt(handle, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#else
    (void)count;
#endif
#endif
}

void Log(const std::string& text) {
    // 用 C++ 的本地时间而不是 Windows 的 GetLocalTime：两端都能编。
    const std::time_t stamp = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &stamp);
#else
    localtime_r(&stamp, &local);
#endif
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::printf("[%02d:%02d:%02d] %s\n", local.tm_hour, local.tm_min, local.tm_sec, text.c_str());
    std::fflush(stdout);
}

// 服务器发出的每条用户可见消息都带上时间戳（hh:mm），客户端据此显示 [HH:MM]
std::string Timed(const std::string& command, const std::string& rest = std::string()) {
    const std::string time = dchat::NowTimeString();
    return dchat::BuildLine(command, rest.empty() ? time : time + " " + rest);
}

struct Client {
    dchat::sock::Handle sock = dchat::sock::kInvalid;
    std::string nick;     // 空表示还没设置昵称
    std::string ip;       // 裸 IP（不含端口），公网加固按它做单 IP 限流
    std::string address;  // 例如 127.0.0.1:51234
    std::mutex sendMutex;
    // 正在上传的文件（收齐后由服务器暂存，别人点击下载时才发出去）
    std::shared_ptr<PendingUpload> upload;
    std::chrono::steady_clock::time_point lastMessage{};  // chatinterval 用：上一条消息的时间

    // ---- 多会话（私聊 / 群聊）----
    // 当前会话：MSG 发到这里。默认大厅 —— 老客户端从不下发 ROOM 命令，
    // 于是一直停在大厅，行为与本功能上线前**逐字节一致**。
    std::string currentRoom = dchat::kHallId;
    // 客户端在 HELLO 里声明过 rooms1 能力。没声明就是老客户端：只收大厅消息、
    // 不参与群聊和私聊。这个开关是"新服务端 + 正在用的旧客户端"能安全共存的关键。
    bool roomsCapable = false;

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
        if (sock == dchat::sock::kInvalid) return false;

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
            const std::ptrdiff_t n = dchat::sock::Send(sock, data.data() + sent, data.size() - sent);
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

// 账号文件写入节流 + 已知名字缓存。
//
// 为什么需要：实测（tools/regflood.cpp）发现每次注册都做两件 O(N) 的事——
//   1. SaveUsers() 持 g_usersMutex 把**整张表**序列化并重写文件；
//   2. BroadcastKnownNames() 对**每个在线客户端**各算一遍完整用户名列表。
// 12 线程并发注册时这两件事把正常用户的消息往返从 105 ms 拖到 10 秒超时。
//
// 写入节流：同一窗口内只落盘一次，中间的改动标记为"待写"，
// 由周期任务补写（见 BanMaintenanceLoop）——**只丢最后不到 1 秒的注册**，
// 而换来的是一条注册路径不再做文件 I/O。
std::mutex g_usersIoCtxMutex;
std::chrono::steady_clock::time_point g_lastUsersWrite{};
bool g_usersDirty = false;
inline constexpr int kUsersWriteThrottleMs = 800;

// 已知名字缓存：账号表或操作员/封禁表一变就置脏，下次广播时重算一次，
// 而不是每个客户端算一遍。
std::mutex g_knownNamesMutex;
std::string g_knownNamesCache;
bool g_knownNamesDirty = true;

void InvalidateKnownNames() {
    std::lock_guard<std::mutex> lock(g_knownNamesMutex);
    g_knownNamesDirty = true;
}

// ---- 注册路径防护 ----
// 记录每个 IP 上次注册成功的时间。换 IP 能绕开这条，所以它只是"抬高成本"，
// 真正兜底的是 maxaccounts（总量上限，换多少 IP 都绕不过）。
// 刻意只保留最近 kRegisterTrackMaxSec 秒的记录：否则长期运行时这张表会无限增长。
std::mutex g_registerMutex;
std::map<std::string, std::chrono::steady_clock::time_point> g_lastRegister;
inline constexpr int kRegisterTrackMaxSec = 86400;

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
    // 节流：窗口内不重复写盘，只标脏。写盘时必须持 g_usersMutex 保证快照一致。
    {
        std::lock_guard<std::mutex> lock(g_usersIoCtxMutex);
        const auto now = std::chrono::steady_clock::now();
        const auto sinceWrite = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - g_lastUsersWrite)
                                    .count();
        if (g_lastUsersWrite.time_since_epoch().count() != 0 &&
            sinceWrite < kUsersWriteThrottleMs) {
            g_usersDirty = true;
            return true;  // 交给周期任务补写，调用方不必关心
        }
        g_lastUsersWrite = now;
        g_usersDirty = false;
    }
    std::lock_guard<std::mutex> lock(g_usersMutex);
    std::ofstream out(g_usersPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << dchat::SerializeUsers(g_users);
    return out.good();
}

/** 周期任务调用：把节流期间攒下的改动补写落盘。 */
void FlushUsersIfDirty() {
    {
        std::lock_guard<std::mutex> lock(g_usersIoCtxMutex);
        if (!g_usersDirty) return;
        g_usersDirty = false;
        g_lastUsersWrite = std::chrono::steady_clock::now();
    }
    std::lock_guard<std::mutex> lock(g_usersMutex);
    std::ofstream out(g_usersPath, std::ios::binary | std::ios::trunc);
    if (out) out << dchat::SerializeUsers(g_users);
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
//
// 名单**存盘**（dchat-admins.txt，和身份密钥同目录，即服务的工作目录）。
//
// 为什么必须存盘：之前只在内存里，服务一重启就全空了。对局域网自娱自乐无所谓，
// 但跑在公网服务器上就很难受——`/op` 只能在控制台做，而控制台要在 systemd
// 前面手动跑才有（stdin 被接到 /dev/null），等于**每次重启都要停一次服**。
// 黑名单（封禁）仍然只在内存里：封禁是有时效的，重启后清空是合理行为。
const char* const kAdminsPath = "dchat-admins.txt";

std::mutex g_opMutex;
std::vector<std::string> g_ops;

/** 把名单写回文件。**调用前必须已经持有 g_opMutex**（内部不做加锁）。 */
void SaveOpsLocked() {
    std::ofstream out(kAdminsPath);
    if (!out) return;  // 写不进去也不影响运行，内存里的名单照常生效
    out << "# dchat 管理员名单，一行一个昵称。\n"
        << "# 由 /op、/deop 自动写回；也可以手工编辑，改完需要重启服务。\n";
    for (const std::string& name : g_ops) out << name << "\n";
}

/** 启动时读回名单。文件不存在就是"还没有管理员"，不是错误。 */
void LoadOps() {
    std::lock_guard<std::mutex> lock(g_opMutex);
    g_ops.clear();
    std::ifstream in(kAdminsPath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g_ops.push_back(line);
    }
}

bool IsOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    return std::find(g_ops.begin(), g_ops.end(), name) != g_ops.end();
}

bool AddOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    if (std::find(g_ops.begin(), g_ops.end(), name) != g_ops.end()) return false;
    g_ops.push_back(name);
    SaveOpsLocked();  // 立刻落盘：进程被 kill 也不会丢
    return true;
}

bool RemoveOp(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_opMutex);
    const auto before = g_ops.size();
    g_ops.erase(std::remove(g_ops.begin(), g_ops.end(), name), g_ops.end());
    if (g_ops.size() == before) return false;
    SaveOpsLocked();
    return true;
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

// ---- 私聊黑名单：谁不想收到谁的消息 ----
//
// 用户明确要求私聊设置里有黑名单。**必须由服务端持久化并拦截** ——
// 只做在客户端上的话拦不住对方：他换个客户端、或者干脆手打协议，消息照样进得来。
// 那就不是"拉黑"，只是"我自己不看"。
//
// 拦截范围刻意**只限私聊**：大厅和群聊是公共空间，在那里拦会让对话看起来缺了一块
// （别人回复他，你只看到回复、看不到原话，比看到更难受）。这和主流 IM 的做法一致。
constexpr const char* kBlocksPath = "dchat-blocks.txt";

std::mutex g_blockMutex;
std::map<std::string, std::vector<std::string>> g_blocks;  // 拉黑者 -> 被拉黑的人

/** 写回黑名单文件。**调用前必须已经持有 g_blockMutex**（和 SaveOpsLocked 同一个约定）。 */
void SaveBlocksLocked() {
    std::ofstream out(kBlocksPath, std::ios::binary | std::ios::trunc);
    if (!out) return;  // 写不进去也不影响运行，内存里的名单照常生效
    out << "# dchat 私聊黑名单，一行一条：<拉黑的人> <被拉黑的人>。\n"
        << "# 由 BLOCK / UNBLOCK 自动写回；也可以手工编辑，改完需要重启服务。\n";
    for (const auto& pair : g_blocks) {
        for (const std::string& target : pair.second) out << pair.first << " " << target << "\n";
    }
}

void LoadBlocks() {
    std::ifstream in(kBlocksPath, std::ios::binary);
    std::lock_guard<std::mutex> lock(g_blockMutex);
    g_blocks.clear();
    if (!in) return;  // 还没有黑名单文件：不是错误
    std::string line;
    std::size_t count = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::size_t space = line.find(' ');
        if (space == std::string::npos || space == 0) continue;
        const std::string from = line.substr(0, space);
        const std::string to = line.substr(space + 1);
        if (to.empty() || from == to) continue;  // 拉黑自己没有意义
        std::vector<std::string>& list = g_blocks[from];
        if (std::find(list.begin(), list.end(), to) == list.end()) {
            list.push_back(to);
            ++count;
        }
    }
    if (count > 0) {
        Log("loaded " + std::to_string(count) + " block entr(ies) from " + kBlocksPath);
    }
}

/** from 有没有拉黑 to。 */
bool IsBlocked(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) return false;
    std::lock_guard<std::mutex> lock(g_blockMutex);
    const auto it = g_blocks.find(from);
    if (it == g_blocks.end()) return false;
    return std::find(it->second.begin(), it->second.end(), to) != it->second.end();
}

/**
 * 这两个人之间的私聊通不通。
 * **任意一方拉黑了对方就不通** —— 单向生效的话，被拉黑的人还能继续骚扰，
 * 而且他发出来的消息会在对方那边凭空消失，双方都以为是对方不理自己。
 */
bool DirectBlocked(const std::string& a, const std::string& b) {
    return IsBlocked(a, b) || IsBlocked(b, a);
}

bool AddBlock(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return false;
    std::lock_guard<std::mutex> lock(g_blockMutex);
    std::vector<std::string>& list = g_blocks[from];
    if (std::find(list.begin(), list.end(), to) != list.end()) return false;
    list.push_back(to);
    SaveBlocksLocked();  // 立刻落盘：拉黑是"我现在就不想收到"，重启丢掉最不能接受
    return true;
}

bool RemoveBlock(const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lock(g_blockMutex);
    const auto it = g_blocks.find(from);
    if (it == g_blocks.end()) return false;
    const auto before = it->second.size();
    it->second.erase(std::remove(it->second.begin(), it->second.end(), to), it->second.end());
    if (it->second.size() == before) return false;
    if (it->second.empty()) g_blocks.erase(it);
    SaveBlocksLocked();
    return true;
}

std::vector<std::string> BlockListOf(const std::string& from) {
    std::lock_guard<std::mutex> lock(g_blockMutex);
    const auto it = g_blocks.find(from);
    return it == g_blocks.end() ? std::vector<std::string>() : it->second;
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

/**
 * 把一个房间的成员昵称抄一份出来。
 *
 * 为什么不直接持着 g_roomsMutex 遍历客户端发消息：SendLine 是**阻塞的 socket 写**，
 * 持锁做网络 I/O 会把"某一个客户端卡住"放大成"整个服务器的房间操作全卡住"。
 * 所以先抄名单、放锁、再发。
 */
std::vector<std::string> RoomAudience(const std::string& roomId) {
    std::vector<std::string> audience;
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    const dchat::Room* room = RoomsLocked().Find(roomId);
    if (!room) return audience;
    for (const dchat::RoomMember& member : room->members) audience.push_back(member.nick);
    return audience;
}

/**
 * 这个客户端能不能看/下载某个房间的附件。
 *
 * 必须校验：文件 ID 是全局递增的（F1、F2…），不校验的话，任何登录用户
 * 随手猜几个 ID 就能把**私聊附件**拉下来 —— 那是直接的隐私泄露，而且
 * 从界面上完全看不出来（卡片不显示，但数据已经到手了）。
 * 大厅保持原行为：它是公共房间，历史上就是谁都能下。
 */
bool CanAccessRoomFile(const Client& client, const std::string& roomId) {
    if (roomId == dchat::kHallId) return true;
    if (client.nick.empty()) return false;
    std::lock_guard<std::mutex> lock(g_roomsMutex);
    const dchat::Room* room = RoomsLocked().Find(roomId);
    return room != nullptr && dchat::IsRoomMember(*room, client.nick);
}

/**
 * 往一个房间发消息。
 *
 * 大厅走**原来的全量 Broadcast**，一个字节都没变 —— 这是"老客户端零影响"这条
 * 硬约束的落点，不能改成"只发给大厅成员"：大厅成员表是登录时才维护的，
 * 而老行为里连还没登录的连接都会收到 SAY。
 *
 * 其他房间只发给该房间成员，而且**必须**是声明过 rooms1 的客户端。
 * 老客户端收到 RMSG 也只会当未知命令丢掉，但它本来也不该出现在群里。
 */
void BroadcastRoom(const std::string& roomId, const std::string& line,
                   const Client* except = nullptr) {
    if (roomId == dchat::kHallId) {
        Broadcast(line, except);
        return;
    }
    const std::vector<std::string> audience = RoomAudience(roomId);
    if (audience.empty()) return;
    for (const auto& client : ClientSnapshot()) {
        if (client.get() == except) continue;
        if (client->nick.empty() || !client->roomsCapable) continue;
        if (std::find(audience.begin(), audience.end(), client->nick) == audience.end()) continue;
        client->SendLine(line);
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
    // 名字列表**只算一次**给所有客户端用。
    // 原先是循环里调 KnownNameList()——每多一个在线客户端就重算一遍整张账号表，
    // 于是单次注册的成本变成 O(账号数 × 在线数)。实测能把正常用户拖到超时。
    // 缓存由 InvalidateKnownNames() 置脏（账号/操作员/封禁表变动时）。
    std::string names;
    {
        std::lock_guard<std::mutex> lock(g_knownNamesMutex);
        if (g_knownNamesDirty) {
            g_knownNamesCache = KnownNameList();
            g_knownNamesDirty = false;
        }
        names = g_knownNamesCache;
    }
    const std::string line = Timed("KNOWN", names);
    for (const auto& client : ClientSnapshot()) {
        if (client->nick.empty()) continue;
        client->SendLine(line);
    }
}

bool NickTaken(const std::string& nick, const Client* self) {
    for (const auto& client : ClientSnapshot())
        if (client.get() != self && client->nick == nick) return true;
    return false;
}

void RemoveClient(Client* target) {
    // 断开时把他的阅读进度记下来（含被踢、崩溃断开）：
    // 此刻 LastSeq() 就是他"在线期间已经收到"的最大序号 —— 因为在线时每条都是实时发的。
    // 万一漏记，后果也只是下次多补几条（重复）而不会漏消息。
    if (target) RecordSeen(target->nick);

    // 从大厅成员表里摘掉。大厅的成员表就是"当前在线的人"（登录时加入、这里移除），
    // 不这么做的话它会一直涨 —— 而它恰恰是唯一一张**不落盘、只活在内存**的表。
    if (target && !target->nick.empty()) {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        dchat::Room* hall = RoomsLocked().Hall();
        if (hall) {
            const std::string nick = target->nick;
            hall->members.erase(std::remove_if(hall->members.begin(), hall->members.end(),
                                               [&](const dchat::RoomMember& member) {
                                                   return member.nick == nick;
                                               }),
                                hall->members.end());
        }
    }

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
    dchat::sock::ShutdownWrite(target->sock);  // 读线程的 recv 会立刻返回，连接随之关闭
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
                RememberHistory(dchat::kHallId, announce);  // 公告进大厅的聊天记录
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
// ---- 定时公告：写文件即广播 ----
// 协议是**行式**的，所以只取第一行非空内容；正文里带换行会把整条命令劈成两条，
// 后面那条多半会被当成非法命令（甚至被别的客户端解析成奇怪的东西）。
std::string g_announcePath = "dchat-announce.txt";
constexpr std::size_t kMaxAnnounceChars = 300;

// 读一行公告文件并广播；没有文件/内容为空就什么都不做。
// 由 BanMaintenanceLoop 每秒调用（那里本来就有个 1 秒周期，不另开线程）。
void PollAnnounceFile() {
    std::error_code ec;
    if (!std::filesystem::exists(g_announcePath, ec)) return;

    std::ifstream in(g_announcePath, std::ios::binary);
    std::string text;
    if (in) {
        std::getline(in, text);          // 只取第一行
        in.close();
    }
    // 剥掉 UTF-8 BOM：在 Windows 上用记事本/PowerShell 存过这个文件就会带上，
    // 不剥的话它会作为正文第一个字符被播出去（客户端显示成一个方块）。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    // 去掉行尾的 \r（Windows 上编辑过这个文件时会有）和首尾空白
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        text.clear();
    } else {
        text = text.substr(begin);
    }
    if (text.size() > kMaxAnnounceChars) text = dchat::Utf8Truncate(text, kMaxAnnounceChars);
    if (text.empty()) {
        // 空文件：清掉就行，别重复读
        std::filesystem::remove(g_announcePath, ec);
        return;
    }

    Log("announce (from file): " + text);
    const std::string announce = Timed("ANNOUNCE", text);
    Broadcast(announce);         // 全服公告：客户端会大字居中显示
    RememberHistory(dchat::kHallId, announce);   // 和 /say 一样，也进大厅的聊天记录

    // 播完就删；删不掉（权限/占用）就清空，**否则每秒都会重播一遍**。
    // CronJob 那边是"先写临时文件再改名"，所以这里不会读到写了一半的内容。
    std::filesystem::remove(g_announcePath, ec);
    if (std::filesystem::exists(g_announcePath, ec)) {
        std::ofstream clear(g_announcePath, std::ios::binary | std::ios::trunc);
        Log("warn: cannot remove " + g_announcePath + ", truncated instead");
    }
}

void BanMaintenanceLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ExpireBans();
        PollAnnounceFile();  // 定时公告：cron 写文件，这里读到就广播
        {  // 暂存的文件也会过期，顺手清一下
            std::lock_guard<std::mutex> lock(g_filesMutex);
            for (const std::string& line : ExpireFilesLocked()) {
                Log("stored file expired: " + line);
            }
        }
        // 登录失败记录也会过期，顺手清掉，免得长期运行时那张表无限增长
        g_loginFails.Sweep();
        // 账号文件的写入被节流了（注册路径不做文件 I/O），这里补写落盘
        FlushUsersIfDirty();
        // 聊天记录同样被节流（最多每秒一次），这里兜底 flush（所有房间一起）
        FlushHistoryIfDirty();
        FlushSeenIfDirty();
        // 进度文件是追加写的，同一用户会攒下多行，攒够就压实
        {
            std::lock_guard<std::mutex> lock(g_seenMutex);
            if (g_seenSinceCompact >= kSeenCompactEvery) {
                g_seenSinceCompact = 0;
                g_seenCompactDue = true;
            }
        }
        if (g_seenCompactDue) {
            g_seenCompactDue = false;
            CompactSeen();
        }
        // 压实标记是 RememberHistory 按房间置的，在这里统一执行。
        // CompactHistory 自己会判断哪些房间"该压实了"，也顺手清掉已解散房间的落盘流。
        CompactHistory();
        // 注册时间记录也要清：只保留窗口内的，否则长期运行时那张表无限增长
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_registerMutex);
            for (auto it = g_lastRegister.begin(); it != g_lastRegister.end();) {
                const auto age =
                    std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                if (age > kRegisterTrackMaxSec) {
                    it = g_lastRegister.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

// 认证通过后加入**大厅**：补大厅的历史和还留着的大厅文件卡片。
//
// 只回放大厅，不回放群聊/私聊：一个用户可能在几十个群里，登录时把它们的历史
// 全推一遍既慢又没意义。客户端切到某个会话时发 ROOM <id>，服务端再按需回放
// 那个房间的历史（见 ReplayRoomHistoryTo）。
// keepchathistory 关闭时直接返回 —— 这是原行为，升级后不变。
void ReplayHistoryTo(const std::shared_ptr<Client>& client) {
    if (!CurrentRules().keepChatHistory) return;

    // 回放"从哪开始"取决于阅读进度：
    //   - 新用户：进度视为 0 -> 看保留范围内的完整历史（keepchathistory 原有行为）
    //   - 老用户且 offlinemessages > 0：只补他错过的，条数受限（离线消息）
    //   - 老用户但 offlinemessages = 0：仍按老行为给完整历史（升级后观感不变）
    const int offlineLimit = CurrentRules().offlineMessages;
    unsigned long long sinceSeq = 0;
    bool catchUp = false;
    if (offlineLimit > 0) {
        unsigned long long seen = 0;
        if (LookupSeen(client->nick, &seen)) {
            sinceSeq = seen;
            catchUp = true;
        }
    }

    std::vector<dchat::HistoryEntry> entries =
        HistorySince(dchat::kHallId, sinceSeq,
                     catchUp ? static_cast<std::size_t>(offlineLimit)
                             : static_cast<std::size_t>(-1));
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const dchat::HistoryEntry& entry : entries) lines.push_back(entry.line);
    if (catchUp) RecordSeen(client->nick);  // 补完就把进度推到最新
    std::vector<std::string> offers;
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        for (const auto& file : g_files) {
            // 只发大厅自己的文件卡片。群聊/私聊的文件在切到那个会话时按需拉 ——
            // 登录时把几十个群的附件卡片一次性推过来，既慢又没意义
            if (file->roomId != dchat::kHallId) continue;
            offers.push_back(Timed("FILE_OFFER", file->owner + " " + file->id + " " + file->nameB64 +
                                                     " " + std::to_string(file->size) + " " +
                                                     (file->thumb.empty() ? "0" : "1")));
        }
    }
    if (lines.empty() && offers.empty()) return;
    client->SendLine(Timed("SYS", catchUp
                                    ? ("—— 你不在的时候有 " + std::to_string(lines.size()) +
                                       " 条消息 ——")
                                    : "—— 以下是加入之前的聊天记录（keepchathistory 已打开）——"));
    for (const std::string& line : lines) client->SendLine(line);
    if (!offers.empty()) {
        client->SendLine(Timed("SYS", "房间里还留着这些文件，点卡片可以下载："));
        for (const std::string& offer : offers) client->SendLine(offer);
    }
}

// ---------------------------------------------------------------------------
// 多会话：把房间信息同步给客户端
// ---------------------------------------------------------------------------
//
// 所有 "SendXxx" 都遵守同一条纪律：**在 g_roomsMutex 里只拼字符串，出了锁才发**。
// SendLine 是阻塞的 socket 写，持锁发消息会把"某个客户端网络卡住"放大成
// "整个服务器的房间操作全卡住"。拼好的行是字符串副本，出锁后房间被删也不影响。

/** 一个会话的详情：RINFO + 全部 RSET 行。**调用方必须已持有 g_roomsMutex。** */
std::vector<std::string> RoomInfoLinesLocked(const dchat::Room& room, const std::string& viewer) {
    dchat::RoomRole role = dchat::RoomRole::Member;
    dchat::TryRoleOf(room, viewer, &role);
    std::vector<std::string> lines;
    // 设置项**不塞进 RINFO**：以后加一个设置就要改 RINFO 的字段数，
    // 而字段数一变三个端会一起解析错位。拆成独立的 RSET 行就没有这个问题。
    lines.push_back(dchat::BuildLine(
        "RINFO", room.id + " " + dchat::RoomKindText(room.kind) + " " +
                     dchat::Base64Encode(dchat::RoomRegistry::DisplayNameFor(room, viewer)) + " " +
                     room.owner + " " + std::to_string(room.members.size()) + " " +
                     std::to_string(room.maxMembers) + " " + dchat::RoomRoleText(role) + " " +
                     dchat::JoinModeText(room.joinMode) + " " + (room.listed ? "true" : "false")));
    for (dchat::RoomSettingKey key : dchat::AllRoomSettingKeys()) {
        lines.push_back(dchat::RoomSettingLine(room, key));
    }
    return lines;
}

void SendRoomInfoTo(const std::shared_ptr<Client>& client, const std::string& roomId) {
    if (!client || !client->roomsCapable) return;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().Find(roomId);
        if (!room) return;
        lines = RoomInfoLinesLocked(*room, client->nick);
    }
    for (const std::string& line : lines) client->SendLine(line);
}

/** 群成员表（一行一个），末尾跟 RMEMBERS_END。 */
void SendRoomMembersTo(const std::shared_ptr<Client>& client, const std::string& roomId) {
    if (!client || !client->roomsCapable) return;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().Find(roomId);
        if (!room) return;
        for (const dchat::RoomMember& member : room->members) {
            lines.push_back(dchat::BuildLine("RMEMBERS", roomId + " " + member.nick + " " +
                                                             dchat::RoomRoleText(member.role) + " " +
                                                             dchat::Base64Encode(member.displayName)));
        }
    }
    for (const std::string& line : lines) client->SendLine(line);
    client->SendLine(dchat::BuildLine("RMEMBERS_END", roomId));
}

/**
 * 房间的成员变了之后，把新状态推给**还留在群里的所有人**。
 *
 * 只推给"新来的 / 走掉的那个人"是不够的：群主那边的成员**表和数**都会停在旧值
 * （自动化联调实测：同意 bob 入群之后，群主看到的成员列表里根本没有 bob，
 * 于是"设为管理员 / 踢人"两个按钮都点不到）。入群、退群、踢人三处都要调它。
 *
 * **必须在锁外面调**：SendRoomInfoTo / SendRoomMembersTo 内部自己也要拿
 * g_roomsMutex，而 std::mutex 不是递归的 —— 持着锁进去就是自己等自己。
 */
void NotifyRoomMembersChanged(const std::string& roomId, const Client* except = nullptr) {
    const std::vector<std::shared_ptr<Client>> online = ClientSnapshot();
    std::vector<std::shared_ptr<Client>> members;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().Find(roomId);
        if (room != nullptr) {
            for (const auto& other : online) {
                if (!other->roomsCapable) continue;
                if (except != nullptr && other.get() == except) continue;
                if (dchat::IsRoomMember(*room, other->nick)) members.push_back(other);
            }
        }
    }
    for (const auto& member : members) {
        SendRoomInfoTo(member, roomId);
        SendRoomMembersTo(member, roomId);
    }
}

/**
 * 会话列表。只发"有哪些会话 + 每个会话的基本信息"，**不发历史**：
 * 一个人可能在几十个群里，登录时把每个群的历史都推一遍既慢又没意义。
 * 历史等他切到那个会话（ROOM）时再按需拉。
 */
void SendRoomList(const std::shared_ptr<Client>& client) {
    if (!client || client->nick.empty() || !client->roomsCapable) return;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        for (const dchat::Room* room : RoomsLocked().RoomsOf(client->nick)) {
            dchat::RoomRole role = dchat::RoomRole::Member;
            dchat::TryRoleOf(*room, client->nick, &role);
            lines.push_back(dchat::BuildLine(
                "ROOMLIST",
                room->id + " " + dchat::RoomKindText(room->kind) + " " +
                    dchat::Base64Encode(dchat::RoomRegistry::DisplayNameFor(*room, client->nick)) +
                    " " + std::to_string(room->members.size()) + " " +
                    std::to_string(room->maxMembers) + " " + dchat::RoomRoleText(role)));
        }
    }
    for (const std::string& line : lines) client->SendLine(line);
    client->SendLine(dchat::BuildLine("ROOMLIST_END"));
}

/** 切到某个会话时回放它最近的历史（旧 -> 新），末尾跟 RHIST_END。 */
// 会话里最多回放多少条历史。刻意不大：切换会话是高频操作，一次推几千条
// 会让界面卡住、也白烧流量。真要翻更早的记录，那是"加载更多"该干的事。
constexpr std::size_t kRoomHistoryReplay = 200;

/** 给一个房间里所有在线的群主/管理员发一条通知（入群申请之类）。 */
void NotifyRoomAdmins(const std::string& roomId, const std::string& line) {
    std::vector<std::string> admins;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().Find(roomId);
        if (!room) return;
        for (const dchat::RoomMember& member : room->members) {
            if (dchat::RoomRoleCanAdminister(member.role)) admins.push_back(member.nick);
        }
    }
    if (admins.empty()) return;
    for (const auto& target : ClientSnapshot()) {
        if (target->nick.empty() || !target->roomsCapable) continue;
        if (std::find(admins.begin(), admins.end(), target->nick) == admins.end()) continue;
        target->SendLine(line);
    }
}

/**
 * 播一条房间内事件（谁加入了、谁退出了、谁被踢了、设置改了什么）。
 *
 * 大厅用 **SYS** 而不是 REVENT：大厅的消息要发给所有人，包括老客户端，
 * 而老客户端不认识 REVENT。SYS 是它们本来就认识、会当系统提示显示的命令，
 * 所以给大厅发 SYS 既保持了老行为，又不会给旧客户端塞未知命令。
 */
void BroadcastRoomEvent(const std::string& roomId, const std::string& text) {
    const std::string line = (roomId == dchat::kHallId) ? Timed("SYS", text)
                                                        : Timed("REVENT", roomId + " " + text);
    BroadcastRoom(roomId, line);
    RememberHistory(roomId, line);
}

/**
 * 检查"我能不能对这个房间行使管理员权限"。
 * 不通过时直接回一条 ERROR 并返回 false，调用方 `return true` 即可。
 */
bool RequireRoomAdmin(const std::shared_ptr<Client>& client, const std::string& roomId,
                      dchat::RoomRole* role) {
    if (!client || client->nick.empty()) return false;
    dchat::RoomRole found = dchat::RoomRole::Member;
    bool exists = false;
    bool member = false;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().Find(roomId);
        if (room) {
            exists = true;
            member = dchat::TryRoleOf(*room, client->nick, &found);
        }
    }
    if (!exists) {
        client->SendLine(Timed("ERROR", "找不到这个群：" + roomId));
        return false;
    }
    if (!member) {
        client->SendLine(Timed("ERROR", "你不在这个群里"));
        return false;
    }
    if (!dchat::RoomRoleCanAdminister(found)) {
        client->SendLine(Timed("ERROR", "只有群主和管理员能做这件事"));
        return false;
    }
    if (role) *role = found;
    return true;
}

void ReplayRoomHistoryTo(const std::shared_ptr<Client>& client, const std::string& roomId,
                         std::size_t limit) {
    if (!client || !client->roomsCapable) return;
    const std::vector<dchat::HistoryEntry> entries = RoomRecentHistory(roomId, limit);
    for (const dchat::HistoryEntry& entry : entries) {
        client->SendLine(dchat::BuildLine("RHIST", roomId + " " + std::to_string(entry.seq) + " " +
                                                       entry.line));
    }
    client->SendLine(dchat::BuildLine("RHIST_END", roomId));
}

/** 这个房间还留着的附件卡片（切进会话时一并给他，否则他看不到群文件）。 */
void SendRoomFilesTo(const std::shared_ptr<Client>& client, const std::string& roomId) {
    if (!client || !client->roomsCapable) return;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        for (const auto& file : g_files) {
            if (file->roomId != roomId) continue;
            const std::string args = file->owner + " " + file->id + " " + file->nameB64 + " " +
                                     std::to_string(file->size) + " " +
                                     (file->thumb.empty() ? "0" : "1");
            lines.push_back((roomId == dchat::kHallId)
                                ? Timed("FILE_OFFER", args)
                                : Timed("RFILE_OFFER", roomId + " " + args));
        }
    }
    for (const std::string& line : lines) client->SendLine(line);
}

/** 把一个房间的完整现状发给一个人：会话信息 + 成员 + 历史 + 还留着的附件。 */
void SendWholeRoomTo(const std::shared_ptr<Client>& client, const std::string& roomId) {
    SendRoomInfoTo(client, roomId);
    SendRoomMembersTo(client, roomId);
    ReplayRoomHistoryTo(client, roomId, kRoomHistoryReplay);
    SendRoomFilesTo(client, roomId);
}

/**
 * 这条命令需要客户端声明过 rooms1。
 * 老客户端不该也不会发这些命令；万一有人手打，给一条明确的报错比默默忽略好 ——
 * 默默忽略会让人以为"服务器坏了"。
 */
bool RequireRoomsFeature(const std::shared_ptr<Client>& client) {
    if (client->nick.empty()) {
        client->SendLine(Timed("ERROR", "请先登录"));
        return false;
    }
    if (!client->roomsCapable) {
        client->SendLine(Timed("ERROR", "你的客户端版本不支持群聊/私聊，请更新到最新版客户端"));
        return false;
    }
    return true;
}

/** 把 Base64 的参数解成文本；解不开返回 false。 */
bool DecodeTextArg(const std::string& text, std::string* out) {
    std::vector<unsigned char> bytes;
    if (!dchat::Base64Decode(text, &bytes)) return false;
    out->assign(bytes.begin(), bytes.end());
    return true;
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

    // ---- 多会话：登记进大厅，然后把会话列表发过去 ----
    // 大厅的成员表就是"当前在线的人"：登录时加入、断开时移除（见 RemoveClient）。
    // 它**不落盘** —— 大厅本来就是所有在线的人，存一份历史成员名单没有意义，
    // 而且会长到几万个名字。老客户端也照样登记：它们只在大厅，但服务端统计
    // 大厅人数、以后给人发群邀请都要用这张表。
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        dchat::Room* hall = RoomsLocked().Hall();
        if (hall && !dchat::IsRoomMember(*hall, nick)) {
            dchat::RoomMember member;
            member.nick = nick;
            hall->members.push_back(member);
        }
    }

    // 先告诉客户端"服务端支持多会话"，再发列表。客户端必须以 FEATURES 为准：
    // 没有 rooms1 就退回"只有大厅"的老界面（连的是旧版服务端时会发生）。
    if (client->roomsCapable) {
        client->SendLine(dchat::BuildLine("FEATURES", "rooms1"));
        SendRoomList(client);
    }

    ReplayHistoryTo(client);                                // keepchathistory 打开时补大厅历史
    Log(client->address + " joined as " + nick);
    return true;
}

// 返回 false 表示客户端要求断开
/** 去掉首尾的 ASCII 空格 / Tab（昵称前后多打一个空格不该算错）。 */
std::string TrimNick(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

/**
 * 建立（或复用）与 peer 的私聊会话。
 *
 * 抽成函数是因为它有**两个入口**：
 *   1. 协议命令 `DM <昵称>`：自动化脚本、以及别的客户端实现走这条；
 *   2. 聊天框里的 `/dm <昵称>`：**用户真正的入口**（见 MSG 分支里的说明）。
 * 两处各写一遍的话，迟早有一边漏掉"拉黑拦截"或"顺手切当前会话"这种细节。
 */
bool OpenDirectMessage(const std::shared_ptr<Client>& client, const std::string& rawPeer) {
    if (!RequireRoomsFeature(client)) return true;
    const std::string peer = TrimNick(rawPeer);
    if (peer.empty()) {
        client->SendLine(Timed("ERROR", "用法：/dm <昵称>"));
        return true;
    }
    if (peer == client->nick) {
        client->SendLine(Timed("ERROR", "不能和自己私聊"));
        return true;
    }
    if (!UserExists(peer, nullptr)) {
        client->SendLine(Timed("ERROR", "没有这个账号：" + peer));
        return true;
    }
    // 拉黑是双向生效的，连会话都不该给建 —— 建了之后发什么都发不出去，
    // 用户只会觉得"私聊坏了"
    if (DirectBlocked(client->nick, peer)) {
        client->SendLine(Timed("ERROR", "你们之间有一个人把对方拉黑了，私聊发不出去"));
        return true;
    }
    std::string roomId;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        const dchat::Room* room = RoomsLocked().EnsureDmRoom(client->nick, peer);
        if (!room) {
            client->SendLine(Timed("ERROR", "无法建立私聊会话"));
            return true;
        }
        roomId = room->id;
    }
    SaveRooms();  // 新会话要落盘，否则重启后这个私聊就没了、历史变成孤儿文件
    // **顺手把当前会话切过去**。不切的话，客户端点开某人的私聊之后紧接着发的
    // 那句会掉进大厅（实测踩过：私聊里发"私聊一句"，大厅里所有人都看到了）。
    // 客户端再补一条 ROOM 也救不了 —— 消息已经发出去了。
    client->currentRoom = roomId;
    client->SendLine(dchat::BuildLine("GDMLINK", roomId + " " + peer));
    SendWholeRoomTo(client, roomId);
    // 对方在线的话，让他那边立刻多出这个会话，不用等他重新登录
    for (const auto& other : ClientSnapshot()) {
        if (other.get() == client.get() || other->nick != peer) continue;
        if (!other->roomsCapable) continue;
        other->SendLine(dchat::BuildLine("GDMLINK", roomId + " " + client->nick));
        SendWholeRoomTo(other, roomId);
    }
    Log("dm opened: " + client->nick + " <-> " + peer + " (" + roomId + ")");
    return true;
}

/**
 * 给自己在**当前会话**里设群昵称（聊天指令 `/nick <群昵称>` 的实现）。
 *
 * 为什么要加这条指令：协议命令 `GNICK` 需要 `<房间ID> <昵称> <群昵称(Base64)>`，
 * 而**客户端从来不发送它** —— 也就是说"成员群昵称"这个设置项在界面上根本没有入口，
 * 谁也改不了。走聊天指令这条路三端通用，客户端一行都不用改。
 *
 * 空参数 = 清掉群昵称（回到账号名）。
 */
bool SetOwnRoomNick(const std::shared_ptr<Client>& client, const std::string& rawDisplay) {
    if (!RequireRoomsFeature(client)) return true;
    const std::string roomId = client->currentRoom;
    if (roomId.empty() || roomId == dchat::kHallId) {
        client->SendLine(Timed("ERROR", "大厅没有群昵称这回事；先切到一个群聊再设"));
        return true;
    }
    const std::string display = TrimNick(rawDisplay);
    std::string error;
    bool ok = false;
    std::string displayLine;
    {
        std::lock_guard<std::mutex> lock(g_roomsMutex);
        dchat::Room* room = RoomsLocked().Find(roomId);
        if (room != nullptr) {
            ok = dchat::SetMemberDisplayName(room, client->nick, display, &error);
            if (ok) {
                const dchat::RoomMember* member = dchat::FindMember(*room, client->nick);
                if (member != nullptr) {
                    displayLine = dchat::BuildLine(
                        "RMEMBERS", roomId + " " + member->nick + " " +
                                        dchat::RoomRoleText(member->role) + " " +
                                        dchat::Base64Encode(member->displayName));
                }
            }
        } else {
            error = "找不到这个会话：" + roomId;
        }
    }
    if (!ok) {
        client->SendLine(Timed("ERROR", error.empty() ? "改群昵称失败" : error));
        return true;
    }
    SaveRooms();
    // 只广播这一个成员的那一行，不用把整张成员表重发一遍（和 GNICK 同一套做法）
    if (!displayLine.empty()) BroadcastRoom(roomId, displayLine);
    client->SendLine(Timed("SYS", display.empty() ? "群昵称已清掉（回到账号名）"
                                                  : "群昵称已设为「" + display + "」"));
    return true;
}

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

        // ---- 可选的第 4 个字段：逗号分隔的能力标签 ----
        // **老客户端只有 3 个字段**，所以它们永远不会进多会话模式：服务端只给它们
        // 推大厅消息、只发 SAY。这是"新服务端 + 正在用的旧客户端"能安全共存的关键，
        // 也是这次改造里最不能省的一步。
        //
        // 放在握手**成功之后**才置位：握手失败（公钥不合法之类）的连接不该
        // 因为报了个能力就被当成新客户端。
        if (words.size() >= 4) {
            const std::string& caps = words[3];
            std::size_t start = 0;
            while (start <= caps.size()) {
                const std::size_t comma = caps.find(',', start);
                const std::string tag =
                    (comma == std::string::npos) ? caps.substr(start)
                                                 : caps.substr(start, comma - start);
                if (tag == "rooms1") client->roomsCapable = true;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (client->roomsCapable) {
            Log("client supports rooms1: " + client->address);
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
            // ---- 注册路径防护（规则 registerinterval / maxaccounts）----
            // 这两条是 2026-10 实测之后补的：在此之前注册路径**完全没有限制**，
            // 换 IP 就能绕开所有基于 IP 的防护（maxconnsperip / loginfails），
            // 12 线程并发注册能跑到 363 个/秒并把正常用户的消息往返拖到超时。
            {
                const dchat::ServerRules rules = CurrentRules();
                // 先看这个 IP 距上次注册过了多久。没有记录 = 这个 IP 没注册过 -> 放行
                long long sinceLast = -1;
                {
                    std::lock_guard<std::mutex> lock(g_registerMutex);
                    const auto it = g_lastRegister.find(client->ip);
                    if (it != g_lastRegister.end()) {
                        sinceLast = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now() - it->second)
                                        .count();
                    }
                }
                std::size_t accountCount = 0;
                {
                    std::lock_guard<std::mutex> lock(g_usersMutex);
                    accountCount = g_users.size();
                }
                const dchat::RegisterDeny deny = dchat::CheckRegisterAllowed(
                    accountCount, rules.maxAccounts, sinceLast, rules.registerIntervalSec);
                if (deny != dchat::RegisterDeny::None) {
                    client->SendLine(Timed(
                        "ERROR",
                        dchat::RegisterDenyText(
                            deny, rules.maxAccounts,
                            dchat::RegisterWaitSeconds(sinceLast, rules.registerIntervalSec),
                            rules.registerIntervalSec)));
                    Log(std::string("register rejected (") +
                        (deny == dchat::RegisterDeny::AccountFull ? "accounts full"
                                                                  : "too soon") +
                        "): " + client->ip + " -> " + name);
                    if (deny == dchat::RegisterDeny::AccountFull) {
                        client->SendLine(Timed("SYS", "服务器已满，请联系管理员"));
                    }
                    return true;
                }
            }
            if (UserExists(name, nullptr)) {
                client->SendLine(Timed("ERROR", "用户名已存在，请到「登录」界面直接登录"));
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(g_usersMutex);
                g_users.push_back(dchat::MakeUser(name, words[1]));
            }
            InvalidateKnownNames();  // 名字表变了，下次广播要重算
            {
                std::lock_guard<std::mutex> lock(g_registerMutex);
                g_lastRegister[client->ip] = std::chrono::steady_clock::now();
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
        // `/dm <昵称>`：**用户真正能用的发起私聊入口**。
        //
        // 为什么不在 server_command.cpp 那张斜杠指令表里加一条：那张表刻意不依赖服务端
        // 状态（房间注册表、拉黑名单、在线名单都在本文件里）。塞进去要么得把状态透出去、
        // 要么还得在这里再派发一次，不划算。
        // 放在 MSG 这条路上还有个好处 —— **三端通用**：客户端只管把输入框的内容当文本
        // 发出来，桌面端和安卓端都不用各自加界面按钮。
        //
        // 背景：客户端从来不发协议命令 `DM`（它只在收到 GDMLINK 时才认识私聊），
        // 所以在这条路之前，"私聊只能被别人发起、自己发不起来" —— 这是补上的缺口。
        {
            std::string head = msg.rest;
            const std::size_t space = head.find(' ');
            std::string name = (space == std::string::npos) ? head : head.substr(0, space);
            const std::string argument =
                (space == std::string::npos) ? std::string() : head.substr(space + 1);
            for (char& ch : name) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (name == "/dm" || name == "/msg" || name == "/pm" || name == "/tell") {
                return OpenDirectMessage(client, argument);
            }
            // `/nick <群昵称>`：和 `/dm` 同一个理由 —— 协议命令 GNICK 没有界面入口，
            // 而"成员群昵称"是明确要求有的功能。空参数 = 清掉。
            if (name == "/nick" || name == "/nickname" || name == "/mynick") {
                return SetOwnRoomNick(client, argument);
            }
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

        // ---- 发到「当前会话」----
        // 默认是大厅。老客户端从不发 ROOM，所以永远走大厅分支，
        // 发出去的那一行和升级前**逐字节一致**（SAY，全量广播）。
        const std::string roomId = client->currentRoom;
        dchat::RoomRole role = dchat::RoomRole::Member;
        bool inRoom = false;
        bool importantOn = true;
        std::string dmPeer;  // 私聊时的对方；空表示这不是私聊
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            const dchat::Room* room = RoomsLocked().Find(roomId);
            if (room && dchat::TryRoleOf(*room, client->nick, &role)) {
                inRoom = true;
                importantOn = room->importantNotice;
                if (room->kind == dchat::RoomKind::Direct) {
                    for (const dchat::RoomMember& member : room->members) {
                        if (member.nick != client->nick) dmPeer = member.nick;
                    }
                }
            }
        }
        if (!inRoom) {
            // 会话被解散、或者自己被踢了：切回大厅而不是把消息默默丢掉 ——
            // 默默丢掉会让用户以为"发出去了但没人理我"
            client->SendLine(Timed("ERROR", "你不在这个会话里了（可能已被移出或群已解散），已切回大厅"));
            client->currentRoom = dchat::kHallId;
            return true;
        }
        // 私聊黑名单：任意一方拉黑了对方，这条消息就不该发出去。
        // 发出去再让对方客户端丢掉是不行的 —— 那样服务端历史里留着、
        // 对方换个客户端就看得见，等于没拦。
        if (!dmPeer.empty() && DirectBlocked(client->nick, dmPeer)) {
            client->SendLine(Timed("ERROR", "你们之间有一个人把对方拉黑了，私聊发不出去"));
            return true;
        }

        // @全体成员 是唯一能越过所有人免打扰的东西，**必须由服务端把关**：
        // 做成纯客户端约定等于没约束 —— 任何改过的客户端都能随便发。
        // 大厅没有群主，那里只有服务器管理员（/op）能发。
        if (dchat::ContainsAllMention(msg.rest)) {
            if (!importantOn) {
                client->SendLine(
                    Timed("ERROR", "这个群关闭了「消息重要提醒」，现在谁都发不了 @全体成员"));
                return true;
            }
            const bool allowed = (roomId == dchat::kHallId) ? IsOp(client->nick)
                                                            : dchat::RoomRoleCanAdminister(role);
            if (!allowed) {
                client->SendLine(
                    Timed("ERROR", "@全体成员 只有群主和管理员能发（大厅里只有服务器管理员能发）"));
                return true;
            }
        }

        const bool hall = (roomId == dchat::kHallId);
        const std::string sayLine =
            hall ? Timed("SAY", client->nick + " " + msg.rest)
                 : Timed("RMSG", roomId + " " + client->nick + " " + msg.rest);
        BroadcastRoom(roomId, sayLine);  // 连发送者一起发，便于回显
        RememberHistory(roomId, sayLine);
        Log("MSG " + client->nick + (hall ? ": " : " [" + roomId + "]: ") + msg.rest);
        return true;
    }

    // =======================================================================
    // 多会话：私聊 / 群聊
    // =======================================================================
    // 这一整块只有声明过 rooms1 的客户端才会走到。老客户端不发这些命令，
    // 也不会收到它们发出的任何消息（大厅那部分走的是原路径，一个字节没变）。

    if (msg.command == "ROOMS") {
        if (!RequireRoomsFeature(client)) return true;
        SendRoomList(client);
        return true;
    }

    // FEATURES <能力列表>：客户端声明自己支持哪些能力。
    //
    // 为什么除了 HELLO 的第 4 个字段还要有这么一条：HELLO 是**加密握手**，
    // 把能力声明绑死在那里，等于"不支持加密的客户端永远用不了群聊"，
    // 而且任何针对多会话的自动化测试都得先实现一遍 ECDH 才能开口说话。
    // 这条命令让能力声明和加密彻底解耦。
    //
    // 安全上不用担心：roomsCapable 只决定服务端**多发**哪些消息给这个连接，
    // 每一个群操作本身都会另外校验成员身份和角色，谎报能力换不到任何权限。
    if (msg.command == "FEATURES") {
        const std::vector<std::string> words = msg.Words();
        if (words.empty()) {
            client->SendLine(Timed("ERROR", "用法：FEATURES <能力列表>"));
            return true;
        }
        const bool wasCapable = client->roomsCapable;
        std::size_t start = 0;
        const std::string& caps = words[0];
        while (start <= caps.size()) {
            const std::size_t comma = caps.find(',', start);
            const std::string tag =
                (comma == std::string::npos) ? caps.substr(start) : caps.substr(start, comma - start);
            if (tag == "rooms1") client->roomsCapable = true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (client->roomsCapable) {
            client->SendLine(dchat::BuildLine("FEATURES", "rooms1"));
            // 已经登录了才补发会话列表；没登录的话，登录流程自己会发
            if (!wasCapable && !client->nick.empty()) SendRoomList(client);
        }
        return true;
    }

    if (msg.command == "ROOM") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：ROOM <房间ID>"));
            return true;
        }
        bool member = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            const dchat::Room* room = RoomsLocked().Find(words[0]);
            member = room != nullptr && dchat::IsRoomMember(*room, client->nick);
        }
        if (!member) {
            client->SendLine(Timed("ERROR", "你不在这个会话里：" + words[0]));
            return true;
        }
        client->currentRoom = words[0];
        SendWholeRoomTo(client, words[0]);
        return true;
    }

    if (msg.command == "DM") {
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：DM <昵称>"));
            return true;
        }
        return OpenDirectMessage(client, words[0]);
    }

    if (msg.command == "GCREATE") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 2) {
            client->SendLine(Timed("ERROR", "用法：GCREATE <群聊ID> <群名(Base64)>"));
            return true;
        }
        dchat::RoomIdError idError = dchat::RoomIdError::None;
        const std::string id = dchat::NormalizeGroupId(words[0], &idError);
        if (id.empty()) {
            client->SendLine(Timed("ERROR", dchat::RoomIdErrorText(idError)));
            return true;
        }
        std::string name;
        if (!DecodeTextArg(words[1], &name)) {
            client->SendLine(Timed("ERROR", "群名编码不对"));
            return true;
        }
        std::string nameError;
        if (!dchat::ValidateRoomText(name, dchat::kMaxRoomNameChars, &nameError, "群名")) {
            client->SendLine(Timed("ERROR", nameError));
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            if (RoomsLocked().Exists(id)) {
                client->SendLine(Timed("ERROR", "群聊 ID「" + id + "」已经被占用了，换一个"));
                return true;
            }
            if (!RoomsLocked().CreateGroup(id, name, client->nick)) {
                client->SendLine(Timed("ERROR", "建群失败"));
                return true;
            }
        }
        SaveRooms();
        client->currentRoom = id;
        BroadcastRoomEvent(id, client->nick + " 创建了群聊「" + name + "」");
        client->SendLine(Timed("SYS", "群聊「" + name + "」创建成功，群聊 ID 是 " + id +
                                         "（ID 不可更改，请记好）"));
        SendRoomList(client);  // 列表里多了一个群
        SendWholeRoomTo(client, id);
        Log("group created: " + id + " by " + client->nick);
        return true;
    }

    if (msg.command == "GSEARCH") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：GSEARCH <关键词(Base64)>"));
            return true;
        }
        std::string keyword;
        if (!DecodeTextArg(words[0], &keyword)) {
            client->SendLine(Timed("ERROR", "关键词编码不对"));
            return true;
        }
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            for (const dchat::Room* room : RoomsLocked().Search(keyword, 30)) {
                lines.push_back(dchat::BuildLine(
                    "GHIT", room->id + " " + dchat::Base64Encode(room->name) + " " +
                                std::to_string(room->members.size()) + " " +
                                std::to_string(room->maxMembers) + " " +
                                dchat::JoinModeText(room->joinMode)));
            }
        }
        // 搜不到时最先要问的两件事：服务端收到的关键词是什么、匹配到几条。
        // 自动化联调里少了这行日志就只能靠猜 —— 客户端那边只看得到"0 条"。
        Log("gsearch from " + client->nick + ": '" + keyword + "' -> " +
            std::to_string(lines.size()) + " hit(s)");
        for (const std::string& line : lines) client->SendLine(line);
        client->SendLine(dchat::BuildLine("GHIT_END"));
        return true;
    }

    if (msg.command == "GJOIN") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：GJOIN <房间ID>"));
            return true;
        }
        const std::string roomId = words[0];
        dchat::JoinResult result = dchat::JoinResult::NotJoinable;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (!room) {
                client->SendLine(Timed("ERROR", "找不到这个群：" + roomId));
                return true;
            }
            result = dchat::RequestJoin(room, client->nick);
        }
        using dchat::JoinResult;
        if (result == JoinResult::Joined || result == JoinResult::AutoJoined) {
            SaveRooms();
            client->SendLine(Timed("SYS", dchat::JoinResultText(result)));
            BroadcastRoomEvent(roomId, client->nick + " 加入了群聊");
            SendRoomList(client);
            // 自动审核也要给管理员一条通知。不给的话"自动审核"和"直接添加"
            // 在管理员眼里完全没差别，这个选项就白设了。
            if (result == JoinResult::AutoJoined) {
                NotifyRoomAdmins(roomId, Timed("REVENT", roomId + " " + client->nick +
                                                             " 通过「自动审核」加入了群聊"));
            }
            Log("group join: " + client->nick + " -> " + roomId);
        } else if (result == JoinResult::Queued) {
            SaveRooms();  // 待审队列也要落盘，否则重启后申请就凭空消失了
            client->SendLine(Timed("SYS", dchat::JoinResultText(result)));
            client->SendLine(Timed("SYS", "已通知群管理员，通过后你会在会话列表里看到这个群"));
            NotifyRoomAdmins(roomId,
                             Timed("GREQ", roomId + " " + client->nick));
        } else {
            client->SendLine(Timed("ERROR", dchat::JoinResultText(result)));
        }
        return true;
    }

    if (msg.command == "GLEAVE") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：GLEAVE <房间ID>"));
            return true;
        }
        const std::string roomId = words[0];
        if (roomId == dchat::kHallId) {
            client->SendLine(Timed("ERROR", "大厅不能退出 —— 它是所有人的公共房间"));
            return true;
        }
        std::string newOwner;
        bool left = false;
        bool isGroup = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (room) {
                isGroup = room->kind == dchat::RoomKind::Group;
                left = dchat::LeaveRoom(room, client->nick, &newOwner);
            }
        }
        if (!left) {
            client->SendLine(Timed(
                "ERROR", isGroup ? "退群失败：你可能不在这个群里；如果你是群主而群里只剩你一个人，"
                                   "那就没人能接手这个群，退不了"
                                 : "这个会话不能退出（私聊只能删本地记录）"));
            return true;
        }
        SaveRooms();
        if (client->currentRoom == roomId) client->currentRoom = dchat::kHallId;
        BroadcastRoomEvent(roomId, client->nick + " 退出了群聊");
        if (!newOwner.empty()) {
            BroadcastRoomEvent(roomId, "群主已自动转交给 " + newOwner);
        }
        client->SendLine(Timed("SYS", "已退出群聊" + (newOwner.empty()
                                                          ? std::string()
                                                          : "，群主已转交给 " + newOwner)));
        SendRoomList(client);
        // 群里**剩下的人**那边的成员数也要跟着变。原先只给退群者发了新名单，
        // 于是留在群里的人看到的还是旧人数（自动化联调实测：bob 退群之后，
        // alice 那边的 room1 成员数还是 2）。和"同意入群"是同一类问题。
        // 群里剩下的人：成员**表和数**都要跟着变（正文见函数的注释）
        NotifyRoomMembersChanged(roomId, client.get());
        Log("group leave: " + client->nick + " <- " + roomId);
        return true;
    }

    if (msg.command == "GSET") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() < 3) {
            client->SendLine(Timed("ERROR", "用法：GSET <房间ID> <设置名> <值>"));
            return true;
        }
        const std::string roomId = words[0];
        dchat::RoomSettingKey key = dchat::RoomSettingKey::Name;
        if (!dchat::ParseRoomSettingKey(words[1], &key)) {
            std::string names;
            for (dchat::RoomSettingKey each : dchat::AllRoomSettingKeys()) {
                if (!names.empty()) names += " / ";
                names += dchat::RoomSettingKeyText(each);
            }
            client->SendLine(Timed("ERROR", "不认识的设置项：" + words[1] + "（可用：" + names + "）"));
            return true;
        }
        if (!RequireRoomAdmin(client, roomId, nullptr)) return true;

        // 群名和公告可能含空格，走 Base64；其余设置项都是不含空格的纯值
        std::string value = words[2];
        if (key == dchat::RoomSettingKey::Name || key == dchat::RoomSettingKey::Announcement) {
            if (!DecodeTextArg(words[2], &value)) {
                client->SendLine(Timed("ERROR", "文本编码不对"));
                return true;
            }
        }

        dchat::RoomSettingChange change;
        std::string broadcastLine;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (!room) {
                client->SendLine(Timed("ERROR", "找不到这个群：" + roomId));
                return true;
            }
            change = dchat::ApplyRoomSetting(room, key, value);
            if (change.ok && change.changed) broadcastLine = dchat::RoomSettingLine(*room, key);
        }
        if (!change.ok) {
            client->SendLine(Timed("ERROR", change.text));
            return true;
        }
        if (!change.changed) {
            client->SendLine(Timed("SYS", "没有变化"));
            return true;
        }
        SaveRooms();
        // 缓存预算改了要**立刻**生效。不重算的话，管理员把 cachemaxmb 从 1GB 调到
        // 1MB，界面显示 1MB 而磁盘占用纹丝不动 —— 那个设置项就成了摆设。
        if (key == dchat::RoomSettingKey::CacheMaxMb) EnforceTempBudget();
        client->SendLine(Timed("SYS", change.text));
        if (!broadcastLine.empty()) BroadcastRoom(roomId, broadcastLine);
        if (key == dchat::RoomSettingKey::Announcement) {
            BroadcastRoomEvent(roomId, "群公告已更新");
        }
        Log("group set: " + roomId + " " + words[1] + " by " + client->nick);
        return true;
    }

    if (msg.command == "GMEMBER") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 3) {
            client->SendLine(Timed("ERROR", "用法：GMEMBER <房间ID> <昵称> <owner|admin|member>"));
            return true;
        }
        const std::string roomId = words[0];
        const std::string target = words[1];
        dchat::RoomRole role = dchat::RoomRole::Member;
        if (!dchat::ParseRoomRole(words[2], &role)) {
            client->SendLine(Timed("ERROR", "角色只能是 owner / admin / member"));
            return true;
        }
        dchat::RoomRole myRole = dchat::RoomRole::Member;
        if (!RequireRoomAdmin(client, roomId, &myRole)) return true;
        // 设/撤管理员是**群主专属**：管理员能互相任免的话，一个管理员就能把
        // 其他人全撤掉、把群主架空
        if (myRole != dchat::RoomRole::Owner) {
            client->SendLine(Timed("ERROR", "只有群主能设/撤管理员"));
            return true;
        }
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            ok = room && dchat::SetMemberRole(room, target, role, &error);
        }
        if (!ok) {
            client->SendLine(Timed("ERROR", error.empty() ? "设置失败" : error));
            return true;
        }
        SaveRooms();
        BroadcastRoomEvent(roomId, target + (role == dchat::RoomRole::Admin
                                                 ? " 被设为管理员"
                                                 : " 的管理员权限已被取消"));
        // 角色变了：**群里所有人都要拿到新的成员表**。原先只回发给操作的管理员
        // 自己，被改的那个人那边看不到 —— 自动化联调实测：把 bob 设为管理员之后，
        // bob 自己界面上显示的还是"成员"。
        NotifyRoomMembersChanged(roomId);
        return true;
    }

    if (msg.command == "GNICK") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 3) {
            client->SendLine(Timed("ERROR", "用法：GNICK <房间ID> <昵称> <群昵称(Base64)>"));
            return true;
        }
        const std::string roomId = words[0];
        const std::string target = words[1];
        std::string display;
        if (!DecodeTextArg(words[2], &display)) {
            client->SendLine(Timed("ERROR", "群昵称编码不对"));
            return true;
        }
        // 改自己不需要权限（这是每个成员自己的事）；改别人要管理员
        if (target != client->nick) {
            if (!RequireRoomAdmin(client, roomId, nullptr)) return true;
        }
        std::string error;
        bool ok = false;
        std::string displayLine;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (room) {
                ok = dchat::SetMemberDisplayName(room, target, display, &error);
                if (ok) {
                    const dchat::RoomMember* member = dchat::FindMember(*room, target);
                    if (member) {
                        displayLine = dchat::BuildLine(
                            "RMEMBERS", roomId + " " + member->nick + " " +
                                            dchat::RoomRoleText(member->role) + " " +
                                            dchat::Base64Encode(member->displayName));
                    }
                }
            } else {
                error = "找不到这个群：" + roomId;
            }
        }
        if (!ok) {
            client->SendLine(Timed("ERROR", error.empty() ? "改群昵称失败" : error));
            return true;
        }
        SaveRooms();
        // 只广播这一个成员的那一行，不用把整张成员表重发一遍
        if (!displayLine.empty()) BroadcastRoom(roomId, displayLine);
        if (target == client->nick) client->SendLine(Timed("SYS", "群昵称已更新"));
        return true;
    }

    if (msg.command == "GKICK") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 2) {
            client->SendLine(Timed("ERROR", "用法：GKICK <房间ID> <昵称>"));
            return true;
        }
        const std::string roomId = words[0];
        const std::string target = words[1];
        dchat::RoomRole myRole = dchat::RoomRole::Member;
        if (!RequireRoomAdmin(client, roomId, &myRole)) return true;
        if (target == client->nick) {
            client->SendLine(Timed("ERROR", "不能踢自己 —— 想离开请用退出群聊"));
            return true;
        }
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (room) {
                dchat::RoomRole targetRole = dchat::RoomRole::Member;
                const bool targetIsMember = dchat::TryRoleOf(*room, target, &targetRole);
                if (targetIsMember && dchat::RoomRoleCanAdminister(targetRole) &&
                    myRole != dchat::RoomRole::Owner) {
                    // 管理员之间不能互相踢：否则两个管理员可以互相踢来踢去，
                    // 而且谁先动手谁赢
                    error = "只有群主能踢管理员";
                } else {
                    ok = dchat::KickMember(room, target, &error);
                }
            } else {
                error = "找不到这个群：" + roomId;
            }
        }
        if (!ok) {
            client->SendLine(Timed("ERROR", error.empty() ? "踢人失败" : error));
            return true;
        }
        SaveRooms();
        BroadcastRoomEvent(roomId, target + " 被移出了群聊");
        client->SendLine(Timed("SYS", "已把 " + target + " 移出群聊"));
        // 被踢的人：切回大厅 + 刷新会话列表，别让他继续对着一个已经不属于他的会话发言
        for (const auto& other : ClientSnapshot()) {
            if (other->nick != target || !other->roomsCapable) continue;
            if (other->currentRoom == roomId) other->currentRoom = dchat::kHallId;
            other->SendLine(Timed("SYS", "你已被移出群聊「" + roomId + "」"));
            SendRoomList(other);
        }
        // 群里剩下的人：成员**表和数**都要跟着变（和入群 / 退群同一类问题）
        NotifyRoomMembersChanged(roomId);
        Log("group kick: " + target + " from " + roomId + " by " + client->nick);
        return true;
    }

    if (msg.command == "GREQS") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：GREQS <房间ID>"));
            return true;
        }
        const std::string roomId = words[0];
        if (!RequireRoomAdmin(client, roomId, nullptr)) return true;
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            const dchat::Room* room = RoomsLocked().Find(roomId);
            if (room) pending = room->pending;
        }
        if (pending.empty()) {
            client->SendLine(Timed("SYS", "现在没有待审核的入群申请"));
        }
        for (const std::string& nick : pending) {
            client->SendLine(dchat::BuildLine("GREQ", roomId + " " + nick));
        }
        client->SendLine(dchat::BuildLine("GREQ_END", roomId));
        return true;
    }

    if (msg.command == "GREQ") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 3) {
            client->SendLine(Timed("ERROR", "用法：GREQ <房间ID> <昵称> <ok|no>"));
            return true;
        }
        const std::string roomId = words[0];
        const std::string target = words[1];
        const bool accept = (words[2] == "ok" || words[2] == "yes" || words[2] == "1");
        if (!RequireRoomAdmin(client, roomId, nullptr)) return true;
        bool handled = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            dchat::Room* room = RoomsLocked().Find(roomId);
            if (room) handled = dchat::ResolveJoinRequest(room, target, accept);
        }
        if (!handled) {
            client->SendLine(Timed("ERROR", "申请列表里没有 " + target + "（可能已经处理过了）"));
            return true;
        }
        SaveRooms();
        if (accept) {
            BroadcastRoomEvent(roomId, target + " 加入了群聊");
            client->SendLine(Timed("SYS", "已同意 " + target + " 入群"));
            // 成员**表和数**都变了，群里所有人都要拿到新状态（含群主自己 ——
            // 不然他那边成员列表里没有新成员，"设为管理员 / 踢人"根本点不到）。
            NotifyRoomMembersChanged(roomId);
            for (const auto& other : ClientSnapshot()) {
                if (other->nick != target || !other->roomsCapable) continue;
                other->SendLine(Timed("SYS", "你加入群聊的申请已通过"));
                SendRoomList(other);
            }
        } else {
            client->SendLine(Timed("SYS", "已拒绝 " + target + " 的入群申请"));
            NotifyByName(target, "你加入群聊的申请被拒绝了");
        }
        return true;
    }

    if (msg.command == "GDELETE") {
        if (!RequireRoomsFeature(client)) return true;
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：GDELETE <房间ID>"));
            return true;
        }
        const std::string roomId = words[0];
        if (roomId == dchat::kHallId) {
            client->SendLine(Timed("ERROR", "大厅不能删除"));
            return true;
        }
        dchat::RoomRole myRole = dchat::RoomRole::Member;
        if (!RequireRoomAdmin(client, roomId, &myRole)) return true;
        if (myRole != dchat::RoomRole::Owner) {
            client->SendLine(Timed("ERROR", "只有群主能解散群聊"));
            return true;
        }
        std::string historyPath;
        std::vector<std::string> members;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            const dchat::Room* room = RoomsLocked().Find(roomId);
            if (!room) {
                client->SendLine(Timed("ERROR", "找不到这个群：" + roomId));
                return true;
            }
            for (const dchat::RoomMember& member : room->members) members.push_back(member.nick);
            RoomsLocked().Remove(roomId, &historyPath);
        }
        SaveRooms();
        // 磁盘上的东西一起清：历史文件、暂存目录、还有文件池里的登记。
        // 不清的话，重建一个同名群会**继承上一个群的历史** —— 那是最难查的一类串会话。
        if (!historyPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(historyPath, ec);
        }
        ClearRoomFilesDir(roomId);
        {
            std::lock_guard<std::mutex> lock(g_filesMutex);
            for (auto it = g_files.begin(); it != g_files.end();) {
                if ((*it)->roomId == roomId) {
                    it = g_files.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const std::string& nick : members) {
            for (const auto& other : ClientSnapshot()) {
                if (other->nick != nick || !other->roomsCapable) continue;
                if (other->currentRoom == roomId) other->currentRoom = dchat::kHallId;
                other->SendLine(Timed("SYS", "群聊已被解散"));
                SendRoomList(other);
            }
        }
        Log("group deleted: " + roomId + " by " + client->nick);
        return true;
    }

    // ---- 私聊黑名单 ----
    if (msg.command == "BLOCK" || msg.command == "UNBLOCK") {
        if (client->nick.empty()) {
            client->SendLine(Timed("ERROR", "请先登录"));
            return true;
        }
        const std::vector<std::string> words = msg.Words();
        if (words.size() != 1) {
            client->SendLine(Timed("ERROR", "用法：" + msg.command + " <昵称>"));
            return true;
        }
        const std::string target = words[0];
        if (target == client->nick) {
            client->SendLine(Timed("ERROR", "不能拉黑自己"));
            return true;
        }
        if (!UserExists(target, nullptr)) {
            client->SendLine(Timed("ERROR", "没有这个账号：" + target));
            return true;
        }
        if (msg.command == "BLOCK") {
            if (AddBlock(client->nick, target)) {
                client->SendLine(Timed("SYS", "已把 " + target +
                                                 " 加入黑名单：你们之间的私聊双向都不通了"));
                Log("block: " + client->nick + " -> " + target);
            } else {
                client->SendLine(Timed("SYS", target + " 已经在你的黑名单里了"));
            }
        } else {
            if (RemoveBlock(client->nick, target)) {
                client->SendLine(Timed("SYS", "已把 " + target + " 移出黑名单"));
            } else {
                client->SendLine(Timed("SYS", target + " 不在你的黑名单里"));
            }
        }
        // 拉黑 / 解除之后**立刻把新的黑名单推回去**：只回一句 SYS 的话，客户端那边
        // 的黑名单要等下次登录才更新（自动化联调实测：拉黑明明成功了，界面上还是空的）。
        // 复用和 BLOCKS 请求完全相同的一串消息，客户端那边不用改协议。
        const std::vector<std::string> updated = BlockListOf(client->nick);
        for (const std::string& nick : updated) {
            client->SendLine(dchat::BuildLine("GBLOCKS", nick));
        }
        client->SendLine(dchat::BuildLine("GBLOCKS_END"));
        return true;
    }

    if (msg.command == "BLOCKS") {
        if (client->nick.empty()) {
            client->SendLine(Timed("ERROR", "请先登录"));
            return true;
        }
        const std::vector<std::string> list = BlockListOf(client->nick);
        if (list.empty()) {
            client->SendLine(Timed("SYS", "你的黑名单是空的"));
        }
        for (const std::string& nick : list) {
            client->SendLine(dchat::BuildLine("GBLOCKS", nick));
        }
        client->SendLine(dchat::BuildLine("GBLOCKS_END"));
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
            // 建临时文件开始接收：**不再把文件内容攒在内存里**。
            // 这台服务器只有 1.6GB 内存，几个大文件就能把它 OOM 掉。
            // 文件按房间分目录暂存，所以这里要先确认自己在哪个会话、并且还在里面。
            const std::string uploadRoom = client->currentRoom;
            {
                std::lock_guard<std::mutex> lock(g_roomsMutex);
                const dchat::Room* room = RoomsLocked().Find(uploadRoom);
                if (!room || !dchat::IsRoomMember(*room, client->nick)) {
                    client->SendLine(Timed("ERROR", "你不在这个会话里，无法上传文件"));
                    return true;
                }
            }
            if (!EnsureFilesDir(uploadRoom)) {
                client->SendLine(Timed("ERROR", "服务器无法准备暂存目录，上传取消"));
                return true;
            }
            unsigned long long uploadSeq = 0;
            {
                std::lock_guard<std::mutex> lock(g_filesMutex);
                uploadSeq = ++g_uploadSeq;
            }
            const std::string partPath = dchat::RoomRegistry::FilesDirFor(uploadRoom) + "/upload-" +
                                         std::to_string(uploadSeq) + kPartSuffix;
            std::ofstream partFile(partPath, std::ios::binary | std::ios::trunc);
            if (!partFile) {
                client->SendLine(Timed("ERROR", "服务器无法建立临时文件，上传取消"));
                Log("upload failed (cannot create part file): " + partPath);
                return true;
            }
            client->upload = std::make_shared<PendingUpload>();
            client->upload->id = words[0];
            client->upload->roomId = uploadRoom;  // 钉死房间：中途切会话不影响这个上传
            client->upload->nameB64 = words[1];
            client->upload->declared = bytes;
            client->upload->partPath = partPath;
            client->upload->out = std::move(partFile);
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
            if (client->upload->received + chunk.size() > client->upload->declared) {
                client->SendLine(Timed("ERROR", "收到的数据超过了声明的大小，上传已中止"));
                Log("upload aborted (too much data): " + client->nick);
                client->upload.reset();  // 析构会把 .part 删掉
                return true;
            }
            if (!chunk.empty()) {
                client->upload->out.write(reinterpret_cast<const char*>(chunk.data()),
                                          static_cast<std::streamsize>(chunk.size()));
                if (!client->upload->out) {
                    client->SendLine(Timed("ERROR", "写入暂存文件失败，上传已中止（磁盘可能满了）"));
                    Log("upload aborted (disk write failed): " + client->nick);
                    client->upload.reset();
                    return true;
                }
                client->upload->received += chunk.size();
            }
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
            if (upload->received != upload->declared) {
                client->SendLine(Timed("ERROR", "文件不完整，上传取消（收到 " +
                                                  dchat::FormatBytes(upload->received) +
                                                  "，声明 " +
                                                  dchat::FormatBytes(upload->declared) + "）"));
                Log("upload aborted (incomplete): " + client->nick);
                return true;  // upload 析构时会把 .part 删掉
            }
            upload->out.close();
            if (!upload->out) {
                client->SendLine(Timed("ERROR", "暂存文件收尾失败（磁盘可能满了），上传取消"));
                Log("upload aborted (close failed): " + client->nick);
                return true;
            }
            // 改名去掉 .part：**先把文件落定、再登记**，
            // 这样进 g_files 的路径一定指向一个完整的文件。
            const std::string partSuffix = kPartSuffix;
            const std::string finalPath =
                upload->partPath.substr(0, upload->partPath.size() - partSuffix.size());
            if (std::rename(upload->partPath.c_str(), finalPath.c_str()) != 0) {
                client->SendLine(Timed("ERROR", "暂存文件改名失败，上传取消"));
                Log("upload aborted (rename failed): " + upload->partPath);
                return true;
            }
            upload->committed = true;  // 已交给 StoredFile 管，析构不要再删它
            // 房间的缓存预算在这里取一次（它决定这个文件能不能存下、以及要淘汰谁）。
            // 取完就放锁：StoreFile 会拿 g_filesMutex，两把锁不嵌套。
            unsigned long long roomBudget = 0;
            {
                std::lock_guard<std::mutex> lock(g_roomsMutex);
                const dchat::Room* room = RoomsLocked().Find(upload->roomId);
                if (room) roomBudget = EffectiveRoomBudget(*room);
            }
            const std::string fileId = StoreFile(upload->roomId, client->nick, upload->nameB64,
                                                 upload->received, finalPath, upload->thumb,
                                                 roomBudget);
            if (fileId.empty()) {
                client->SendLine(Timed("ERROR", "服务器暂时存不下这个文件（可能超出了这个会话的缓存上限），稍后再试"));
                RemoveStoredFileQuiet(finalPath);  // 没登记成功，磁盘上那份不能留
                return true;
            }
            std::vector<unsigned char> nameBytes;
            const std::string showName = dchat::Base64Decode(upload->nameB64, &nameBytes)
                                             ? dchat::SanitizeFileName(std::string(
                                                   nameBytes.begin(), nameBytes.end()))
                                             : std::string("(文件名无法解析)");
            // "还有几个人能下载"要按**同一个会话里**的在线人数算。
            // 以前只有一个房间，数所有在线的人是对的；多会话之后那么数会告诉用户
            // "房间里 5 位成员"，而实际上这个群只有他一个人。
            std::size_t others = 0;
            {
                const std::vector<std::string> audience = RoomAudience(upload->roomId);
                for (const auto& other : ClientSnapshot()) {
                    if (other.get() == client.get() || other->nick.empty()) continue;
                    if (std::find(audience.begin(), audience.end(), other->nick) != audience.end()) {
                        ++others;
                    }
                }
            }
            client->SendLine(Timed("SYS", others ? ("文件已上传，会话里的 " +
                                                    std::to_string(others) +
                                                    " 位成员点一下卡片就能下载")
                                                 : "文件已上传，但这个会话里目前没有其他人"));
            // 通知别人"有文件可以下载了"（上传者自己不用再下自己发的文件）
            //
            // 种类标记是**追加在末尾的第 7 位**，绝不能插到中间：老客户端按位置
            // 读到第 5 个字段就停了，插进去会让它们把标记当成字节数解析。
            // 这和 RULES 行当初的扩法一样——只追加、不重排。
            //
            // 非大厅的附件多一层房间号（RFILE_OFFER）：客户端可能同时收着好几个
            // 会话的消息，不给房间号它就不知道该把这张卡片放进哪一段对话里。
            const std::string fileArgs = client->nick + " " + fileId + " " + upload->nameB64 + " " +
                                         std::to_string(upload->received) + " " +
                                         (upload->thumb.empty() ? "0" : "1") + " " + upload->kind;
            const std::string offerLine =
                (upload->roomId == dchat::kHallId)
                    ? Timed("FILE_OFFER", fileArgs)
                    : Timed("RFILE_OFFER", upload->roomId + " " + fileArgs);
            BroadcastRoom(upload->roomId, offerLine, client.get());
            Log("file stored: id=" + fileId + " owner=" + client->nick + " " + showName + "（" +
                dchat::FormatBytes(upload->received) + "）");
            // **也要告诉上传者它的附件 ID。**
            //
            // 以前这里只 Broadcast 给别人（except = 上传者），结果上传者自己拿不到
            // 服务器分配的 ID：Linux 客户端上传完想再 /get 下来验证一下就做不到，
            // 用户也会问"我刚传的文件 id 是多少"。服务器分配 ID（形如 F1），
            // 客户端无从猜测，所以必须回一条。
            //
            // 用**和广播完全相同的命令与字段顺序**，只是把接收者换成上传者：
            // 另外两端的界面本来就按"有人上传好了"处理 FILE_OFFER，
            // 显示出来正好是"文件已上传（自己传的）"，语义正确、解析零改动。
            // 非大厅时同样要走 RFILE_OFFER，否则客户端会把自己传的群文件
            // 错放进大厅那段对话里。
            client->SendLine(offerLine);
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
            if (!CanAccessRoomFile(*client, thumbFile->roomId)) {
                client->SendLine(dchat::BuildLine("FILE_FAIL",
                                                  words[0] + " 你不在这个文件所属的会话里"));
                Log("thumb denied (not a member): " + client->nick + " -> " + thumbFile->id);
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

        // FILE_GET <文件ID> [已有字节数]
        // 第二个参数是断点续传用的：客户端说"我这儿已经有前 N 字节了"，服务器从 N 接着发。
        // **参数可选**，老客户端不带就还是从头发 —— 协议向后兼容。
        if ((words.size() != 1 && words.size() != 2) || !IsValidTransferId(words[0])) {
            client->SendLine(Timed("ERROR", "用法：FILE_GET <文件ID> [已下载字节数]"));
            return true;
        }
        unsigned long long resumeFrom = 0;
        if (words.size() == 2 && !dchat::ParseUint64(words[1], &resumeFrom)) {
            client->SendLine(Timed("ERROR", "断点位置不是合法数字：" + words[1]));
            return true;
        }
        const std::shared_ptr<StoredFile> file = FindStoredFile(words[0]);
        if (!file) {
            // 控制消息不带时间戳，和其他文件消息一致，客户端解析更简单
            client->SendLine(dchat::BuildLine("FILE_FAIL", words[0] + " 文件不存在或已经过期"));
            Log("download miss: " + client->nick + " -> " + words[0]);
            return true;
        }
        // 下载权限：只能下自己所在会话的附件。少了这一步，猜一个文件 ID
        // 就能把别人的私聊附件拿走。
        if (!CanAccessRoomFile(*client, file->roomId)) {
            client->SendLine(dchat::BuildLine("FILE_FAIL",
                                              words[0] + " 你不在这个文件所属的会话里"));
            Log("download denied (not a member): " + client->nick + " -> " + file->id);
            return true;
        }
        // 续传起点必须落在文件范围内。超出说明客户端手里是**旧文件**的大小
        // （同一 ID 的文件被重新上传过）。这时必须拒绝，绝不能"从头再发一遍" ——
        // 那会让客户端把新旧两份内容拼成一个坏文件，而且它会以为下载成功了。
        if (resumeFrom > file->size) {
            client->SendLine(dchat::BuildLine(
                "FILE_FAIL", file->id + " 续传位置超出文件大小（文件可能已被重新上传过）"));
            Log("download resume rejected: " + client->nick + " offset=" +
                std::to_string(resumeFrom) + " size=" + std::to_string(file->size));
            return true;
        }
        Log("download: " + client->nick + " 下载 " + file->id + "（由 " + file->owner +
            " 上传，" + dchat::FormatBytes(file->size) + "）" +
            (resumeFrom > 0 ? "，从 " + dchat::FormatBytes(resumeFrom) + " 处续传" : ""));
        // FILE_BEGIN **保持 3 格不变**。
        // 这里原本想追加第 4 格把续传起点回给客户端，但 Windows 客户端的解析是
        // `fields.size() != 3 -> return`（要求恰好 3 格），多一格会让它静默忽略
        // FILE_BEGIN、下载整个失效 —— 而且不报错，极难查。
        // 客户端本来就知道自己请求的 offset（是它自己算出来发过来的），不需要回显；
        // "服务器是否接受这个 offset" 由 FILE_FAIL 表达。
        // 教训：协议文档写着"加字段只追加在末尾、老客户端读到旧格数就停"，
        // 但**实现里可能有 `!= N` 这种更严的判断**，加字段前必须去客户端确认。
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size)));
        // 打开磁盘上的暂存文件，按 offset 顺序读出来发走（内容不再驻留内存）
        std::ifstream in(file->path, std::ios::binary);
        if (!in) {
            client->SendLine(dchat::BuildLine("FILE_FAIL", file->id + " 文件已不在暂存目录里"));
            Log("download failed (staged file missing): " + file->path);
            return true;
        }
        if (resumeFrom > 0) in.seekg(static_cast<std::streamoff>(resumeFrom));
        for (unsigned long long offset = resumeFrom; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {
            const unsigned long long remain = file->size - offset;
            const std::size_t length = remain < dchat::kFileChunkBytes
                                           ? static_cast<std::size_t>(remain)
                                           : dchat::kFileChunkBytes;
            std::string buffer(length, '\0');
            in.read(&buffer[0], static_cast<std::streamsize>(length));
            if (in.gcount() != static_cast<std::streamsize>(length)) {
                // 暂存文件在下载过程中被别的线程过期/淘汰掉了。
                // 客户端会收到不完整的数据并可以重试 —— 现在有断点续传，代价不大。
                Log("download read failed (staged file gone?): " + file->id);
                ok = false;
                break;
            }
            const std::string chunk = dchat::Base64Encode(
                reinterpret_cast<const unsigned char*>(buffer.data()), length);
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
        // 超时的设置方式两端不同（Windows 用 DWORD 毫秒、Linux 用 timeval），
        // 这层差异在 socket_util 里吸收掉
        if (dchat::sock::SetReceiveTimeout(client->sock, handshakeSec * 1000)) {
            handshakeTimeoutActive = true;
        }
    }

    dchat::LineBuffer buffer;
    std::vector<char> chunk(2048);
    bool keepGoing = true;
    while (keepGoing) {
        const std::ptrdiff_t received = dchat::sock::Receive(client->sock, chunk.data(), chunk.size());
        // 超时 / 出错都走 dchat::sock::WouldBlock，不直接比错误码：
        // 两端的"超时"错误码不同（Windows 是 WSAETIMEDOUT，Linux 是 EAGAIN）
        if (received < 0) {
            if (dchat::sock::WouldBlock()) {
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
            dchat::sock::SetReceiveTimeout(client->sock, 0);
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
    dchat::sock::ShutdownWrite(client->sock);
    dchat::sock::Close(client->sock);
    client->sock = dchat::sock::kInvalid;
}

}  // namespace

// ---------------------------------------------------------------------------
// 管理端口：给网页管理员页面用的**本机**通道
//
// **为什么需要它**：/chatrule 是"仅控制台"指令，协议里没有任何对应命令。网页要能改
// 规则，服务器就必须认一条新通道。三条设计原则，缺一条就不安全：
//
//   1. **只绑 127.0.0.1** —— 外网物理上连不上，就算端口出现在公网扫描里也连不通；
//   2. **要求随机口令** —— 同机的其他用户/进程不知道口令照样进不来；
//   3. **复用控制台那条路径** —— 请求翻译成 `/chatrule ...` 交给 HandleServerCommand，
//      校验、写盘（dchat-rules.txt）、广播 RulesLineForClient、maxservertemp 触发立即
//      清理……全都和控制台完全一致，不会出现"页面改了但没生效/没存盘"的两套逻辑。
//
// 不要在客户端协议里加一条 ADMIN 命令来代替它：那等于把服务器规则的写权限
// 暴露给网络上任何一个连上来的人。
// ---------------------------------------------------------------------------

#include <random>      // 管理口令的随机字节
#include <thread>      // 管理端口的监听线程
#ifndef _WIN32
#include <sys/stat.h>  // chmod：把口令文件收成 0600
#endif

std::string g_adminToken;

/** 恒定时间比较：别让人靠"响应快慢"一位一位猜口令。 */
bool SlowTokenEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i] ^ b[i])));
    }
    return diff == 0;
}

/** 读已有口令；没有就生成一个 48 位十六进制并写成 0600（只有服务账号能读）。 */
std::string LoadOrCreateAdminToken(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file != nullptr) {
        char buffer[256] = {0};
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        std::string token(buffer, got);
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r' ||
                                  token.back() == ' ')) {
            token.pop_back();
        }
        if (!token.empty()) return token;
    }
    std::random_device device;
    const char* kHex = "0123456789abcdef";
    std::string token;
    token.reserve(48);
    for (int i = 0; i < 48; ++i) token.push_back(kHex[device() & 0x0F]);
    file = std::fopen(path.c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(token.data(), 1, token.size(), file);
        std::fclose(file);
#ifndef _WIN32
        ::chmod(path.c_str(), 0600);  // 别让同机其他用户读到
#endif
        Log("已生成管理口令：" + path + "（网页管理员页面要用它登录）");
    } else {
        Log("！管理口令写入失败：" + path + "（管理端口将无法登录）");
    }
    return token;
}

/** 处理一条已认证的管理请求，回一行结果。 */
std::string HandleAdminRequest(const std::string& line) {
    if (line == "LIST") {
        std::string out = "RULES_BEGIN\n";
        std::vector<std::pair<std::string, std::string>> values;
        {
            std::lock_guard<std::mutex> lock(g_rulesMutex);
            values = dchat::RuleValues(g_rules);
        }
        for (const auto& item : values) {
            out += item.first + "\t" + item.second + "\n";
        }
        out += "RULES_END\n";
        return out;
    }
    if (line.rfind("SET ", 0) == 0) {
        const std::string rest = line.substr(4);
        const std::size_t space = rest.find(' ');
        if (space == std::string::npos) return "ERR 格式应为 SET <规则> <值>\n";
        const std::string name = rest.substr(0, space);
        std::string value = rest.substr(space + 1);
        // 就地去掉首尾空白。刻意不叫 TrimAscii：不想和别处的同名函数撞上。
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                  value.back() == '\r')) {
            value.pop_back();
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        if (!dchat::IsKnownRule(name)) return "ERR 没有这条规则：" + name + "\n";
        // 数值规则只收整数（布尔规则收 true/false），先挡一道再交给控制台解析
        if (dchat::IsBoolRule(name)) {
            if (value != "true" && value != "false") return "ERR 这条规则只能是 true / false\n";
            HandleServerCommand("/chatrule " + name + " " + value);
        } else {
            bool numeric = !value.empty();
            for (std::size_t i = 0; i < value.size(); ++i) {
                const char ch = value[i];
                if (!(ch >= '0' && ch <= '9') && !(i == 0 && ch == '-')) numeric = false;
            }
            if (!numeric) return "ERR 这条规则只收整数\n";
            HandleServerCommand("/chatrule " + name + " set " + value);
        }
        // 回读一次当前值：HandleServerCommand 可能把越界的值夹到范围内，
        // 所以要报"改完到底是多少"，不能回显用户输入的那个数。
        std::string now;
        {
            std::lock_guard<std::mutex> lock(g_rulesMutex);
            for (const auto& item : dchat::RuleValues(g_rules)) {
                if (item.first == name) now = item.second;
            }
        }
        return "OK " + name + "=" + now + "\n";
    }
    return "ERR 不认识的请求（只支持 LIST 和 SET）\n";
}

/** 管理端口的一个连接：先认证，再逐行处理请求。 */
void AdminServeClient(dchat::sock::Handle sock) {
    dchat::sock::SetNoSigpipe(sock);
    dchat::sock::SetReceiveTimeout(sock, 30000);
    bool authed = false;
    std::string pending;
    char buffer[4096];
    while (true) {
        const std::ptrdiff_t got = dchat::sock::Receive(sock, buffer, sizeof(buffer));
        if (got <= 0) break;
        pending.append(buffer, static_cast<std::size_t>(got));
        std::size_t newline = 0;
        bool stop = false;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!authed) {
                if (line.rfind("ADMIN ", 0) == 0 && SlowTokenEquals(line.substr(6), g_adminToken)) {
                    authed = true;
                    const std::string ok = "OK 已认证\n";
                    dchat::sock::SendAll(sock, ok.data(), ok.size());
                } else {
                    Log("管理端口：口令错误，断开连接");
                    const std::string bad = "ERR 口令错误\n";
                    dchat::sock::SendAll(sock, bad.data(), bad.size());
                    stop = true;
                }
                continue;
            }
            const std::string reply = HandleAdminRequest(line);
            Log("管理端口：[" + line + "] -> " + reply.substr(0, 60));
            dchat::sock::SendAll(sock, reply.data(), reply.size());
        }
        if (stop) break;
    }
    dchat::sock::Close(sock);
}

/** 起管理端口的监听线程（端口 0 = 不启用）。 */
void StartAdminChannel(int adminPort, const std::string& token) {
    if (adminPort <= 0 || adminPort > 65535) return;
    const dchat::sock::Handle listener = dchat::sock::CreateTcp();
    if (listener == dchat::sock::kInvalid) {
        Log("！管理端口建 socket 失败");
        return;
    }
    dchat::sock::SetReuseAddress(listener);
    dchat::sock::Address address{};
    address.sin_family = AF_INET;
    // **只绑回环**：这是整个方案安全性的地基，不能改成 INADDR_ANY。
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    address.sin_port = htons(static_cast<unsigned short>(adminPort));
    if (!dchat::sock::Bind(listener, address) || !dchat::sock::Listen(listener, 8)) {
        Log("！管理端口绑定 127.0.0.1:" + std::to_string(adminPort) + " 失败");
        dchat::sock::Close(listener);
        return;
    }
    (void)token;
    std::thread([listener]() {
        while (true) {
            dchat::sock::Address peer{};
            const dchat::sock::Handle client = dchat::sock::Accept(listener, &peer);
            if (client == dchat::sock::kInvalid) continue;
            std::thread([client]() { AdminServeClient(client); }).detach();
        }
    }).detach();
    Log("管理端口已启动：127.0.0.1:" + std::to_string(adminPort) + "（仅本机，供网页管理员页面用）");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认不是 UTF-8，不设的话中文日志和中文昵称都会乱码。
    // Linux 终端本来就是 UTF-8，不需要（也没有这两个 API）。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);  // 控制台输入也按 UTF-8，中文昵称才能匹配
#endif

    int port = dchat::kDefaultPort;
    std::string bindAddress;  // 空 = 监听所有网卡
    int adminPort = 0;        // 0 = 不启用管理端口
    std::string adminTokenPath = "dchat-admin-token.txt";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) port = std::atoi(argv[++i]);
        if ((arg == "--users" || arg == "-u") && i + 1 < argc) g_usersPath = argv[++i];
        if ((arg == "--bind" || arg == "-b") && i + 1 < argc) bindAddress = argv[++i];
        if ((arg == "--rules" || arg == "-r") && i + 1 < argc) g_rulesPath = argv[++i];
        // 管理端口默认**不开**：不想要网页管理就不加这个参数，攻击面为零。
        if (arg == "--admin-port" && i + 1 < argc) adminPort = std::atoi(argv[++i]);
        if (arg == "--admin-token" && i + 1 < argc) adminTokenPath = argv[++i];
    }
    if (port <= 0 || port > 65535) {
        std::printf("invalid port: %d\n", port);
        return 1;
    }

    if (!dchat::sock::Startup()) {
        std::printf("socket startup failed\n");
        return 1;
    }

    const dchat::sock::Handle listener = dchat::sock::CreateTcp();
    if (listener == dchat::sock::kInvalid) {
        std::printf("socket() failed: %d\n", dchat::sock::LastError());
        dchat::sock::Cleanup();
        return 1;
    }

    dchat::sock::SetReuseAddress(listener);

    dchat::sock::Address address{};
    address.sin_family = AF_INET;
    if (bindAddress.empty()) {
        address.sin_addr.s_addr = INADDR_ANY;  // 默认：所有网卡都监听
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
        std::printf("--bind 只支持 IPv4 地址，例如 --bind 0.0.0.0 或 --bind 192.168.31.251\n");
        dchat::sock::Close(listener);
        dchat::sock::Cleanup();
        return 1;
    }
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (!dchat::sock::Bind(listener, address)) {
        std::printf("bind() failed on port %d: %d\n", port, dchat::sock::LastError());
        dchat::sock::Close(listener);
        dchat::sock::Cleanup();
        return 1;
    }
    if (!dchat::sock::Listen(listener, SOMAXCONN)) {
        std::printf("listen() failed: %d\n", dchat::sock::LastError());
        dchat::sock::Close(listener);
        dchat::sock::Cleanup();
        return 1;
    }

    Log("dchat server listening on port " + std::to_string(port) + " (Ctrl+C to stop)");
    // 管理端口（网页管理员页面用）。没加 --admin-port 就完全不启用。
    if (adminPort > 0) {
        g_adminToken = LoadOrCreateAdminToken(adminTokenPath);
        StartAdminChannel(adminPort, g_adminToken);
    }
    LoadUsers();
    Log("accounts file: " + g_usersPath + "（密码以加盐哈希保存，不存明文）");
    LoadRules();
    ClearFilesDir();
    // 管理员名单也要读回来——否则每次重启都得重新 /op 一遍，
    // 而 /op 只能在控制台做（systemd 下 stdin 是 /dev/null）。
    LoadOps();
    Log("admins: " + OpList());
    // 房间名单必须在 LoadHistory 之前读：历史是**按房间**加载的，
    // 顺序反了的话群里和私聊的历史会因为"房间还不存在"而全部被丢掉。
    LoadRooms();
    LoadHistory();
    LoadSeen();
    LoadBlocks();
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
        dchat::sock::Address peer{};
        
        const dchat::sock::Handle sock = dchat::sock::Accept(listener, &peer);
        if (sock == dchat::sock::kInvalid) break;

        // 关掉 Nagle：聊天都是小消息，等合并会让人感觉卡顿
        dchat::sock::SetNoDelay(sock);
        // macOS 上必须在这里就把 SO_NOSIGPIPE 设上：那边没有 MSG_NOSIGNAL 标志，
        // 漏了它的后果是「第一个断开的客户端把整个服务器打死」。Linux 上是空操作。
        dchat::sock::SetNoSigpipe(sock);
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
                dchat::sock::SendAll(sock, payload.data(), payload.size());
                dchat::sock::ShutdownWrite(sock);
                dchat::sock::Close(sock);
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

    dchat::sock::Close(listener);
    dchat::sock::Cleanup();
    return 0;
}
