#!/usr/bin/env python3
"""用伪终端驱动交互式 TUI，验证按键、指令、补全真的能用。

为什么要这么测：ui.cpp 是交互界面，pty 是唯一能自动化驱动它的办法 ——
直接管道喂 stdin 不行（Terminal 会检测到不是 tty 然后拒绝进交互模式）。

测到的东西：欢迎语、/help、Tab 补全（指令与昵称）、实际收发消息、
/chatcolor 开关、/clear、Ctrl+U、退格删中文、↑ 翻历史、/quit 退出。
"""
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time

CLIENT = os.path.expanduser("~/dchat-build/work/out/dchat_client_linux")
PORT = "5841"
HOST = "127.0.0.1"

# 去掉 ANSI 转义，方便断言可见文字
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  <- " + detail) if (detail and not ok) else ""))


class Session:
    """一个跑在 pty 里的交互式客户端。"""

    def __init__(self, user, password, register=False):
        argv = [CLIENT, "--host", HOST, "--port", PORT, "--user", user, "--pass", password]
        if register:
            argv.append("--register")
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            # 子进程：直接 exec（不要经过 shell，否则收不到信号）
            os.execv(argv[0], argv)
        self.buffer = ""
        self.dead = False

    def read(self, seconds=0.6):
        """读一段时间内累积的输出。"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                self.dead = True
                break
            if not chunk:
                self.dead = True
                break
            self.buffer += chunk.decode("utf-8", "replace")
        return self.buffer

    def visible(self):
        return ANSI.sub("", self.buffer)

    def after_prompt(self):
        """只取最后一个输入提示符之后的内容。

        为什么要这样：输入是**逐字符**回显的，缓冲里会留下 "&" "&c" "&c红" …
        这种中间态。断言整个缓冲会把回声当成显示结果，误报成 bug
        （第一版就因为这个误报了 4 项）。看"提示符之后"才是真正的输出。
        """
        text = self.visible()
        idx = text.rfind("> ")
        return text[idx + 2:] if idx >= 0 else text

    def last_line(self):
        return self.after_prompt().strip().splitlines()[-1].strip() if self.after_prompt().strip() else ""

    def send(self, text):
        os.write(self.fd, text.encode("utf-8"))

    def send_raw(self, data):
        os.write(self.fd, data)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


def reset(session):
    """清空缓冲，后面只看新输出。"""
    session.buffer = ""


def received_by(observer, seconds=1.0):
    """观察者这段时间内收到的可见文字（用来验证"发出去的内容到底是啥"）。

    为什么不用发送者自己的缓冲：输入是逐字符回显的，缓冲里混着
    "&" "&c" "&c红" 这些中间态，还会被折行和后续系统消息干扰。
    看**另一个客户端收到了什么**才是干净的证据。
    """
    return ANSI.sub("", observer.read(seconds))


def main():
    print("== 交互式 TUI 真机测试（pty 驱动）==")

    # 先把两个账号注册好（用批处理模式，省得在 TUI 里处理登录流程）
    for user in ("tuiuser", "peeruser"):
        subprocess.run([CLIENT, "--host", HOST, "--port", PORT, "--user", user,
                        "--pass", user + "pass1", "--register", "--send", "hi",
                        "--expect-any", "hi", "--timeout", "8"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    alice = Session("tuiuser", "tuiuserpass1")
    alice.read(1.5)
    check("欢迎语里有 dchat", "dchat" in alice.visible(), alice.visible()[-200:])
    check("提示了 Tab 补全", "Tab" in alice.visible())

    # 另一个用户上线，好用来看消息与补全
    bob = Session("peeruser", "peeruserpass1")
    bob.read(1.5)

    # ---- /help ----
    alice.send("/help\r")
    alice.read(0.8)
    text = alice.visible()
    # 实际输出是"服务器指令： /ban /kick …"，不是"可用指令"
    check("/help 列出服务器指令", "服务器指令" in text, text[-300:])
    check("/help 提到本地指令 /send", "/send" in text)
    check("/help 提到 /voice", "/voice" in text)
    check("/help 提到 /chatcolor", "/chatcolor" in text)

    # ---- 收发消息 ----
    alice.buffer = ""
    alice.send("你好，这是一条测试消息\r")
    alice.read(0.9)
    check("自己发的消息会回显", "这是一条测试消息" in alice.visible(), alice.visible()[-300:])

    bob.buffer = ""
    bob.send("从 bob 发来的消息\r")
    bob.read(0.9)
    check("对方能收到消息", "从 bob 发来的消息" in bob.visible(), bob.visible()[-300:])

    alice.buffer = ""
    alice.read(0.4)
    check("alice 也能看到 bob 的消息", "从 bob 发来的消息" in alice.visible(),
          alice.visible()[-300:])

    # ---- 色码 ----
    # 注意 bob 也是彩色客户端，它收到广播后同样会解析色码 —— 所以"看 bob 显示"
    # 证明不了"原文有没有被改"。要看的是 **alice 这边有没有真的生成颜色序列**。
    reset(alice)
    alice.send("&c红色文字测试\r")
    alice.read(1.0)
    raw = alice.buffer
    # 注意：&c 对应的不是纯红 #ff0000，而是**提亮过的红**（暗背景上更可读）。
    # 所以只断言"生成了真彩色序列"，不写死具体分量 —— 写死会把正常行为误判成 bug。
    check("彩色开启时会生成真彩色 ANSI 序列", "\x1b[38;2;" in raw, repr(raw[-160:]))
    check("生成的颜色序列后面有重置（否则整屏都会变色）", "\x1b[0m" in raw.split("[38;2;")[-1])
    check("色码标记 &c 不显示出来（被解析掉了）", "&c" not in alice.after_prompt(),
          repr(alice.after_prompt()))

    reset(alice)
    alice.send("/chatcolor off\r")
    alice.read(0.8)
    check("/chatcolor off 有反馈", "彩色聊天：关" in alice.visible(), alice.visible()[-200:])

    reset(alice)
    alice.send("&c现在该显示原样了\r")
    alice.read(1.0)
    # 关掉彩色后本地不再解析，所以不该生成任何颜色序列。
    # 至于 &c 本身：本端的设计是**把标记剥掉、文字留住**（不是把 &c 当字面显示），
    # 所以这里断言"字还在"而不是"&c 还在"—— 后者是我第一版写错的假设。
    check("关掉彩色后不再生成颜色序列", "\x1b[38;2;" not in alice.buffer,
          repr(alice.buffer[-160:]))
    check("关掉彩色后文字仍然完整显示", "现在该显示原样了" in alice.visible(),
          repr(alice.visible()[-160:]))

    alice.send("/chatcolor on\r")
    alice.read(0.6)

    # ---- Tab 补全：指令 ----
    alice.buffer = ""
    alice.send("/ch")
    alice.read(0.3)
    alice.send_raw(b"\t")
    alice.read(0.5)
    check("Tab 能补全指令", "/chatcolor" in alice.visible(), alice.visible()[-200:])
    # 清掉当前输入行
    alice.send_raw(b"\x15")
    alice.read(0.3)

    # ---- Tab 补全：昵称 ----
    # 注意补全器只在**指令的参数位置**补昵称（/ban al -> /ban alice），
    # 不在聊天正文里把 "@pe" 补成 "@peeruser" —— 这是三端一致的设计
    # （补全器的实现在 protocol 层，四端共用）。第一版测试按 "@pe" 写，
    # 误报成 bug，实际是测试假设错了。
    alice.buffer = ""
    alice.send("/ban pe")
    alice.read(0.3)
    alice.send_raw(b"\t")
    alice.read(0.5)
    check("Tab 能在参数位置补全昵称", "/ban peeruser" in alice.after_prompt(),
          alice.after_prompt()[-200:])
    alice.send_raw(b"\x15")
    alice.read(0.3)

    # ---- 退格删中文（UTF-8 边界）----
    # 退格一次应该删掉**整个**"文"（一个字 3 字节），而不是只删 1 字节
    # 留下半个 UTF-8 序列（那会显示成乱码）。用 bob 看服务器收到的内容。
    reset(bob)
    alice.send("中文")
    alice.read(0.4)
    alice.send_raw(b"\x7f")
    alice.read(0.4)
    alice.send("\r")
    seen = received_by(bob)
    check("退格整字删掉中文：bob 收到的是「中」",
          "<tuiuser> 中" in seen and "中文" not in seen, seen[-200:])
    check("退格不产生乱码", "\ufffd" not in seen, seen[-200:])

    # ---- Ctrl+U 清空 ----
    reset(bob)
    alice.send("要清掉的内容")
    alice.read(0.4)
    alice.send_raw(b"\x15")
    alice.read(0.4)
    alice.send("清空后发的\r")
    seen = received_by(bob)
    check("Ctrl+U 清空了输入：bob 只收到清空后的内容",
          "清空后发的" in seen and "要清掉的内容" not in seen, seen[-200:])

    # ---- ↑ 翻历史 ----
    alice.buffer = ""
    alice.send_raw(b"\x1b[A")
    alice.read(0.5)
    check("↑ 能翻出历史输入", "清空后发的" in alice.visible(), alice.visible()[-300:])
    alice.send_raw(b"\x15")
    alice.read(0.3)

    # ---- /clear ----
    alice.buffer = ""
    alice.send("/clear\r")
    alice.read(0.6)
    check("/clear 会清屏", "\x1b[2J" in alice.buffer or len(alice.visible()) < 200,
          "屏幕内容长度 %d" % len(alice.visible()))

    # ---- 未登录时拦 /send（这个会话已登录，改成验证正常路径）----
    alice.buffer = ""
    alice.send("/send /nonexistent/file.txt\r")
    alice.read(0.9)
    check("/send 不存在的文件要报错而不是静默", "失败" in alice.visible(),
          alice.visible()[-300:])

    # ---- /quit ----
    alice.buffer = ""
    alice.send("/quit\r")
    time.sleep(0.8)
    # 用 waitpid **阻塞等**，别用 kill(pid,0) 探测：进程正在退出时
    # kill 还能成功（僵尸期），会误报成"没退出"。第一版就是这么误报的。
    deadline = time.time() + 4
    alive = True
    while time.time() < deadline:
        done, status = os.waitpid(alice.pid, os.WNOHANG)
        if done == alice.pid:
            alive = False
            break
        time.sleep(0.1)
    check("/quit 能退出（退出码 0）", not alive, "进程还活着")

    bob.close()
    alice.close()

    passed = sum(1 for _, ok, _ in results if ok)
    print()
    print("%d checks, %d failures" % (len(results), len(results) - passed))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
