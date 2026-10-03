# Linux 端

dchat 的 Linux 端和 Windows 端**共用同一份协议层源码**（`src/protocol*.cpp`、
`src/render.cpp`、`src/auth.cpp`、`src/crypto.cpp`、`src/file_transfer.cpp`、
`src/server_rules.cpp`、`src/server_command.cpp`、`src/socket_util.cpp`），
只有两处按平台分开：

| 平台 | 密码学后端 | socket |
| --- | --- | --- |
| Windows | `src/crypto_backend_win.cpp`（CNG / bcrypt.dll，系统自带） | Winsock2 |
| Linux | `src/crypto_backend_openssl.cpp`（OpenSSL 3） | POSIX socket |

分界点在 `src/crypto_backend.h` 和 `src/socket_util.h` 后面，业务代码看不到平台差异。

## 构建

依赖（Debian / Kali）：

```bash
sudo apt-get install -y cmake ninja-build g++ libssl-dev
```

> 如果 apt 报 `connect (101: 网络不可达)`，多半是它解析到了 IPv6 镜像而这台机器没有
> IPv6 路由。加一行偏好 IPv4 即可：`echo "precedence ::ffff:0:0/96  100" | sudo tee -a /etc/gai.conf`

构建与测试：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && ctest
```

产物：
- `build/dchat_server` —— 服务端（可直接跑，参数和 Windows 端一致）
- `build/dchat_client_linux` —— 客户端（终端界面 + 批处理模式）
- `build/test_*` —— 9 个跨平台测试套件（727 项检查）

界面层（`dchat_client`、GDI+ 相关的那几个测试和出图工具）**只在 Windows 上构建**，
CMake 里用 `if(WIN32)` 圈起来了；Linux 端没有那套界面，客户端是另写的终端版。

## 客户端

### 交互模式（日常用）

```bash
./build/dchat_client_linux --host 服务器地址 --port 5555 --user 名字 --pass 密码
./build/dchat_client_linux --host 服务器地址 --user 新名字 --pass 密码 --register
```

连上之后就是一个终端聊天界面：消息往上滚，输入行固定在下面。

| 操作 | 说明 |
| --- | --- |
| 直接输入 + 回车 | 发言 |
| `Tab` | 补全指令和在线昵称（多候选时连续按会循环） |
| `↑` / `↓` | 翻输入历史 |
| `Ctrl+U` | 清空当前输入 |
| `Ctrl+C` / `/quit` | 退出 |

本地指令（不会发给服务器）：

| 指令 | 说明 |
| --- | --- |
| `/send <文件路径>` | 上传文件（上限 64 MB），成功后显示服务器分配的附件 id |
| `/get <附件 id>` | 下载别人上传的附件，存到 `./dchat-downloads/`（重名自动加序号） |
| `/voice [秒数]` | 录音；再敲一次 `/voice` 停下并发送。不带秒数就按协议上限 |
| `/play <附件 id>` | 播放一个音频附件（语音**会自动下载**，不用手动 `/get`） |
| `/chatcolor on\|off` | 本机的彩色聊天开关 |
| `/clear` | 清屏 |
| `/help` | 列出所有服务器指令 |
| `/quit` | 退出 |

其余 `/ban`、`/kick`、`/say` 之类的指令**原样发给服务器**，用法和 Windows 端一致。

### 语音

格式和另外两端**完全一致**：16 kHz / 单声道 / 16 位 PCM WAV，上限 2 MB
（约 64 秒），短于 1 秒的不发（点一下也会触发录音，全是噪音）。这些常量都取自
`src/voice_notes.h`，Linux 端不自己写死数字——**码率不一致的表现是"对方放不出来"，
不报任何错**。

实现是调 `arecord` / `aplay`（`alsa-utils` 包），不链 ALSA 库：这样客户端不多一个
编译期依赖，而这两个命令几乎所有发行版都有。

```bash
sudo apt-get install -y alsa-utils
```

注意两点：

- **没有声卡的机器**（容器、服务器）会在按录音时明确报"这台机器没有声卡设备"，
  而不是崩掉或卡住。
- 停止录音用的是 `SIGINT` 而不是 `SIGKILL`：arecord 要**回填 WAV 头里的长度字段**，
  直接杀掉会留下一个头部写着 0 字节的文件，播放器打不开。

### 批处理模式（脚本 / 自动化验证用）

```bash
./build/dchat_client_linux --user alice --pass secret --register \
    --send "你好" --expect-any "你好" --timeout 8
```

连上 → 握手 → 注册/登录 → 发一条 → 等到收到包含指定文字的行 → 退出并返回退出码
（0 成功、2 连不上/发不出、3 超时、4 指纹变了被中止）。**跨端互通验证就是用这个模式做的。**

### 彩色文字

聊天里可以直接用色码，三端语法一致：

| 写法 | 效果 |
| --- | --- |
| `&0` ~ `&f` | 十六个快捷色（和 Windows、安卓的颜色表完全相同） |
| `&#rrggbb` | 真彩色，例如 `&#ff8800` 是橙色 |
| `&&` | 一个字面量 `&` |

服务器用 `chatcolor=0` 关掉彩色聊天时，色码会**原样显示**而不是被吃掉——这是刻意
的：否则用户会以为自己的字被吞了。

### 安全

第一次连一台服务器会记住它的指纹（存在 `dchat-known-servers.txt`）。以后每次连
都会比对，**指纹变了会明确警告并中止**（确认是管理员换了密钥，才用 `--insecure-ok`
继续）。指纹错了不报错就等于中间人换钥匙悄无声息地通过，所以这一条是硬拦。

## 跑服务端

```bash
./build/dchat_server                 # 默认端口 5555
./build/dchat_server 5599            # 指定端口
./build/dchat_server --bind 0.0.0.0  # 指定监听地址（只支持 IPv4 字面量）
./build/dchat_server --rules my-rules.txt
```

首次启动会在当前目录生成 `dchat-server-key.txt`（身份密钥）、`dchat-users.txt`（账号）、
`dchat-rules.txt`（规则）。**这几个文件两端通用**：Windows 上生成的密钥文件能被 Linux 读，
反过来也一样（格式是"裸私有标量 + 裸公钥"的十六进制，和 CNG 的内部布局无关）。

## 三端一致性是怎么保证的

`#RRGGBB` 那条线以下的细节，任何一处不一致都会导致"握手成功但解出来是乱码"，
而且**不会有任何报错**。所以两边跑的是同一批测试向量（`tests/test_crypto.cpp`）：

- HKDF-SHA256 对 RFC 5869 官方测试向量
- AES-256-GCM 对 NIST 官方测试向量
- ECDH P-256 固定密钥对，共享密钥逐字节比对
- 和安卓 Kotlin 实现的跨语言一致性

实测中抓到的三个坑（都写在代码注释里了）：

1. **ECDH 共享密钥的字节序**：CNG 返回小端要翻转，OpenSSL 本来就是大端不用翻。
2. **私钥标量的字节序**：CNG 的 `BCRYPT_ECCPRIVATE_BLOB` 里标量是小端，OpenSSL 的
   BIGNUM 是大端。不翻转的话两边"同一个标量"其实是两个数——导入成功、ECDH 也
   算得出 32 字节，但结果完全不同。
3. **AES-GCM 的输出顺序**：密文在前、16 字节 tag 在后（和 Java 的 `Cipher.doFinal` 一致）。

## 安全与压力测试（实测数据从哪来）

`tools/loadtest.cpp` 是手写的压力/攻击工具（POSIX socket，五个独立场景），
给安全结论提供**实测数据**而不是估算。编译：

```bash
g++ -std=c++17 -O2 -pthread -o build/loadtest tools/loadtest.cpp
```

| 场景 | 命令 | 干什么 |
| --- | --- | --- |
| `flood` | `./build/loadtest flood 200` | 并发建 200 条连接，统计存活/被拒 |
| `slowloris` | `./build/loadtest slowloris 40 25` | 连上不登录占住连接，看握手超时是否清掉 |
| `badlogin` | `./build/loadtest badlogin 12 victim` | 错误密码连续登录，看封禁是否生效 |
| `garbage` | `./build/loadtest garbage` | 11 类畸形数据（超长行 / 乱字节 / 伪造 HELLO_OK / 半包即断等） |
| `echo` | `./build/loadtest echo 用户 密码 500 8` | 持续压测吞吐 |

目标地址用 `DCHAT_HOST` / `DCHAT_PORT` 指定，默认 `127.0.0.1:5555`。

**两个会让结论完全反过来的测量陷阱**（工具第一版都踩了）：

1. **TCP 连上 ≠ 被服务器接受**。服务器可以在应用层检查完连接数上限后立刻关闭连接，
   此时 `connect()` 仍然成功。必须再等一会儿看对端有没有发 FIN，否则会把
   「140 条被拒」测成「200 条全通」。
2. **`send` 成功 ≠ 连接还在**。判断「连接是否被断开」必须读**到 FIN**，而且要先排空
   缓冲里已有的数据——服务器连上就发 `WELCOME`、超时前还会发一条 `ERROR`，
   只 recv 一次会拿到这些数据并误判成「没断开」。

修掉这两处之后，实测结论才和服务器日志对得上。

### 加固配置下的实测结果（Kali 虚拟机，真服务端）

```bash
cat > hardened-rules.txt <<'EOF'
maxconns 150
maxconnsperip 60
loginfails 5
handshaketimeout 10
maxtextlen 4096
maxtextlines 200
uploadrate 512
downloadrate 1024
registerinterval 60
maxaccounts 5000
EOF
./build/dchat_server --port 5555 --rules hardened-rules.txt
```

| 攻击 | 结果 |
| --- | --- |
| 连接洪泛 200 条（`maxconnsperip=60`） | **正好 60 条存活**、140 条被拒，日志逐条记录拒绝原因 |
| 慢速耗尽 40 条占 25 秒（`handshaketimeout=10`） | **40/40 被按时断开**，日志出现 72 次 `handshake timeout` |
| 暴力破解 12 次（`loginfails=5`） | **12/12 被拒**，一次都没登上 |
| 畸形数据 11 类 | 全部未命中，**服务端未崩溃** |
| 攻击后正常业务 | 仍能正常注册、登录、收发消息 |
| 内存 | 全程稳定 **9.3–10.3 MB** |
| 持续吞吐（16 客户端） | 处理 **474,296 条**消息，墙钟 2.4 秒，服务端 CPU 17 秒 |

> ⚠️ 默认配置里 `maxconns` / `maxconnsperip` / `loginfails` 都是 **0（不限）**，
> 只有 `handshaketimeout` 默认 30 秒。要上公网请先按上面的例子加固——
> **默认状态没有连接数上限，也没有暴破封禁**。

> 这些数字证明的是「规则按设计生效」，**不是**「服务器打不死」。网络层的大流量
> DDoS 需要上游清洗，不在本项目范围内。

### 注册路径防护（2026-10 补上，实测通过）

上面那些防护**全部打在"已有账号"的路径上**（登录失败封禁、连接数上限、握手超时）。
**注册路径当初一次都没测过**——2026-10-03 补测发现这条路没有防护，随后已修复。

复现：

```bash
g++ -std=c++17 -O2 -pthread -o build/regflood tools/regflood.cpp
./build/regflood rate 1000 bot          # 单线程连续注册
./build/regflood parallel 12 60 flood   # 12 线程并发（模拟换 12 个 IP）
```

实测结果（即使开了全部防护规则）：

| 项目 | 实测 |
| --- | --- |
| 单线程注册速率 | **503 个/秒**，120/120 全部成功 |
| 12 线程并发注册 | **363–503 个/秒**，720/720 全部成功、0 失败 |
| 服务端拦截记录 | **0 条**（日志里只有"注册成功"，没有任何拒绝） |
| 账号增长 | 2 秒 720 个；一轮 3 秒洪水灌进 1586 个 |
| 正常用户消息往返 | 无洪水 **105 ms** → 洪水期间 **10035 ms（超时）** |
| 每账号成本 | 账号文件约 115 字节；约 12.7 KB 内存 |
| 服务端 RSS | 并发洪水期间 9 → 28 MB；更高并发见过 60 MB |

**为什么换 IP 就能绕开**：现有防护全部基于 IP（`maxconnsperip` / `loginfails`），
换 IP 之后逐条失效。而注册路径**不检查任何配额**。

**为什么洪水会拖死正常用户**：每次注册都要做三件昂贵的事——

1. `SaveUsers()`：拿 `g_usersMutex`，把**整个**账号表序列化并**全量重写**文件；
2. `BroadcastKnownNames()`：对**每个已登录客户端**各算一遍完整用户名列表并发送；
3. 上面两步里 `g_usersMutex` 被反复加解锁，而 `SaveUsers()` **持锁做文件 I/O**。

账号越多、在线人越多，单次注册越贵（前者 O(账号数)，后者 O(账号数 × 在线数)）。

**能做什么（建议，尚未实现）**：

- 注册限流：同一 IP 每 N 分钟最多注册 M 个（最直接，但要配合换 IP 的现实）；
- 账号总量上限或注册开关（比如 `maxaccounts` / `registration off`）；
- 写入节流：账号表改成**追加一行**而不是全量重写，或合并 200 ms 内的多次写入；
- 广播节流：`KnownNameList()` 算一次复用给所有客户端（现在是循环里重复算）；
- 邀请码 / 管理员审批：把开放注册变成需要门槛，这才是根治。

> 这条缺口也说明一件事：**安全测试很容易只覆盖自己想到的路径**。
> 之前五类攻击全打在登录和连接上，注册一直没人碰——直到有人问"疯狂注册怎么办"。

#### 已实现的修复

新增两条规则（都可运行时用 `/chatrule` 调整，默认 0 = 不限制，保持向后兼容）：

| 规则 | 作用 | 公网建议 |
| --- | --- | --- |
| `registerinterval` | 同一 IP 两次注册之间的最小间隔（秒） | 60 |
| `maxaccounts` | 账号总数上限 | 按需，比如 5000 |

**刻意做成两层**：`registerinterval` 抬高出单个 IP 的成本，但**换 IP 就能绕**；
`maxaccounts` 是总量硬闸，**换多少 IP 都绕不过**。两者互补，不是二选一。

同时修掉了两个让洪水能放大伤害的性能问题：

1. **账号文件写入节流**：原先每次注册都持 `g_usersMutex` 把整张表序列化并重写文件。
   现在 800 ms 窗口内只落盘一次，中间的改动标记为待写，由周期任务（每秒）补写，
   进程退出前也会兜底写一次。注册路径因此**完全不做文件 I/O**。
2. **已知名字广播只算一次**：原先对每个在线客户端各算一遍完整用户名列表
   （O(账号数 × 在线数)）。现在算一次缓存起来，账号表变动时才置脏重算。

**修复后的实测对比**（同样的 `tools/regflood.cpp`）：

| 配置 | 单线程 30 个 | 8 线程并发 80 个 | 最终账号数 | 服务端拦截 |
| --- | --- | --- | --- | --- |
| 无防护（对照） | 30/30 成功 | 80 成功 0 失败 | **111** | 0 条 |
| `registerinterval 60` | **4/30 成功** | 4 成功 76 失败 | **9** | 102 条 |
| `maxaccounts 20` | 20/30 成功 | 0 成功 80 失败 | **21** | 120 条 |
| 两条一起开 | 4/30 成功 | 1 成功 79 失败 | **6** | 105 条 |

**不误伤正常用户**（这条和拦攻击一样重要，专门测过）：

- 首次 IP 的新用户注册：**2 ms 成功**（实现里把"这个 IP 没注册过"当放行，
  不是当成 0 秒——否则所有人都注册不了，服务器直接不可用）；
- 同 IP 紧接着注册第二个账号：被冷却拦住，提示"请等 N 秒后再试"；
- 已注册用户登录、发言：**完全不受影响**。

判定逻辑抽成了纯函数 `src/register_guard.{h,cpp}`，`tests/test_register_guard.cpp`
用 44 项检查把两个方向都钉死（放行方向 + 拒绝方向 + 边界 + 优先级 + 规则可配置/可序列化）。

> 顺带说清一件事：`registerinterval` 是**基于 IP** 的，共享出口 IP 的场景
> （学校、公司 NAT）下会限制"同一个出口 60 秒只能注册一个账号"。
> 真遇到这种情况把间隔调小，或者干脆只靠 `maxaccounts` 兜底。