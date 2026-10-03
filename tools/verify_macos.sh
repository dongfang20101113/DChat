#!/bin/bash
# 在 Linux 上做 macOS 交叉编译验证。
#
# 用途：没有 Mac 的时候，用它检查"这些源码能不能编出 macOS 二进制"。
# 能验证：语法 / 类型 / 平台宏分支 / libSystem 调用名是否正确。
# 不能验证：链接（缺 macOS 版 OpenSSL）、运行行为、任何 framework 代码
#           （zig 不带 Foundation/AppKit 头，Cocoa 那部分一行都编不了）。
#
# 用法：tools/verify_macos.sh [仓库根目录]
set -u

REPO="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$REPO" || exit 1

ZIG_BIN=""
# 注意要拿**绝对路径**：command -v 返回的是名字（"zig"），
# 后面用 [ -x ] 判断可执行性时会当成相对当前目录找，必然失败。
if command -v zig >/dev/null 2>&1; then
  ZIG_BIN=$(command -v zig)
else
  ZIG_BIN=$(python3 -c "
import ziglang, os
print(os.path.join(os.path.dirname(ziglang.__file__), 'zig'))
" 2>/dev/null)
fi
if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
  echo "找不到 zig（试过 PATH 和 pip 装的 ziglang）。装法：pip3 install ziglang"
  exit 1
fi
echo "zig: $("$ZIG_BIN" version)"

# 共享源码（三端一致的那部分）。刻意**不含** crypto_backend_openssl.cpp：
# 它要 macOS 版 OpenSSL 头，没有 Mac 就编不了，硬编只会刷一屏无关报错。
SHARED="src/protocol.cpp src/render.cpp src/input_history.cpp src/server_command.cpp \
        src/auth.cpp src/file_transfer.cpp src/server_rules.cpp src/register_guard.cpp \
        src/crypto.cpp src/socket_util.cpp"
# 客户端核心（已从 linux/client/ 提升到 client_core/，两端共用）
CLIENT_CORE="client_core/net.cpp client_core/trust.cpp client_core/files_parse.cpp \
             client_core/chat_color.cpp client_core/files.cpp client_core/voice.cpp \
             client_core/chat_core.cpp"

fail=0
for target in x86_64-macos aarch64-macos; do
  echo
  echo "########## 目标 $target ##########"

  for f in $SHARED $CLIENT_CORE; do
    printf "  %-34s " "$(basename "$f")"
    out=$("$ZIG_BIN" c++ -target "$target" -std=c++17 -O1 -Isrc -Iclient_core \
              -c "$f" -o /tmp/macos-verify.o 2>&1)
    if [ -z "$out" ]; then
      echo "OK"
    else
      echo "FAIL"
      echo "$out" | grep -E "error|fatal" | head -3 | sed 's/^/      /'
      fail=1
    fi
  done

  # server.cpp 是和上面同一批，但单独跑一次方便只看它
  printf "  %-34s " "server.cpp"
  out=$("$ZIG_BIN" c++ -target "$target" -std=c++17 -O1 -Isrc -c src/server.cpp \
            -o /tmp/macos-verify.o 2>&1)
  [ -z "$out" ] && echo "OK" || { echo "FAIL"; echo "$out" | grep -E "error|fatal" | head -3 | sed 's/^/      /'; fail=1; }
done

echo
if [ "$fail" = "0" ]; then
  echo "✅ 全部通过（注意：这只证明「编得动」，不证明「跑得起来」）"
else
  echo "❌ 有文件编不过，见上面的报错"
fi
exit $fail
