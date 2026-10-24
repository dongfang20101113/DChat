# 网页版补齐客户端能力：房间面板 / 语音 / 指令面板

## 做完的三件事

| 之前 | 现在 |
| --- | --- |
| 房间搜索、建群、成员、踢人、审核、公告只能手敲指令 | 头上「＋」按钮打开面板，五个页签 |
| `/voice` 直接回「网页版暂不支持录音」 | 🎤 按钮录音，气泡里直接播放 |
| `/ban` `/kick` `/op` `/say` `/block` 只能手敲 | 面板里「指令」页签，点按钮就行 |

写操作**全部走和聊天框完全相同的路径**（`INPUT\t/xxx` → `ChatCore::SubmitInput`），
所以权限判断只有服务器那一份，网页不做假拦截，也不会出现「网页能改、客户端不能改」这种分裂。

## 为什么要先扩网关

以前网关只往浏览器推 8 种帧（STATE / SAY / SYS / ERROR / INFO / FILE / ROOMS / SETTINGS）。
`GHIT`（搜索结果）、`RMEMBERS`（成员表）、`GREQ`（待审申请）虽然进了 ChatCore，
**浏览器一个字都收不到**——没有数据，画不出图形界面。

所以先给 ChatCore 加了三个「列表版本号」，收到对应的 `_END` 时 +1：

```cpp
unsigned GroupHitsVersion() const;   // GHIT_END 时 ++
unsigned MembersVersion() const;     // RMEMBERS_END 时 ++
unsigned PendingVersion() const;     // GREQ_END 时 ++
```

网关收到 `FIND` / `AUDIT` 时先记下版本号，再起一个短线程轮询（50 ms，最多 4 秒），
版本变了就说明这批收全了，此时把结构化数据推给浏览器：

```
GHITS\t<n> / GHIT\t<id>\t<名>\t<人数>\t<上限>\t<加群方式> / GHITS_END
MEMBERS\t<n> / MEMBER\t<昵称>\t<角色>\t<群昵称> / MEMBERS_END
PENDING\t<n> / REQ\t<昵称> / PENDING_END
```

**为什么用版本号而不是看 `size()`**：两次搜索结果可能一样多，光看数量判断不出「新的到了」。

## 语音：浏览器里自己拼 WAV

协议要求 **16 kHz / 单声道 / 16 位 PCM WAV，≤2 MB（约 64 秒），短于 1 秒不发**
（`src/voice_notes.h`），因为另外三端都按这个格式播放。

浏览器没有现成的 WAV 录音（`MediaRecorder` 出来是 WebM/Opus，另外三端放不了），所以：

1. `getUserMedia` 拿麦克风
2. `AudioContext({sampleRate: 16000})` 取 PCM
3. **按真实采样率重采样到 16 kHz**（有些浏览器不认 sampleRate 请求，不重采样的话
   发出去的语音对方听到的是快放/慢放）
4. 自己拼 44 字节 WAV 头
5. Base64 一帧发给网关

网关收到 `VOICE\t<base64>` 后落盘成临时 `.wav`，以 **`kind = "voice"`** 上传——
服务器和另外几端就靠这一格判断这是语音，**不看扩展名**（安卓录的是 .m4a）。

播放：附件卡片点「▶ 播放」，网关先把文件取到本机（`/get`），再让浏览器用
`<audio>` 播 `/file/<id>`。**`/play` 不再在服务器那台机器上放音**。

## 顺手修掉的老 bug：群里的附件卡片不显示

服务器对非大厅会话发的是 **`RFILE_OFFER`**（最前面多一层房间号），
而 `ChatCore` **从来没处理过这个命令**——直接掉进兜底分支，显示成一行原始协议文本。

影响面：Linux CLI 和网页版（都用这套解析）**在群里收文件完全没有卡片**，大厅里才有。
桌面端有自己的解析器，所以一直没露。

修法：按 `RFILE_OFFER` 解析，在当前会话就原地显示，不是当前会话就在正文前加
`[房间名]`（和 `RMSG` 一致）。

## 验证

### 端到端（沙箱，真 server + 真 webd，15 项全过）

```
登录 → 建群 guild → 进群 → 开「向公众展示」
MEMBERS  → 收到 MEMBERS 块，自己是群主
FIND 测试 → 收到 GHITS 块，搜到 guild，字段完整（id/名/人数/上限/加群方式）
AUDIT    → 收到 PENDING 块，0 条
VOICE 1.2 秒 WAV → 服务端广播 FILEOFFER，isVoice=1
UPLOAD 普通文件   → FILEOFFER，isVoice=0
```

### 其它

- 页面里三段 `<script>` 全部通过 `node --check` 语法检查
- Ubuntu 构建 0 error 0 warning；`ctest` 16/16
- 线上已部署，外网 `/room/hall` 200，页面含 `rpanel` / `mic` / `btnPanel` /
  `__EXTRA_FRAME__`，占位符残留 0

## 改动文件

- `client_core/chat_core.h` / `.cpp` — 三个列表版本号；**新增 `RFILE_OFFER` 解析**
- `webd/webd.cpp` — `FIND` / `AUDIT` / `MEMBERS` / `VOICE` 四条浏览器命令；
  三张列表的结构化推送；`FILEOFFER` 帧带 isVoice；日志截断 120 字符
  （以前会把几十 KB 的 Base64 原样打进日志）
- 页面 — 房间面板（搜索/建群/成员/管理/指令五页签）、🎤 录音、附件卡片

## 还没做的

- 网页版的语音**只能在 https 或 localhost 下录音**（浏览器安全限制）。
  现在线上是 `http://120.26.223.225:8080`，🎤 会报「需要 https 或 localhost」。
  要么配 HTTPS，要么用 `http://localhost:8080` 访问。这是浏览器规矩，绕不过去。
