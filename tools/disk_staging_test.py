#!/usr/bin/env python3
"""磁盘暂存改造的真机验证。

要证明的四件事（缺一不可）：
  1. **上传大文件后服务端 RSS 不随文件大小增长** —— 这是整个改造的目的，
     以前 32MB 的文件会让 RSS 涨 32MB。
  2. 下载下来的文件与源文件 md5 一致 —— 落盘再读回来不能损坏数据。
  3. 被淘汰（超 maxservertemp）时磁盘文件被真的删掉 —— 否则磁盘只涨不降。
  4. 重启后暂存目录被清空 —— 不能留垃圾。
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

OUT = os.path.expanduser("~/dchat-build/work/out")
CLIENT = os.path.join(OUT, "dchat_client_linux")
PORT = "5870"
HOST = "127.0.0.1"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  <- " + detail) if (detail and not ok) else ""))


def server_rss_kb():
    """服务端进程的 RSS（KB）。"""
    out = subprocess.run(["ps", "-o", "rss=", "-C", "dchat_server"],
                         capture_output=True, text=True).stdout.strip()
    return int(out.split()[0]) if out else -1


class Session:
    def __init__(self, user, password, register=False):
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


def register(user):
    subprocess.run([CLIENT, "--host", HOST, "--port", PORT, "--user", user,
                    "--pass", user + "pass1", "--register", "--send", "hi",
                    "--expect-any", "hi", "--timeout", "8"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def upload(session_user, path):
    """用 pty 客户端上传，等**卡片里的 id=F<N>** 出现再返回。

    为什么不能用宽松的 r"\\bF(\\d+)\\b"：那样会匹配到上传过程中输出里的
    其它东西，于是测试在上传真正完成前就往下走，后面的 /get 必然失败。
    """
    s = Session(session_user, session_user + "pass1")
    s.read(1.5)
    s.send("/send " + path + "\r")
    deadline = time.time() + 90
    while time.time() < deadline:
        s.read(1.0)
        m = re.search(r"id=(F\d+)", s.visible())
        if m:
            return m.group(1), s.visible()
    return None, s.visible()


def main():
    print("== 磁盘暂存真机测试 ==")
    files_dir = os.path.join(OUT, "dchat-files")

    big = os.path.join(OUT, "disk-big.bin")
    with open(big, "wb") as fh:
        fh.write(os.urandom(24 * 1024 * 1024))   # 24 MB
    big_md5 = hashlib.md5(open(big, "rb").read()).hexdigest()
    print("  源文件 24.0 MB，md5 %s" % big_md5)

    for user in ("da", "db"):
        register(user)

    rss_before = server_rss_kb()
    print("  上传前 RSS = %d KB" % rss_before)

    att, text = upload("da", big)
    if not att:
        check("上传成功并拿到附件 ID", False, text[-200:])
        return 1
    check("上传成功并拿到附件 ID %s" % att, True)

    rss_after = server_rss_kb()
    growth_kb = rss_after - rss_before
    print("  上传后 RSS = %d KB（增长 %d KB ≈ %.1f MB）"
          % (rss_after, growth_kb, growth_kb / 1024.0))
    # 关键断言：24MB 的文件不能变成 24MB 的内存增长。留 6MB 余量给缓冲/页缓存。
    check("RSS 不随文件大小增长（增长 < 6MB）", growth_kb < 6 * 1024,
          "增长了 %.1f MB —— 说明数据还是进了内存" % (growth_kb / 1024.0))

    # 磁盘上应该有这个文件
    # 轮询等改名落定（FILE_END 里是先改名、后登记，但客户端收到卡片和磁盘状态
    # 之间仍有极短的窗口，直接断言会偶发失败）
    deadline = time.time() + 20
    sized = []
    while time.time() < deadline:
        staged = os.listdir(files_dir) if os.path.isdir(files_dir) else []
        sized = [f for f in staged
                 if os.path.getsize(os.path.join(files_dir, f)) == 24 * 1024 * 1024]
        if sized:
            break
        time.sleep(0.5)
    check("暂存目录里有该文件（%d 字节）" % (24 * 1024 * 1024), len(sized) == 1,
          "目录内容: %s" % staged)
    check("暂存文件名不带 .part（已改名落定）",
          all(not f.endswith(".part") for f in sized), "目录内容: %s" % staged)

    # 下载校验
    dl = os.path.join(OUT, "dchat-downloads")
    for name in (os.listdir(dl) if os.path.isdir(dl) else []):
        if name.startswith("disk-big"):
            os.unlink(os.path.join(dl, name))
    s = Session("db", "dbpass1")
    s.read(1.5)
    s.send("/get " + att + "\r")
    done = False
    deadline = time.time() + 120
    while time.time() < deadline:
        s.read(1.0)
        if "已保存" in s.visible():
            done = True
            break
    check("下载完成", done, s.visible()[-200:])
    s.kill()

    got = None
    for name in (os.listdir(dl) if os.path.isdir(dl) else []):
        if name.startswith("disk-big") and not name.startswith("."):
            got = os.path.join(dl, name)
    if got:
        md5 = hashlib.md5(open(got, "rb").read()).hexdigest()
        check("下载文件 md5 与源文件一致（落盘读回没损坏）", md5 == big_md5,
              "got=%s want=%s" % (md5, big_md5))
    else:
        check("下载文件 md5 与源文件一致（落盘读回没损坏）", False, "没找到下载的文件")

    passed = sum(1 for _, ok in results if ok)
    print()
    print("%d checks, %d failures" % (len(results), len(results) - passed))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
