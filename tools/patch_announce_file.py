"""给服务端加「看文件就广播」：写一个文件，服务端读到就发全服公告。

用途：systemd timer 在重启前 10/5/3/1 分钟和 10 秒各写一次这个文件，
玩家就能收到倒计时公告。**不需要在服务器上存任何密码**。

为什么不让定时任务直接发 /say：/say 是控制台指令，而 systemd 跑的服务
stdin 接的是 /dev/null，定时任务没有"键盘"可以往里打字。

设计：
  - 文件 dchat-announce.txt（与 dchat-history.txt 同目录，只有 root/dchat 能写）
  - 复用 /say 那条完全相同的广播路径，保证格式和真公告一致
  - 只取**第一行非空内容**：协议是行式的，正文里带换行会破坏整条消息
  - 广播完就删文件；删不掉就清空，避免每秒重复刷屏
  - 轮询挂在已有的 BanMaintenanceLoop 上（本来就每秒跑一次），不新增线程
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

ANCHOR = "void BanMaintenanceLoop() {"

HELPER = '''// ---- 定时公告：写文件即广播 ----
// 协议是**行式**的，所以只取第一行非空内容；正文里带换行会把整条命令劈成两条，
// 后面那条多半会被当成非法命令（甚至被别的客户端解析成奇怪的东西）。
std::string g_announcePath = "dchat-announce.txt";
constexpr std::size_t kMaxAnnounceChars = 300;

// 读一行公告文件并广播；没有文件/内容为空就什么都不做。
// 由 BanMaintenanceLoop 每秒调用（那里本来就有个 1 秒周期，不另开线程）。
void PollAnnounceFile() {
    std::error_code ec;
    if (!std::filesystem::exists(g_announcePath, ec)) return;

    std::ifstream in(g_announcePath, std::ios::binary);
    std::string text;
    if (in) {
        std::getline(in, text);          // 只取第一行
        in.close();
    }
    // 去掉行尾的 \\r（Windows 上编辑过这个文件时会有）和首尾空白
    while (!text.empty() && (text.back() == '\\r' || text.back() == ' ' || text.back() == '\\t')) {
        text.pop_back();
    }
    const std::size_t begin = text.find_first_not_of(" \\t");
    if (begin == std::string::npos) {
        text.clear();
    } else {
        text = text.substr(begin);
    }
    if (text.size() > kMaxAnnounceChars) text = dchat::Utf8Truncate(text, kMaxAnnounceChars);
    if (text.empty()) {
        // 空文件：清掉就行，别重复读
        std::filesystem::remove(g_announcePath, ec);
        return;
    }

    Log("announce (from file): " + text);
    const std::string announce = Timed("ANNOUNCE", text);
    Broadcast(announce);         // 全服公告：客户端会大字居中显示
    RememberHistory(announce);   // 和 /say 一样，也进聊天记录

    // 播完就删；删不掉（权限/占用）就清空，**否则每秒都会重播一遍**。
    // CronJob 那边是"先写临时文件再改名"，所以这里不会读到写了一半的内容。
    std::filesystem::remove(g_announcePath, ec);
    if (std::filesystem::exists(g_announcePath, ec)) {
        std::ofstream clear(g_announcePath, std::ios::binary | std::ios::trunc);
        Log("warn: cannot remove " + g_announcePath + ", truncated instead");
    }
}

void BanMaintenanceLoop() {'''

CALL_ANCHOR = '''            std::this_thread::sleep_for(std::chrono::seconds(1));
            ExpireBans();'''

CALL_NEW = '''            std::this_thread::sleep_for(std::chrono::seconds(1));
            ExpireBans();
            PollAnnounceFile();  // 定时公告：cron 写文件，这里读到就广播'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as fh:
        text = fh.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    if "PollAnnounceFile" in text:
        raise SystemExit("已经加过了")

    ok = 0
    for old, new, label in [
        (ANCHOR, HELPER, "新增 PollAnnounceFile"),
        (CALL_ANCHOR, CALL_NEW, "挂到每秒循环"),
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
