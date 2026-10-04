"""README 更新：文件暂存已改磁盘 + 新增每夜自动重启/倒计时公告章节。"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "README.md")

OLD_STAGE = '''  - 服务器在**内存**里暂存上传的文件：**单个 64 MB、最多 16 个、总共 256 MB、30 分钟过期**，超出就先淘汰最旧的；服务器重启后暂存的文件就没了（那时点下载会提示"文件不存在或已经过期"）'''

NEW_STAGE = '''  - 服务器**在磁盘上**暂存上传的文件（工作目录下的 `dchat-files/`）：**单个上限看 `documentsize`（默认 64 MB）、最多 16 个、总量上限看 `maxservertemp`（默认 1048 MB）、30 分钟过期**，超出就先淘汰最旧的，**并且把磁盘文件一起删掉**
  - 为什么是磁盘不是内存：早先整个文件内容都放在 `std::string` 里驻留内存。一台 1.6 GB 内存的机器传几个大文件就会被 OOM 杀掉——而且当时那台机器的 `maxservertemp` 被设成了 32 GB，是物理内存的 20 倍，那道闸门形同虚设。现在上传边收边写 `dchat-files/upload-N.part`，收齐后改名落定，下载时按 offset 从文件读。**进程内存不再随文件大小增长**（实测传 24 MB，RSS 只涨 52 KB）
  - 语音消息走的是同一条路径（`kind=voice`），所以一样落在磁盘上
  - 服务器重启后暂存的文件就没了——启动时会清空 `dchat-files/`，顺便清掉上次崩溃留下的 `.part`（那时点下载会提示"文件不存在或已经过期"）
  - 上传中途断线/取消/校验失败，`.part` 会被删掉（靠 `PendingUpload` 的析构兜底，不用在每个退出分支手写清理）'''

# 新增章节：插在「消息不会因为…而丢」那一节之前
ANCHOR = "## 消息不会因为「你不在」或「进程重启」而丢"

NEW_SECTION = '''## 每夜自动重启与倒计时公告

生产上是这么跑的：每天 **02:00** 自动重启服务端，重启前在 **10 / 5 / 3 / 1 分钟和 10 秒**各发一条全服公告。

```ini
# /etc/systemd/system/dchat-restart.timer —— 六个触发时刻
OnCalendar=*-*-* 01:50:00     # 10 分钟前
OnCalendar=*-*-* 01:55:00     # 5 分钟前
OnCalendar=*-*-* 01:57:00     # 3 分钟前
OnCalendar=*-*-* 01:59:00     # 1 分钟前
OnCalendar=*-*-* 01:59:50     # 10 秒前
OnCalendar=*-*-* 02:00:00     # 重启
Persistent=false              # 关机期间错过的不补跑（否则开机瞬间会突然重启一次）
```

### 公告是怎么发出去的：写文件即广播

`/say` 是**服务端控制台**指令，而 systemd 跑的服务 stdin 接的是 `/dev/null`——
定时任务**没有"键盘"可以往里打字**。所以服务端支持一个更简单的入口：

```bash
# 服务端每秒轮询工作目录下的 dchat-announce.txt，读到就当成 /say 广播出去
sudo dchat-announce "服务器将在 5 分钟后自动重启"

# 或者自己写文件（先写临时文件再改名，避免服务端读到写了一半的内容）
printf '%s\\n' "消息" > /var/lib/dchat/.tmp && mv /var/lib/dchat/.tmp /var/lib/dchat/dchat-announce.txt
```

实现上的几个要点（都在 `PollAnnounceFile()` 里）：

- 复用 `/say` 那条**完全相同**的广播路径（`Timed("ANNOUNCE", …)` + `Broadcast` + `RememberHistory`），
  所以公告的显示效果和真公告一模一样，也会进聊天记录
- **只取第一行非空内容**：协议是行式的，正文里带换行会把整条命令劈成两条
- 剥掉 **UTF-8 BOM** 和行尾 `\\r`：在 Windows 上编辑过这个文件就会带上，
  不剥的话 BOM 会作为正文第一个字符被播出去（客户端显示成一个方块）
- 播完就删文件；删不掉就清空——**否则每秒都会重播一遍**
- 轮询挂在已有的 `BanMaintenanceLoop` 上（本来就每秒跑一次），不新增线程
- 这个入口**不需要任何密码**：能写 `/var/lib/dchat/` 的只有 root 和 `dchat` 用户

## 消息不会因为「你不在」或「进程重启」而丢
'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as fh:
        text = fh.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    ok = 0
    for old, new, label in [
        (OLD_STAGE, NEW_STAGE, "文件暂存改磁盘（第 103 行）"),
        (ANCHOR, NEW_SECTION, "新增每夜重启与公告章节"),
    ]:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        ok += 1
        print("  ✅", label)

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("共 %d / 2 处" % ok)


if __name__ == "__main__":
    main()
