# macOS 端

> **⚠️ 请先读这一段：macOS 版本是"静态验证通过、但从未在真机运行过"。**
>
> 开发这一版的机器上没有 macOS，也没有 macOS SDK。具体验证到哪一步：
>
> | 部分 | 验证程度 |
> | --- | --- |
> | 服务端 + 协议层 + 加密（除 OpenSSL 后端） | ✅ 用 `zig cc -target *-macos` **真交叉编译**成 Mach-O，逐个文件零错误 |
> | 客户端核心（net / trust / files_parse / chat_color） | ✅ 同上，编到 macOS 零错误 |
> | `crypto_backend_openssl.cpp` | ⚠️ 编译需要 macOS 版 OpenSSL 头，本机没有 → **未编译验证** |
> | Cocoa 图形界面（`macos/` 下的 ObjC++） | ❌ **完全未编译验证** —— 本机没有任何 Foundation/AppKit 头，zig 也不带 framework 头 |
> | 运行行为（握手、收发、文件、语音） | ❌ 未验证 |
>
> 所以：**代码写好了、能查到的兼容问题都改了，但第一次在 Mac 上编译大概率还会有报错要修。**
> 把报错贴回来我来改。

## 依赖

```bash
brew install cmake ninja openssl@3 sox
```

- `openssl@3`：加密后端（三端里 macOS 和 Linux 共用 OpenSSL 实现）
- `sox`：语音录制与播放。用它是因为 `rec` / `play` 在 macOS 上都能直接驱动
  CoreAudio，不需要额外权限配置；命令行和 Linux 端的 `arecord`/`aplay` 一样简单

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build -j"$(sysctl -n hw.ncpu)"
cd build && ctest
```

产物：

- `build/dchat_server` —— 服务端（和 Linux 端同一份源码，只是换了平台分支）
- `build/dchat_client_macos` —— Cocoa 图形界面客户端（`.app` 见下）
- `build/test_*` —— 测试程序。**加密、协议、账号、规则这些测试与 Windows/Linux
  完全相同**，在 Mac 上跑一遍就等于把三端一致性又验了一次

## 打包成 .app

```bash
cmake --build build --target dchat_app_bundle
open build/dchat.app
```

（如果这个 target 还没做出来，说明这一版只到"能编译出可执行文件"。

## 这一版为 macOS 改了什么

移植过程中一共只有四处不兼容，都改掉了，且都写了注释说明原因：

| 位置 | 问题 | 处理 |
| --- | --- | --- |
| `src/socket_util.h/.cpp` | macOS **没有 `MSG_NOSIGNAL`**。不处理的话「往已断开的连接写」会收到 SIGPIPE，**直接杀掉整个进程**（服务端会被一个断开的客户端打死） | 新增 `SetNoSigpipe()`：macOS 上设 `SO_NOSIGPIPE`（per-socket），Linux 上是空操作（那边用 `kSendFlags = MSG_NOSIGNAL`）。**新建立的连接都要调它** |
| `src/server.cpp` | macOS **没有 `TCP_KEEPIDLE`**，同一个选项叫 `TCP_KEEPALIVE`；也**没有 `TCP_KEEPCNT`** | 加 `__APPLE__` 分支；`TCP_KEEPCNT` 用 `#ifndef __APPLE__` 包起来 |
| `src/socket_util.cpp` | macOS 上 **`htons` 是宏**（`#define htons(x) __DARWIN_OSSwapInt16(x)`），所以 `::htons(...)` 会展开成 `::((__uint16_t)...)` 这种非法语法 | 去掉 `::` 前缀。Windows/Linux 上它是函数，不加 `::` 一样能编 —— 统一不加，三端通吃 |
| 语音（客户端） | macOS 没有 `arecord`/`aplay` | 改用 `sox` 的 `rec` / `play` |

另外三个端共用的 `src/voice_notes.h` 之前为了两端共用常量，已经**不引 `windows.h`** 了，
macOS 直接受益（这也是那一处改动的价值）。

## 在 Linux 上做交叉编译验证（开发时用的手段）

没有 Mac 也可以先验证"服务端能不能编出 macOS 二进制"，用的工具是 `zig`：
它自带 macOS 的 libSystem 桩，`zig cc -target x86_64-macos` 能直接产出 Mach-O。

```bash
pip3 install ziglang                     # zig 二进制，不走 GitHub
ZIG=$(python3 -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig'))")

# 逐个文件验证能不能编到 macOS（这是开发时用的脚本，见 tools/verify_macos.sh）
"$ZIG" c++ -target x86_64-macos -std=c++17 -Isrc -c src/server.cpp -o /tmp/s.o
```

**能验证**：语法、类型、平台宏分支、libSystem 调用名对不对。
**不能验证**：链接（缺 macOS 版 OpenSSL）、运行行为、framework 相关代码
（zig **不带 Foundation/AppKit 头**，所以 GUI 一行都编不了）。
