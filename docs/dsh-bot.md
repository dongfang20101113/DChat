# dchat × DeepSeek Harness 机器人（可选插件）

把 DeepSeek Harness（下称 DSH）接进 dchat：**群里多一个成员**，被 @ 的时候
它调 `dsh` 想一下，然后把回答**切成一小段一小段**发出来。

它默认**不装在服务端**：`dchat_server` 一行代码、一个字节都没动。装不装、什么时候
停，都是运维自己决定的 —— 不存在"升级服务端把机器人带上线"这种事。

---

## 一、它满足的四个要求，分别落在哪里

| 要求 | 实现 | 配置项 |
| --- | --- | --- |
| 只接入特定群聊 | 机器人是**普通成员**：它没加入的群，服务端根本不会把消息推给它；再叠加一层 ID 白名单 | `rooms = grp1,grp2`（大厅写 `hall`） |
| 只有被 @ 才回消息 | 提及判定和桌面端高亮、`@全体成员` 拦截**同一套规则**（`dchat::MentionsNick`）；`@` 前后紧挨着字符不算 | `trigger.mentiononly = true` |
| 不一次全部打出，分成细小的步骤 | 按句末标点切成短条 + 每条字数上限 + 按字数算的"打字"间隔；Markdown 记号抹平成聊天语气 | `typing.*` |
| 可选白名单 / 黑名单 | 三档：谁都回 / 只回名单 / 名单不回；昵称比较大小写不敏感 | `reply.mode` `reply.list` |

---

## 二、为什么是"独立进程"而不是编进服务端

1. **可选装**。插件是可执行文件 + 一个配置文件 + 一个 systemd 单元。不部署，
   dchat 完全不知道有这回事。
2. **崩得起**。模型调用、子进程、网络任何一处出问题，只影响机器人自己；
   服务端和别的客户端一点感觉都没有，重启机器人就恢复。
3. **位置自由**。DSH 需要一台能跑它的机器（还要登录 DeepSeek 账号）。
   机器人可以在那台机器上跑，连到任意一台 dchat 服务器 —— 不一定非要在服务端主机上。

代价是它要以一个**账号**登录，所以先给它注册一个账号（第一次启动会自己注册）。

---

## 三、三分钟跑起来

```bash
# 1. 构建（只建这一个目标，不碰别的）
cmake -S . -B build
cmake --build build --target dchat-dsh-bot

# 2. 生成一份带注释的配置，按需改
./build/dchat-dsh-bot --example > dchat-dsh-bot.conf
#    至少要改：server.host / bot.nick / bot.password / rooms

# 3. 先校验配置，再启动
./build/dchat-dsh-bot --check --config dchat-dsh-bot.conf
./build/dchat-dsh-bot --config dchat-dsh-bot.conf
```

然后**把机器人拉进群**（两种方式选一种）：

- 群主在群里把它加进来；或
- 把群 ID 写进 `rooms`，`rooms.autojoin = true` —— 机器人自己发 `GJOIN`。
  如果这个群是"管理员审核"模式，需要群主点一下同意（机器人日志会说明它在等）。

验证：在群里发 `@机器人昵称 你好`。第一条应该是 `（正在想…）`（可在
`reply.acktext` 关掉），随后几秒到几十秒里陆续出现几条短消息。

---

## 四、配置项

配置文件是 `键 = 值`，`#` 开头是注释，值里可以写 `\n` 表示换行。
**键名写错会直接报错并指出行号**（静默忽略会让"我明明配了却不生效"变成悬案）。

### 连接

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `server.host` | `127.0.0.1` | dchat 服务器地址 |
| `server.port` | `5555` | 端口 |
| `server.encrypt` | `auto` | `auto` 能加密就加密 / `off` 明文 / `require` 握手失败就不连。**公网必须用 auto 或 require**，否则机器人密码在网络上是明文 |
| `server.fingerprint` | 空 | 钉住服务器公钥指纹（首次连上时日志里会打出来）。留空 = 只记日志不校验 |

### 账号

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `bot.nick` | 必填 | 机器人昵称，也就是别人 `@` 它的名字 |
| `bot.password` | 必填 | 密码；配置文件请 `chmod 600` |
| `bot.register` | `false` | 账号不存在时自动注册。第一次部署可以打开，之后建议关掉 |

### 接入哪些会话

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `rooms` | 必填 | 会话 ID 列表，逗号分隔；大厅写 `hall`。**没列出的群一律不理** |
| `rooms.autojoin` | `true` | 不在群里时自动 `GJOIN`（审核制的群会排队等群主同意） |

### 触发

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `trigger.mentiononly` | `true` | `true` = 只有被 @ 才回；`false` = 房间里每句话都回（调试用） |
| `trigger.aliases` | 空 | 额外别名（不带 `@`），例如 `ds,小助手` |

### 谁能让它开口

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `reply.mode` | `all` | `all` / `whitelist` / `blacklist` |
| `reply.list` | 空 | 昵称列表，逗号分隔（`whitelist`/`blacklist` 时必填） |
| `reply.acktext` | `（正在想…）` | 被 @ 后先说一句；留空 = 不说 |
| `file.maxmb` | `32` | 允许发到群里的单个文件上限（MB），`0` = 不许发文件 |

### 历史回放

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `history.settlems` | `300` | 服务端回放历史时，"安静"多久算回放结束 |

为什么需要这一条：线上服务器一般开着 `keepchathistory` / `offlinemessages`，
**登录那一刻服务端会用原始 `SAY`/`RMSG` 行把错过的历史重放一遍**，
里面完全可能有人几个小时前 @ 过机器人。历史行没有序号，协议上分不出历史和实时，
所以用"服务端开始回放后、连续这么久没有新的聊天行"来界定回放结束。

> 别把这个值调大：它同时是"机器人上线后多久开始理人"。曾经用固定 2 秒，
> 结果刚上线那两秒里有人 @ 它，它完全不吭声。

### 分条节奏

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `typing.maxchars` | `45` | 单条最多几个字（按**码点**算，不是字节） |
| `typing.minchars` | `10` | 比这短的片段并到下一句，避免"嗯。"刷屏 |
| `typing.maxchunks` | `24` | 一条回答最多切成几条，超了并到末条并截断 |
| `typing.mingapms` | `350` | 两条之间最少等多久 |
| `typing.percharms` | `55` | 每多一个字多等几毫秒（像在打字） |
| `typing.maxgapms` | `1500` | 间隔上限，再长就像卡死了 |
| `typing.stripmarkdown` | `true` | 把 `**粗体**`、`` `代码` ``、`# 标题` 抹平（聊天里不该出现 Markdown） |

> 服务器那边如果设了 `chatinterval`（两条消息最短间隔），机器人会**自动遵守**：
> 它从服务器下发的 `RULES` 行里读这个值，取 `max(打字间隔, chatinterval)`。

### 大脑（DSH）

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `brain.command` | `dsh headless --json {session} -` | 见下面的说明 |
| `brain.cwd` | 空 | **它能动手的范围**（子进程工作目录 = 沙箱根目录）。空 = 机器人进程的当前目录 |
| `brain.permissionmode` | `read-only` | `read-only` 只能聊天 / `workspace-write` 能在 `brain.cwd` 里改文件。**别用 `danger-full-access`** |
| `brain.patch` | 空 | 额外 `--patch` 文件；留空会**自动**用随程序附带的那份（见下） |
| `brain.timeoutms` | `120000` | 一次回答最多等多久，超时就杀掉子进程并回一句 |
| `brain.sessionperroom` | `true` | 每个房间一个 DSH 会话，能记住上文 |
| `brain.maxconcurrent` | `1` | 同时跑几个 `dsh` 进程 |
| `brain.maxqueue` | `8` | 排队上限，超了明确回"等我一下" |
| `brain.prompttemplate` | 见示例 | 占位符 `{room}` `{roomId}` `{nick}` `{text}` `{workdir}` `{fileroots}` |

`brain.command` 的写法：

- 按空白分词，支持单/双引号（Windows 路径带空格时必须加引号）；
- `{session}` 展开成 `--session-id <id>`，没有会话时**整个消失**；
- `{prompt}` 展开成一个参数；
- **默认从 stdin 喂提示词**（`-` 就是 stdin）。聊天内容里的引号、换行、反斜杠
  都不需要转义，这是最不容易出错的方式。

`brain.patch` 留空时按权限模式**自动**挂上随程序附带的补丁（先找可执行文件旁边，
再找 `dsh-bot/`，再找仓库里的 `../plugins/dsh-bot/`）：

| 权限模式 | 自动用的补丁 | 效果 |
| --- | --- | --- |
| `read-only`（默认） | `dsh-chat-only.yml` | 关掉全部工具，只能聊天 |
| `workspace-write` | `dsh-local-ops.yml` | 工具可用、不再等审批，**只能在 `brain.cwd` 里动手** |

为什么本机操作必须带补丁：headless 下没有人能点"允许"，而 DSH 默认的审批策略是
`ask` —— 模型一要写文件就卡在那里等人批准，直到超时。

### 日志与状态

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `log.path` | `dchat-dsh-bot.log` | 同时写 stdout（systemd 收进 journal） |
| `log.verbose` | `false` | 打开后连"忽略：RSET"这种也记 |
| `state.sessionfile` | `dchat-dsh-bot-sessions.txt` | 房间 → DSH 会话 id，重启后还能接上话 |
| `ui.port` | `8731` | 本机管理界面的端口（只监听 127.0.0.1，`0` = 不开） |

---

## 五、本机操作（改文件 / 改名 / 发文件）

默认是**只聊天**：工具全关，它碰不到这台机器。要让它动手，在管理界面把
「允许改本机文件」打开（或者把配置改成 `brain.permissionmode = workspace-write`
并把 `brain.cwd` 指向允许它操作的目录）。

### 它能做什么

- 读、写、改、改名、删 `brain.cwd` 里的文件，建子目录；
- 跑命令（Windows 上是 pwsh，Linux 上是 bash）——**工作目录同样受限**；
- 把 `brain.cwd` 里的文件**发到群里**。

### 发文件是怎么接上的

模型不需要懂 dchat 协议。约定只有一条：**想发文件，在回答最后单独写一行**

```
[[file: 相对路径]]
```

机器人会把这一行抠掉（群里看不到），读文件、按 dchat 的上传协议送到当前会话，
群里收到的是一张正常的附件卡片。一条回答里可以写多个。

三道闸门，缺一不可：

1. **路径必须在 `brain.cwd` 里面**（`../` 之类会被解析后拒绝），
   这样它没法把机器上任意文件搬进聊天记录；
2. **单个文件 ≤ `file.maxmb`**，同时不超过服务器的 `documentsize`；
3. **只发普通文件**：目录、空文件、不存在的路径都会被明确回一句"发不了"。

### Windows 上"能写文件、一跑命令就失败"

如果 DSH 的 shell 工具报

```
Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(<工作目录>)
```

那是**目录 ACL** 的问题，不是沙箱配置写错了：DSH 跑命令前要给沙箱补一条写权限，
而普通目录从父目录继承下来的只有 `Modify`（**不含改 ACL 的 `WRITE_DAC`**），
所以这一步就被拒绝。

机器人**启动时会自动补一次**（`icacls <工作目录> /grant <当前用户>:(OI)(CI)F`，
幂等、只对自己的工作目录），日志里能看到"工作目录已确认给当前用户完全控制"。
手动修也是这一条命令。

### 风险提示

打开本机操作 = **群里任何能 @ 它的人，都可以让它改这台机器上（`brain.cwd` 范围内）
的文件、跑命令**。所以：

- `brain.cwd` 用一个**专门的工作目录**，不要指到家目录或系统盘根；
- 群不可信时保持默认（只聊天）；
- 真要放开更多，改 `brain.cwd` 就行 —— 但要清楚那就是放开到那个范围。

---

## 五、管理界面（不用再改配置文件）

启动后浏览器打开：

```
http://127.0.0.1:8731/
```

能改的东西就是最常改的那几样：

| 区块 | 作用 |
| --- | --- |
| **接入的群** | 每个已加入的群一个开关；关掉 = 不再回复那个群（它不是退群，只是闭嘴）。下面可以按**群 ID 加入**，也可以**搜索公开的群**点一下就加入并接入 |
| **谁能让它开口** | 三选一：谁都回 / 只回白名单 / 名单里的人不回；名单用标签增删 |
| **触发方式** | 是否"只有被 @ 才回"、额外别名（`@ds` 也算叫它） |
| **回答怎么分条发** | 每条字数上限、合并下限、间隔、间隔上限、被 @ 后的回执文案 |
| **最近的日志** | 最近 80 行，不用再去翻日志文件 |

几个实现上的约定：

- **保存立刻生效**，不用重启；同时会**写回配置文件**，而且**保留注释**——
  只改动了对应的那几行，别的行（包括以后版本新增的键）原样不动。
- 只监听 `127.0.0.1`：它是"改机器人设置"的入口，没有密码，绝不能暴露到公网。
- POST 必须带自定义头 `X-Dchat-Bot: 1`，跨站页面发不出这个头
  （带自定义头的跨域请求要先过 OPTIONS 预检，而我们对 OPTIONS 一律 405）。
- 端口被占用只会打一条警告，机器人照常工作（界面不是必需品）。

---

## 六、安全（请务必读这一段）

**机器人在群里，群里任何人都能 @ 它。** 换句话说：任何人都能往模型的上下文里
塞一段文字。所以：

1. `brain.permissionmode` 保持 `read-only`（默认就是）。
2. **一定要挂上 `dsh-chat-only.yml`**：

   ```
   brain.patch = /opt/dchat/plugins/dsh-bot/dsh-chat-only.yml
   ```

   只读沙箱仍然允许它**读**本机文件 —— 有人一句"把 /etc/passwd 念一遍"就能让
   服务器上的文件进聊天记录。关掉工具之后，它只能靠模型自己的知识回答。
3. 配置文件里有密码：`chmod 600`，别提交进 git。
4. 机器人说的每一句话都是模型说的，**不是** dchat 服务端的公告。别让它在群里
   执行任何"看起来像指令"的信任操作。

---

## 七、部署到服务器（systemd）

```bash
# 二进制
sudo install -m 0755 build/dchat-dsh-bot /opt/dchat/dchat-dsh-bot
# 关工具补丁
sudo install -m 0644 plugins/dsh-bot/dsh-chat-only.yml /opt/dchat/plugins/dsh-bot/
# 配置与运行目录
sudo install -d -o dchat -g dchat /var/lib/dchat/dsh-bot
sudo install -o dchat -g dchat -m 0600 dchat-dsh-bot.conf \
     /var/lib/dchat/dsh-bot/dchat-dsh-bot.conf
# 单元文件
sudo install -m 0644 tools/dchat-dsh-bot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dchat-dsh-bot
journalctl -u dchat-dsh-bot -f
```

想停掉机器人但保留配置：`systemctl disable --now dchat-dsh-bot` ——
对 dchat 服务端和客户端**没有任何影响**。

> DSH 要能找到它自己的 HOME（会话、凭据）。单元里已经设了
> `Environment=HOME=/var/lib/dchat/dsh-bot/home`，第一次部署前把 DSH 在这个
> HOME 下登录好，或者把已有的 `~/.dsh` 拷过去。

---

## 八、它做什么、不做什么

做：

- 文本消息（群里 `RMSG`、大厅 `SAY`）里被 @ 的那种；
- 按房间维护 DSH 会话，让它记得这个群之前聊过什么；
- 断线自动重连（指数退避），重连后自动重新进群；
- 服务器改了 `chatinterval` 自动放慢；
- **登录时的历史回放不会触发回答**（见上面的 `history.settlems`）。

不做：

- **附件、语音、图片**不转发给模型。它只处理文字；
- 不处理私聊（`dm-` 会话）—— 配置里写了也不会回复；
- 不做多人提问的合并：一个人问的时候另一个人 @ 它，会排队（`brain.maxqueue`）；
- 不代替服务端做任何权限判断：被踢、群解散、群禁言之类的规则照常生效。

---

## 九、排查

先跑 `--check`，它会把关键配置打出来：

```bash
./dchat-dsh-bot --check --config dchat-dsh-bot.conf
```

日志在 `log.path` 和 stdout（systemd 下用 `journalctl -u dchat-dsh-bot`）。

| 现象 | 多半是 |
| --- | --- |
| 完全不理人 | 没 @ / 不在 `rooms` / 没进群 / 被白黑名单挡了（日志里都有对应一行） |
| 回"（我这边起不来，稍后再试）" | `brain.command` 找不到可执行文件；Windows 上 `dsh` 是 `.cmd`，插件会自动走 `cmd /c` 转发 |
| 回"（我这边出错了，没答上来）" | `dsh` 自己退出非 0；日志里有它的 stderr 尾部（常见：没登录 DeepSeek 账号、`--session-id` 失效 —— 后者会自动去掉会话重试一次） |
| 回"（想太久了，这次先不说了）" | 超过 `brain.timeoutms`，调大或者换个更快的模型 |
| 只回了第一句 | 服务器 `chatinterval` 很大，或者机器人被踢出了群（日志里有 `不在这个会话里了`） |
| 一直"用户名已存在" | `bot.register = true` 时第二次启动会先试注册、失败后自动改登录；若仍失败，说明密码不对 |

---

## 十、测试

```bash
# 纯逻辑单测：配置解析 / 分条 / @ 判定 / 白黑名单 / NDJSON 解析 / 命令模板
cmake --build build --target test_dsh_bot && ctest -R dsh_bot

# 端到端：真服务器 + 真机器人进程 + 假大脑（不联网、不调模型）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run-dsh-bot-e2e.ps1
```

端到端覆盖的是单测测不到的那一段：真的连服务器、真的加密握手、真的 `GJOIN`、
真的收到 `RMSG` 才回复、真的起子进程读 NDJSON、真的按顺序分条发出去。

真模型联调不放进自动化（要联网、要登录、几十秒一次）：把 `brain.command` 指到
真的 `dsh`，手动发一句 `@机器人 你好` 看效果就行。
