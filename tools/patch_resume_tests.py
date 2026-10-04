"""给 ParseUint64 补单测（续传的起点解析全靠它）。"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "tests", "test_file_transfer.cpp")

ANCHOR = '''    std::printf("\\n%d checks, %d failures\\n", g_checks, g_failures);'''

NEW_BLOCK = '''    {
        std::printf("[续传] ParseUint64：断点位置解析\\n");
        unsigned long long value = 0;
        check(dchat::ParseUint64("0", &value) && value == 0, "0 是合法起点（从头下）");
        check(dchat::ParseUint64("4096", &value) && value == 4096, "普通数字");
        check(dchat::ParseUint64("67108864", &value) && value == 67108864ull,
              "64MB 也能解析（文件上限就是它）");
        // 下面是"必须拒绝"的一组：解析错了会导致写指针错位、拼出坏文件
        check(!dchat::ParseUint64("", &value), "空串要拒绝");
        check(!dchat::ParseUint64("-1", &value), "负号要拒绝（无符号数没有负数）");
        check(!dchat::ParseUint64("+5", &value), "正号也要拒绝，别做宽容解析");
        check(!dchat::ParseUint64("12a", &value), "夹杂字母要拒绝");
        check(!dchat::ParseUint64(" 5", &value), "前导空格要拒绝");
        check(!dchat::ParseUint64("5 ", &value), "尾随空格要拒绝");
        check(!dchat::ParseUint64("1.5", &value), "小数要拒绝");
        check(!dchat::ParseUint64("0x10", &value), "十六进制要拒绝");
        // 溢出必须失败而不是回绕：回绕出来的小数字会让续传位置跳到文件开头附近，
        // 后果是"悄悄拼出一个坏文件"，比直接拒绝严重得多
        check(!dchat::ParseUint64("99999999999999999999999", &value), "溢出要拒绝");
    }

    std::printf("\\n%d checks, %d failures\\n", g_checks, g_failures);'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    if "ParseUint64" in text:
        raise SystemExit("已经加过")
    if ANCHOR not in text:
        raise SystemExit("找不到结尾锚点")
    text = text.replace(ANCHOR, NEW_BLOCK, 1)
    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("已加 ParseUint64 单测")


if __name__ == "__main__":
    main()
