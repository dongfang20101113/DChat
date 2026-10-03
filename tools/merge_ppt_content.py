"""把被并入页的内容真的搬进目标页（上一步只改了装配顺序，这一步补内容）。

每条合并都尽量给出一个"合并理由"式的注释：说明为什么这两页放在一起比分开更好，
而不是"为了凑页数硬删"。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

# ---------------------------------------------------------------- 1. 安全设计 + 压测实测
SEC_OLD = '''    layers = [
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
    return slide'''

SEC_NEW = '''    # 每层都配一条**实测证据** —— 只有"设计成四层"没有说服力，
    # 要能说出"这一层对着什么攻击、打出来的结果是什么"。
    layers = [
        ("第 1 层  传输加密", "ECDH P-256 · HKDF-SHA256 · AES-256-GCM 逐条认证",
         "抓包看不到密码与内容；改一字节就解不开", "畸形数据 11 类全部未命中", BLUE, BLUE_L),
        ("第 2 层  身份与信任", "TOFU 指纹：第一次记住，之后每次比对，变了就中止",
         "服务器密钥被换掉时立刻明确警告", "与 Kotlin 跨语言握手比对一致", GREEN, GREEN_L),
        ("第 3 层  资源与准入", "连接上限 · 单 IP 上限 · 握手超时 · 限速 · 发言间隔",
         "把占着连接不说话、狂建连接在入口掐掉",
         "洪泛 200 条 → 正好 60 存活 / 140 被拒\\n慢速 40 条 × 25 秒 → 40/40 按时断开",
         AMBER, AMBER_L),
        ("第 4 层  凭据保护", "PBKDF2-HMAC-SHA256 加盐 · 登录失败窗口封禁 · 不落盘",
         "拖走账号文件也拿不到密码；在线暴破被时间窗限住",
         "错密码 12 次 → 12/12 被拒，一次没登上", RED, RED_L),
    ]
    y = 1.22
    for title, detail, effect, measured, color, light in layers:
        card(slide, 0.62, y, 12.1, 1.22, light, color)
        textbox(slide, 0.85, y + 0.06, 4.3, 0.38, [title], size=14.5, color=color, spacing=0)
        textbox(slide, 0.85, y + 0.46, 5.2, 0.68, [detail], size=11, color=TEXT, spacing=0)
        textbox(slide, 6.25, y + 0.32, 2.6, 0.8, [effect], size=10.5, color=GRAY, spacing=0)
        textbox(slide, 9.0, y + 0.12, 3.55, 0.98, measured.split("\\n"), size=11, color=color,
                spacing=0)
        y += 1.30
    note(slide, "四层互相独立（加密被绕过仍有指纹，指纹被忽略仍有连接上限，上限被绕开仍有凭据哈希）；"
                "右栏是每一层的实测证据，全部在真服务端上打出来。攻击后内存稳定在 9.3–10.3 MB、"
                "新客户端照样能注册登录收发")
    return slide'''

# ---------------------------------------------------------------- 2. 两张曲线合成一张
CONN_OLD = '''    add_image_fit(slide, os.path.join(ASSETS, "chart-conn-limit.png"), 0.5, 1.18, 12.4, 4.35)
    card_text(slide, 0.6, 5.68, 6.0, 1.42, "读这张图要看什么", [
        "· 左侧蓝线在 60 处变成水平——上限是硬封顶，不是「大概拦一拦」",
        "· 右侧红线严格 1:1：超出多少就拒多少，一条不漏也不多拒",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11.5)
    card_text(slide, 6.85, 5.68, 6.0, 1.42, "数据来源", [
        "· 8 个测试点各有实测记录（20/40/60/80/100/150/200/300）",
        "· 复现：tools/loadtest.cpp flood <条数>，配置见 linux/README.md",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11.5)
    return slide'''

CONN_NEW = '''    # 两张曲线讲的是同一件事（"规则到底有没有生效"），并排放一起更好对比，
    # 也省掉一次翻页打断。
    add_image_fit(slide, os.path.join(ASSETS, "chart-conn-limit.png"), 0.35, 1.12, 6.25, 4.05)
    add_image_fit(slide, os.path.join(ASSETS, "chart-timeout.png"), 6.9, 1.12, 6.25, 4.05)
    card_text(slide, 0.6, 5.32, 4.0, 1.85, "连接上限：硬封顶", [
        "· 左侧蓝线在 60 处变成水平——是硬封顶，不是「大概拦一拦」",
        "· 右侧红线严格 1:1：超出多少拒多少，一条不漏也不多拒",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 4.75, 5.32, 4.0, 1.85, "慢速耗尽：按时清零", [
        "· 三条阶跃线精确落在设定值（5/10/20 秒），无延迟漂移",
        "· 无论配置多长最终都清零，攻击者拿不到「一直占着」",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.32, 3.85, 1.85, "数据来源", [
        "· 8 个连接数测试点 + 3 种超时配置，各有实测记录",
        "· 复现：tools/loadtest.cpp",
        "· 关闭前先回一条 ERROR，用户看得懂",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    return slide'''

# ---------------------------------------------------------------- 3. 注册防护 + 规则表
REG_OLD = '''    add_image_fit(slide, os.path.join(ASSETS, "chart-registration.png"), 0.45, 1.15, 12.5, 4.3)
    card_text(slide, 0.6, 5.6, 4.0, 1.5, "缺口是什么", [
        "· 原有防护全部基于 IP：换 IP 即失效",
        "· 注册路径当时零限制、零拦截记录",
        "· 洪水期间正常用户消息 105 ms → 超时",
    ], accent=RED, fill=RED_L, title_size=13, body_size=11)
    card_text(slide, 4.75, 5.6, 4.0, 1.5, "怎么修的", [
        "· 新增两条规则：registerinterval / maxaccounts",
        "· IP 冷却抬成本，总量上限兜底（换 IP 也绕不过）",
        "· 顺带修掉全量重写账号文件与重复算名单",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.6, 3.85, 1.5, "不误伤验证", [
        "· 首次 IP 新用户 2 ms 注册成功",
        "· 已注册用户登录发言不受影响",
        "· 44 项单测把放行/拒绝两个方向都钉死",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    return slide'''

REG_NEW = '''    add_image_fit(slide, os.path.join(ASSETS, "chart-registration.png"), 0.45, 1.08, 12.5, 3.35)
    card_text(slide, 0.6, 4.55, 4.0, 1.25, "缺口是什么", [
        "· 原有防护全部基于 IP：换 IP 即失效",
        "· 注册路径当时零限制、零拦截记录",
        "· 洪水期间正常用户消息 105 ms → 超时",
    ], accent=RED, fill=RED_L, title_size=12.5, body_size=10.5)
    card_text(slide, 4.75, 4.55, 4.0, 1.25, "怎么修的", [
        "· 新增两条规则：registerinterval / maxaccounts",
        "· IP 冷却抬成本，总量上限兜底（换 IP 也绕不过）",
        "· 顺带修掉全量重写账号文件与重复算名单",
    ], accent=BLUE, fill=BLUE_L, title_size=12.5, body_size=10.5)
    card_text(slide, 8.9, 4.55, 3.85, 1.25, "不误伤验证", [
        "· 首次 IP 新用户 2 ms 注册成功",
        "· 已注册用户登录发言不受影响",
        "· 44 项单测把放行/拒绝两个方向都钉死",
    ], accent=GREEN, fill=GREEN_L, title_size=12.5, body_size=10.5)
    # 规则表并到这一页：它是"修完之后到底有哪些旋钮"，紧接着修复讲最顺；
    # 也顺手把默认值说清（默认全是 0 = 不限，要上公网再按需开）
    rule_rows = [
        ["规则", "作用", "默认"],
        ["maxconns / maxconnsperip", "连接总数 / 同一 IP 上限", "0"],
        ["loginfails", "同 IP 每 5 分钟登录失败次数", "0"],
        ["handshaketimeout", "连上后多久必须登录", "30 秒"],
        ["registerinterval / maxaccounts", "注册冷却 / 账号总量上限（本次新增）", "0"],
        ["uploadrate / downloadrate", "单客户端上传 / 下载限速", "0"],
        ["maxtextlen / maxtextlines / chatinterval", "文本长度 / 行数 / 发言间隔", "0"],
        ["documentsize / maxservertemp", "单文件 / 暂存总量上限", "64MB / 1024MB"],
    ]
    add_table(slide, 0.62, 5.92, 12.1, 1.75, rule_rows, col_widths=[4.5, 6.1, 1.5],
              font_size=9.5)
    return slide'''

# ---------------------------------------------------------------- 4. 时间线 + 转折点
TL_OLD = '''    rows = [
        ["阶段", "做了什么", "关键产物"],
        ["第 1 天  起步", "局域网聊天室：C++ 客户端 + 服务端，先跑通再说", "协议层、自定义气泡界面"],
        ["第 1 天  第二个端", "安卓客户端（Kotlin + Compose）", "三端格局成型"],'''

TL_NEW = '''    rows = [
        ["阶段", "做了什么", "关键产物"],
        ["第 1 天  起步与第二个端", "C++ 客户端 + 服务端先跑通；随即加安卓客户端（Kotlin + Compose）",
         "协议层、气泡界面、三端格局成型"],'''

TL_NOTE_OLD = '''    note(slide, "时间按提交日期归并，不是精确工时；阶段划分依据是提交信息里能看出的功能边界")
    return slide'''

TL_NOTE_NEW = '''    # 表格下方并进"几个转折点"：这些决定改变了后面的走向，
    # 单独一页会显得空，贴在时间线下面正好说明"为什么那样排期"。
    card_text(slide, 0.62, 5.55, 5.95, 1.75, "几个改变了走向的决定", [
        "· 加密放在第 3 天而不是第 1 天：先跑通聊天、摸熟协议再套加密，",
        "   否则每次调协议都要先怀疑是不是加密错了",
        "· 先立「附件种类字段」再加功能：表情/贴纸/语音都走这一条路径",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 6.78, 5.55, 5.95, 1.75, "吃过亏的地方", [
        "· 界面被退回重做两次（「太丑」「太挤」）→ 先离屏渲染三套方案再写",
        "· 新端放最后且先做密码学：真正的风险是「各端算出来的字节不一样」",
        "· 时间按提交日期归并，不是精确工时",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide'''

# ---------------------------------------------------------------- 5. AI 一页装两页
AI_OLD = '''    add_table(slide, 0.62, 1.25, 12.1, 4.9, rows, col_widths=[2.5, 6.3, 3.3], font_size=11)
    note(slide, "这一页的价值在于：知道 AI 会怎么错，才知道该在哪些地方加检查")
    return slide'''

AI_NEW = '''    add_table(slide, 0.62, 1.22, 12.1, 3.45, rows, col_widths=[2.5, 6.3, 3.3], font_size=10)
    # "AI 做了什么 / 人做了什么"并到这一页：挨着翻车案例看才有意义 ——
    # 分工说清了，才知道上面这些错为什么需要人来兜。
    card_text(slide, 0.62, 4.85, 5.95, 2.45, "分工：AI 做了什么 / 人做了什么", [
        "· AI 写全部代码的第一版（C++ / Kotlin / Python 一样）",
        "· AI 跑构建、跑测试、读日志、定位失败；操作 Kali 做跨平台验证",
        "· 人定优先级、在真机试并给界面反馈、定项目约定、管仓库与发布",
        "· 人做的是「判断」：什么该先做、什么算做完、哪些结论不能写",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 6.78, 4.85, 5.95, 2.45, "AI 最有效的三个用法", [
        "① 写可被测试的纯逻辑：解析/判定/格式化抽成纯函数，一改就能验",
        "② 交叉核对而不是相信「应该对」：写探针把中间量打出来逐字节 diff",
        "③ 顺着日志往下挖：临时日志 + strace 看实际发出的字节",
        "④ 知道它会怎么错，才知道该在哪些地方加检查（就是左边那五条）",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    return slide'''

# ---------------------------------------------------------------- 6. 架构页补一句协议选择
ARCH_OLD = '''    note(slide, "Windows / Linux / macOS 三个客户端与服务端共用同一份 C++ 协议层；安卓端是 Kotlin 重写，靠同一套测试向量保证行为一致")'''
ARCH_NEW = '''    note(slide, "Windows / Linux / macOS 三个客户端与服务端共用同一份 C++ 协议层；安卓端是 Kotlin 重写，靠同一套测试向量保证行为一致。"
                "协议刻意选「行式文本」：能用 telnet 手工测、加字段只追加不重排——"
                "字段按位置解析，附件种类放在第 7 格，插到中间老客户端就会把标记当字节数")'''

# ---------------------------------------------------------------- 7. 修正行数/检查数
NUMBER_EDITS = [
    ("\"Linux 端 10 个测试程序\", \"785 项检查，全部通过\"",
     "\"Linux 端 11 个测试程序\", \"821 项检查，全部通过\"（含终端界面 pty 测试）"),
]

EDITS = [
    (SEC_OLD, SEC_NEW, "安全设计并入压测实测"),
    (CONN_OLD, CONN_NEW, "两张曲线合成一张"),
    (REG_OLD, REG_NEW, "注册防护并入可调规则"),
    (TL_OLD, TL_NEW, "时间线合并起步两个阶段"),
    (TL_NOTE_OLD, TL_NOTE_NEW, "时间线并入转折点"),
    (AI_OLD, AI_NEW, "AI 一页装两页"),
    (ARCH_OLD, ARCH_NEW, "架构页补协议选择理由"),
]


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    changed = 0
    for old, new, label in EDITS:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("  ✅", label)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("共 %d / %d 处" % (changed, len(EDITS)))


if __name__ == "__main__":
    main()
