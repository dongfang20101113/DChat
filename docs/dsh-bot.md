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
| `brain.cwd` | 空 | 子进程工作目录 |
| `brain.permissionmode` | `read-only` | 写到子进程的 `DSH_PERMISSION_MODE`。**别用 `danger-full-access`** |
| `brain.patch` | 空 | 额外 `--patch` 文件；强烈建议指向 `plugins/dsh-bot/dsh-chat-only.yml`（关掉工具） |
| `brain.timeoutms` | `120000` | 一次回答最多等多久，超时就杀掉子进程并回一句 |
| `brain.sessionperroom` | `true` | 每个房间一个 DSH 会话，能记住上文 |
| `brain.maxconcurrent` | `1` | 同时跑几个 `dsh` 进程 |
| `brain.maxqueue` | `8` | 排队上限，超了明确回"等我一下" |
| `brain.prompttemplate` | 见示例 | 占位符 `{room}` `{roomId}` `{nick}` `{text}` |

`brain.command` 的写法：

- 按空白分词，支持单/双引号（Windows 路径带空格时必须加引号）；
- `{session}` 展开成 `--session-id <id>`，没有会话时**整个消失**；
- `{prompt}` 展开成一个参数；
- **默认从 stdin 喂提示词**（`-` 就是 stdin）。聊天内容里的引号、换行、反斜杠
  都不需要转义，这是最不容易出错的方式。

### 日志与状态

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `log.path` | `dchat-dsh-bot.log` | 同时写 stdout（systemd 收进 journal） |
| `log.verbose` | `false` | 打开后连"忽略：RSET"这种也记 |
| `state.sessionfile` | `dchat-dsh-bot-sessions.txt` | 房间 → DSH 会话 id，重启后还能接上话 |

---

## 五、安全（请务必读这一段）

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

## 六、部署到服务器（systemd）

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

## 七、它做什么、不做什么

做：

- 文本消息（群里 `RMSG`、大厅 `SAY`）里被 @ 的那种；
- 按房间维护 DSH 会话，让它记得这个群之前聊过什么；
- 断线自动重连（指数退避），重连后自动重新进群；
- 服务器改了 `chatinterval` 自动放慢。

不做：

- **附件、语音、图片**不转发给模型。它只处理文字；
- 不处理私聊（`dm-` 会话）—— 配置里写了也不会回复；
- 不做多人提问的合并：一个人问的时候另一个人 @ 它，会排队（`brain.maxqueue`）；
- 不代替服务端做任何权限判断：被踢、群解散、群禁言之类的规则照常生效。

---

## 八、排查

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

## 九、测试

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
