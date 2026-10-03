# 画压力测试的对比曲线：连接数 vs 存活数、超出量 vs 被拒数、时间 vs 断开数。
#
# 数据全部来自 Kali 虚拟机上的实测（见 linux/README.md 的复现命令），不是示意。
from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
]


def load_font(size, bold=False):
    order = ([FONT_CANDIDATES[1]] + FONT_CANDIDATES) if bold else FONT_CANDIDATES
    for path in order:
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


BG = (255, 255, 255)
NAVY = (22, 40, 74)
BLUE = (31, 111, 235)
BLUE_L = (232, 241, 254)
GREEN = (22, 138, 87)
GREEN_L = (230, 246, 238)
RED = (176, 58, 43)
RED_L = (253, 236, 234)
AMBER = (176, 108, 12)
AMBER_L = (252, 245, 230)
GRAY = (120, 130, 145)
GRID = (225, 230, 238)
TEXT = (32, 41, 56)

# 实测数据：目标连接数 -> (存活, 被拒)
CONN_DATA = [
    (20, 20, 0),
    (40, 40, 0),
    (60, 60, 0),
    (80, 60, 20),
    (100, 60, 40),
    (150, 60, 90),
    (200, 60, 140),
    (300, 60, 240),
]

# 实测数据：handshaketimeout -> 被清掉的连接数（每次 30 条）
TIMEOUT_DATA = [(5, 30), (10, 30), (20, 30)]


def setup_axes(d, box, x_label, y_label, f_label, x_max, y_max, y_ticks, x_ticks,
               x_tick_labels=None):
    x0, y0, x1, y1 = box
    # 网格
    for i in range(len(y_ticks)):
        value = y_ticks[i]
        y = y1 - (y1 - y0) * value / y_max
        d.line((x0, y, x1, y), fill=GRID, width=2)
        d.text((x0 - 14, y), str(value), font=f_label, fill=GRAY, anchor="rm")
    # 轴
    d.line((x0, y0, x0, y1), fill=GRAY, width=2)
    d.line((x0, y1, x1, y1), fill=GRAY, width=2)
    # x 刻度：**按数值定位**，不能按索引均分——否则 120 和 180 会被画成等距，
    # 曲线看起来很平，是误导（第一版就犯了这个错）
    for index, tick in enumerate(x_ticks):
        x = x0 + (x1 - x0) * tick / x_max
        label = str(tick) if x_tick_labels is None else x_tick_labels[index]
        d.text((x, y1 + 16), label, font=f_label, fill=GRAY, anchor="ma")
    d.text(((x0 + x1) / 2, y1 + 52), x_label, font=f_label, fill=TEXT, anchor="ma")
    d.text((x0 - 62, (y0 + y1) / 2), y_label, font=f_label, fill=TEXT, anchor="mm")


def to_xy(box, x, y, x_max, y_max):
    x0, y0, x1, y1 = box
    px = x0 + (x1 - x0) * (x / x_max)
    py = y1 - (y1 - y0) * (y / y_max)
    return px, py


def draw_line(d, box, points, x_max, y_max, color, width=5, dots=9):
    pixels = [to_xy(box, x, y, x_max, y_max) for x, y in points]
    if len(pixels) > 1:
        d.line(pixels, fill=color, width=width, joint="curve")
    for px, py in pixels:
        d.ellipse((px - dots / 2, py - dots / 2, px + dots / 2, py + dots / 2),
                  fill=(255, 255, 255), outline=color, width=4)
    return pixels


def make_conn_chart(path):
    """左：目标连接数 vs 存活数（在 60 处硬性封顶）；右：超出量 vs 被拒数（严格 1:1）。"""
    W, H = 2000, 800
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(34, bold=True)
    f_label = load_font(23)
    f_note = load_font(21)

    # ---- 左图 ----
    d.text((100, 40), "目标连接数 vs 实际存活连接数", font=f_title, fill=NAVY)
    box = (200, 120, 900, 620)
    setup_axes(d, box, "目标连接数（条）", "存活连接数", f_label, 300, 300,
               [0, 100, 200, 300], [0, 60, 100, 150, 200, 300])
    d.line((to_xy(box, 60, 0, 300, 300), to_xy(box, 60, 300, 300, 300)),
           fill=AMBER, width=4)
    d.text(to_xy(box, 66, 270, 300, 300), "上限 maxconnsperip = 60", font=f_label, fill=AMBER)
    draw_line(d, box, [(x, ok) for x, ok, _ in CONN_DATA], 300, 300, BLUE)
    # 理想线（如果没有任何限制）
    d.line([to_xy(box, 0, 0, 300, 300), to_xy(box, 300, 300, 300, 300)],
           fill=(200, 206, 216), width=3)
    d.text(to_xy(box, 175, 205, 300, 300), "无限连接时的理论线", font=f_note, fill=GRAY)

    # ---- 右图 ----
    d.text((1080, 40), "超出上限的连接数 vs 被拒绝数", font=f_title, fill=NAVY)
    box2 = (1200, 120, 1900, 620)
    setup_axes(d, box2, "超出的连接数（条）", "被拒绝数", f_label, 240, 240,
               [0, 80, 160, 240], [0, 20, 60, 120, 180, 240])
    draw_line(d, box2, [(attempts - 60, refused) for attempts, _, refused in CONN_DATA],
              240, 240, RED)
    d.line([to_xy(box2, 0, 0, 240, 240), to_xy(box2, 240, 240, 240, 240)],
           fill=(200, 206, 216), width=3)
    d.text(to_xy(box2, 90, 150, 240, 240), "严格的 1:1（一条不漏）", font=f_note, fill=GRAY)

    d.text((100, 700),
           "实测：上限封顶精确——无论尝试 80 还是 300 条，存活恒为 60，被拒数恰好等于超出量",
           font=f_label, fill=TEXT)
    img.save(path)
    return path


def make_timeout_chart(path):
    """时间轴：未登录连接被服务端清掉的过程（三种 handshaketimeout 配置）。"""
    W, H = 2000, 800
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(34, bold=True)
    f_label = load_font(23)
    f_note = load_font(21)
    f_tiny = load_font(20)

    d.text((100, 40), "时间 vs 仍被占住的连接数（慢速耗尽攻击）", font=f_title, fill=NAVY)
    box = (200, 130, 1850, 600)
    x_max = 30
    y_max = 30
    setup_axes(d, box, "经过时间（秒）", "仍被占住的连接数", f_label, x_max, y_max,
               [0, 10, 20, 30], [0, 5, 10, 15, 20, 25, 30])

    colors = [(5, GREEN), (10, BLUE), (20, AMBER)]
    for timeout, color in colors:
        steps = []
        for second in range(0, 31):
            alive = 30 if second < timeout else 0
            steps.append((second, alive))
        # 画成阶跃线，更贴近"到点一次性清掉"的真实行为
        pixels = []
        for index, (second, alive) in enumerate(steps):
            pixels.append(to_xy(box, second, alive, x_max, y_max))
            if index + 1 < len(steps) and steps[index + 1][1] != alive:
                pixels.append(to_xy(box, steps[index + 1][0], alive, x_max, y_max))
        d.line(pixels, fill=color, width=5)
        px, py = to_xy(box, timeout, 0, x_max, y_max)
        d.ellipse((px - 7, py - 7, px + 7, py + 7), fill=color)
        d.text((px + 14, py - 40), "handshaketimeout = " + str(timeout) + " 秒",
               font=f_note, fill=color)

    d.text((100, 650),
           "实测：30 条未登录连接在三组配置下全部被按时清掉（30/30），一条都没漏",
           font=f_label, fill=TEXT)
    d.text((100, 700),
           "服务端在关闭前还会先回一条 ERROR「太久没有登录，连接已关闭」，并在日志里记录 handshake timeout",
           font=f_tiny, fill=GRAY)
    img.save(path)
    return path




# 注册洪水防护的对比数据（实测，来自 tools/regflood.cpp）
# (配置名, 单线程尝试数, 单线程成功数, 并发尝试数, 并发成功数, 服务端拦截数, 最终账号数)
REG_DATA = [
    ("无防护", 30, 30, 80, 80, 0, 111),
    ("registerinterval 60", 30, 4, 80, 4, 102, 9),
    ("maxaccounts 20", 30, 20, 80, 0, 120, 21),
    ("两条一起开", 30, 4, 80, 1, 105, 6),
]


def make_registration_chart(path):
    """注册洪水：防护前后账号增长对比（左：成功注册数；右：被拦截数）。"""
    W, H = 2000, 860
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(34, bold=True)
    f_label = load_font(23)
    f_note = load_font(21)
    f_small = load_font(20)

    # ---- 左图：成功注册数（尝试 110 个 = 30 单线程 + 80 并发）----
    d.text((100, 40), "尝试注册 110 个：实际成功了多少", font=f_title, fill=NAVY)
    box = (200, 130, 1120, 640)
    attempts = 110
    setup_axes(d, box, "配置", "成功注册数", f_label, attempts, 140,
               [0, 35, 70, 105, 140], [0, 1, 2, 3], [""] * 4)

    bar_w = 130
    for index, (label, single_try, single_ok, par_try, par_ok, blocked, accounts) in enumerate(REG_DATA):
        total_ok = single_ok + par_ok
        x = box[0] + 90 + index * 215
        y_top = to_xy(box, 0, total_ok, attempts, 140)[1]
        color = RED if index == 0 else GREEN
        d.rectangle((x, y_top, x + bar_w, box[3]), fill=color)
        d.text((x + bar_w / 2, y_top - 30), str(total_ok), font=f_title, fill=color, anchor="ma")
        # x 轴标签分两行放
        d.text((x + bar_w / 2, box[3] + 16), label.split(" ")[0], font=f_small, fill=TEXT,
               anchor="ma")
        if len(label.split(" ")) > 1:
            d.text((x + bar_w / 2, box[3] + 42), " ".join(label.split(" ")[1:]), font=f_small,
                   fill=GRAY, anchor="ma")
    # 参考线：无防护时的成功数
    # 参考线画在 110 上方并留出间距：柱子正好到 110，压在一起会看不清
    ref = to_xy(box, 0, 122, attempts, 140)[1]
    d.line((box[0], ref, box[2], ref), fill=(200, 206, 216), width=3)
    d.text((box[0] + 12, ref - 30), "参考：110 个全部成功（无防护时）", font=f_note, fill=GRAY)

    # ---- 右图：被服务端拦截的次数 ----
    d.text((1220, 40), "服务端的拦截记录（条）", font=f_title, fill=NAVY)
    box2 = (1320, 130, 1900, 640)
    setup_axes(d, box2, "配置", "拦截次数", f_label, attempts, 150,
               [0, 50, 100, 150], [0, 1, 2, 3], [""] * 4)
    for index, (label, single_try, single_ok, par_try, par_ok, blocked, accounts) in enumerate(REG_DATA):
        x = box2[0] + 30 + index * 138
        y_top = to_xy(box2, 0, blocked, attempts, 150)[1]
        color = GRAY if blocked == 0 else BLUE
        d.rectangle((x, y_top, x + 82, box2[3]), fill=color)
        d.text((x + 41, y_top - 28), str(blocked), font=f_label, fill=color, anchor="ma")
        d.text((x + 41, box2[3] + 16), label.split(" ")[0], font=f_small, fill=TEXT, anchor="ma")
        if len(label.split(" ")) > 1:
            d.text((x + 41, box2[3] + 42), " ".join(label.split(" ")[1:]), font=f_small,
                   fill=GRAY, anchor="ma")

    d.text((100, 740),
           "实测：加上两条规则后，同样 110 次尝试只剩 4~21 个成功；账号总量从 111 压到 6~21",
           font=f_label, fill=TEXT)
    d.text((100, 785),
           "不误伤验证：首次 IP 的新用户 2 ms 成功注册；已注册用户登录与发言完全不受影响",
           font=f_note, fill=GREEN)
    img.save(path)
    return path


# ---------------------------------------------------------------------------
# macOS：验证边界图
# ---------------------------------------------------------------------------
# 实测事实：zig 0.16 **不带任何 framework 头**（Foundation/AppKit/CoreFoundation
# 全都没有），所以 Cocoa 界面在这台开发机上编不了。这张图的作用就是把
# "验过什么 / 没验过什么"一次说清 —— 这种图不画出来，读者很容易默认"都测过了"。
MAC_VERIFY = [
    ("服务端 + 协议 + 加密", "真交叉编译成 Mach-O", 2),
    ("客户端核心 18 个文件", "两个 macOS 架构各编一遍", 2),
    ("Cocoa 界面 main.mm", "只做了文本层静态检查", 1),
    ("OpenSSL 后端", "缺 macOS 头，未编译", 0),
    ("运行行为（收发/文件/语音）", "没有 Mac，未运行", 0),
]
MAC_LEVEL_TEXT = {2: "已编译验证", 1: "部分检查", 0: "未验证"}


def make_macos_verify_chart(path):
    W, H = 2000, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(34, bold=True)
    f_row = load_font(24)
    f_note = load_font(21)
    f_badge = load_font(20, bold=True)

    d.text((90, 44), "macOS 端验证到了哪一步（不夸大）", font=f_title, fill=NAVY)
    d.text((90, 96), "同一张图里的每一项都在仓库里有对应证据：脚本、错误信息或缺失的头文件",
           font=f_note, fill=GRAY)

    colors = {2: GREEN, 1: AMBER, 0: RED}
    fills = {2: GREEN_L, 1: AMBER_L, 0: RED_L}
    y = 170
    for label, detail, level in MAC_VERIFY:
        d.rounded_rectangle((90, y, 1910, y + 118), radius=16, fill=fills[level],
                            outline=colors[level], width=3)
        d.text((130, y + 26), label, font=f_row, fill=TEXT)
        d.text((130, y + 68), detail, font=f_note, fill=GRAY)
        # 右侧徽标
        badge = MAC_LEVEL_TEXT[level]
        d.rounded_rectangle((1560, y + 30, 1870, y + 88), radius=14,
                            fill=colors[level])
        d.text((1715, y + 59), badge, font=f_badge, fill=(255, 255, 255), anchor="mm")
        y += 138

    d.text((90, 860),
           "结论：能编的都编过了、能查的都查了，但 macOS 版本第一次在真机上编译大概率还需要修",
           font=f_note, fill=TEXT)
    img.save(path)
    return path
if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "build", "ppt-assets")
    os.makedirs(out, exist_ok=True)
    print(make_conn_chart(os.path.join(out, "chart-conn-limit.png")))
    print(make_timeout_chart(os.path.join(out, "chart-timeout.png")))
    print(make_registration_chart(os.path.join(out, "chart-registration.png")))
    print(make_macos_verify_chart(os.path.join(out, "chart-macos-verify.png")))
