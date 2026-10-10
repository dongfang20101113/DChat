# dchat 多会话（私聊 / 群聊）部署方案

本文只写**这次改造特有**的东西。通用的服务器运维（systemd、端口、防火墙）见 README。

---

## ⚠️ 先看这一条：线上服务器必须是 **Linux 版二进制**

现在手上编译出来的是 **Windows 版** `build\dchat_server.exe`（MinGW）。
线上那台是阿里云 Ubuntu 22.04，**不能直接把 .exe 传上去**。

必须先在 Linux 上编一次：

```bash
sudo apt install -y build-essential cmake ninja-build libssl-dev   # OpenSSL 是 Linux 的加密后端
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target dchat_server
```

`CMakeLists.txt` 里已经分好了：Windows 走 CNG（bcrypt），Linux 走 OpenSSL；
`socket_util.h` 把 Winsock / POSIX 的差异收拢在一处（连 Linux 的 `MSG_NOSIGNAL`、
macOS 的 `SO_NOSIGPIPE` 都分开处理了）。

### 2026-10-11：Linux 分支已经**真的编过一遍**，不再只是静态判断

没有 Linux 机器也能做：**zig 自带 Linux libc 头**，`-target x86_64-linux-gnu`
会把 `#ifndef _WIN32` 那半边**真正编译**（不是跳过）。

```bash
tools/verify_linux.sh                 # 需要 zig（winget install zig.zig / brew install zig）
tools/verify_linux.sh . /path/to/zig  # Windows 上也能跑，第二个参数指向 zig.exe
```

实测结果：

| 部分 | 结果 |
|---|---|
| 服务端 + 共用源码 14 个文件（含 `server.cpp`、`room.cpp`、`room_registry.cpp`、`socket_util.cpp`、`history_store.cpp`…） | `x86_64-linux-gnu` **全部 OK** |
| 客户端核心 8 个文件（`client_core/*`） | **全部 OK** |
| `aarch64-linux-gnu` 抽查（server / room / socket_util） | **OK** |
| `src/crypto_backend_openssl.cpp` | **编不过 —— 但只是因为本机没有 Linux 版 OpenSSL 头**（`openssl/core_names.h` file not found）。目标机 `apt install libssl-dev` 后这个缺口就没了 |

界面层（Win32 / GDI+）本来就不在 Linux 上构建，由 `CMakeLists.txt` 的 `if(WIN32)` 挡掉。

**这仍然只证明"编得动"，不证明"跑得起来"** —— 到服务器上还是要 `ctest` + 真连一次。

构建完顺手跑一遍测试，确认不是"能编但行为不对"：

```bash
cd build-linux && ctest --output-on-failure
```

---

## 部署步骤

```bash
# 1. 备份（**必做**：回滚全靠它）
sudo systemctl stop dchat
sudo cp /opt/dchat/dchat_server /opt/dchat/dchat_server.bak-$(date +%Y%m%d-%H%M)

# 2. 换二进制
sudo cp build-linux/dchat_server /opt/dchat/dchat_server
sudo chown dchat:dchat /opt/dchat/dchat_server
sudo chmod +x /opt/dchat/dchat_server

# 3. 起服务
sudo systemctl start dchat
sudo systemctl status dchat --no-pager
```

工作目录是 `/var/lib/dchat`，所以数据文件都在那里。

---

## 数据兼容：**升级不会丢任何东西**

这是这次改造里最要紧的一条，逐条说清楚为什么安全：

| 数据 | 升级前 | 升级后 | 为什么安全 |
| --- | --- | --- | --- |
| 大厅历史 | `dchat-history.txt` | **同一个文件** | `HistoryPathFor("hall")` 刻意返回原文件名 |
| 大厅文件 | `dchat-files/` | **同一个目录** | `FilesDirFor("hall")` 同样刻意返回原路径 |
| 账号 / 管理员 / 规则 | 不变 | 不变 | 没碰这几个文件 |
| 群聊历史 | —— | `dchat-history-<群ID>.txt` | 新建的，互不影响 |
| 群文件 | —— | `dchat-files/<群ID>/` | 新建的，互不影响 |
| 房间名单 | —— | `dchat-rooms.txt` | 新建的；**大厅不写进去**（由代码建） |
| 私聊黑名单 | —— | `dchat-blocks.txt` | 新建的 |

启动日志里应该能看到这几行，用来确认读对了：

```
loaded N room(s) from dchat-rooms.txt
loaded N history line(s) from dchat-history.txt（末序号 N）
loaded N block entr(ies) from dchat-blocks.txt
```

`dchat-rooms.txt` 不存在是**正常的**（还没建过群），日志会写
`no rooms file yet; only the hall exists`。

---

## 兼容性：老客户端一个字节都不受影响

上线后**不需要**同时升级客户端。服务端对没声明 `rooms1` 能力的连接走的是原路径：

- 只维护大厅，**不**发 `ROOMLIST` / `FEATURES` / `REMSG` / `REVENT`
- 大厅消息**仍然**是 `SAY <时间> <昵称> <文本>`，和升级前一模一样
- 群里发生的事（谁建了群、谁加入了）老客户端完全看不到 —— 这是对的，它不在那些群里

验证方法（本机就能做）：拿**旧版客户端**连上新服务端，只在大厅发消息，应该
完全正常。这条已经由 `tools/run-rooms-e2e.ps1` 的第 9 阶段自动覆盖
（那条"老客户端收不到任何 RMSG/ROOMLIST/FEATURES/REVENT"）。

---

## 回滚

```bash
sudo systemctl stop dchat
sudo cp /opt/dchat/dchat_server.bak-<时间戳> /opt/dchat/dchat_server
sudo systemctl start dchat
```

**回滚是安全的**：新版本只**新增**文件（`dchat-rooms.txt` 等），没有改写老文件。
旧版本不认识那些新文件，会当作不存在的普通文件忽略掉。
唯一要注意的是：新版本期间产生的**群聊和私聊内容**，回滚后旧版本看不到
（它本来也没有那些功能），但文件还在，再升回去就能看到。

---

## 上线后要做的检查

1. **大厅照常**：用旧客户端连上，发几条消息，确认收发正常、历史能回放
2. **老客户端隔离**：旧客户端不该收到任何 `ROOMLIST` / `RMSG`（看客户端界面有没有出现看不懂的协议原文）
3. **建群**：用新客户端建一个群，确认 `dchat-rooms.txt` 出现了、`dchat-history-<群ID>.txt` 在发消息后出现
4. **重启不丢**：`systemctl restart dchat`，确认群、历史、群设置都还在
5. **缓存预算真的生效**：把某个群的「总缓存最大大小」调到 1 MB，再发一堆消息，
   确认 `dchat-history-<群ID>.txt` 的实际大小被压到 1 MB 以内
   （这条最容易做成"设置项是个摆设"—— 所以专门列出来）

   ⚠️ **量的时候要跨过"压实检查点"，否则会误判成没生效**（2026-10-11 实测踩过）：
   磁盘文件不是每条消息都重写，而是**每 512 条追加**才检查一次要不要压实
   （`kHistoryCompactEvery`）。两次检查点之间文件**会超过预算** —— 实测发到 670 条时
   是 1,372,131 字节（超 31%），跨过 1024 那条线之后立刻回落到 1,046,553（预算内）。

   所以：**别在刚发完几百条的时候去量**。要么发够 1000 条以上再看，要么直接看
   `server.log` 里的这三行（服务端每个检查点都会打）：
   ```
   history budget [cachegroup]: appended=1024 kept=511 bytes=1046553 budget=1048576 compactDue=1
   history compacted [cachegroup]: 511 line(s) kept
   ```
   `kept < appended` = 真的淘汰了最旧的；`bytes <= budget` = 内存里严格守住了预算。
   一键复现：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\rooms-cache-budget.ps1`
6. **文件隔离**：往群里传个文件，确认落在 `dchat-files/<群ID>/` 而不是 `dchat-files/`

---

## 还没验过的东西（诚实清单）

- ~~**Linux 构建**：新增的 `room.cpp` / `room_registry.cpp` 只在 Windows 上编过~~
  → **2026-10-11 已用 zig 交叉编译验证**：服务端侧 14 个文件 + 客户端核心 8 个文件在
  `x86_64-linux-gnu` 下全部编过（`tools/verify_linux.sh`），只剩
  `crypto_backend_openssl.cpp` 需要目标机装 `libssl-dev`。**仍未做的是：在真 Linux 上
  链接 + 跑 ctest + 真连一次。**
- **界面验证**：桌面端已经有 `tools/ui-rooms-auto.ps1`（81 项检查，驱动**真实窗口消息**、
  断言客户端自己导出的内部状态），安卓端有 `RoomInteropTest`（连**真实服务端**跑完整
  群聊流程）。所以"界面从来没被真正驱动过"**已经不成立**了；
  仍然成立的是：**没有真人手动点过**（真机手感、输入法、DPI 这些还得人看）
- **多客户端并发**：端到端测试是串行跑的（一个客户端说完另一个说），
  没有测过"两个人在不同群里同时说话"
- **弱网 / 大群**：没测过 200 人群、没测过快慢网络下的表现
