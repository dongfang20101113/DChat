# 往 tools/make_ppt.py 里插入安全/性能相关的幻灯片并更新装配顺序。
# 单独写成脚本是因为改动量大，用 Replace 拼容易出错。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

NEW_SLIDES = '''

def slide_sec_design(prs):
    slide = add_slide(prs, "安全设计：纵深防御",
                      "没有单点神话，是四层叠起来——每一层都假设上一层可能被绕过")
    layers = [
        ("第 1 层  传输加密", "ECDH P-256 握手 · HKDF-SHA256 派生 · AES-256-GCM 逐条认证",
         "抓包看不到密码与聊天内容；改一个字节就解不开", BLUE, BLUE_L),
        ("第 2 层  身份与信任", "TOFU 指纹：第一次记住，之后每次比对，变了就中止连接",
         "服务器密钥被换掉时，用户会立刻看到明确警告", GREEN, GREEN_L),
        ("第 3 层  资源与准入", "连接数上限 · 单 IP 上限 · 握手超时 · 上传下载限速 · 发言间隔",
         "把「占着连接不说话」和「狂建连接」在入口掐掉", AMBER, AMBER_L),
        ("第 4 层  凭据保护", "PBKDF2-HMAC-SHA256 加盐 · 登录失败窗口封禁 · 密码不落盘",
         "拖走账号文件也拿不到密码；在线暴破被时间窗限住", RED, RED_L),
    ]
    y = 1.28
    for title, detail, effect, color, light in layers:
        card(slide, 0.62, y, 12.1, 1.14, light, color)
        textbox(slide, 0.85, y + 0.05, 4.3, 0.42, [title], size=15, color=color, spacing=0)
        textbox(slide, 0.85, y + 0.49, 6.3, 0.6, [detail], size=11.5, color=TEXT, spacing=0)
        textbox(slide, 7.35, y + 0.3, 5.2, 0.7, [effect], size=11.5, color=GRAY, spacing=0)
        y += 1.26
    note(slide, "四层互相独立：加密被绕过仍有指纹，指纹被忽略仍有连接上限，上限被绕开仍有凭据哈希")
    return slide


def slide_ddos(prs):
    slide = add_slide(prs, "压力与攻击实测（一）：防护是否真的生效",
                      "在 Kali 虚拟机上对着真服务端打出来的，不是估算值")
    rows = [
        ["攻击方式", "配置", "实测结果"],
        ["连接洪泛 200 条", "maxconnsperip = 60",
         "正好 60 条存活、140 条被拒——上限精确生效，服务端逐条记录拒绝原因"],
        ["慢速连接耗尽 40 条 × 占住 25 秒", "handshaketimeout = 10 秒",
         "40/40 全部被按时断开；服务端日志出现 72 次 handshake timeout"],
        ["暴力破解 12 次错误密码", "loginfails = 5（5 分钟窗口）",
         "12/12 全部被拒，攻击方一次都没登上"],
        ["畸形数据 11 类", "—",
         "超长行 / 二进制乱字节 / 无换行灌数据 / 伪造 HELLO_OK / 未握手注入 ENC / 半包即断——全部未命中，服务端未崩溃"],
        ["攻击后正常业务", "—", "新客户端仍能正常注册、登录、收发消息"],
        ["内存占用", "—", "攻击全程稳定在 9.3–10.3 MB，无增长失控"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.35, rows, col_widths=[2.9, 2.5, 6.7], font_size=11)
    note(slide, "复现方式：tools/loadtest.cpp（POSIX socket 手写，五个独立场景：flood / slowloris / badlogin / garbage / echo）")
    return slide


def slide_throughput(prs):
    slide = add_slide(prs, "压力与攻击实测（二）：吞吐与稳定性",
                      "数量级是真实吞吐，不是写缓冲假象——和服务端日志逐条计数核对过")
    card_text(slide, 0.62, 1.3, 3.9, 2.15, "16 客户端持续压测", [
        "处理消息 474,296 条",
        "墙钟 2.4 秒",
        "服务端 CPU 17 秒（多核并行）",
        "广播式分发，非单线程上限",
    ], accent=BLUE, fill=BLUE_L, title_size=14, body_size=12.5)
    card_text(slide, 4.72, 1.3, 3.9, 2.15, "资源占用", [
        "空载内存 9.2 MB",
        "攻击 + 压测全程 9.8–10.8 MB",
        "全程零崩溃、零重启",
        "单文件静态二进制，无外部依赖",
    ], accent=GREEN, fill=GREEN_L, title_size=14, body_size=12.5)
    card_text(slide, 8.82, 1.3, 3.9, 2.15, "建连能力", [
        "200 条并发连接",
        "默认配置建连 6 ms",
        "约 33,000 连接/秒",
        "（本机回环，非广域网指标）",
    ], accent=AMBER, fill=AMBER_L, title_size=14, body_size=12.5)

    card_text(slide, 0.62, 3.62, 12.1, 2.55, "这些数字为什么可信", [
        "· 吞吐不是「写进去就算成功」：服务端日志逐条统计 MSG，实测 474,296 条，与客户端计数一致",
        "· 连接不是「connect 返回 0 就算建立」：必须等不到 FIN 才算存活——这条修正让「连接洪泛」的结论从「200 条全通」变成「恰好 60 条」",
        "· 慢速耗尽不是「send 成功就算还占着」：必须读到 FIN 才算被断开——修正后结论从「0 条被断开」变成「40/40 全部断开」",
        "· 换句话说：每一条「防护有效」的结论，都先把测量工具本身验证过一遍",
    ], accent=RED, fill=RED_L, title_size=15, body_size=12)
    return slide


def slide_rules(prs):
    slide = add_slide(prs, "可调防护规则", "所有防护都是运行时参数：改一行、重启即生效")
    rows = [
        ["规则", "作用", "默认值"],
        ["maxconns", "同时连接总数上限", "0（不限）"],
        ["maxconnsperip", "同一 IP 的同时连接上限", "0（不限）"],
        ["loginfails", "同 IP 每 5 分钟允许的登录失败次数，超出即封", "0（不限）"],
        ["handshaketimeout", "连上后多少秒内必须登录，否则断开", "30 秒"],
        ["uploadrate / downloadrate", "单客户端上传 / 下载限速（KB/s）", "0（不限）"],
        ["maxtextlen / maxtextlines", "单条消息最大字符数 / 行数", "0（不限）"],
        ["chatinterval", "发言最小间隔，防刷屏", "0（不限）"],
        ["documentsize / maxservertemp", "单文件上限 / 暂存总量上限", "64 MB / 1024 MB"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.4, rows, col_widths=[3.7, 5.9, 2.5], font_size=11.5)
    note(slide, "默认只开握手超时是刻意的：先保证「拿下来就能用」，要上公网再按需开启——加固后的实测见前两页")
    return slide


def slide_claims(prs):
    slide = add_slide(prs, "边界：哪些是防护，哪些不是", "一份诚实的威胁模型，比一堆形容词有用")
    card_text(slide, 0.62, 1.3, 6.0, 2.55, "已经做到的（有实测支撑）", [
        "· 被动窃听：抓包看不到密码与聊天内容",
        "· 内容篡改：GCM 逐条认证，改一个字节即失效",
        "· 密钥变更可见：TOFU 指纹变了会中止并告警",
        "· 应用层资源耗尽：连接上限 + 握手超时实测生效",
        "· 在线暴力破解：失败次数窗口封禁实测生效",
        "· 畸形输入：11 类攻击样本未造成崩溃",
    ], accent=GREEN, fill=GREEN_L, title_size=15, body_size=12)
    card_text(slide, 6.72, 1.3, 6.0, 2.55, "明确没做的（写在代码注释里）", [
        "· 主动中间人：裸 ECDH 没有身份认证，能劫持线路的人",
        "  理论上可以分别和两端握手——要挡它需要证书或预共享密钥",
        "· 网络层大流量 DDoS：本项目只做应用层防护，",
        "  真正的流量清洗要靠上游运营商 / 云清洗 / CDN",
        "· 端到端加密：服务端能解密消息（它要转发、要存历史）",
        "· 密码找回、多设备同步、消息签名——都没有",
    ], accent=RED, fill=RED_L, title_size=15, body_size=12)
    card_text(slide, 0.62, 4.02, 12.1, 2.25, "为什么要把「没做的」也写出来", [
        "· 安全材料里最危险的不是「防护少」，而是「你以为防护了很多」——",
        "  使用者按错误假设去用，风险比明说大得多",
        "· 这不是事后补的：项目从第一版加密起就在 crypto.h 里写着「不保护主动中间人」，README 照抄同一句话",
        "· 前面的实测数字证明的是「规则按设计生效」，不是「服务器打不死」",
    ], accent=AMBER, fill=AMBER_L, title_size=15, body_size=12)
    return slide

'''

OLD_ORDER = """    slide_timeline(prs)
    slide_phases(prs)
    slide_verification(prs)
    slide_bugs(prs)"""

NEW_ORDER = """    slide_timeline(prs)
    slide_phases(prs)
    slide_verification(prs)
    slide_sec_design(prs)
    slide_ddos(prs)
    slide_throughput(prs)
    slide_rules(prs)
    slide_claims(prs)
    slide_bugs(prs)"""


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    anchor = "\ndef slide_bugs(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_bugs 锚点")
    if "slide_sec_design" in text:
        raise SystemExit("新幻灯片已存在，无需重复插入")
    text = text.replace(anchor, NEW_SLIDES + "\ndef slide_bugs(prs):", 1)

    if OLD_ORDER not in text:
        raise SystemExit("找不到 main() 里的装配顺序锚点")
    text = text.replace(OLD_ORDER, NEW_ORDER, 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已插入 5 页并更新装配顺序，当前文件行数:",
          len(text.splitlines()))


if __name__ == "__main__":
    main()
