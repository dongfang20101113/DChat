# 网页版部署（dchat-webd）

## 为什么需要它

dchat 的协议是 **5555 端口上的裸 TCP + 自定义 ECDH/AES-GCM 握手**。浏览器只能走
HTTP/WebSocket，**开不了裸 TCP**，所以"网页直接连服务器"物理上做不到。

`dchat_webd` 是那层网关：一边 HTTP/WebSocket 对浏览器，一边用**真实的 dchat 协议**
连 5555。它复用 `client_core` 里的 `ChatCore` / `ClientConnection` —— 也就是桌面端和
Linux 端用的同一份实现，所以：

- 网页版天然具备全部能力（群聊、私聊、文件、黑名单、审核、复核都在）；
- 协议和加密只在**一处**实现，不会多出一份要跟三端对齐的代码；
- 浏览器侧**一行加密代码都没有**（加密在网关和服务器之间）。

## ⚠️ 端口与备案（实测数据）

2026-10-18 从公网实测 `dchat.xin`：

| 端口 | 结果 | 说明 |
|---|---|---|
| 5555 | **OPEN** | dchat 本身，一直可用 |
| 8080 | 超时 | 安全组没放行 |
| 80 | **连接被重置** | **未备案，被阿里云拦掉** |
| 443 | **连接被重置** | 同上 |

**结论：大陆服务器上，未备案的域名访问 80/443 会被直接拦。** 这不是配置问题，
配对了也没用。所以网页版走 **8080**：

- `http://dchat.xin` → ❌ 打不开
- `http://dchat.xin:8080` → ✅ 能用（限制只针对 80/443）

想用干净的 `http://dchat.xin`，只有一条路：**去做 ICP 备案**（约 1～2 周）。
备案下来之后把 `--port` 改成 80（或用 nginx 反代到 8080）即可，其它都不用动。

## 上线步骤（一条命令）

前提：阿里云安全组**入方向放行 8080**（这是唯一需要人工在控制台做的事，
因为改防火墙不该由脚本自动完成）。

```bash
# 1. 装服务和下载目录
sudo mkdir -p /opt/dchat /srv/dchat-downloads
sudo cp build-linux/dchat_webd /opt/dchat/
sudo cp dist/* /srv/dchat-downloads/          # 三端安装包 + SHA256SUMS

# 2. 装 systemd 单元（见 tools/dchat-webd.service）
sudo cp tools/dchat-webd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dchat-webd

# 3. 验证
systemctl is-active dchat-webd
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/           # 期望 200
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/download   # 期望 200
curl -sS -o /dev/null -w '%{http_code} %{size_download}\n' \
     http://127.0.0.1:8080/dl/dchat-windows-x64.zip                        # 期望 200 + 文件大小

# 4. 从公网确认（在本机执行，不是服务器）
#    curl -sS -o /dev/null -w '%{http_code}\n' http://dchat.xin:8080/
```

## 网关参数

```
dchat_webd --port 8080 --server 127.0.0.1:5555 --dl /srv/dchat-downloads
```

| 参数 | 含义 |
|---|---|
| `--port` | 对浏览器监听的端口（默认 8080） |
| `--server` | 上游 dchat 服务器 `host:port`（默认 127.0.0.1:5555） |
| `--dl` | 安装包目录（默认 `./dchat-downloads`） |

**上游指向本机 5555** 是最省事的：网关和 dchat 服务在同一台机器上，走回环，
不占公网带宽。前端也可以在连接页里改服务器地址（`?server=host:port`）。

## 页面

| 路径 | 内容 |
|---|---|
| `/` | 聊天页：登录/注册 → 大厅 → `/rooms` `/open` `/create` `/send` `/get` … |
| `/download` | 下载页：三端安装包 + 源码，**SHA-256 从 `/dl/SHA256SUMS` 动态读** |
| `/dl/<文件>` | 静态下载（文件名过滤了 `..` 和斜杠） |

两个页面都**内嵌在二进制里**，部署只拷一个可执行文件，不会出现"忘了拷页面目录"。

## 浏览器 → 网关的协议

行式文本，Tab 分隔。

浏览器 → 网关：

```
LOGIN \t <用户名> \t <密码> \t <0|1 是否注册>
INPUT \t <一行文本>        # 等价于在客户端输入框里敲这行（含 /rooms、/send 等指令）
QUIT
```

网关 → 浏览器：

```
STATE \t <未连接|连接中|已登录> \t <说明>
SAY   \t <昵称> \t <时间> \t <正文> \t <0|1 自己> \t <0|1 @我>
SYS   \t <正文>      ERROR \t <正文>      INFO \t <正文>
```

## 已经验过的（证据）

| 项 | 结果 |
|---|---|
| Linux 编译 | 0 error / 0 warning |
| HTTP 首页 / 下载页 / 文件下载 | 200 / 200 / 200，下载回来的 SHA-256 与清单一致 |
| 路径穿越 `/dl/..%2f..%2fetc%2fpasswd` | 400（挡掉） |
| 端到端（真实服务端，SSH 隧道） | 登录（真实 ECDH）→ 发言 → 建群 → `/rooms` → 历史回放 → **传文件**（拿到 id=F1） |
| 浏览器截图 | `docs/screenshots/web-login.png`、`web-download.png`（Headless Edge 实拍） |
| `ctest` | 14/14；`test_linux_client` 108 项 0 失败 |

## 已知限制

1. **网页版不支持语音**：录音要 `getUserMedia` + 编码，属另一个量级，暂时明确回一句"网页版不支持录音"。
2. **文件上传**目前走 `/send <服务器上的本地路径>`——因为 `ChatCore` 的上传接口读的是本地文件。
   浏览器选文件上传需要再加一条"把字节流传给网关"的通道，**还没做**。
   下载（`/get <id>` 落到网关所在机器）已通。
3. 一个浏览器连接 = 一个 dchat 连接。刷新页面等于重新登录。
