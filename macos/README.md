# macOS 端

> **✅ 2026-10 更新：macOS 版本已在真机上跑通。**
>
> 原先这一版是在没有 Mac 的机器上开发的，只做到了静态验证（下方表格记录了当时
> 验证到哪一步）。现在补充了真机结果：**首次编译零报错**、能登录发消息、
> 与 Windows / Linux / Android 双向互发、四端同时在线互通、附件传输正常、
> 彩色文字显示正常。**语音消息这一项没测。**
>
> 下面保留当时的验证边界记录 —— 它解释了"为什么当时不敢说已经能用"，
> 也说明**哪些结论是编译期拿不到的**（比如 SIGPIPE 那条，写错只在运行时炸）。
>
> | 部分 | 验证程度 |
> | --- | --- |
> | 服务端 + 协议层 + 加密（除 OpenSSL 后端） | ✅ 用 `zig cc -target *-macos` **真交叉编译**成 Mach-O，逐个文件零错误 |
> | 客户端核心 `client_core/`（含 `chat_core`，即全部会话逻辑） | ✅ 同上，**18 个文件 × 两个 macOS 架构全部通过** |
> | `crypto_backend_openssl.cpp` | ⚠️ 当时没编过（本机没有 macOS 版 OpenSSL 头）→ **真机上编过了**（首次编译零报错） |
> | Cocoa 图形界面 `macos/main.mm` | ⚠️ 当时只做了文本层面的检查（selector 是否都实现了、括号配对、用到的核心成员是否存在） |
> | 运行行为（握手、登录、收发、文件） | ❌ 当时未验证 → ✅ **真机已测**（语音除外） |
>
> 当时的结论是"**代码写好了、能查到的兼容问题都改了，但第一次在 Mac 上编译大概率还会有报错要修**"。
> **实际结果比预期好：首次真机编译零报错。** 但这属于运气好，不代表静态验证够用 ——
> 语音那一项到现在也还没测。

## 当时的验证边界（历史记录，保留是为了说明静态验证能到哪一步）

```
client_core/     可移植核心 —— 两个平台共用，**能交叉编译验证**
  net / trust / chat_color / files / files_parse / voice
  chat_core      会话逻辑：登录注册、收发、指令派发、附件、语音
linux/client/    Linux 终端界面 + 入口
macos/main.mm    Cocoa 界面 —— 编不了的那一层，只负责"把字符串画出来"
```

界面层刻意做薄，就是因为它无法验证：逻辑都在 `client_core` 里，
`macos/main.mm` 只剩窗口搭建 + 把 `ChatCore` 的回调转成 `NSAttributedString`。

## 依赖

```bash
brew install cmake ninja openssl@3 sox
```

- `openssl@3`：加密后端（三端里 macOS 和 Linux 共用 OpenSSL 实现）
- `sox`：语音录制与播放（`rec` / `play`）。用它是因为能直接驱动 CoreAudio、
  不需要额外权限配置，参数写法也能和 Linux 的 `arecord`/`aplay` 一一对应

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

`dchat_client_macos` 声明成 `MACOSX_BUNDLE`，CMake 会自动生成 `.app` 结构，
直接在 Finder 里双击即可：

```bash
open build/dchat_client_macos.app
```

**首次运行会被 Gatekeeper 拦**（没有签名）：右键 →「打开」→ 再点「打开」，
或 `xattr -d com.apple.quarantine build/dchat_client_macos.app`。

## 这一版为 macOS 改了什么

移植过程中的不兼容点**全部是交叉编译探测出来的，不是猜的**。逐条：

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

### 「缺什么」不该靠我手写一句话，有脚本证明

```bash
tools/probe_macos_openssl.sh
```

它逐条检查：macOS SDK 在不在、osxcross 有没有、Homebrew 的 macOS 版 OpenSSL 头有没有、
能不能拿来做 `-fsyntax-only`。任何一条通了，它就会把可用的方法打出来。
在开发这台机器上的实测结果是**四条全空**，所以 `crypto_backend_openssl.cpp`
编到 macOS 这件事在本机确实做不到，而不是"我懒得试"。

> 这也是唯一一个"本可以验证却没验证"的项。其余未验证项（Cocoa GUI、运行行为）
> 是**结构上不可能**在本机验证的。

## 关于那个编不了的 GUI：做了哪些替代检查

`macos/main.mm` 一行都没被编译过，所以除了"小心写"之外，还做了两件事：

**1. 把逻辑全部挪到能验证的地方。** 会话逻辑（登录注册、收发、指令、附件、
TOFU 判定）全在 `client_core/chat_core.cpp` 里，那个文件在两个 macOS 架构上
都真编译过。GUI 里只剩窗口搭建和字符串拼装。

**2. 文本层面的静态检查**：`tools/check_objc_static.py`

```bash
python3 tools/check_objc_static.py
```

它查的是"最容易犯、而且在 Mac 上最难一次发现"的几类错：

| 查什么 | 为什么值得单独查 |
| --- | --- |
| 调用了自己没实现的 selector | 编译只给 warning，**运行到那行直接 unrecognized selector 崩溃** |
| 括号配对 | 长文件最容易漏，编译器报的位置往往离真正出错处很远 |
| `main.mm` 用到的 `ChatCore` 成员是否真在头文件里 | 头文件改了忘了同步，在 Mac 上才发现就白跑一趟 |
| GNU 扩展写法 | clang on macOS 未必接受（这里因此改掉了一处 `?:` 简写） |

**它不是编译器**，只能减少错误、不能替代在 Mac 上真编一次。

## 一个反直觉的坑：加了 `::` 反而编不过

`socket_util.cpp` 里原本写的是 `::htons(port)`。这在 Linux/Windows 上完全正确
（`htons` 是函数），但在 macOS 上 `htons` 是**宏**：

```c
#define htons(x) __DARWIN_OSSwapInt16(x)
```

于是 `::htons(80)` 展开成 `::((__uint16_t)...)` —— 非法语法，报错还是
"expected unqualified-id"，完全看不出跟 `htons` 有关。

结论：**跨平台代码里不要给可能是宏的名字加作用域限定符**。
`htons`/`htonl`/`ntohs`/`ntohl` 统一不加 `::`，三端通吃。