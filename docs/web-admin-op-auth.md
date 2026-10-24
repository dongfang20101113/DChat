# `/admin` 改成 op 权限：登录即判定，不再有共享口令

## 结论先说

`/admin` 现在**没有口令输入框**。能不能进只看一件事：

> 这个浏览器**已经用 dchat 账号登录过聊天**，而且那个账号在
> `/var/lib/dchat/dchat-admins.txt` 里（也就是 op）。

三条同时成立才放行，缺一条都进不去：

1. 登录成功（账号密码由 dchat 服务器校验，不是网关自己认的）
2. 昵称在 op 名单里
3. 浏览器拿着网关在登录时发的那个 op 会话号

## 为什么不用共享口令

原来的设计是「服务器上有一个 48 位口令，谁拿到谁进」。问题很直白：

- **口令 ≠ 身份**。拿到口令的人不需要任何 dchat 账号，也就无从判断他是不是 op。
- 口令一旦泄露就等于 op 泄露，而且撤不掉——只能换口令、通知所有人重登。
- `/deop 某人` 之后他还进得去，因为口令没变。

## 怎么实现的

### 1. 登录成功时判定（`webd.cpp` 的 `Session::OnLoggedIn`）

```cpp
if (IsOpNick(nick)) {
    const std::string id = MintOpSession(nick);
    SendLine("ADMINSTATE\t1\t" + id);
} else {
    SendLine("ADMINSTATE\t0");
}
```

- `IsOpNick()` 现读 `dchat-admins.txt`，一行一个昵称，`#` 开头是注释。
  **读不到文件就当谁都不是 op**（失败时关闭，不是失败时放行）。
- 只有 op 才拿得到号。非 op 收到的永远是 `ADMINSTATE 0`，什么号都没有。

### 2. 号怎么变成 Cookie

聊天页 JS 收到 `ADMINSTATE 1 <号>` 后写 Cookie：
`dchat_op=<号>; path=/admin; SameSite=Strict`，同时把页头「管理」链接显示出来。
非 op 收到 `ADMINSTATE 0` 时会把这个 Cookie 删掉。

### 3. `/admin` 只认这个 Cookie

```cpp
const std::string sessionId = OpSessionFromRequest(headLower, head);
std::string opNick;
if (!OpSessionNick(sessionId, &opNick)) {
    send("403 Forbidden", "", AdminNeedOpPage(...));   // 页面里没有任何输入框
    return;
}
```

- 号是 `/dev/urandom` 取的 24 字节（48 位 hex），**不是**原来的 LCG。
- 绑昵称、12 小时过期。
- CSRF 仍然是「隐藏字段 == 会话号」那一套，没变。
- `AdminCall`（webd → 服务器 5556）仍然用服务器上的口令文件，
  但那个口令**只给机器用**，不再给人用。

## 端口分工（容易误会的一点）

| 端口 | 绑定 | 谁在用 |
| --- | --- | --- |
| 8080 | 0.0.0.0 | 浏览器，外网可访问。`/admin` 从这里出去 |
| 5555 | 0.0.0.0 | dchat 客户端 |
| 5556 | **127.0.0.1** | 只有 webd。外网扫不到，也不该开放 |

`/admin` 外网可访问，跟 5556 只绑回环**不矛盾**：5556 是「改服务器规则」这条
内部通道，浏览器永远走 8080。

## 顺带修掉一个会让网关崩掉的 bug

跑测试时 webd 段错误了，ASan 报的是 **heap-use-after-free**：

```
READ of size 1 ... in CryptoSession::Decrypt  (接收线程)
freed by ... ~ChatCore  (另一个线程，Session 析构)
```

根因在 `client_core/net.cpp`：`StartReceiveLoop()` 起线程时用了 `.detach()`，
而 `Close()` 关掉 socket 就直接返回。于是这个对象（连同 `crypto_`）会在接收线程
还在解密下一行的时候被析构。

网页版每个浏览器连接都新建/销毁一次 `ChatCore`，所以**用户只要在数据还在传的
时候关掉标签页，就可能把整个网关打崩**。这不是这次改出来的，是一直都有。

修法：接收线程不再 detach，`Close()` 里按顺序来——

```
shutdown(SHUT_RDWR)  →  唤醒阻塞在 recv 的线程
join()               →  等它真的退出
close(fd)            →  最后才关 fd、让成员析构
```

两个细节：

- `ShutdownWrite` 只关写方向，**唤不醒 recv**，所以用 `SHUT_RDWR`。
- 接收线程自己回调进来调 `Disconnect()` 时不能 join 自己（会死锁），
  这条路径直接返回，让持有它的线程稍后收尾；并用 `exchange` 保证只 join 一次。

## 验证

### 沙箱（Ubuntu，真实 server + webd，**ASan 版本**）

18 项全过，连跑 3 轮都是 18/18，ASan 报告 0 条：

| 检查 | 结果 |
| --- | --- |
| op 账号登录成功 | PASS |
| op 拿到 `ADMINSTATE=1` | PASS |
| op 拿到的号是 48 位 hex | PASS |
| 非 op 登录成功 | PASS |
| 非 op 得到 `ADMINSTATE=0` | PASS |
| 非 op 连号都拿不到 | PASS |
| 没 Cookie 进 `/admin` → 403 | PASS |
| 403 页面写明需要 op 权限 | PASS |
| 403 页面**没有**口令输入框 | PASS |
| 伪造的号 → 403 | PASS |
| op 的号 → 200 | PASS |
| 页面含规则表单 | PASS |
| 页面显示 op 昵称 | PASS |
| CSRF 字段 == 会话号 | PASS |
| `POST /admin/set` → 200 | PASS |
| 改完提示已保存 | PASS |
| 新值真的出现在页面上 | PASS |
| CSRF 不对 → 403 | PASS |

另外单跑 40 轮「登录完立刻关连接」，无崩溃、无 ASan 报告。

> 测试里写 `maxtextlen=4321` 那次失败过，那是**测试值写错了**：上限是 4096，
> 服务器把 4321 夹成 4096，所以页面上不是 `4321`。改成 3000 就对了。
> 顺便说明服务器确实会把越界值夹到范围内。

### 线上（120.26.223.225）

| 检查 | 结果 |
| --- | --- |
| `dchat.service` / `dchat-webd.service` | 都 active |
| 5555 / 8080 / 5556 监听 | 都在；**5556 只绑 127.0.0.1** |
| 口令文件权限 | `0600 dchat:dchat`，48 字符 |
| webd 用户能读 op 名单和口令 | 都能读 |
| 管理通道认证 + `LIST` | `OK 已认证`，返回 **16 条**规则 |
| 外网 `/` `/room/hall` | 200 |
| 外网 `/admin`（没登录） | **403** |
| 重启后 warning 日志 | 无 |

**还没验的一条**：用 `dongfang` 账号登录后 `/admin` 是否放行——
这需要那个账号的密码，我没有。你登一次就能确认。

## 改动文件

- `client_core/net.cpp` / `net.h` — 接收线程 join（修 UAF）
- `webd/webd.cpp` — op 判定、`ADMINSTATE`、`--admins`、`/admin` 鉴权改成 Cookie，
  删掉口令登录和 CSRF 之外的旧逻辑
- 线上 systemd drop-in（可整文件删除回滚）：
  `/etc/systemd/system/dchat.service.d/admin.conf`
  `/etc/systemd/system/dchat-webd.service.d/admin.conf`

## 回滚

```bash
sudo rm /etc/systemd/system/dchat.service.d/admin.conf \
        /etc/systemd/system/dchat-webd.service.d/admin.conf
sudo cp -p /opt/dchat/dchat_webd.bak-<时间戳> /opt/dchat/dchat_webd
sudo systemctl daemon-reload && sudo systemctl restart dchat.service dchat-webd.service
```
