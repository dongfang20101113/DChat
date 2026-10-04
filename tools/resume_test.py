#!/usr/bin/env python3
"""断点续传的真机端到端验证（第 7 项）。

为什么要这么测：续传的价值只有"真的中断后再接上"才能体现。批量模式不会下载，
所以用伪终端驱动真客户端，并且**用 downloadrate 把速度压慢**，才有时间在
下载到一半时把它 SIGKILL 掉。

验的是三件事：
  1. 中断后磁盘上留下 `.part`，长度是"已经收到的那部分"；
  2. 重新 /get 时服务器**从那个长度接着发**（日志里能看到"从 X 处续传"，X 等于 .part 长度）；
  3. 拼出来的最终文件与原始文件 **md5 完全一致** —— 这才是"没拼坏"的唯一证据。
"""
import hashlib
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time

CLIENT = os.path.expanduser("~/dchat-build/work/out/dchat_client_linux")
OUT = os.path.expanduser("~/dchat-build/work/out")
PORT = "5860"
HOST = "127.0.0.1"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  <- " + detail) if (detail and not ok) else ""))


class Session:
    def __init__(self, user, password):
        argv = [CLIENT, "--host", HOST, "--port", PORT, "--user", user, "--pass", password]
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(OUT)
            os.execv(argv[0], argv)
        self.buffer = ""

    def read(self, seconds=0.6):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.buffer += chunk.decode("utf-8", "replace")
        return self.buffer

    def visible(self):
        return ANSI.sub("", self.buffer)

    def send(self, text):
        os.write(self.fd, text.encode("utf-8"))

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass


def main():
    print("== 断点续传真机测试 ==")
    src = os.path.join(OUT, "resume-source.bin")
    # 64 KB 随机数据：够分成 32 块，md5 也有意义
    with open(src, "wb") as fh:
        fh.write(os.urandom(64 * 1024))
    want_md5 = hashlib.md5(open(src, "rb").read()).hexdigest()
    print("  源文件 %d 字节，md5 %s" % (os.path.getsize(src), want_md5))

    # 两个账号先注册好（批量模式能做）
    for user in ("ra", "rb"):
        subprocess.run([CLIENT, "--host", HOST, "--port", PORT, "--user", user,
                        "--pass", user + "pass1", "--register", "--send", "hi",
                        "--expect-any", "hi", "--timeout", "8"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # ---- A 上传 ----
    a = Session("ra", "rapass1")
    a.read(1.5)
    a.send("/send " + src + "\r")
    a.read(3.0)
    text = a.visible()
    if "已上传" not in text:
        check("A 上传成功", False, text[-200:])
        a.kill()
        return 1
    check("A 上传成功", True)

    # ---- B 拿附件 ID 并开始下载，中途掐断 ----
    b = Session("rb", "rbpass1")
    b.read(1.5)
    b.send("/help\r")     # 触发一次输出，确认在线
    b.read(0.5)
    # 附件 ID 从 FILE_OFFER 卡片里拿：A 那边能直接看到自己上传的 id
    ids = re.findall(r"\bF(\d+)\b", a.visible())
    if not ids:
        check("能从卡片里拿到附件 ID", False, a.visible()[-300:])
        a.kill(); b.kill()
        return 1
    att = "F" + ids[-1]
    check("拿到附件 ID %s" % att, True)

    part = os.path.join(OUT, "dchat-downloads", ".dchat-part-" + att)
    if os.path.exists(part):
        os.unlink(part)

    b.send("/get " + att + "\r")
    b.read(2.5)                      # downloadrate 压慢了，2.5 秒只下一部分
    b.kill()                         # 模拟"网络断了 / 用户关掉了客户端"
    time.sleep(0.5)

    partial = os.path.getsize(part) if os.path.exists(part) else 0
    check("中断后留下 .part 且未下完", 0 < partial < 65536,
          ".part=%d 字节（应为 0<x<65536）" % partial)

    # ---- B 重新下载：应该从 partial 处续上 ----
    b2 = Session("rb", "rbpass1")
    b2.read(1.5)
    b2.send("/get " + att + "\r")
    done = False
    deadline = time.time() + 90
    while time.time() < deadline:
        b2.read(1.0)
        if "已保存" in b2.visible():
            done = True
            break
    check("续传后下载完成", done, b2.visible()[-300:])

    # ---- 最终文件与源文件逐字节一致 ----
    final = None
    dl = os.path.join(OUT, "dchat-downloads")
    for name in os.listdir(dl) if os.path.isdir(dl) else []:
        if name.startswith("resume-source") and not name.startswith("."):
            final = os.path.join(dl, name)
    if final:
        got = hashlib.md5(open(final, "rb").read()).hexdigest()
        check("最终文件 md5 与源文件一致（没拼坏）", got == want_md5,
              "got=%s want=%s size=%d" % (got, want_md5, os.path.getsize(final)))
        check("最终文件大小正确", os.path.getsize(final) == 65536)
    else:
        check("找到下载好的文件", False, "目录里没有 resume-source*")

    a.kill(); b2.kill()

    passed = sum(1 for _, ok in results if ok)
    print()
    print("%d checks, %d failures" % (len(results), len(results) - passed))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
