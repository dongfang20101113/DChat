// 终端渲染层的单元测试。
//
// 为什么值得单独测：渲染错了**不会报错，只会显示不对** —— 颜色串错位会让整行花掉、
// 折行时把 ANSI 序列从中间截断会让后面所有文字都用错颜色，这类问题只能靠眼睛在
// 真终端里发现，很容易漏。所以把渲染抽成纯函数，在这里钉死。
#include <cstdio>
#include <string>
#include <vector>

#include "chat_color.h"
#include "chat_core.h"
#include "cli_render.h"

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

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/** 一行里"看得见的字符数"，ANSI 序列不算。 */
std::size_t VisibleLen(const std::string& text) {
    std::size_t visible = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b') {
            const std::size_t end = text.find('m', i);
            if (end == std::string::npos) break;
            i = end;
            continue;
        }
        ++visible;
    }
    return visible;
}

/** 把一条发言拼成 ChatMessage，省得每个用例都写一遍。 */
dchat::ChatMessage SayMessage(const std::string& nick, const std::string& text, bool mention) {
    dchat::ChatMessage message;
    message.valid = true;
    message.kind = dchat::ChatMessage::Kind::Say;
    message.nick = nick;
    message.text = text;
    message.time = "12:34";
    message.mentionMe = mention;
    message.hasColor = text.find('&') != std::string::npos;
    return message;
}

}  // namespace

int main() {
    std::printf("== dchat cli render tests ==\n");
    using namespace dchat;

    {
        std::printf("[1] 普通发言\n");
        const RenderedLine line = RenderChatMessage(SayMessage("bob", "你好", false), true);
        check(line.visible, "有效消息要显示");
        check(Contains(line.text, "[12:34]"), "带时间戳");
        check(Contains(line.text, "<bob>"), "带昵称");
        check(Contains(line.text, "你好"), "带正文");
        check(!Contains(line.text, kAnsiMention), "没 @我 就不高亮");
    }

    {
        std::printf("[2] @我 高亮\n");
        const RenderedLine line = RenderChatMessage(SayMessage("bob", "@alice 在吗", true), true);
        check(Contains(line.text, kAnsiMention), "有人 @我 要用高亮色");
        check(Contains(line.text, "@alice 在吗"), "正文原样保留");
        // 高亮要能被重置，否则后面所有输出都会是黄的
        check(Contains(line.text, AnsiReset()), "高亮后面要重置颜色");
    }

    {
        std::printf("[3] 色码：开着时要上色\n");
        const RenderedLine line = RenderChatMessage(SayMessage("bob", "&c红色字", false), true);
        check(Contains(line.text, "\x1b[38;2;"), "要生成真彩色 ANSI 序列");
        check(Contains(line.text, "红色字"), "文字本身保留");
        check(!Contains(line.text, "&c"), "色码标记不该显示出来");
        check(Contains(line.text, AnsiReset()), "上色后要重置");
    }

    {
        std::printf("[4] 色码：服务器/用户关掉彩色时要原样显示\n");
        // 这条最容易被忽略：关掉彩色后如果还是把 &c 吃掉，用户会以为字丢了
        const RenderedLine line = RenderChatMessage(SayMessage("bob", "&c红色字", false), false);
        check(Contains(line.text, "&c红色字"), "关掉彩色时色码原样显示");
        check(!Contains(line.text, "\x1b[38;2;"), "不该生成颜色序列");
    }

    {
        std::printf("[5] 其它类型的消息\n");
        ChatMessage error;
        error.valid = true;
        error.kind = ChatMessage::Kind::Error;
        error.text = "出错了";
        const RenderedLine renderedError = RenderChatMessage(error, true);
        check(Contains(renderedError.text, kAnsiError), "错误用红色");
        check(Contains(renderedError.text, "[错误]"), "错误有前缀");

        ChatMessage system;
        system.valid = true;
        system.kind = ChatMessage::Kind::System;
        system.text = "服务器通知";
        check(Contains(RenderChatMessage(system, true).text, kAnsiSystem), "系统提示用青色");

        ChatMessage offer;
        offer.valid = true;
        offer.kind = ChatMessage::Kind::FileOffer;
        offer.text = "报告.pdf (1.2 MB)";
        check(Contains(RenderChatMessage(offer, true).text, "报告.pdf"), "附件卡片要显示文件名");

        ChatMessage invalid;
        invalid.valid = false;
        check(!RenderChatMessage(invalid, true).visible, "无效消息不显示");
    }

    {
        std::printf("[6] 折行\n");
        const std::vector<std::string> lines = WrapAnsi("aaaaabbbbbcccccddddd", 5);
        check(lines.size() == 4, "20 个字符按宽度 5 折成 4 行");
        check(lines[0] == "aaaaa", "第一行正确");
        check(lines[3] == "ddddd", "最后一行正确");
        check(VisibleLen(lines[0]) == 5, "每行可见宽度等于给定宽度");
    }

    {
        std::printf("[7] 折行**不能**把 ANSI 序列截断（最容易出的显示 bug）\n");
        // 构造：红色 "abcdefghij"，宽度 5。ANSI 序列本身有 19 字节，
        // 如果按字节折行就会把它切开，后面所有文字都会用错颜色。
        const std::string colored = std::string(AnsiForeground(0xFF0000)) + "abcdefghij" + AnsiReset();
        const std::vector<std::string> lines = WrapAnsi(colored, 5);
        check(lines.size() >= 2, "要折成多行");
        for (std::size_t i = 0; i < lines.size(); ++i) {
            // 每一行里的转义序列都必须完整（有 ESC 就必须有配对的 'm'）
            const std::size_t escape = lines[i].find('\x1b');
            if (escape != std::string::npos) {
                check(lines[i].find('m', escape) != std::string::npos,
                      "每行里的 ANSI 序列都完整");
            }
        }
        // 拼回来应该和原文一模一样（不丢字符、不多字符）
        std::string joined;
        for (const std::string& line : lines) joined += line;
        check(joined == colored, "折行后拼回来必须和原文完全一致");
    }

    {
        std::printf("[8] 折行边界\n");
        check(WrapAnsi("", 5).size() == 1, "空串也给一行（否则调用方要特判）");
        check(WrapAnsi("abc", 0).size() == 1, "宽度 0 时不折（终端宽度取不到的情况）");
        check(WrapAnsi("abc", -1).size() == 1, "负宽度也不折");
        check(WrapAnsi("abcde", 5).size() == 1, "刚好等于宽度不折");
        check(WrapAnsi("abcdef", 5).size() == 2, "超一个字符就折");
    }

    {
        std::printf("[9] 纯文字的行不该被加颜色\n");
        const RenderedLine notice = RenderStatusLine("正在上传 xx …");
        check(notice.visible, "状态行要显示");
        check(notice.text == "正在上传 xx …", "状态行原样输出，不加色");
        check(!Contains(notice.text, "\x1b["), "状态行里不该有 ANSI 序列");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
