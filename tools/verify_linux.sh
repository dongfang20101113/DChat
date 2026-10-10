#!/bin/bash
# 在没有 Linux 机器的情况下，用 zig 交叉编译**验证 Linux 构建能不能编过**。
#
# 为什么需要它：线上是 Ubuntu，而这份代码里的 Linux 分支（socket_util.h 的 POSIX 路径、
# file_transfer.cpp 的 unistd.h 路径、CMakeLists 里非 WIN32 的那套链接）**在本机从来没被
# 执行过**。等到服务器上才发现编不过，就是最不该出现的那种意外。
#
# 能验证：语法 / 类型 / **平台宏分支**（zig 自带 Linux libc 头，所以 #ifndef _WIN32
#         那半边是真编的，不是跳过的）。
# 不能验证：链接（缺 Linux 版 OpenSSL）、运行行为、界面层（Win32/GDI+ 本来就不在
#         Linux 上构建，由 CMakeLists 的 if(WIN32) 挡掉）。
#
# 用法：
#   tools/verify_linux.sh [仓库根目录] [zig可执行文件]
# Windows 上也可以用（zig.exe 交叉编译）：
#   pwsh> & zig.exe c++ -target x86_64-linux-gnu -c src/server.cpp -Isrc -o NUL
set -u

REPO="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$REPO" || exit 1

ZIG_BIN="${2:-}"
if [ -z "$ZIG_BIN" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG_BIN=$(command -v zig)
  elif [ -x "./zig/zig" ]; then
    ZIG_BIN="$PWD/zig/zig"
  fi
fi
if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
  echo "找不到 zig。装法：winget install zig.zig / brew install zig / 从 ziglang.org 下压缩包"
  echo "（Windows 上直接传第二个参数指向 zig.exe：tools/verify_linux.sh . /d/zig/zig.exe）"
  exit 1
fi
echo "zig: $("$ZIG_BIN" version)"

# 服务端 + 三端共用源码。**刻意不含** src/crypto_backend_openssl.cpp：它要 Linux 版
# OpenSSL 头，没有就编不了，硬编只会刷一屏无关报错（和 verify_macos.sh 同一个理由）。
# 上层 crypto.cpp 只认 crypto_backend.h 的接口，所以把它排除掉不影响验证其余部分。
SHARED="src/socket_util.cpp src/protocol.cpp src/render.cpp src/input_history.cpp \
        src/server_command.cpp src/auth.cpp src/file_transfer.cpp src/server_rules.cpp \
        src/register_guard.cpp src/history_store.cpp src/room.cpp src/room_registry.cpp \
        src/crypto.cpp"
SERVER="src/server.cpp"
# 客户端核心（Linux/macOS 两端共用；界面层不在这里）
CLIENT_CORE="client_core/net.cpp client_core/trust.cpp client_core/files_parse.cpp \
             client_core/chat_color.cpp client_core/files.cpp client_core/voice.cpp \
             client_core/chat_core.cpp client_core/cli_render.cpp"
TESTS="tests/test_room.cpp tests/test_room_registry.cpp tests/test_protocol.cpp \
       tests/test_server_rules.cpp tests/test_history_store.cpp tests/test_ui_layout.cpp"

fail=0
for target in x86_64-linux-gnu aarch64-linux-gnu; do
  echo
  echo "########## 目标 $target ##########"
  for f in $SHARED $SERVER $CLIENT_CORE $TESTS; do
    [ -f "$f" ] || continue
    printf "  %-34s " "$(basename "$f")"
    out=$("$ZIG_BIN" c++ -target "$target" -std=c++17 -O1 -Isrc -Iclient_core \
              -c "$f" -o /tmp/linux-verify.o 2>&1)
    if [ -z "$out" ]; then
      echo "OK"
    else
      echo "FAIL"
      echo "$out" | grep -E "error|fatal" | head -3 | sed 's/^/      /'
      fail=1
    fi
  done
done

echo
if [ "$fail" = "0" ]; then
  echo "✅ 全部通过（只证明「编得动」，不证明「跑得起来」；真跑还得在 Linux 上 cmake+ctest）"
else
  echo "❌ 有文件编不过，见上面的报错"
fi
exit $fail
