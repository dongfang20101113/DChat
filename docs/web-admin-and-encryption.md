# 网页版管理页面 + 群文件加密：结论

这一轮给 dchat 加了四样东西。每样都附了**怎么验的**，没验的地方也写清楚了。

## 一、网页版管理员页面 ✅

`/admin` 把服务器控制台 `/chatrule` 能控制的 **16 项规则**全做成了开关或输入框。

### 为什么需要一条新通道

`/chatrule` 原本是**仅控制台**指令，协议里没有任何对应命令。所以服务器新增了一个
**只绑 `127.0.0.1` 的管理端口**（`--admin-port`，默认不开）：

- 外网物理上连不上（`ss` 实测 `127.0.0.1:5556`，不是 `0.0.0.0`）
- 48 位随机口令，`dchat-admin-token.txt` 权限 0600，**恒定时间比较**
- 请求被翻译成 `/chatrule ...` 交给 `HandleServerCommand` —— 校验、写盘、广播、
  `maxservertemp` 触发清理，全部和控制台走同一条路径，不搞两套逻辑

### 网页这一层的认证

| 措施 | 做法 |
| --- | --- |
| 口令不落 Cookie | Cookie 里只有随机 session id（48 hex），口令原文不离开服务器 |
| Cookie 加固 | `HttpOnly; SameSite=Strict; Path=/admin; Max-Age=3600` |
| CSRF | 表单隐藏字段 == 会话 id，服务端再比一次（**双提交**） |
| 失败限流 | 同 IP 5 分钟内错 5 次 → 429 |
| 响应头 | `X-Frame-Options: DENY`、`X-Content-Type-Options: nosniff`、`Referrer-Policy: no-referrer` |
| 默认关闭 | 网关没配 `--admin` 时 `/admin` 直接 404，攻击面为零 |

### 怎么验的

在真服务器上（5611 服务端 + 5612 管理端口 + 8090 网关，**不碰生产**）：

```
未登录 GET /admin      -> 200，显示登录页
错口令 POST            -> 401
对口令 POST            -> 303 + Set-Cookie: dchat_admin=…
带 Cookie GET          -> 200，16 条规则全部渲染
不带 csrf 提交          -> 403
带 csrf 提交 3 项       -> 303
页面回读               -> maxtextlen=777 / documentsize=32 / keepchathistory 已勾
服务器规则文件          -> dchat-rules.txt 里 "maxtextlen 777" 等三行都在
非法值 maxtextlen=abc  -> 303 带错误消息，没崩
```

## 二、会话选择器 ✅

网关每秒比对一次 `ChatCore::Rooms()`，**变化时**才推一批
`ROOMS / ROOM / ROOMS_END`。浏览器画成左侧列表，点一下等于发 `/open <id>`。

用轮询而不是回调的原因：`ChatCore` 没有"房间列表变了"的通知，而列表会因**别人**
建群、把你踢了、改群名而变化 —— 光看自己发过什么指令推不出来。

## 三、手机窄屏适配 ✅

`≤640px` 时侧栏变抽屉（☰ 展开）、输入框字号 ≥16px（防 iOS 自动放大）、
底部 `env(safe-area-inset-bottom)`、触摸目标加高。

**验证的边界**：560px 截图确认布局正确；**390px 拍不出来** —— Windows 下 Edge 有
最小窗口宽度，截图会被裁切，看着像页面坏了。真机效果需人工确认。

## 四、群文件加密 ✅

见 `docs/file-encryption.md`（设计与取舍）和 `docs/rooms-protocol.md`（协议语义）。

### 落盘格式

```
DCHATENC1(9) + IV(12) + 密文 + GCM 标签(16)
```

### 端到端实测（本地起测试服务端跑的）

```
群创建成功 -> RSET encgroup2 fileencryption false（默认关）
设置开启   -> RSET encgroup2 fileencryption true
上传 58 字节
磁盘文件   -> 95 字节 = 58 + 9 + 12 + 16
文件头     -> DCHATENC1
明文可搜到 -> False
dchat-roomkeys/encgroup2.key -> 32 字节
下载       -> FILE_BEGIN F2 … 58（**明文大小**，不是磁盘上的 95）
下载内容   -> 与原文一字不差
关掉开关后 -> 仍能下载，内容仍一致
```

## 这一轮踩到并修掉的真问题

| 问题 | 后果 | 修法 |
| --- | --- | --- |
| 密钥差点放进 `dchat-files/` | `ClearFilesDir()` 每次启动清空该目录 → **每次重启把之前所有加密文件变成打不开的废文件，且不报错** | 密钥放独立的 `dchat-roomkeys/` |
| `FILE_BEGIN` 报磁盘大小 | 客户端一直等那多出来的 37 字节，表现为"卡在 99% 且不报错" | 报**明文**大小 |
| IV 写成 16 字节 | AES-GCM 标准 nonce 是 12 字节 → 加密直接失败且无报错 | 改成 12 |
| 测试断言"7 个可设项" | 加了第 8 项后测试红 | 更新为 8（这条断言是故意写死数字的：数量变化必须被人看见） |
| 测试没防住加密失败 | `substr` 抛异常 → core dump，一条失败信息都看不到 | 先判长度再取 |

## 测试基线

| 项 | 结果 |
| --- | --- |
| Windows 全量构建 | exit 0，**0 警告** |
| Linux 全量构建 | **0 error / 0 warning** |
| `ctest` | **14/14** |
| `test_room` | **286 项，0 失败**（含新增 8 项加密检查） |
| `test_linux_client` | **108 项，0 失败** |
| `test_file_crypto` | **27 项，0 失败** |
| 加密端到端 | 上传/下载/关开关后仍可读，全部通过 |

## 还没做的

1. **生产启用管理端口** —— 要改 `dchat.service` 加 `--admin-port 5556`、网关加
   `--admin 127.0.0.1:5556` 并重启（在线用户断几秒）。属生产操作，等明确同意。
2. **管理员页面截图** —— 做不到：`/admin` 需要登录，而 Headless Edge 提交不了表单、
   也注入不了 Cookie。
3. **网页版从浏览器选文件上传仍限 8 MB**（整文件塞一帧 WebSocket）；桌面端无此限制。
4. **CLI/安卓端不显示 🔒 标记** —— 协议里已有 `RSET … fileencryption`，三端都能收到，
   但目前只有网页端把它当成一个可操作项；桌面端群设置面板里也没有这一行。
