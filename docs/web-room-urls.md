# 网页版的地址结构：`/room/<房间名>` / `/room/<房间名>/settings` / `/admin`

网页版网关（`dchat_webd`）把「聊天」「房间设置」「服务器设置」分成三个**互不相交的命名空间**：

| 地址 | 是什么 |
| --- | --- |
| `/` | 聊天。不预设房间，左边点哪个进哪个 |
| `/room/<房间名>` | 聊天，登录成功后自动进这个房间 |
| `/room/<房间名>/settings` | 这个房间的设置（群主 / 管理员才能改） |
| `/admin` | **服务器整体**设置，也就是 `chatrule` 那 16 项 |
| `/download`、`/dl/*`、`/file/*` | 下载页和文件 |

大厅是 `/room/hall`，它的设置是 `/room/hall/settings`；群 `study` 是 `/room/study`、
`/room/study/settings`。

## 为什么房间要多一层 `/room/`

因为**根路径是公共的**。`/admin` 已经占了一段，以后再加 `/files`、`/search`、`/settings`
之类，房间名迟早和它们撞上——有个群就叫 `admin` 怎么办？

加一层前缀之后命名空间彻底分开：**静态功能都在根上，房间一律在 `/room/` 下面**。
代价是地址长了一点，换来的是以后加功能永远不用再考虑「会不会撞到某个群名」。

顺带的好处：房间名**不再需要保留字黑名单**。第一版是 `/<房间名>`，为了保住 `/admin`
只能把 `admin` / `download` / `dl` / `file` / `ws` / `index` 全部禁掉当房间名——
也就是说这些名字的群在网页上永远打不开。现在 `/room/admin` 就是那个群，不冲突。

## 实现要点

### 路由

`webd/webd.cpp` 的 `ServeConnection` 里，固定路径先匹配，房间页兜底：

```
/  /index.html  /download  /admin*  /dl/*  /file/*   ← 固定路由
/room                       → 首页（没指定房间）
/room/<房间名>               → 房间聊天页
/room/<房间名>/settings      → 房间设置页
其他                         → 404
```

`IsWebRoomId()` 只校验字符集 `[a-z0-9_-]`、长度 2–64，**不再有保留字黑名单**。
字符集之所以收死，是因为这个值要进页面的 JS 字符串字面量。

**不要把它放松**：放宽就是开 XSS 口子。

### 房间号怎么进到页面里

页面模板里有占位符 `__ROOM_ID__`，网关用 `FillRoom()` 换成真实房间号：

```html
<script>window.DCHAT_ROOM = "__ROOM_ID__";</script>
```

三点必须同时成立，缺一个就是洞或者 bug：

1. **占位符和 JS 变量名必须不同名**。第一版两个都叫 `__DCHAT_ROOM__`，
   `FillRoom` 把 `window.__DCHAT_ROOM__` 里的同名子串一起换掉了，页面直接坏。
2. **房间号字符集收死**（见上）。
3. **`/` 和 `/room` 也要过一遍 `FillRoom`（填空串）**。不然页面里留着字面量
   `__ROOM_ID__`，前端会拿它当房间名去 `/open`，每次打开首页都白报一条错。

### 设置值怎么送到浏览器

网关的 `StartRoomListPump` 每秒比对一次会话列表，变了才推。现在它额外推一帧
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

为什么不塞进 `ROOM` 行：`ROOM` 已经是 7 段固定格式，三个端都按下标取值，往后追加
字段要牵动三端解析；而且设置只有「当前房间」用得着。

设置值也进了「签名」，所以群主在别处改了「群文件加密」，这边的设置页会在一秒内跟着变。

### 写回：不新开一条路

设置页保存时发的是 `INPUT\t/set <项> <值>`，和聊天框里手敲 `/set` **同一条路径**
（`ChatCore::SubmitInput` → `GSET`）。所以：

- 权限判断只有服务器那一份，网页不做假拦截（按钮灰掉只是省一次白跑）。
- 群名 / 群公告的 Base64 编码由 `ChatCore` 负责，页面传明文。
- 只提交**真的改了**的项。

### 地址栏跟着走

点左边房间列表用 `history.pushState` 把地址改成 `/room/<id>`，前进/后退也切房间。
重连后进的是 `wantRoom` 而不是最初那个 `pageRoom`，所以「切过房间再断线重连」
不会被拽回最开始的房间。

## 验证（在 Ubuntu 上做，本地 Windows 编不了 webd）

```
cmake --build build -j2   # 0 error 0 warning
ctest                     # 16/16
```

路由（真起 server + webd，curl 打）：

| 路径 | 期望 | 实测 |
| --- | --- | --- |
| `/` | 200 | 200 |
| `/room` | 200 | 200 |
| `/room/` | 200 | 200 |
| `/room/hall` | 200 | 200 |
| `/room/hall/settings` | 200 | 200 |
| `/download` | 200 | 200 |
| `/room/admin` | 200（当成房间，撞名问题已解决） | 200 |
| `/room/download` | 200 | 200 |
| `/admin`（没配 `--admin`） | 404 | 404 |
| `/hall`（旧形式已废） | 404 | 404 |
| `/room/HALL`（大写） | 404 | 404 |
| `/room/a`（太短） | 404 | 404 |
| `/room/hall/x`（未知子页） | 404 | 404 |

注入：`/` 与 `/room` → `""`；`/room/hall` → `"hall"`；`/room/hall/settings` → `"hall"`；
`/room/admin` → `"admin"`；页面里残留的 `__ROOM_ID__` 个数 = 0。

WebSocket 端到端（浏览器视角，真登录 → 进房 → 收设置帧）：11/11 通过，
8 项设置齐全（`name='大厅'`、`maxmembers='0'`、`joinmode='direct'`、`listedy=false`、
`cachemaxmb='1024'`、`announcement=''`、`importantnotice='true'`、`fileencryption='false'`）。

## 线上部署记录（2026-10-24 20:02）

已部署到 `120.26.223.225`：

- 备份：`/opt/dchat/dchat_server.bak-1024-200245`、`/opt/dchat/dchat_webd.bak-1024-200245`
- 装了新 `dchat_server`（补齐 `--admin-port` 与 `fileencryption`）和新 `dchat_webd`
- 两个服务 `active`，5555 / 8080 正常监听，`/var/lib/dchat` 数据未动
- 外网实测：`/`、`/room/hall`、`/room/hall/settings`、`/download` 都 200；`/admin` 404

**`/admin` 线上还是 404**：要它可用必须同时做两件事，都还没做：

1. `dchat.service` 加 `--admin-port 5556 --admin-token /var/lib/dchat/dchat-admin-token.txt`
2. `dchat-webd.service` 加 `--admin 127.0.0.1:5556 --admin-token /var/lib/dchat/dchat-admin-token.txt`

管理端口只绑 `127.0.0.1`，外网访问不到；但这是给公网站点新开一个**口令登录入口**，
所以单独确认后再开。
