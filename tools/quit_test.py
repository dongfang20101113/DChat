#!/usr/bin/env python3
"""单独验证 /quit 能不能退出进程，以及 exit code 是多少。"""
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


def run_case(label, keys, expect_exit=True):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(CLIENT, [CLIENT, "--host", "127.0.0.1", "--port", "5842",
                          "--user", "quituser", "--pass", "quituserpass1"])
    # 等登录
    deadline = time.time() + 3
    buf = ""
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                buf += os.read(fd, 65536).decode("utf-8", "replace")
            except OSError:
                break
    visible = ANSI.sub("", buf)
    logged_in = "已登录" in visible

    for k in keys:
        os.write(fd, k if isinstance(k, bytes) else k.encode())
        time.sleep(0.35)

    # 最多等 4 秒看它退不退
    exited = False
    code = None
    wait_deadline = time.time() + 4
    while time.time() < wait_deadline:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done == pid:
            exited = True
            code = os.waitstatus_to_exitcode(status)
            break
        time.sleep(0.1)

    if not exited:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)

    verdict = "PASS" if (exited == expect_exit) else "FAIL"
    print("  %s %s  已登录=%s 键=%r 退出=%s code=%s"
          % (verdict, label, logged_in, keys, exited, code))
    return exited == expect_exit


def main():
    print("== /quit 与退出码 ==")
    ok = True
    ok &= run_case("/quit 应退出", ["/quit\r"])
    ok &= run_case("/exit 应退出", ["/exit\r"])
    ok &= run_case("Ctrl+C 应退出", [b"\x03"])
    print()
    print("%s" % ("全部符合预期" if ok else "有不符合预期的"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
