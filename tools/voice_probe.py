#!/usr/bin/env python3
"""单独看 /voice 到底发生了什么：arecord 有没有真的在跑、StopAndSave 成不成功。

为什么单独查：/voice 只回了"正在录音"就说不出别的了 —— 但这台 VM 有
/dev/snd/controlC0 而没有 pcm 设备，arecord 很可能起来就立刻失败。
界面只说"正在录音"，用户就永远等不到结果。这正是要看清楚的地方。
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
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def visible(fd, seconds):
    buf = ""
    deadline = time.time() + seconds
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                buf += os.read(fd, 65536).decode("utf-8", "replace")
            except OSError:
                break
    return ANSI.sub("", buf)


def main():
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(CLIENT, [CLIENT, "--host", "127.0.0.1", "--port", "5843",
                          "--user", "voiceuser", "--pass", "voiceuserpass1"])
    visible(fd, 2.5)

    os.write(fd, "/voice\r".encode())
    time.sleep(1.0)
    out = visible(fd, 1.0)
    print("  敲 /voice 之后界面说: %r" % out.strip()[-120:])

    # 这时候有没有 arecord 子进程？
    ps = subprocess.run(["pgrep", "-a", "arecord"], capture_output=True, text=True)
    has_proc = bool(ps.stdout.strip())
    print("  arecord 进程: %s" % (ps.stdout.strip() or "无"))

    # 直接手动跑一次 arecord，看它报什么（这才是根因）
    manual = subprocess.run(
        ["arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", "-t", "wav",
         "-d", "1", "/tmp/voice-probe.wav"],
        capture_output=True, text=True, timeout=15)
    print("  手动 arecord 退出码: %d" % manual.returncode)
    err = (manual.stderr or "").strip().splitlines()
    if err:
        print("  手动 arecord stderr: %s" % err[0][:160])
    size = os.path.getsize("/tmp/voice-probe.wav") if os.path.exists("/tmp/voice-probe.wav") else 0
    print("  手动 arecord 产出的文件大小: %d 字节" % size)

    # 再敲一次 /voice 停：看 StopAndSave 走哪条路
    os.write(fd, "/voice\r".encode())
    time.sleep(1.5)
    out2 = visible(fd, 1.5)
    print("  再敲 /voice 之后界面说: %r" % out2.strip()[-200:])

    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
