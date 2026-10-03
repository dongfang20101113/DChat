// Linux 客户端纯逻辑部分的单元测试：色码解析、TOFU 判定、Tab 补全、ANSI 渲染。
//
// 为什么这几块值得测：它们**错了不会报错**——颜色不对只是"看着不对"，
// 而 TOFU 判错就等于中间人换钥匙悄无声息地通过了。所以必须钉死。
#include <cstdio>
#include <string>
#include <vector>

#include "chat_color.h"
#include "server_command.h"
#include "trust.h"

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

std::string Join(const std::vector<dchat::ColorSegment>& segments) {
    std::string out;
    for (const dchat::ColorSegment& segment : segments) out += segment.text;
    return out;
}

}  // namespace

int main() {
    std::printf("== dchat linux client tests ==\n");

    {
        std::printf("[1] 色码解析\n");
        const std::uint32_t white = 0xFFFFFF;
        auto plain = dchat::ParseColorSegments("hello", white, true);
        check(plain.size() == 1, "没有色码时只有一段");
        check(plain[0].text == "hello", "文字原样");
        check(!plain[0].hasColor, "没有色码时 hasColor 为假");

        auto quick = dchat::ParseColorSegments("&1blue", white, true);
        check(quick.size() == 1, "&1 之后只有一段");
        check(quick[0].text == "blue", "色码本身不出现在文字里");
        check(quick[0].hasColor, "&1 上了色");
        check(quick[0].rgb == 0x0000AA, "&1 是深蓝 0x0000AA（和 Windows/安卓一致）");

        auto two = dchat::ParseColorSegments("&1a&2b", white, true);
        check(two.size() == 2, "两个色码切成两段");
        check(two[0].rgb == 0x0000AA && two[1].rgb == 0x00AA00, "两段颜色各自正确");
        check(Join(two) == "ab", "文字一个字都没丢");

        auto trueColor = dchat::ParseColorSegments("&#ff8800橙", white, true);
        check(trueColor.size() == 1, "真彩色一段");
        check(trueColor[0].rgb == 0xFF8800, "&#rrggbb 解析正确");
        check(trueColor[0].text == "橙", "真彩色后的文字正确");

        auto escaped = dchat::ParseColorSegments("a&&b", white, true);
        check(Join(escaped) == "a&b", "&& 变成一个字面量 &");

        auto trailing = dchat::ParseColorSegments("abc&", white, true);
        check(Join(trailing) == "abc&", "结尾孤立的 & 原样保留");

        auto badHex = dchat::ParseColorSegments("&#zzz", white, true);
        check(Join(badHex) == "&#zzz", "非法十六进制原样保留（不吞字）");

        // 这是 Windows 端和安卓端都踩过的坑：达到段数上限后**只停止开新段**，
        // 剩下的文字必须继续解析并完整保留。
        std::string many;
        for (int i = 0; i < 500; ++i) many += "a";
        auto capped = dchat::ParseColorSegments(many, white, true, 8);
        check(Join(capped) == many, "达到段数上限后文字一个字都没丢");
        check(capped.size() <= 8 + 1, "段数不超过上限（+1 是收尾段）");
    }

    {
        std::printf("[2] 关掉彩色聊天时不解析\n");
        auto off = dchat::ParseColorSegments("&1red", 0xFFFFFF, false);
        check(off.size() == 1, "关掉时只有一段");
        check(off[0].text == "&1red", "关掉时色码原样显示（否则用户以为字被吃了）");
        check(!off[0].hasColor, "关掉时不上色");
    }

    {
        std::printf("[3] 快捷色表\n");
        check(dchat::QuickColorRgb('0') == 0x000000, "&0 是黑");
        check(dchat::QuickColorRgb('f') == 0xFFFFFF, "&f 是白");
        check(dchat::QuickColorRgb('7') == 0xAAAAAA, "&7 是浅灰");
        check(dchat::QuickColorRgb('z') == 0xFFFFFFFFu, "认不出来的字符返回无效值");
        check(std::string(dchat::QuickColorDigits()) == "0123456789abcdef", "色码字符表是 0-9a-f");
    }

    {
        std::printf("[4] RGB 与十六进制互转\n");
        check(dchat::RgbToHex(0x0000AA) == "#0000aa", "转小写 #rrggbb");
        std::uint32_t rgb = 0;
        check(dchat::HexToRgb("#FF8800", &rgb) && rgb == 0xFF8800, "解析大写十六进制");
        check(dchat::HexToRgb("ff8800", &rgb) && rgb == 0xFF8800, "不带 # 也认");
        check(!dchat::HexToRgb("#ff88", &rgb), "长度不对要拒绝");
        check(!dchat::HexToRgb("#gggggg", &rgb), "非十六进制字符要拒绝");
    }

    {
        std::printf("[5] ANSI 渲染\n");
        const std::string red = dchat::AnsiForeground(0xFF0000);
        check(red == "\x1b[38;2;255;0;0m", "真彩色前景序列");
        check(std::string(dchat::AnsiReset()) == "\x1b[0m", "重置序列");
    }

    {
        std::printf("[6] 服务器规则里的 chatcolor\n");
        check(dchat::ChatColorEnabledFromRules("chatcolor=1") == true, "chatcolor=1 是开");
        check(dchat::ChatColorEnabledFromRules("chatcolor=0") == false, "chatcolor=0 是关");
        check(dchat::ChatColorEnabledFromRules("其它规则") == true, "没提就按开着算");
    }

    {
        std::printf("[7] TOFU 判定\n");
        using dchat::TrustKind;
        check(dchat::DecideTrust("", "AA:BB").kind == TrustKind::FirstUse, "没见过 -> 首次");
        check(dchat::DecideTrust("AA:BB", "AA:BB").kind == TrustKind::Unchanged, "一样 -> 没变");
        check(dchat::DecideTrust("AA:BB", "CC:DD").kind == TrustKind::Changed, "不一样 -> 变了");
        check(dchat::DecideTrust("AA:BB", "").kind == TrustKind::None, "没有当前指纹 -> 无");

        const std::string changed = dchat::DescribeTrust(dchat::DecideTrust("AA:BB", "CC:DD"));
        check(changed.find("AA:BB") != std::string::npos, "变了要说清原来是什么");
        check(changed.find("CC:DD") != std::string::npos, "变了要说清现在是什么");

        // 文件读写：同一个键要覆盖，不能越攒越多
        std::string content = dchat::UpsertKnownFingerprint("", "a:1", "AA");
        content = dchat::UpsertKnownFingerprint(content, "b:2", "BB");
        content = dchat::UpsertKnownFingerprint(content, "a:1", "CC");
        check(dchat::LookupKnownFingerprint(content, "a:1") == "CC", "同键覆盖成新值");
        check(dchat::LookupKnownFingerprint(content, "b:2") == "BB", "别的键不受影响");
        check(content.find("AA") == std::string::npos, "旧值不再留在文件里");
        check(dchat::LookupKnownFingerprint(content, "c:3").empty(), "没有的键返回空");
        check(dchat::LookupKnownFingerprint("# 注释\na:1 ZZ", "a:1") == "ZZ", "跳过注释行");
    }

    {
        std::printf("[8] Tab 补全（复用服务器指令补全器）\n");
        dchat::TabCompleter completer;
        auto result = completer.Next("/ba", {"bob"}, {});
        check(result.picked >= 0, "输入 /ba 能选出候选");
        check(result.text.find("ban") != std::string::npos, "补出了 /ban");

        // 昵称补全：参数位置应该给出在线昵称
        dchat::TabCompleter nameCompleter;
        auto named = nameCompleter.Next("/kick bo", {"bob", "carol"}, {});
        check(named.text.find("bob") != std::string::npos, "参数位置补出在线昵称 bob");

        const auto& all = dchat::AllCommandNames();
        check(!all.empty(), "指令表不为空");
        bool hasChatColor = false;
        for (const std::string& name : all) {
            if (name == "chatcolor") hasChatColor = true;
        }
        check(hasChatColor, "指令表里有 chatcolor（Tab 能补出来）");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
