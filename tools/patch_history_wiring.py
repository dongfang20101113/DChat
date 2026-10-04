"""补上历史落盘的两处必需接线（第 6 项收尾）。

上一版补丁留了两个问题：
  1. RememberHistory 只把计数器归零、**从没真正调用 CompactHistory** ——
     结果是"记了该压实"却永远不压实，文件会一直涨；
  2. FlushHistoryIfDirty 没人调用 —— 节流后的数据要等进程退出才落盘，
     而退出路径又不保证走到（强杀就没了）。

压实不能在 RememberHistory 里直接调：那个函数持有 g_historyMutex，
而 CompactHistory 自己要拿同一把锁 —— 带着锁调用就是死锁。
所以改成"置标志、由周期任务去压实"，顺便绕开锁顺序问题。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

OLD_FLAG_DECL = '''unsigned long long g_historySinceCompact = 0;
constexpr unsigned long long kHistoryCompactEvery = 512;'''

NEW_FLAG_DECL = '''unsigned long long g_historySinceCompact = 0;
constexpr unsigned long long kHistoryCompactEvery = 512;
// 该压实了：由 RememberHistory 置位、周期任务执行。
// **不能在 RememberHistory 里直接压实** —— 那里持有 g_historyMutex，
// 而 CompactHistory 要拿同一把锁，带着锁调用就是死锁。
bool g_historyCompactDue = false;'''


OLD_DUE = '''    if (++g_historySinceCompact >= kHistoryCompactEvery) {
        // 先在锁内把状态备好，再交给 CompactHistory（它自己会再拿一次锁，
        // 这里不能带着锁调用，否则死锁）
        const unsigned long long dropped = static_cast<unsigned long long>(store.TotalAppended()) -
                                           static_cast<unsigned long long>(store.Size());
        g_historySinceCompact = 0;
        if (dropped > 0) {
            // 有被裁掉的旧行，压实才有意义
            Log("history file compaction due（已裁掉 " + std::to_string(dropped) + " 条旧记录）");
        }
    }'''

NEW_DUE = '''    if (++g_historySinceCompact >= kHistoryCompactEvery) {
        // 只有在"确实有旧行被裁掉"时压实才有意义：
        // 否则重写一遍文件一个字节都省不下来，纯属白费 I/O。
        const bool droppedAny = store.TotalAppended() > store.Size();
        if (droppedAny) g_historyCompactDue = true;
        g_historySinceCompact = 0;
    }'''


OLD_LOOP = '''        // 账号文件的写入被节流了（注册路径不做文件 I/O），这里补写落盘
        FlushUsersIfDirty();'''

NEW_LOOP = '''        // 账号文件的写入被节流了（注册路径不做文件 I/O），这里补写落盘
        FlushUsersIfDirty();
        // 聊天记录同样被节流（最多每秒一次），这里兜底 flush
        FlushHistoryIfDirty();
        // 压实标记是 RememberHistory 置的，在这里执行 —— 它要自己拿锁
        if (g_historyCompactDue) {
            g_historyCompactDue = false;
            CompactHistory();
        }'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    edits = [
        (OLD_FLAG_DECL, NEW_FLAG_DECL, "压实标志声明"),
        (OLD_DUE, NEW_DUE, "置压实标志"),
        (OLD_LOOP, NEW_LOOP, "周期任务调用 flush 与压实"),
    ]
    changed = 0
    for old, new, label in edits:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("  ✅", label)

    # 退出前补一次 flush（和账号文件一样）
    old_exit = "    FlushUsersIfDirty();"
    if "FlushHistoryIfDirty();\n    return 0;" not in text:
        idx = text.rfind(old_exit)
        if idx >= 0:
            text = text[:idx + len(old_exit)] + "\n    FlushHistoryIfDirty();" + text[idx + len(old_exit):]
            changed += 1
            print("  ✅ 退出前 flush 历史")

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("共 %d 处" % changed)


if __name__ == "__main__":
    main()
