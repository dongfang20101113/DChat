# dchat 桌面端全自动联调（`tools/ui-rooms-auto.ps1`）

**状态：84 项检查全绿（退出码 0）。** 跑法：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-rooms-auto.ps1
#   -Port 5603  -KeepProcesses（留进程看现场）
# 每次运行清空 build\rooms-auto-server；产物在 build\ui-rooms-auto\
```

覆盖：大厅互通 → 建群 → 默认不公开搜不到 → 勾「向公众展示」（**服务端确认收到**）→
再搜搜到 → 加入进待审 → 群主审核同意（成员数变 2）→ 群消息不乱窜 →
免打扰/置顶乐观回显 + 置顶重排 → 私聊发起与隔离 → **大厅默认免打扰且没有退出/解散/拉黑** →
**私聊设置里有免打扰/置顶/拉黑** → **拉黑双向拦截** → **退群后会话消失、成员数回落** →
**退群后还能重新加入** → **设管理员 / 降回成员（对方界面跟着变）** → **踢人后对方会话消失** →
**关掉客户端重开再登录，免打扰/置顶还在，且服务端房名单里没有这两个字段**（本地项确实没上传）。
**全程不需要人点。**

回归基线（每次改完服务端/客户端都应该一起跑）：

| 项目 | 命令 | 现状 |
|---|---|---|
| 桌面端全自动联调 | `tools\ui-rooms-auto.ps1` | **84/84** |
| C++ 单元测试 | `ctest --test-dir build` | **19/19** |
| 协议端到端（**含"老客户端零影响"**） | `tools\run-rooms-e2e.ps1` | **54/54** |
| 安卓单元测试 | `cd android && gradlew.bat testDebugUnitTest` | **385 项，0 失败** |

其中 `android/app/src/test/.../RoomInteropTest.kt` 是**安卓端的真服务端端到端**：
它起真实的 `dchat_server.exe`，用安卓客户端自己的 `DchatConnection` 连上去，跑完
能力协商 → 房间列表 → 建群 → 新群默认不公开（搜不到）→ 勾公开 → 搜得到 →
申请入群（管理员收到 GREQ）→ 同意（双方成员数变 2）→ 群消息只进群不进大厅 →
`/dm` 发起私聊 → 私聊不掉大厅。**本机没有安卓设备，这是"安卓端端到端"目前
唯一能自动化的做法**（路子是 `ServerInteropTest` 趟出来的，照抄即可）。
找不到 `dchat_server.exe` 时它整体跳过，别人 clone 下来不会构建失败。

---

## 一、四个关键设计

1. **客户端导出可断言的状态**（`src/client.cpp` 的 `WriteClientState`）。
   带 `--state-file <路径>` 启动时每 200ms 写一次 `键=值` 文本，末尾 `END=1`
   （读的一侧靠它判断写完整）。不带参数时一个字节都不多写。
   **记录区取当前会话的活副本 `g_items`，非当前会话才是 `session.items`** ——
   导出必须照同一个不变量取，否则导出和界面差一份，测试等于白测。`conn.*` 让登录
   断言不再依赖自绘状态栏的文案。

2. **客户端把它"这一帧真正用来命中的矩形"也导出来**，脚本照着点：
   `menu.*`、`sessionList.{search,new,row.*}`、`bottom.{plus,send}`、`main.gear`、
   `dialog.roomSettings.row.*.{rect,box,extra}`、`dialog.group.{create,search,join.*}`、
   `dialog.group.edit.*`（真控件句柄，直接发 WM_SETTEXT）。
   **不在脚本里重算布局**：项目已吃过"两边各算一次、迟早错位"的亏。群搜索的「加入」
   按钮因此抽成了 `GroupJoinRect()`，命中测试和导出共用一份坐标。

3. **打字用 `WM_SETTEXT`**（`SendText($edit, 0x000C, 0, '文本')`）。客户端提交时是
   `GetWindowTextW(ui.hInput, ...)` 从控件读，所以设文本必然生效。
   当年"合成按键送不进编辑框"说的是 `WM_KEYDOWN`，那条路确实不通。

4. **不靠截图断言**：屏幕抓图在 DWM 下会拿到旧内容（实测），一切读状态文件。

---

## 二、驱动界面的四条硬规矩（都踩过）

1. **点击一律 `PostMessage`，绝不用 `SendMessage`** —— 点到会弹模态对话框的按钮时，
   SendMessage 会阻塞到那个对话框关掉（实测卡死 10 分钟）。
2. **点之前必须先发 `WM_MOUSEMOVE`** —— 会话设置窗的 `WM_LBUTTONDOWN` 用的是
   `state->hover`（只在 WM_MOUSEMOVE 里更新），不先移动就点了等于没点。
3. **每个客户端要有独立的工作目录** —— `dchat-rooms.prefs`、`dchat-known-servers.txt`
   都是相对路径的本地文件；共用会互相覆盖 + 上一轮残留被读进来，测试不再幂等
   （实测：A 点完免打扰变成 0，还有假的"服务器指纹变了"警告）。
4. **用句柄前重新读一次状态** —— 状态文件 200ms 才刷一次，拿旧快照里的 HWND 会打到空气上。
   **判断"点击有没有生效"要看 `server.log`，不要只看客户端状态文件**：客户端一旦出事，
   状态文件会停更，断言会"看起来很合理地"失败。

---

## 三、自动化抓到的真 bug（都已修）

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 1 | **关掉群聊窗 / 会话设置窗 → 整个客户端退出** | 这两个窗口的 `WM_DESTROY` 里都 `PostQuitMessage(0)` 来跳出自己的**模态嵌套循环**，但 WM_QUIT 是**线程级**的，主循环也收到 → 整个进程退出 | 改用只属于本次调用的 `bool* closed` 标志通知循环退出；主窗口保留唯一的 `PostQuitMessage` |
| 2 | 置顶会话后左栏**不重排**（要等改窗口大小） | `RefreshSessionPanel()` 只调了 `InvalidateRect`；顺序/几何（`g_sessionOrder`/`g_sessionLayout`）在 `LayoutWindowChrome()` 里算，平时只在 WM_SIZE 跑 | `RefreshSessionPanel()` 先 `Layout(ui.hwnd)` 再重画 |
| 3 | **私聊只能被别人发起** | 客户端从不发送协议命令 `DM`（只在收 `GDMLINK` 时认识私聊），服务端斜杠指令表里也没有 `dm` | 服务端加 `/dm <昵称>`（`/msg` `/pm` `/tell` 同义），与协议命令 `DM` 共用 `OpenDirectMessage()`；三端通用 |
| 4 | 群主同意入群后，**自己那边的成员数不刷新** | 服务端只给新成员发 `SendRoomList`，没刷新群主（及其他成员）的 `RINFO` | 审核通过后给**群里所有在线成员**发 `SendRoomInfoTo` |
| 5 | **状态导出自己每 200ms 崩一次客户端**（`0xc0000005`） | 导出里用 `FindWindowW(kRoomSettingsClass, nullptr)` 找自己的窗口——但窗口类名在**两个客户端进程里相同**，会跨进程找到别人的窗口，再拿它的 `GWLP_USERDATA` 当本进程指针解引用 | 改用进程内全局 `g_roomSettingsWindow` 记录句柄（群聊窗本来就用 `g_groupWindow`，所以没这问题） |

第 5 条是**测试代码自己的 bug**，但很值得记：调试时"客户端莫名消失、状态文件停更"
的现场，第一件事是**看 `server.log` 里有没有 `left (nick)`**、以及**查 Windows 事件日志的
APPCRASH**（`Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'}`）。
用 `nm -n -C <exe>` + ImageBase 就能把 WER 报的偏移换算回函数名——这次就是这么定位到
`EscapeStateValue` 的，比读代码猜快得多。

**第二轮（补大厅 / 私聊 / 黑名单 / 退群时）又抓到三个，同一类根因：
"服务端改了状态，但没把新状态推给受影响的人"。**

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 6 | **黑名单整个是坏的**：拉黑成功，界面上永远看不到 | 客户端**从来没请求过** `BLOCKS`，而且 `g_blockList.clear()` 一次都没被调用过；服务端 `BLOCK` 做完也只回一句 SYS，不推新名单 | 客户端登录后请求一次 `BLOCKS`；收 `GBLOCKS` 时按"上一轮已收完"判断新一轮开头并清空；服务端 `BLOCK`/`UNBLOCK` 直接把新名单推回去 |
| 7 | **退群后那个群永远留在左栏** | 客户端的 `ROOMLIST` 只增不减（`EnsureSession`），名单里没有的群也不会被拿掉 | `ROOMLIST_END` 时把"本地有、本轮名单里没有"的**群聊**删掉（只删群聊：大厅不是退出语义，私聊不在这份名单里），并守住 `g_items` 是当前会话活副本这条不变量 |
| 8 | 退群后**留在群里的人**看到的成员数不变 | `GLEAVE` 只给退群者发了新名单，没刷新剩余成员的 `RINFO`（和"同意入群"同一类） | 退群后给群里其它在线成员发 `SendRoomInfoTo` |

第 8 条第一次修的时候我还踩了个坑：把 `SendRoomInfoTo` 写在了持有 `g_roomsMutex` 的
作用域里，而它**内部自己也要拿这把锁**（`std::mutex` 非递归）→ 那条连接的线程会永久卡住。
**规矩：凡是 `Send*To` 这类函数，一律在锁外面调**（先把要发给谁收集到一个 vector 里，出作用域再发）。

**第三轮（补成员管理 / 踢人时）发现前面几处修得还不够 —— 同一个坑的第四、五处。**

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 9 | 同意入群后，**群主看到的成员列表里根本没有新成员**，于是"设为管理员/踢人"两个按钮点不到 | 前一轮只补了 `SendRoomInfoTo`（成员**数**），没补 `SendRoomMembersTo`（成员**表**） | 抽成 `NotifyRoomMembersChanged(roomId, except)`：一次把 `RINFO` + `RMEMBERS` 发给**还留在群里的所有人** |
| 10 | 把某人设为管理员后，**那个人自己的界面里角色还是"成员"** | `GMEMBER` 只把新成员表回发给**操作的管理员自己** | 同上，改调 `NotifyRoomMembersChanged` |

教训：这类"服务端改了状态却没推给受影响的人"的问题，**一处一处补是补不干净的**。
现在的做法是三个入口（入群 / 退群 / 踢人 / 改角色）统一走 `NotifyRoomMembersChanged`，
加了新的成员变动点就调它，别再各写一份。**判据**：凡是能改变 `room.members` 或某个成员
`role`/`displayName` 的地方，都必须调它。

**第四轮：又抓到一个"功能写好了但没人能用到"的缺口（和 `/dm` 同一类）。**

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 11 | **「成员群昵称」根本没人能设置** | 服务端有 `GNICK` 协议命令、客户端也能显示群昵称，但**客户端从来不发送 `GNICK`** —— 界面上没有任何入口 | 服务端加 `/nick <群昵称>`（`/nickname` `/mynick` 同义；空参数 = 清掉），在**当前会话**给自己设，复用 GNICK 那套"只广播这一个成员那一行"。三端通用，客户端不用改 |

判据（和 `/dm` 那次一样）：**"服务端支持 + 客户端能显示"不等于"用户能用"** ——
要检查有没有人**发送**那条命令。`GNICK` 和 `DM` 都是这么漏掉的。

顺带加了服务端 `gsearch` 日志（收到的关键词 + 命中数）："搜不到"这类问题没有它就是靠猜。

---

## 四、其它已知问题

- `tools/ui-smoke-test.ps1` **已经过时**：界面改版后菜单栏和底栏都是自绘的，它那句
  `PostMessage(WM_COMMAND, 1001)`（点「连接」）已经点不动了。复用它的登录流程要改成
  点 `menu.*` 里 `action=connect` 的那一项。
- 登录成功的断言不要再用状态栏文案（自绘了，读不到），改用 `conn.nick`。
- **本文件里的 .ps1 必须存成 UTF-8 with BOM**：PowerShell 5.1 会把无 BOM 的当 GBK 读，
  中文全乱。`edit` 工具会**去掉 BOM**，所以每次编辑之后要补一次：
  ```powershell
  $t=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8); [IO.File]::WriteAllText($p,$t,(New-Object Text.UTF8Encoding($true)))
  ```
