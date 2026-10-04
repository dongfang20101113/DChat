"""把三项新能力写进 README。"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "README.md")

ANCHOR = "## 功能\n"

NEW_SECTION = '''## 消息不会因为「你不在」或「进程重启」而丢

2026-10 补上的三项能力，都是以前**直接丢掉**的东西：

| 能力 | 以前 | 现在 |
| --- | --- | --- |
| **聊天历史落盘** | 只在内存（`std::deque`），服务端重启就没了 | 追加写盘 + 上限压实 + 每秒节流 flush；`keepchathistory` 的回放跨重启仍然有效 |
| **离线消息** | 你不在线时别人说的话，**永远看不到** | 每用户记「读到第几条」，重新登录补上错过的（条数由规则限制） |
| **文件断点续传** | 下载中断就得从头再来 | `FILE_GET <id> [已有字节数]`，服务器从断点接着发；客户端用 `.part` 中间文件，下完才改名 |

三条规则/协议细节：

- 新增规则 **`offlinemessages <条数>`**（默认 **0 = 不补发**）。默认关闭是刻意的：
  升级后行为和以前完全一样，要开再开。与 `keepchathistory` 配合使用，
  两者**不会重复发送**——有阅读进度的用户只补差额，新用户仍看到保留范围内的完整历史。
- `FILE_GET` 的第二个参数是**可选**的：不带就还是从头下，所以老客户端不受影响。
- 历史文件的每一行是 `<序号>\\t<原始协议行>`。**崩溃时写了一半的那行会被丢弃** ——
  宁可少一条，也不能凭空多出一条残缺消息；序号持久化且单调递增，
  这样"谁读到第几条"在重启后依然成立。

> 一个真实的兼容性事故（写在这里给以后加字段的人）：续传本来打算让服务器在
> `FILE_BEGIN` 后**追加一格**回显续传起点，看起来完全合理，协议文档也写着
> "新增字段只追加在末尾，老客户端读到旧格数就停"。但 Windows 客户端的解析是
> `fields.size() != 3` —— **要求恰好 3 格**，多一格会让它静默忽略 `FILE_BEGIN`、
> 下载整个失效而且不报错。最后改成不回显（客户端自己记着请求的 offset），
> 并顺手把那个 `!= 3` 放宽成 `< 3`，否则下一个追加字段的人还会踩。

## 功能
'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    if "消息不会因为" in text:
        raise SystemExit("已经加过")
    if ANCHOR not in text:
        raise SystemExit("找不到 ## 功能 锚点")
    text = text.replace(ANCHOR, NEW_SECTION, 1)
    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("README 已补三项能力说明")


if __name__ == "__main__":
    main()
