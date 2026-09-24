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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
