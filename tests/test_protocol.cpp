// 协议层单元测试：不需要网络即可运行。
#include <cstdio>
#include <cstring>
#include <string>

#include "protocol.h"

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}
}  // namespace

int main() {
    std::printf("== dchat protocol tests ==\n");

    {
        std::printf("[1] parsing\n");
        const auto m1 = dchat::ParseLine("MSG hello world");
        check(m1.command == "MSG" && m1.rest == "hello world", "command and text are split");
        const auto m2 = dchat::ParseLine("  nick   bob  ");
        check(m2.command == "NICK" && m2.rest == "bob", "command is uppercased, rest is trimmed");
        const auto m3 = dchat::ParseLine("SAY alice hi   there");
        const auto words = m3.Words();
        check(words.size() == 3 && words[0] == "alice" && words[2] == "there",
              "words split keeps inner spacing inside each word");
        check(m3.rest == "alice hi   there", "rest preserves inner spaces");
        const auto m4 = dchat::ParseLine("   ");
        check(m4.command.empty(), "blank line yields empty command");
        const auto m5 = dchat::ParseLine("QUIT\r");
        check(m5.command == "QUIT" && m5.rest.empty(), "trailing carriage return is ignored");
    }

    {
        std::printf("[2] building lines\n");
        check(dchat::BuildLine("say", "alice hi") == "SAY alice hi", "command is uppercased");
        check(dchat::BuildLine("MSG", "line\nbreak") == "MSG linebreak", "newlines are stripped");
    check(dchat::BuildLine("bad command", "x").empty(), "invalid command name is rejected");
    // 文件传输的 FILE_SEND / FILE_DATA 带下划线，必须能正常构造（曾经在这里被误判成非法命令）
    check(dchat::BuildLine("FILE_SEND", "id name 10") == "FILE_SEND id name 10",
          "带下划线的命令名可以构造");
    check(dchat::BuildLine("FILE_DATA", "id YWJj") == "FILE_DATA id YWJj", "FILE_DATA 也能构造");
    check(dchat::BuildLine("FILE_END", "id") == "FILE_END id", "FILE_END 也能构造");
    check(dchat::BuildLine("X2", "y") == "X2 y", "命令名里的数字也可以");
    check(dchat::BuildLine("BAD-NAME", "x").empty(), "连字符仍然算非法命令名");
        check(dchat::BuildLine("", "x").empty(), "empty command is rejected");
        const std::string longText(5000, 'a');
        check(dchat::BuildLine("MSG", longText).size() == dchat::kMaxLineBytes, "line length is capped");
    }

    {
        std::printf("[3] nickname rules\n");
        dchat::NickError err = dchat::NickError::None;
        check(dchat::NormalizeNick("  bob ", &err) == "bob" && err == dchat::NickError::None,
              "surrounding spaces are trimmed");
        check(dchat::NormalizeNick("", &err).empty() && err == dchat::NickError::Empty,
              "empty nickname is rejected");
        check(dchat::NormalizeNick("a b", &err).empty() && err == dchat::NickError::IllegalChar,
              "inner space is rejected");
        check(dchat::NormalizeNick("a:b", &err).empty() && err == dchat::NickError::IllegalChar,
              "colon is rejected");
        check(dchat::NormalizeNick("1234567890123", &err).empty() && err == dchat::NickError::TooLong,
              "13 characters is too long");
        check(dchat::NormalizeNick("123456789012", &err) == "123456789012", "12 characters is fine");
        check(dchat::NormalizeNick("小雷同学", &err) == "小雷同学", "chinese nickname is allowed");
        // 中文按字符计，而不是按字节：6 个汉字 = 6 个字符
        check(dchat::Utf8CharCount("小雷同学") == 4, "utf-8 character counting");
        check(dchat::Utf8Truncate("小雷同学", 2) == "小雷", "utf-8 truncate keeps whole characters");
    }

    {
        std::printf("[4] stream framing\n");
        dchat::LineBuffer buffer;
        const char* chunk = "one\r\ntwo\nthr";
        buffer.Append(chunk, std::strlen(chunk));
        std::string line;
        check(buffer.PopLine(&line) && line == "one", "first line extracted without CR");
        check(buffer.PopLine(&line) && line == "two", "second line extracted");
        check(!buffer.PopLine(&line), "partial line is buffered");
        buffer.Append("ee\n", 3);
        check(buffer.PopLine(&line) && line == "three", "line completed by later chunk");
        check(!buffer.PopLine(&line), "buffer is empty afterwards");
        check(!buffer.bad(), "healthy stream is not flagged");

        dchat::LineBuffer over;
        over.Append(std::string(5000, 'x').c_str(), 5000);
        check(over.bad(), "over-long line without terminator is flagged");

        dchat::LineBuffer split;
        const std::string payload = "MSG " + std::string(4090, 'y') + "\n";
        split.Append(payload.c_str(), payload.size());
        check(!split.bad() && split.PopLine(&line), "line at the size limit is accepted");
    }

    {
        std::printf("[5] message builders\n");
        check(dchat::MakeSay("alice", "你好") == "SAY alice 你好", "say message format");
        check(dchat::MakeJoined("bob") == "JOINED bob", "joined message format");
        check(dchat::MakeError(dchat::NickErrorText(dchat::NickError::Taken)) ==
                  "ERROR 昵称已被占用",
              "error text is utf-8 chinese");
    }

    // ------------------------------------------------------------------
    // 多行文本转义（协议是行式的，真换行会被 BuildLine 丢掉，
    // 所以多行消息必须先转义；maxtextlines 这条规则也依赖它）
    // ------------------------------------------------------------------
    {
        std::printf("[6] text escaping\n");
        check(dchat::EscapeText("abc") == "abc", "no special chars -> unchanged");
        check(dchat::EscapeText("a\nb") == "a\\nb", "newline -> backslash n");
        check(dchat::EscapeText("a\r\nb") == "a\\nb", "crlf collapses to one newline");
        check(dchat::EscapeText("a\rb") == "a\\nb", "lone cr is also a newline");
        check(dchat::EscapeText("a\\b") == "a\\\\b", "backslash is doubled");
        check(dchat::EscapeText("a\\nb") == "a\\\\nb",
              "literal backslash-n gets escaped (so it is NOT read as a newline)");
        check(dchat::EscapeText("") == "", "empty stays empty");
    }

    {
        std::printf("[7] text unescaping\n");
        check(dchat::UnescapeText("abc") == "abc", "plain text unchanged");
        check(dchat::UnescapeText("a\\nb") == "a\nb", "backslash n -> newline");
        check(dchat::UnescapeText("a\\\\b") == "a\\b", "doubled backslash -> one backslash");
        // 关键：旧客户端发来的真实反斜杠不能被吃掉
        check(dchat::UnescapeText("C:\\x") == "C:\\x", "unknown escape is preserved verbatim");
        check(dchat::UnescapeText("tail\\") == "tail\\", "trailing lone backslash is preserved");
        check(dchat::UnescapeText("") == "", "empty stays empty");
    }

    {
        std::printf("[8] escape round-trip\n");
        const char* samples[] = {
            "hello",
            "第一行\n第二行",
            "a\\b",
            "路径 C:\\Users\\test",
            "混合 \\ 和 \n 都有",
            "\n\n开头两个换行",
            "结尾换行\n",
        };
        bool allOk = true;
        for (const char* sample : samples) {
            const std::string original(sample);
            if (dchat::UnescapeText(dchat::EscapeText(original)) != original) allOk = false;
        }
        check(allOk, "escape -> unescape returns the original for all samples");

        // 转义后的文本里不能再有真换行，否则会破坏分帧
        bool noRawNewline = true;
        for (const char* sample : samples) {
            const std::string escaped = dchat::EscapeText(sample);
            if (escaped.find('\n') != std::string::npos ||
                escaped.find('\r') != std::string::npos) {
                noRawNewline = false;
            }
        }
        check(noRawNewline, "escaped text never contains a raw newline");
    }

    {
        std::printf("[9] line counting\n");
        check(dchat::CountTextLines("") == 0, "empty text is 0 lines");
        check(dchat::CountTextLines("abc") == 1, "single line");
        check(dchat::CountTextLines("a\\nb") == 2, "two lines");
        check(dchat::CountTextLines("a\\nb\\nc") == 3, "three lines");
        check(dchat::CountTextLines("a\\\\nb") == 1,
              "escaped backslash followed by n is NOT a line break");
        check(dchat::CountTextLines("a\\\\\\nb") == 2,
              "a literal backslash then a real newline is 2 lines");
        check(dchat::CountTextLines("end\\") == 1, "trailing lone backslash does not crash");
        check(dchat::CountTextLines("\\n") == 2, "text starting with a newline is 2 lines");
    }

    {
        std::printf("[10] escaping survives the wire format\n");
        // 多行文本经过 BuildLine 之后必须仍是一行，且换行信息不丢
        const std::string multi = "第一行\n第二行\n第三行";
        const std::string line = dchat::BuildLine("MSG", dchat::EscapeText(multi));
        check(line.find('\n') == std::string::npos, "built line has no raw newline");
        const dchat::Message parsed = dchat::ParseLine(line);
        check(parsed.command == "MSG", "command survives");
        check(dchat::UnescapeText(parsed.rest) == multi, "text survives the round trip intact");
        check(dchat::CountTextLines(parsed.rest) == 3, "server can count 3 lines from the wire form");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
