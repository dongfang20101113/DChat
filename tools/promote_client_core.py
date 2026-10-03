# 把可移植的客户端核心从 linux/client/ 提升到 client_core/。
#
# 为什么要搬：macOS 客户端要用同一套核心（网络、信任、附件解析、颜色、语音），
# 如果它去 #include "linux/client/net.h" 会很别扭，而且以后再加平台会更乱。
# 搬完 linux/ 只留终端界面相关的东西。
import io
import os
import shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "linux", "client")
DST = os.path.join(REPO, "client_core")

# 可移植的核心（macOS 与 Linux 共用）
PORTABLE = [
    "net.h", "net.cpp",
    "trust.h", "trust.cpp",
    "chat_color.h", "chat_color.cpp",
    "files.h", "files.cpp",
    "files_parse.h", "files_parse.cpp",
    "voice.h", "voice.cpp",
]
# 留在 linux/ 的终端专有部分
STAYS = ["terminal.h", "terminal.cpp", "ui.h", "ui.cpp", "main.cpp"]


def main():
    os.makedirs(DST, exist_ok=True)
    moved = []
    for name in PORTABLE:
        src = os.path.join(SRC, name)
        if not os.path.exists(src):
            raise SystemExit("找不到 " + src)
        shutil.move(src, os.path.join(DST, name))
        moved.append(name)
    print("搬走 %d 个文件:" % len(moved))
    for name in moved:
        print("   linux/client/%s -> client_core/%s" % (name, name))

    # 留在原地的文件里，对核心的 include 要改成相对 client_core
    # （CMake 会把 client_core 加进 include 目录，所以 `#include "net.h"` 仍然可用，
    #   不需要改 include 语句本身 —— 这也是当初把 include 都写成裸文件名的好处）
    print()
    print("留在 linux/client/ 的:", ", ".join(STAYS))
    print("include 语句不用改：CMake 会把 client_core 加进包含目录")


if __name__ == "__main__":
    main()
