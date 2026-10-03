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
| `/chatcolor on\|off` | 本机的彩色聊天开关 |
| `/clear` | 清屏 |
| `/help` | 列出所有服务器指令 |
| `/quit` | 退出 |

其余 `/ban`、`/kick`、`/say` 之类的指令**原样发给服务器**，用法和 Windows 端一致。

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
