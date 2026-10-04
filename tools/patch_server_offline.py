"""第 5 项：离线消息的服务端实现。

核心设计（避免和 keepchathistory 重复）：
  回放历史时**复用阅读进度** ——
    - 新用户（没有进度记录）：进度视为 0，看到保留范围内的完整历史，
      这就是 keepchathistory 原来的行为，不变；
    - 老用户（有进度记录）且 offlinemessages > 0：只补他错过的那些，条数受限。
  这样两件事各司其职：keepchathistory 管"房间上下文"，offlinemessages 管"你错过的"，
  同时出现时也不会把同一条消息发两遍。

兼容性：offlinemessages 默认 0。此时**老用户也按老行为**拿完整历史 ——
不因为升级就突然改变已有部署的观感。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

# ---------------------------------------------------------------- 1) 进度存储
ANCHOR_GLOBALS = '''/** 把攒着的记录刷到磁盘（周期任务与退出前调用）。 */'''

NEW_SEEN_BLOCK = '''// ---- 每用户的阅读进度（离线补发用）----
// 单独一个文件，不塞进账号文件：那是"用户凭据"，格式改动会牵连四端登录解析；
// 而这是服务端自己的阅读进度，客户端根本不需要知道。
std::mutex g_seenMutex;
std::map<std::string, unsigned long long> g_lastSeen;
std::string g_seenPath = "dchat-seen.txt";
std::ofstream g_seenOut;
bool g_seenDirty = false;
unsigned long long g_seenSinceCompact = 0;
constexpr unsigned long long kSeenCompactEvery = 512;

/** 记下某个人"已经读到第几条"。nick 为空直接返回（还没登录的连接）。 */
void RecordSeen(const std::string& nick) {
    if (nick.empty()) return;
    // **先取序号、再拿 g_seenMutex**，两把锁不嵌套：
    // 嵌套会引入锁顺序问题，而这里完全没必要。
    unsigned long long seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        seq = HistoryLocked().LastSeq();
    }
    std::lock_guard<std::mutex> lock(g_seenMutex);
    unsigned long long& slot = g_lastSeen[nick];
    if (seq < slot) return;  // 进度只前进不后退
    slot = seq;
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

/** 把一个用户的进度追加写盘（调用方已持有 g_seenMutex）。 */
void AppendSeenLocked(const std::string& nick, unsigned long long seq) {
    if (!g_seenOut.is_open()) {
        g_seenOut.open(g_seenPath, std::ios::binary | std::ios::app);
        if (!g_seenOut) return;
    }
    g_seenOut << dchat::FormatSeenFileLine({nick, seq});
    ++g_seenSinceCompact;
}

/** 把攒着的记录刷到磁盘（周期任务与退出前调用）。 */'''

OLD_REPLAY = '''void ReplayHistoryTo(const std::shared_ptr<Client>& client) {
    if (!CurrentRules().keepChatHistory) return;
    const std::vector<std::string> lines = HistorySnapshot();'''

NEW_REPLAY = '''void ReplayHistoryTo(const std::shared_ptr<Client>& client) {
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
        HistorySince(sinceSeq, catchUp ? static_cast<std::size_t>(offlineLimit)
                                       : static_cast<std::size_t>(-1));
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const dchat::HistoryEntry& entry : entries) lines.push_back(entry.line);
    if (catchUp) RecordSeen(client->nick);  // 补完就把进度推到最新'''

OLD_REPLAY_BANNER = '''    if (lines.empty() && offers.empty()) return;
    client->SendLine(Timed("SYS", "—— 以下是加入之前的聊天记录（keepchathistory 已打开）——"));'''

NEW_REPLAY_BANNER = '''    if (lines.empty() && offers.empty()) return;
    client->SendLine(Timed("SYS", catchUp
                                    ? ("—— 你不在的时候有 " + std::to_string(lines.size()) +
                                       " 条消息 ——")
                                    : "—— 以下是加入之前的聊天记录（keepchathistory 已打开）——"));'''

# ---------------------------------------------------------------- 2) 移除客户端时记进度
OLD_REMOVE = '''void RemoveClient(Client* target) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);'''

NEW_REMOVE = '''void RemoveClient(Client* target) {
    // 断开时把他的阅读进度记下来（含被踢、崩溃断开）：
    // 此刻 LastSeq() 就是他"在线期间已经收到"的最大序号 —— 因为在线时每条都是实时发的。
    // 漏记的后果只是下次多补几条（重复），不会漏消息，所以这里不追求绝对精确。
    if (target) {
        const std::string nick = target->nick;
        if (!nick.empty()) {
            RecordSeen(nick);
            std::lock_guard<std::mutex> seenLock(g_seenMutex);
            if (g_seenDirty) {
                AppendSeenLocked(nick, g_lastSeen[nick]);
                g_seenDirty = false;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_clientsMutex);'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    edits = [
        (ANCHOR_GLOBALS, NEW_SEEN_BLOCK, "进度存储"),
        (OLD_REPLAY, NEW_REPLAY, "ReplayHistoryTo 按进度回放"),
        (OLD_REPLAY_BANNER, NEW_REPLAY_BANNER, "回放提示语"),
        (OLD_REMOVE, NEW_REMOVE, "断开时记进度"),
    ]
    changed = 0
    for old, new, label in edits:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("  ✅", label)

    # main 里加载进度；周期任务兜底 flush；退出前 flush
    if "LoadSeen();" not in text:
        text = text.replace("    LoadHistory();", "    LoadHistory();\n    LoadSeen();", 1)
        print("  ✅ main 调用 LoadSeen")
    if "FlushSeenIfDirty();" not in text.split("void BanMaintenanceLoop")[1][:1200]:
        text = text.replace("        FlushHistoryIfDirty();",
                            "        FlushHistoryIfDirty();\n        FlushSeenIfDirty();", 1)
        print("  ✅ 周期任务 flush 进度")
    if "if (g_seenSinceCompact >= kSeenCompactEvery)" not in text:
        text = text.replace("        if (g_historyCompactDue) {",
                            "        // 进度文件是追加写的，同一用户会攒下多行，攒够就压实\n"
                            "        {\n"
                            "            std::lock_guard<std::mutex> lock(g_seenMutex);\n"
                            "            if (g_seenSinceCompact >= kSeenCompactEvery) {\n"
                            "                g_seenSinceCompact = 0;\n"
                            "                g_seenCompactDue = true;\n"
                            "            }\n"
                            "        }\n"
                            "        if (g_seenCompactDue) {\n"
                            "            g_seenCompactDue = false;\n"
                            "            CompactSeen();\n"
                            "        }\n"
                            "        if (g_historyCompactDue) {", 1)
        text = text.replace("bool g_seenDirty = false;", "bool g_seenDirty = false;\nbool g_seenCompactDue = false;", 1)
        print("  ✅ 进度压实")
    idx = text.rfind("    FlushHistoryIfDirty();")
    if idx >= 0 and "FlushSeenIfDirty();\n    return 0;" not in text:
        text = text[:idx + len("    FlushHistoryIfDirty();")] + "\n    FlushSeenIfDirty();" + text[idx + len("    FlushHistoryIfDirty();"):]
        print("  ✅ 退出前 flush 进度")

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("共 %d / %d 处" % (changed, len(edits)))


if __name__ == "__main__":
    main()
