"""PPT 更新：把「文件暂存改磁盘」和「每夜 02:00 自动重启 + 倒计时公告」写进去。

不改页数（仍是 20 页，用户要求 ≤20）：只在现有那页补一行实测、改标题与副标题。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

OLD_TITLE = '''    slide = add_slide(prs, "补上三项能力：历史落盘 · 离线消息 · 断点续传",
                      "都是「以前直接丢掉」的东西：重启后记录没了、你不在线消息没了、断了要重下")'''

NEW_TITLE = '''    slide = add_slide(prs, "补上三项能力：历史落盘 · 离线消息 · 断点续传",
                      "都是「以前直接丢掉」的东西：重启后记录没了、你不在线消息没了、断了要重下"
                      "；顺带把文件暂存从内存搬到磁盘、加了每夜自动重启")'''

OLD_MEASURE = '''        "· 续传：64 KB 文件下到 46.0 KB 时掐断，重连后服务端日志 "
        "「从 46.0 KB 处续传」，最终文件 md5 与源文件完全一致",
    ], accent=NAVY, fill=BLUE_L, title_size=13, body_size=11)'''

NEW_MEASURE = '''        "· 续传：64 KB 文件下到 46.0 KB 时掐断，重连后服务端日志 "
        "「从 46.0 KB 处续传」，最终文件 md5 与源文件完全一致",
        "· 存储改磁盘：24 MB 上传后服务端 RSS 只涨 52 KB（以前涨 24 MB）；"
        "淘汰 / 过期 / 重启都会把磁盘文件一起清掉；每日 02:00 自动重启并提前发 5 条倒计时公告",
    ], accent=NAVY, fill=BLUE_L, title_size=13, body_size=11)'''


def main():
    with io.open(TARGET, encoding="utf-8") as fh:
        text = fh.read()
    if "存储改磁盘：24 MB" in text:
        raise SystemExit("已经改过了")
    ok = 0
    for old, new, label in [
        (OLD_TITLE, NEW_TITLE, "标题与副标题"),
        (OLD_MEASURE, NEW_MEASURE, "实测块补一行"),
    ]:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        ok += 1
        print("  ✅", label)
    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("共 %d / 2 处" % ok)


if __name__ == "__main__":
    main()
