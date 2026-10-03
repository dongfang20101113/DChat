#!/bin/bash
# 探测能否在本机编译 crypto_backend_openssl.cpp 到 macOS。
#
# 为什么单独写：这个文件是三端加密的唯一实现，**编不过就等于 macOS 版没法加密**。
# 本机默认编不了（缺 macOS 版 OpenSSL 头），但"缺"这件事应该被脚本证明，
# 而不是靠我在文档里手写一句"未验证"。任何一条路通了，脚本就会把方法打出来。
set -u

echo "=== 1) 系统里有没有 macOS SDK（有的话一切都好办）==="
FOUND_SDK=""
for base in /opt/MacOSX*.sdk /usr/local/MacOSX*.sdk "$HOME/MacOSX"*.sdk \
            /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/*.sdk; do
  if [ -d "$base" ]; then
    echo "  找到 SDK: $base"
    FOUND_SDK="$base"
  fi
done
[ -z "$FOUND_SDK" ] && echo "  无"

echo
echo "=== 2) 有没有 osxcross 工具链 ==="
for tool in o64-clang oa64-clang x86_64-apple-darwin-clang; do
  printf "  %-28s " "$tool"
  command -v "$tool" >/dev/null 2>&1 && echo "有" || echo "无"
done

echo
echo "=== 3) Homebrew 的 macOS 版 OpenSSL 头在不在（brew --prefix 装在 /opt/homebrew 或 /usr/local）==="
for prefix in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
  printf "  %-36s " "$prefix"
  if [ -f "$prefix/include/openssl/core_names.h" ]; then
    echo "有（可用来做语法检查）"
    echo "    头文件版本: $(grep -m1 OPENSSL_VERSION_STR "$prefix/include/openssl/opensslv.h" 2>/dev/null | tr -d ' ')"
  else
    echo "无"
  fi
done

echo
echo "=== 4) 用 macOS 头 + Linux 库能不能过语法检查（-fsyntax-only，不链接）==="
ZIG_BIN=""
if command -v zig >/dev/null 2>&1; then ZIG_BIN=$(command -v zig);
else ZIG_BIN=$(python3 -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig'))" 2>/dev/null); fi
if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
  echo "  没有 zig，跳过"
  exit 0
fi
MAC_HEADERS=""
for prefix in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
  [ -f "$prefix/include/openssl/core_names.h" ] && MAC_HEADERS="$prefix/include"
done
if [ -n "$MAC_HEADERS" ]; then
  echo "  用 $MAC_HEADERS 做 -fsyntax-only"
  "$ZIG_BIN" c++ -target x86_64-macos -std=c++17 -Isrc -I"$MAC_HEADERS" -fsyntax-only \
      src/crypto_backend_openssl.cpp 2>&1 | head -12
  echo "  （没有输出就是通过）"
else
  echo "  没有 macOS 版 OpenSSL 头 —— 这一项确实无法在本机验证。"
  echo "  在 Mac 上验证：cmake --build build --target dchat_protocol（会自动找到 brew 的 openssl@3）"
fi
