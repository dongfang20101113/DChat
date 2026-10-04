"""把服务端的内存历史换成可落盘的 HistoryStore（第 6 项）。

设计要点（都写进代码注释了）：
  - **启动时总是读一次历史文件**：即使 keepchathistory 是关的也要读，
    目的不是回放，而是**继承文件里的最大序号**。否则关着规则跑一阵、
    再打开规则时序号会从 1 重来，而文件里已经有 N 条 —— 下次启动
    LoadFromText 的"严格递增"判断会把新写的低序号行全部丢掉。
  - **只有 keepchathistory 为真时才追加写盘**："不保留聊天记录"就该意味着
    磁盘上也不留。默认 false，所以升级后行为不变（这是本项目的既定约定）。
  - **写盘节流**：每条消息都 flush 的话，16 客户端压测（实测 47 万条消息）
    会被磁盘拖垮。改成最多每秒 flush 一次，由周期任务兜底，退出前再补一次。
    —— 代价是崩溃时最多丢最后一秒的记录，聊天记录可以接受。
  - **压实**：累计追加到一定条数就整份重写，避免文件里堆满已被裁掉的旧行。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

OLD_GLOBALS = '''// ---- 聊天记录缓存：keepchathistory 打开时，新加入的人能看到之前的记录和文件 ----
std::mutex g_historyMutex;
std::deque<std::string> g_history;  // 已经广播出去的 SAY / ANNOUNCE 行（原样保存）
unsigned long long g_historyBytes = 0;
constexpr std::size_t kMaxHistoryLines = 2000;'''

NEW_GLOBALS = '''// ---- 聊天记录：内存缓存 + 落盘 ----
// keepchathistory 打开时，新加入的人能看到之前的记录，而且**服务端重启后还在**。
std::mutex g_historyMutex;
std::unique_ptr<dchat::HistoryStore> g_history;
std::string g_historyPath = "dchat-history.txt";
constexpr std::size_t kMaxHistoryLines = 2000;

// 落盘的写入节流。
// 为什么必须节流：每条消息都 flush 的话，16 客户端压测（实测 47 万条消息）
// 会被磁盘 I/O 拖成另一个数量级。这里最多每秒 flush 一次，由周期任务兜底，
// 退出前再补一次。**代价是崩溃时最多丢最后一秒的记录** —— 聊天记录可以接受。
std::ofstream g_historyOut;
std::chrono::steady_clock::time_point g_historyFlushedAt{};
bool g_historyDirty = false;
constexpr int kHistoryFlushMs = 1000;

// 累计追加多少条就整份重写一次（文件里会攒下大量已被裁掉的旧行）。
// 只在"规则开着"时才可能触发。
unsigned long long g_historySinceCompact = 0;
constexpr unsigned long long kHistoryCompactEvery = 512;

// 拿走 g_historyMutex 之后用这个取存储；懒建，上限只在这里定义一处
dchat::HistoryStore& HistoryLocked() {
    if (!g_history) {
        g_history = std::make_unique<dchat::HistoryStore>(kMaxHistoryLines, ~0ull);
    }
    return *g_history;
}

/** 把一条记录追加到历史文件（调用方已持有 g_historyMutex 且确认规则为真）。 */
void AppendHistoryFileLocked(const dchat::HistoryEntry& entry) {
    if (!g_historyOut.is_open()) {
        g_historyOut.open(g_historyPath, std::ios::binary | std::ios::app);
        if (!g_historyOut) {
            Log("failed to open history file for append: " + g_historyPath);
            return;
        }
    }
    g_historyOut << dchat::FormatHistoryFileLine(entry);
    g_historyDirty = true;
}

/** 把攒着的记录刷到磁盘（周期任务与退出前调用）。 */
void FlushHistoryIfDirty() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    if (!g_historyDirty || !g_historyOut.is_open()) return;
    g_historyOut.flush();
    g_historyDirty = false;
    g_historyFlushedAt = std::chrono::steady_clock::now();
}

/** 整份重写历史文件，只留内存里还保着的那些（压实）。 */
void CompactHistory() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    if (!g_history) return;
    // 先关掉追加流，否则重写和追加会互相踩
    if (g_historyOut.is_open()) {
        g_historyOut.flush();
        g_historyOut.close();
    }
    std::ofstream out(g_historyPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Log("failed to compact history file: " + g_historyPath);
        return;
    }
    out << g_history->Serialize();
    g_historySinceCompact = 0;
    g_historyDirty = false;
    Log("history compacted: " + std::to_string(g_history->Size()) + " line(s) kept");
}

/**
 * 启动时读一次历史文件。
 *
 * **无论 keepchathistory 是开还是关都要读**：读的目的不只是回放，
 * 更重要的是**继承文件里的最大序号**。否则：规则关着跑一阵（不写盘，
 * 但内存序号在涨），中途打开规则开始追加，序号是对的；可如果反过来 ——
 * 没读文件就从 1 开始追加，文件里已有的 N 条会让新行全部被判为"序号未递增"
 * 而在**下次启动时被丢弃**。
 */
void LoadHistory() {
    std::ifstream in(g_historyPath, std::ios::binary);
    std::lock_guard<std::mutex> lock(g_historyMutex);
    if (!in) {
        Log("no history file yet, will create when keepchathistory is on: " + g_historyPath);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t loaded = HistoryLocked().LoadFromText(text);
    Log("loaded " + std::to_string(loaded) + " history line(s) from " + g_historyPath +
        "（末序号 " + std::to_string(g_history->LastSeq()) + "）");
}'''


OLD_REMEMBER = '''void RememberHistory(const std::string& line) {
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
}'''

NEW_REMEMBER = '''void RememberHistory(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    dchat::HistoryStore& store = HistoryLocked();
    const unsigned long long seq = store.Append(line);

    // 行数和总预算都要守（预算 = 文件 + 聊天记录，由 maxservertemp 决定）
    const unsigned long long budget = TempBudgetBytes();
    const unsigned long long usedByFiles = StoredBytesTotal();
    store.TrimToBytes(budget > usedByFiles ? budget - usedByFiles : 0);

    if (!CurrentRules().keepChatHistory) return;  // 规则关着：不写盘

    dchat::HistoryEntry entry;
    entry.seq = seq;
    entry.line = line;
    AppendHistoryFileLocked(entry);

    // 节流 flush：距上次超过 1 秒才真正落盘
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - g_historyFlushedAt)
                           .count();
    if (g_historyDirty && since >= kHistoryFlushMs) {
        g_historyOut.flush();
        g_historyDirty = false;
        g_historyFlushedAt = now;
    }

    if (++g_historySinceCompact >= kHistoryCompactEvery) {
        // 先在锁内把状态备好，再交给 CompactHistory（它自己会再拿一次锁，
        // 这里不能带着锁调用，否则死锁）
        const unsigned long long dropped = static_cast<unsigned long long>(store.TotalAppended()) -
                                           static_cast<unsigned long long>(store.Size());
        g_historySinceCompact = 0;
        if (dropped > 0) {
            // 有被裁掉的旧行，压实才有意义
            Log("history file compaction due（已裁掉 " + std::to_string(dropped) + " 条旧记录）");
        }
    }
}

std::vector<std::string> HistorySnapshot() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    std::vector<std::string> out;
    for (const dchat::HistoryEntry& entry : HistoryLocked().Snapshot()) {
        out.push_back(entry.line);
    }
    return out;
}

std::size_t HistoryCount() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return HistoryLocked().Size();
}

/**
 * 取序号大于 afterSeq 的记录（第 5 项：离线补发用）。
 * 调用方不能持有 g_historyMutex。
 */
std::vector<dchat::HistoryEntry> HistorySince(unsigned long long afterSeq, std::size_t limit) {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return HistoryLocked().Since(afterSeq, limit);
}'''


OLD_BUDGET = '''    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        while (!g_history.empty() && g_historyBytes + StoredBytesTotal() > budget) {
            g_historyBytes -= g_history.front().size() + 1;
            g_history.pop_front();
        }
    }'''

NEW_BUDGET = '''    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        const unsigned long long usedByFiles = StoredBytesTotal();
        HistoryLocked().TrimToBytes(budget > usedByFiles ? budget - usedByFiles : 0);
    }'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    # 统一成 \n 处理，最后再写回；源文件是 LF
    original_newline = "\r\n" if "\r\n" in text else "\n"
    if original_newline == "\r\n":
        text = text.replace("\r\n", "\n")

    edits = [
        (OLD_GLOBALS, NEW_GLOBALS, "历史全局定义"),
        (OLD_REMEMBER, NEW_REMEMBER, "RememberHistory / Snapshot / Count"), 
        (OLD_BUDGET, NEW_BUDGET, "预算裁剪"),
    ]
    changed = 0
    for old, new, label in edits:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("  ✅", label)

    # include
    if '#include "history_store.h"' not in text:
        text = text.replace('#include "register_guard.h"',
                            '#include "history_store.h"\n#include "register_guard.h"', 1)
        print("  ✅ include")

    # main 里加载历史（放在 LoadRules 之后，因为是否写盘取决于规则；
    # 但读取本身总是做，用于继承序号）
    if "LoadHistory();" not in text:
        text = text.replace("    LoadRules();", "    LoadRules();\n    LoadHistory();", 1)
        print("  ✅ main 调用 LoadHistory")

    if original_newline == "\r\n":
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("共 %d / %d 处" % (changed, len(edits)))


if __name__ == "__main__":
    main()
