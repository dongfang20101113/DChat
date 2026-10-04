"""收尾第 5 项：删掉已无人调用的 HistorySnapshot，并把 RecordSeen 改成"自己负责追加"。

为什么要改 RecordSeen：上一版把"追加写盘"放在 RemoveClient 里，靠一个
g_seenDirty 标志判断要不要写。那有两个问题：
  1. 两个用户几乎同时断开时，后一个会把标志清掉，前一个的进度就漏写了
     （后果是多补几条重复消息，不算严重，但没必要留着）；
  2. 读代码的人得跳到 RemoveClient 才明白进度是什么时候落盘的。
现在改成"谁改进度谁追加一行"，标志只用来决定**何时 flush**（节流），
语义清楚，也不会漏。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

OLD_RECORD = '''/** 记下某个人"已经读到第几条"。nick 为空直接返回（还没登录的连接）。 */
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
}'''

NEW_RECORD = '''/**
 * 记下某个人"已经读到第几条"，并**当场追加一行到进度文件**。
 *
 * 为什么追加放在这里、而不是交给调用方：谁改进度谁写盘，就不会出现
 * "两个用户几乎同时断开、后一个把标志清掉、前一个的进度漏写"这种情况。
 * 真正耗时的 flush 仍然由周期任务节流（最多每秒一次）。
 */
void RecordSeen(const std::string& nick) {
    if (nick.empty()) return;
    // **先取序号、再拿 g_seenMutex**：两把锁不嵌套，避免锁顺序问题。
    unsigned long long seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        seq = HistoryLocked().LastSeq();
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
}'''

OLD_APPEND_FN = '''/** 把一个用户的进度追加写盘（调用方已持有 g_seenMutex）。 */
void AppendSeenLocked(const std::string& nick, unsigned long long seq) {
    if (!g_seenOut.is_open()) {
        g_seenOut.open(g_seenPath, std::ios::binary | std::ios::app);
        if (!g_seenOut) return;
    }
    g_seenOut << dchat::FormatSeenFileLine({nick, seq});
    ++g_seenSinceCompact;
}

'''

OLD_REMOVE = '''    // 断开时把他的阅读进度记下来（含被踢、崩溃断开）：
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
'''

NEW_REMOVE = '''    // 断开时把他的阅读进度记下来（含被踢、崩溃断开）：
    // 此刻 LastSeq() 就是他"在线期间已经收到"的最大序号 —— 因为在线时每条都是实时发的。
    // 万一漏记，后果也只是下次多补几条（重复）而不会漏消息。
    if (target) RecordSeen(target->nick);
'''

OLD_SNAPSHOT = '''std::vector<std::string> HistorySnapshot() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    std::vector<std::string> out;
    for (const dchat::HistoryEntry& entry : HistoryLocked().Snapshot()) {
        out.push_back(entry.line);
    }
    return out;
}

'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    edits = [
        (OLD_RECORD, NEW_RECORD, "RecordSeen 自己追加"),
        (OLD_APPEND_FN, "", "删掉 AppendSeenLocked"),
        (OLD_REMOVE, NEW_REMOVE, "简化 RemoveClient"),
        (OLD_SNAPSHOT, "", "删掉未使用的 HistorySnapshot"),
    ]
    changed = 0
    for old, new, label in edits:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("  ✅", label)

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("共 %d / %d 处" % (changed, len(edits)))


if __name__ == "__main__":
    main()
