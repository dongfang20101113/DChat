# 网页版的地址结构：房间页 / 房间设置 / 服务器设置

网页版网关（`dchat_webd`）把「聊天」和「设置」拆成三种地址，互不重叠：

| 地址 | 是什么 |
| --- | --- |
| `/` | 聊天。不预设房间，左边点哪个进哪个（和以前一样） |
| `/<房间名>` | 聊天，登录成功后自动进这个房间 |
| `/<房间名>/settings` | 这个房间的设置（群主 / 管理员才能改） |
| `/admin` | **服务器整体**设置，也就是 `chatrule` 那 16 项 |
| `/download`、`/dl/*`、`/file/*` | 下载页和文件，和以前一样 |

举例：大厅是 `/hall`，它的设置是 `/hall/settings`；群 `study` 是 `/study`、
`/study/settings`。

## 为什么要按这个结构分

「房间设置」和「服务器设置」是两套完全不同的东西：

- 房间设置走的是**普通客户端协议**（`GSET` / `RSET`），服务器会按「你是不是这个群的主/管」
  校验，每一端都能用，结果会广播给在线成员。
- 服务器设置走的是**回环管理端口**（`/chatrule`），只有服务器本机能写，网关只是转发。

把它们混在一个页面里，等于让一个群主看到他能改「最大连接数」——权限语义就乱了。
所以 `/admin` 只放服务器那 16 项，房间设置一律在 `/<房间名>/settings`。

## 实现要点

### 路由

`webd/webd.cpp` 的 `ServeConnection` 里，房间页分支放在**最后**：

```
/  /index.html  /download  /admin*  /dl/*  /file/*   ← 固定路由，永远优先
/<房间名>  /<房间名>/settings                          ← 剩下才当房间
```

这样有人建了个叫 `download` 的群，也不会把下载页顶掉。

`IsWebRoomId()` 除了字符集，还挡掉 `admin` / `download` / `dl` / `file` / `ws` /
`index` 这几个保留名。**这一条是踩出来的**：没挡之前，没配管理端口时 `/admin`
本该 404，却因为 `admin` 是合法房间名而被当成房间页返回了 200。

### 房间号怎么进到页面里

页面模板里有占位符 `__ROOM_ID__`，网关用 `FillRoom()` 换成真实房间号：

```html
<script>window.DCHAT_ROOM = "__ROOM_ID__";</script>
```

三点必须一起成立，缺一个就是洞或者 bug：

1. **占位符和 JS 变量名必须不同名**。第一版两个都叫 `__DCHAT_ROOM__`，
   `FillRoom` 把 `window.__DCHAT_ROOM__` 里的同名子串一起换掉了，页面直接坏掉。
2. **房间号字符集收到 `[a-z0-9_-]`**，长度 2–64。它要进 JS 字符串字面量，
   放宽就是开 XSS 口子。服务器那边的群 ID 本来就是这套字符集，挡不住合法的群。
3. **`/` 也要过一遍 `FillRoom`（填空串）**。不然首页里留着字面量 `__ROOM_ID__`，
   前端会拿它当房间名去 `/open`，每次打开首页都白报一条错。

### 设置值怎么送到浏览器

网关的 `StartRoomListPump` 本来每秒比对一次会话列表，变了才推。现在它额外推一帧
当前会话的 8 项设置：

```
SETTINGS\t<房间ID>\t<kind>\t<我的角色>\t8
SETTING\tname\t群名
SETTING\tmaxmembers\t200
SETTING\tjoinmode\tapproval
SETTING\tlisted\tfalse
SETTING\tcachemaxmb\t256
SETTING\tannouncement\t（明文）
SETTING\timportantnotice\ttrue
SETTING\tfileencryption\tfalse
SETTINGS_END
```

为什么单独一帧而不塞进 `ROOM` 行：`ROOM` 已经是 7 段固定格式，三个端都按下标取值，
往后追加字段要牵动三端解析；而且设置只有「当前房间」用得着。

设置值也进了「签名」，所以群主在别处改了「群文件加密」，这边的设置页会在一秒内跟着变。

### 写回：不新开一条路

设置页保存时发的是 `INPUT\t/set <项> <值>`，和聊天框里手敲 `/set` **完全同一条路径**
（`ChatCore::SubmitInput` → `GSET`）。所以：

- 权限判断只有服务器那一份，网页不做假的拦截（按钮灰掉只是省一次白跑）。
- 群名 / 群公告的 Base64 编码由 `ChatCore` 负责，页面传明文。
- 只提交**真的改了**的项，只改一项就只发一条。

### 地址栏跟着走

点左边房间列表时用 `history.pushState` 把地址改成 `/<id>`，前进/后退也跟着切房间。
重连后进的是 `wantRoom` 而不是最初那个 `pageRoom`，所以「切过房间再断线重连」
不会被拽回最开始的房间。

## 验证

在本机（Windows）编不了 `webd`（POSIX only），所以验证在 Ubuntu 上做：

```
cmake -S . -B build && cmake --build build -j2   # 0 error 0 warning
ctest                                             # 16/16
```

HTTP 路由（真实起 server + webd，curl 打）：

| 路径 | 期望 | 实测 |
| --- | --- | --- |
| `/` | 200 | 200 |
| `/hall` | 200 | 200 |
| `/hall/settings` | 200 | 200 |
| `/download` | 200 | 200 |
| `/admin`（没配 `--admin`） | 404 | 404 |
| `/HALL`（大写） | 404 | 404 |
| `/a`（太短） | 404 | 404 |
| `/hall/x`（未知子页） | 404 | 404 |

注入：`/` → `window.DCHAT_ROOM = ""`；`/hall` → `"hall"`；`/hall/settings` → `"hall"`；
页面里残留的 `__ROOM_ID__` 个数 = 0。

WebSocket 端到端（浏览器视角，真的登录 → 建群 → 进群 → 改设置），16 项全过：

- 登录成功
- 建群后会话列表里出现该群（`ROOM` 行）
- 收到该群的 `SETTINGS` 块，8 项齐全
- 群名中文没坏（`测试群`）
- `/set fileencryption true` 之后推回来的帧里 `fileencryption` 变成 `true`，且没有被服务器拒绝

## 没做的事

- **生产环境还没换二进制**。`/opt/dchat/dchat_webd` 还是旧版本，`/admin` 在线上
  依然是 404（因为没开 `--admin`）。换二进制要单独确认。
- 保留名（`admin` 等）的群在网页上只能用 `/?` 或从房间列表进，短地址打不开。
