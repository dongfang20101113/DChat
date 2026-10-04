// 历史落盘的单元测试。
//
// 为什么必须测：落盘错了**不会报错**，只会"少一条"或"多一条"，而且往往
// 只在进程被强杀这种难以复现的时机暴露出来。所以把最容易错的三件事钉死：
//   1. 崩溃时写了一半的最后一行，绝不能被当成合法记录加载回来；
//   2. 序号必须单调递增 —— 离线补发靠它判断"谁还没看过"，
//      序号一旦回退或复用，消息就会被永远跳过；
//   3. 上限（行数 / 字节）在内存和文件两侧都要守住。
#include <cstdio>
#include <string>

#include "history_store.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

}  // namespace

int main() {
    std::printf("== dchat history store tests ==\n");
    using dchat::FormatHistoryFileLine;
    using dchat::HistoryEntry;
    using dchat::HistoryStore;
    using dchat::ParseHistoryFileLine;

    {
        std::printf("[1] 追加与序号单调\n");
        HistoryStore store(100, 1024 * 1024);
        const unsigned long long a = store.Append("12:00 SAY alice 一");
        const unsigned long long b = store.Append("12:01 SAY bob 二");
        const unsigned long long c = store.Append("12:02 SAY alice 三");
        check(a == 1, "第一条序号从 1 开始（0 留给「从没看过」）");
        check(b == a + 1 && c == b + 1, "序号逐条 +1");
        check(store.LastSeq() == c, "LastSeq 是最大值");
        check(store.Size() == 3, "三条都在");
    }

    {
        std::printf("[2] 行数上限：丢最旧的，但序号继续往前走\n");
        HistoryStore store(3, 1024 * 1024);
        for (int i = 1; i <= 6; ++i) store.Append("line " + std::to_string(i));
        check(store.Size() == 3, "只留 3 条");
        const std::vector<HistoryEntry> snap = store.Snapshot();
        check(snap.front().line == "line 4", "留下的是最新的三条");
        check(snap.back().line == "line 6", "最后一条是最新的");
        // 关键：被丢掉的那三条不会让序号回退
        check(snap.front().seq == 4 && snap.back().seq == 6, "序号仍是 4/5/6，没有回退");
        const unsigned long long next = store.Append("line 7");
        check(next == 7, "新消息接在 6 后面");
    }

    {
        std::printf("[3] 字节上限\n");
        HistoryStore store(1000, 60);
        for (int i = 0; i < 20; ++i) store.Append("0123456789");  // 每条 10+ 字节
        check(store.Bytes() <= 60, "字节数被限制在预算内");
        check(store.Size() < 20, "确实裁掉了旧的");
        check(store.LastSeq() == 20, "序号仍然是 20（裁剪不影响序号）");
    }

    {
        std::printf("[4] 文件行格式：拼装与解析往返\n");
        HistoryEntry entry;
        entry.seq = 42;
        entry.line = "12:34 SAY alice 你好 世界";
        const std::string text = FormatHistoryFileLine(entry);
        check(!text.empty() && text.back() == '\n', "每条以换行结尾");
        HistoryEntry back;
        check(ParseHistoryFileLine(text.substr(0, text.size() - 1), &back), "能解析回来");
        check(back.seq == 42, "序号一致");
        check(back.line == entry.line, "正文逐字节一致");
    }

    {
        std::printf("[5] 正文里含制表符也不能解析错位\n");
        // 按**第一个**制表符切分，所以正文里的制表符属于正文
        HistoryEntry entry;
        entry.seq = 7;
        entry.line = "12:00 SAY alice 前\t后";
        HistoryEntry back;
        check(ParseHistoryFileLine("7\t12:00 SAY alice 前\t后", &back), "能解析");
        check(back.seq == 7, "序号取的是第一个制表符之前");
        check(back.line == entry.line, "制表符保留在正文里");
    }

    {
        std::printf("[6] 坏行必须被丢掉（这是崩在写一半时的样子）\n");
        HistoryEntry out;
        check(!ParseHistoryFileLine("", &out), "空行");
        check(!ParseHistoryFileLine("没有制表符的一行", &out), "没有分隔符");
        check(!ParseHistoryFileLine("\t只有正文", &out), "序号为空");
        check(!ParseHistoryFileLine("abc\t正文", &out), "序号不是数字");
        check(!ParseHistoryFileLine("12", &out), "只有序号没正文");
        check(!ParseHistoryFileLine("12\t", &out), "正文为空");
        check(!ParseHistoryFileLine("-5\t正文", &out), "负号不是合法数字");
    }

    {
        std::printf("[7] 加载：容忍最后一行被截断（强杀场景）\n");
        std::string file;
        file += FormatHistoryFileLine({1, "12:00 SAY a 一"});
        file += FormatHistoryFileLine({2, "12:01 SAY b 二"});
        // 第三行只写了一半：没有结尾换行、正文也是断的
        file += "3\t12:02 SAY c 三";
        HistoryStore store(100, 1024 * 1024);
        const std::size_t loaded = store.LoadFromText(file);
        check(loaded == 3, "完整的三行都读进来了（第三行本身是合法格式）");
        check(store.LastSeq() == 3, "序号到 3");

        // 真正的"半行"：连制表符后面都被截断成了非法内容
        std::string broken = FormatHistoryFileLine({1, "12:00 SAY a 一"});
        broken += "2";  // 只写了序号，正文还没落盘
        HistoryStore s2(100, 1024 * 1024);
        check(s2.LoadFromText(broken) == 1, "半行被丢弃，只读回完整的那一条");
        check(s2.LastSeq() == 1, "序号不会因为半行而跳到 2");
    }

    {
        std::printf("[8] 加载：序号必须严格递增（文件被手工改过）\n");
        std::string file;
        file += FormatHistoryFileLine({1, "a"});
        file += FormatHistoryFileLine({1, "b"});   // 重复
        file += FormatHistoryFileLine({5, "c"});
        file += FormatHistoryFileLine({3, "d"});   // 回退
        file += FormatHistoryFileLine({6, "e"});
        HistoryStore store(100, 1024 * 1024);
        check(store.LoadFromText(file) == 3, "只接受严格递增的三条");
        const std::vector<HistoryEntry> snap = store.Snapshot();
        check(snap.size() == 3, "内存里三条");
        check(snap[0].seq == 1 && snap[1].seq == 5 && snap[2].seq == 6,
              "序号是 1/5/6，乱序与重复被丢弃");
    }

    {
        std::printf("[9] 加载后还要守当前配置的上限\n");
        std::string file;
        for (int i = 1; i <= 50; ++i) file += FormatHistoryFileLine(
            {static_cast<unsigned long long>(i), "line " + std::to_string(i)});
        HistoryStore store(5, 1024 * 1024);  // 上限被调小过
        store.LoadFromText(file);
        check(store.Size() == 5, "加载后按上限裁到 5 条");
        check(store.LastSeq() == 50, "序号仍是文件里的最大值");
    }

    {
        std::printf("[10] 序列化往返（压实重写用）\n");
        HistoryStore store(100, 1024 * 1024);
        store.Append("12:00 SAY a 一");
        store.Append("12:01 SAY b 二");
        const std::string text = store.Serialize();
        HistoryStore reloaded(100, 1024 * 1024);
        check(reloaded.LoadFromText(text) == 2, "重写后能原样读回");
        check(reloaded.Serialize() == text, "再序列化结果一致");
    }

    {
        std::printf("[11] 离线补发：Since\n");
        HistoryStore store(100, 1024 * 1024);
        for (int i = 1; i <= 5; ++i) store.Append("m" + std::to_string(i));
        check(store.Since(0, 100).size() == 5, "从没看过（0）能拿到全部");
        check(store.Since(3, 100).size() == 2, "看过 3 条后剩 2 条");
        check(store.Since(3, 100)[0].seq == 4, "补发从第 4 条开始");
        check(store.Since(5, 100).empty(), "全看过了就没有要补的");
        check(store.Since(0, 0).empty(), "limit=0 表示功能没开，返回空");
        check(store.Since(0, 2).size() == 2, "limit 限制条数");
        check(store.Since(0, 2).back().seq == 2, "limit 是从最旧的开始数");
        check(store.Since(999, 100).empty(), "序号超前（客户端记录被改过）也不越界");
    }

    {
        std::printf("[12] Clear 不能重置序号（运行中清屏的安全性）\n");
        HistoryStore store(100, 1024 * 1024);
        store.Append("a");
        store.Append("b");
        store.Clear();
        check(store.Size() == 0, "清空后没有内容");
        const unsigned long long next = store.Append("c");
        check(next == 3, "新消息序号继续是 3，没有回到 1");
        // 否则：客户端记着"我看到第 2 条"，新消息若也叫 1/2 就会被永远跳过
        HistoryStore fresh(100, 1024 * 1024);
        check(fresh.Since(2, 100).empty() || fresh.Size() == 0, "（对照）新 store 是空的");
    }

    {
        std::printf("[13] 字节账要和实际写出的文件对得上（预算才准）\n");
        HistoryStore store(100, 1024 * 1024);
        store.Append("12:34 SAY alice 你好");
        store.Append("12:35 SAY bob 世界和平");
        const std::size_t serialized = store.Serialize().size();
        // 允许小误差：序号位数按十进制估、结尾换行等
        const long long diff = static_cast<long long>(store.Bytes()) -
                               static_cast<long long>(serialized);
        check(diff >= -2 && diff <= 4, "内存记账与实际文件大小基本一致");
    }

    {
        std::printf("[14] 阅读进度：格式与解析\n");
        using dchat::FormatSeenFileLine;
        using dchat::ParseSeenFile;
        using dchat::ParseSeenFileLine;
        using dchat::SeenRecord;
        SeenRecord record;
        record.nick = "alice";
        record.seq = 123;
        const std::string text = FormatSeenFileLine(record);
        check(text == "alice\t123\n", "格式是 昵称\\t序号\\n");
        SeenRecord back;
        check(ParseSeenFileLine("alice\t123", &back), "能解析");
        check(back.nick == "alice" && back.seq == 123, "昵称与序号一致");
        check(!ParseSeenFileLine("alice\t", &back), "空序号要拒绝");
        check(!ParseSeenFileLine("\t5", &back), "空昵称要拒绝");
        check(!ParseSeenFileLine("alice 5", &back), "没有制表符要拒绝");
    }

    {
        std::printf("[15] 阅读进度：同名取最大值（进度只能前进）\n");
        using dchat::ParseSeenFile;
        // 追加写的文件里同一用户会有多行；顺序还可能被打乱
        std::string file;
        file += "alice\t10\n";
        file += "bob\t3\n";
        file += "alice\t25\n";
        file += "alice\t7\n";   // 回退的一行，必须被忽略
        file += "半行没有制表符\n";  // 强杀留下的坏行
        const std::vector<dchat::SeenRecord> records = ParseSeenFile(file);
        check(records.size() == 2, "只得到两个用户");
        unsigned long long alice = 0;
        unsigned long long bob = 0;
        for (const dchat::SeenRecord& r : records) {
            if (r.nick == "alice") alice = r.seq;
            if (r.nick == "bob") bob = r.seq;
        }
        check(alice == 25, "alice 取最大值 25（7 那行被忽略）");
        check(bob == 3, "bob 是 3");
        check(ParseSeenFile("").empty(), "空文件返回空");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
