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
- `build/test_*` —— 8 个跨平台测试套件（661 项检查）

界面层（`dchat_client`、GDI+ 相关的那几个测试和出图工具）**只在 Windows 上构建**，
CMake 里用 `if(WIN32)` 圈起来了；Linux 端目前只有服务端。

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
