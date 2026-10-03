// Linux 客户端纯逻辑部分的单元测试：色码解析、TOFU 判定、Tab 补全、ANSI 渲染。
//
// 为什么这几块值得测：它们**错了不会报错**——颜色不对只是"看着不对"，
// 而 TOFU 判错就等于中间人换钥匙悄无声息地通过了。所以必须钉死。
#include <cstdio>
#include <string>
#include <vector>

#include "chat_color.h"
#include "file_transfer.h"
#include "files_parse.h"
#include "voice.h"
#include "voice_notes.h"  // 语音采样率/上限：三端共用同一套常量
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

    {
        std::printf("[9] 附件（FILE_OFFER）解析\n");
        // 服务器实际发的格式：**第一格是上传者**。第一版漏了它，把昵称当成附件 ID，
        // 表现是"附件卡片不显示"——所以这条必须有测试。
        const std::string real = "FILE_OFFER 02:32 alice F1 " +
                                 dchat::Base64Encode(std::string("报告.pdf")) + " 123456 0 0";
        const dchat::FileOffer parsed = dchat::ParseFileOffer(real);
        check(parsed.valid, "能解析服务器发的 FILE_OFFER");
        check(parsed.owner == "alice", "上传者解析成 alice（不是被当成 ID）");
        check(parsed.id == "F1", "附件 ID 是 F1");
        check(parsed.name == "报告.pdf", "文件名从 Base64 解出来");
        check(parsed.size == 123456, "字节数正确");
        check(!parsed.IsVoice(), "没有种类 -> 普通文件");
        check(!parsed.hasThumbnail, "缩略图标记 0");

        // 不带上传者的老写法也要能解析
        const std::string old = "FILE_OFFER F2 " +
                                dchat::Base64Encode(std::string("a.txt")) + " 10";
        const dchat::FileOffer parsedOld = dchat::ParseFileOffer(old);
        check(parsedOld.valid, "不带上传者的写法也能解析");
        check(parsedOld.id == "F2" && parsedOld.name == "a.txt", "老写法字段正确");

        // 种类在最后一格：voice 要认出来（安卓录的是 .m4a、桌面是 .wav，不能看扩展名）
        const std::string voice = "FILE_OFFER bob F3 " +
                                  dchat::Base64Encode(std::string("voice.wav")) + " 5000 0 voice";
        const dchat::FileOffer parsedVoice = dchat::ParseFileOffer(voice);
        check(parsedVoice.valid && parsedVoice.IsVoice(), "voice 种类被认成语音");

        // 残缺的行不能崩，也不能当成有效
        check(!dchat::ParseFileOffer("FILE_OFFER F4").valid, "字段不够 -> 无效");
        check(!dchat::ParseFileOffer("FILE_OFFER F5 !!!notbase64!!! 10").valid,
              "文件名字段不是 Base64 -> 无效");
        check(!dchat::ParseFileOffer("FILE_OFFER F6 " +
                                     dchat::Base64Encode(std::string("x.txt")) + " 0")
                   .valid,
              "0 字节 -> 无效（协议不接受空附件）");
        check(!dchat::ParseFileOffer("").valid, "空行 -> 无效");
    }
    {
        std::printf("[10] 语音判定\n");
        // 上限和另外两端用的是同一套常量（voice_notes.h），谁都不能自己写死数字
        check(dchat::kVoiceSampleRate == 16000, "采样率 16 kHz（三端一致）");
        check(dchat::kVoiceChannels == 1, "单声道");
        check(dchat::kVoiceBitsPerSample == 16, "16 位");
        check(dchat::kMaxVoiceSeconds >= 60, "2 MB 上限换算出来至少能录 60 秒");

        // 太短的不能发：点一下也会触发录音，全是噪音
        check(!dchat::WhyCannotSendVoice("a.wav", 1000, 0).empty(), "0 秒 -> 拒绝");
        // 负数秒是**时长未知**的哨兵值（读不出 WAV 头时界面会拿到 -1），
        // 这种情况必须放行：拒了用户就永远发不出"我们算不出时长"的录音，
        // 而大小那条上限照样兜得住。
        check(dchat::WhyCannotSendVoice("a.wav", 16000, -1).empty(),
              "时长未知（-1）-> 放行，交给大小那条兜");
        check(dchat::WhyCannotSendVoice("a.wav", 16000, 1).empty(), "1 秒可以发");
        check(dchat::WhyCannotSendVoice("a.wav", 16000, 30).empty(), "30 秒可以发");
        check(!dchat::WhyCannotSendVoice("a.wav", dchat::kMaxVoiceBytes + 1, 30).empty(),
              "超过 2 MB -> 拒绝");
        check(dchat::WhyCannotSendVoice("a.wav", dchat::kMaxVoiceBytes, 64).empty(),
              "正好 2 MB 可以发（边界不算超）");

        // 时长格式：分钟不补零、秒补零（和另外两端一致）
        check(dchat::FormatDuration(7) == "0:07", "7 秒 -> 0:07");
        check(dchat::FormatDuration(83) == "1:23", "83 秒 -> 1:23");
        check(dchat::FormatDuration(60) == "1:00", "60 秒 -> 1:00");
        check(dchat::FormatDuration(-3) == "0:00", "负数夹成 0:00");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
