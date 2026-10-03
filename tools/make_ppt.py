# 生成 dchat 项目介绍 PPT。
#
# 设计约定：
#   - 16:9，深蓝标题栏 + 左侧色条，正文用可编辑文本框和表格（不用图片代替文字）
#   - 中文字体统一用「微软雅黑」，避免 LibreOffice/PowerPoint 渲染时回退成方框
#   - 所有数字都来自仓库实测（提交数、行数、测试项），不写估计值
import os

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.util import Inches, Pt, Emu

# 素材和输出都相对**脚本所在仓库**定位，而不是相对当前工作目录——
# 否则从别的目录调用就会找不到图（踩过一次）
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(REPO, "build", "ppt-assets")
OUT = os.environ.get("DCHAT_PPT_OUT", os.path.join(REPO, "dchat-项目介绍.pptx"))

FONT = "微软雅黑"
NAVY = RGBColor(0x16, 0x28, 0x4A)
BLUE = RGBColor(0x1F, 0x6F, 0xEB)
BLUE_L = RGBColor(0xE8, 0xF1, 0xFE)
GREEN = RGBColor(0x16, 0x8A, 0x57)
GREEN_L = RGBColor(0xE6, 0xF6, 0xEE)
AMBER = RGBColor(0xB0, 0x6C, 0x0C)
AMBER_L = RGBColor(0xFD, 0xF4, 0xE2)
RED = RGBColor(0xB0, 0x3A, 0x2B)
RED_L = RGBColor(0xFD, 0xEC, 0xEA)
GRAY = RGBColor(0x6C, 0x75, 0x84)
GRAY_L = RGBColor(0xF3, 0xF5, 0xF8)
TEXT = RGBColor(0x20, 0x29, 0x38)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)

SW, SH = 13.333, 7.5


def new_deck():
    prs = Presentation()
    prs.slide_width = Inches(SW)
    prs.slide_height = Inches(SH)
    return prs


def add_slide(prs, title, kicker=None):
    """空白版式 + 标题栏。返回 slide。"""
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    # 顶部标题栏
    bar = slide.shapes.add_shape(1, Inches(0), Inches(0), Inches(SW), Inches(1.02))
    bar.fill.solid()
    bar.fill.fore_color.rgb = NAVY
    bar.line.fill.background()
    bar.shadow.inherit = False
    tf = bar.text_frame
    tf.margin_left = Inches(0.45)
    tf.margin_top = Inches(0.1)
    tf.margin_bottom = Inches(0.05)
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = title
    r.font.size = Pt(28)
    r.font.bold = True
    r.font.color.rgb = WHITE
    r.font.name = FONT
    if kicker:
        p2 = tf.add_paragraph()
        r2 = p2.add_run()
        r2.text = kicker
        r2.font.size = Pt(13)
        r2.font.color.rgb = RGBColor(0xA9, 0xC2, 0xE8)
        r2.font.name = FONT
    return slide


def textbox(slide, left, top, width, height, lines, size=16, color=TEXT, spacing=8,
            bold_first=False, align=PP_ALIGN.LEFT, line_spacing=1.15):
    """
    lines 是列表；每一项可以是：
      字符串                      -> 普通段落
      (文本, 级别)                -> 级别 1 缩进、级别 2 带圆点前缀
      (文本, 级别, 颜色)          -> 指定颜色
    """
    box = slide.shapes.add_textbox(Inches(left), Inches(top), Inches(width), Inches(height))
    tf = box.text_frame
    tf.word_wrap = True
    first = True
    for item in lines:
        text, level, col = item, 0, color
        if isinstance(item, tuple):
            text = item[0]
            level = item[1] if len(item) > 1 else 0
            col = item[2] if len(item) > 2 else color
        p = tf.paragraphs[0] if first else tf.add_paragraph()
        first = False
        p.alignment = align
        p.line_spacing = line_spacing
        p.space_after = Pt(spacing if level == 0 else max(2, spacing - 4))
        if level >= 1:
            p.level = min(level, 4)
        run = p.add_run()
        run.text = ("· " + text) if level == 1 else ("– " + text if level == 2 else text)
        run.font.size = Pt(size if level == 0 else size - 2)
        run.font.color.rgb = col
        run.font.name = FONT
        if level == 0 and bold_first and first is False and text == lines[0] if isinstance(lines[0], str) else False:
            run.font.bold = True
    return box


def bullets(slide, left, top, width, height, items, size=16, spacing=9):
    return textbox(slide, left, top, width, height, items, size=size, spacing=spacing)


def card(slide, left, top, width, height, fill, line=None):
    shp = slide.shapes.add_shape(1, Inches(left), Inches(top), Inches(width), Inches(height))
    shp.fill.solid()
    shp.fill.fore_color.rgb = fill
    if line:
        shp.line.color.rgb = line
        shp.line.width = Pt(1.25)
    else:
        shp.line.fill.background()
    shp.shadow.inherit = False
    shp.text_frame.text = ""
    return shp


def card_text(slide, left, top, width, height, title, body, accent=BLUE, fill=BLUE_L,
              title_size=15, body_size=12):
    card(slide, left, top, width, height, fill, accent)
    textbox(slide, left + 0.16, top + 0.1, width - 0.32, 0.4, [title], size=title_size,
            color=accent, spacing=2)
    textbox(slide, left + 0.16, top + 0.5, width - 0.32, height - 0.6, body,
            size=body_size, color=TEXT, spacing=3)


def add_image_fit(slide, path, left, top, max_w, max_h):
    """按比例缩放图片，完整放进给定框内并居中。"""
    from PIL import Image
    with Image.open(path) as im:
        iw, ih = im.size
    scale = min(max_w / iw, max_h / ih)
    w, h = iw * scale, ih * scale
    x = left + (max_w - w) / 2
    y = top + (max_h - h) / 2
    return slide.shapes.add_picture(path, Inches(x), Inches(y), Inches(w), Inches(h))


def add_table(slide, left, top, width, height, rows, col_widths=None, font_size=12,
              header_fill=NAVY):
    n_rows = len(rows)
    n_cols = len(rows[0])
    shape = slide.shapes.add_table(n_rows, n_cols, Inches(left), Inches(top),
                                   Inches(width), Inches(height))
    table = shape.table
    if col_widths:
        total = sum(col_widths)
        for i, cw in enumerate(col_widths):
            table.columns[i].width = Emu(int(Inches(width) * cw / total))
    for r, row in enumerate(rows):
        for c, value in enumerate(row):
            cell = table.cell(r, c)
            cell.text = str(value)
            cell.margin_left = Inches(0.08)
            cell.margin_right = Inches(0.06)
            cell.margin_top = Inches(0.02)
            cell.margin_bottom = Inches(0.02)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            para = cell.text_frame.paragraphs[0]
            para.line_spacing = 1.0
            for run in para.runs:
                run.font.size = Pt(font_size if r else font_size)
                run.font.name = FONT
                run.font.bold = (r == 0)
                run.font.color.rgb = WHITE if r == 0 else TEXT
            cell.fill.solid()
            cell.fill.fore_color.rgb = header_fill if r == 0 else (
                GRAY_L if r % 2 == 0 else WHITE)
    return shape


def note(slide, text, top=6.62, color=GRAY, size=12, left=0.62, width=12.1):
    textbox(slide, left, top, width, 0.6, [text], size=size, color=color)


# ===========================================================================
# 幻灯片内容
# ===========================================================================

def slide_cover(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    bg = slide.shapes.add_shape(1, Inches(0), Inches(0), Inches(SW), Inches(SH))
    bg.fill.solid()
    bg.fill.fore_color.rgb = NAVY
    bg.line.fill.background()
    bg.shadow.inherit = False

    textbox(slide, 1.1, 1.85, 11, 1.5, ["dchat"], size=66, color=WHITE, spacing=0)
    textbox(slide, 1.15, 3.05, 11, 0.9,
            ["一个自研的局域网 / 公网聊天室"], size=26,
            color=RGBColor(0xA9, 0xC2, 0xE8), spacing=0)
    textbox(slide, 1.15, 3.75, 11, 0.7,
            ["Windows · 安卓 · Linux 三端互通 · 端到端加密 · 逐行手写的协议"], size=16,
            color=RGBColor(0x8F, 0xAD, 0xD8), spacing=0)

    line = slide.shapes.add_shape(1, Inches(1.15), Inches(4.6), Inches(3.2), Pt(3))
    line.fill.solid()
    line.fill.fore_color.rgb = BLUE
    line.line.fill.background()
    line.shadow.inherit = False

    textbox(slide, 1.15, 5.0, 11, 1.6, [
        "项目介绍 · 功能与架构 · 开发过程梗概",
        "以及 AI 在整个过程中的使用情况与真实复盘",
    ], size=17, color=RGBColor(0xC7, 0xD7, 0xEF), spacing=6)

    textbox(slide, 1.15, 6.42, 11, 0.6,
            ["github.com/dongfang20101113/DChat   ·   53 次提交 / 9 天   ·   三端共 2.7 万行自有代码"],
            size=12, color=RGBColor(0x7E, 0x9B, 0xC6), spacing=2)
    textbox(slide, 1.15, 6.78, 11, 0.5,
            ["990+ 项测试全过   ·   端到端加密   ·   压测与攻击实测防护生效   ·   内存稳定 10 MB"],
            size=12, color=RGBColor(0x7E, 0x9B, 0xC6), spacing=0)
    return slide


def slide_overview(prs):
    slide = add_slide(prs, "这个项目是什么", "一句话：不依赖任何聊天框架，从协议到加密自己搭的三端聊天系统")
    bullets(slide, 0.62, 1.35, 6.1, 5.0, [
        ("定位", 0, NAVY),
        ("局域网里几个人就能开聊；配上端口映射也能跑公网", 1),
        ("服务端一个人跑，客户端随手拷走就能用", 1),
        ("三端互通：Windows、安卓、Linux 互发消息 / 文件 / 语音", 1),
        ("", 0),
        ("技术上刻意选择的难点", 0, NAVY),
        ("协议自己定：行式 UTF-8，每行一条命令", 1),
        ("加密自己做：ECDH 握手 + AES-256-GCM，不发明原语", 1),
        ("三端字节级一致：同一条消息在三端解出来必须一模一样", 1),
        ("界面自己画：桌面端 GDI+ 自绘气泡，不用现成 UI 框架", 1),
    ], size=15)

    card_text(slide, 7.0, 1.35, 5.7, 2.35, "它不是玩具的地方", [
        "· 有账号体系：PBKDF2 加盐哈希，不存明文",
        "· 有公网加固：连接数上限、防爆破、握手超时、限速",
        "· 有 TOFU：记住服务器指纹，指纹变了会明确警告",
        "· 有测试：三端合计 990+ 项检查全部通过",
    ], accent=GREEN, fill=GREEN_L, body_size=13)

    card_text(slide, 7.0, 3.9, 5.7, 2.45, "代码规模（实测，不含 build 产物）", [
        "· C++ 协议层 + 服务端 + Windows 客户端：13,852 行",
        "· 安卓 Kotlin：10,853 行",
        "· Linux 客户端：2,602 行",
        "· 测试代码：3,738 行（16 个测试程序）",
    ], accent=AMBER, fill=AMBER_L, body_size=13)
    return slide


def slide_arch(prs):
    slide = add_slide(prs, "整体架构", "三个客户端共用一份协议层源码，服务端两头都能跑")
    add_image_fit(slide, os.path.join(ASSETS, "diagram-arch.png"), 0.45, 1.2, 12.45, 5.3)
    note(slide, "Linux 端和服务端共用同一份 C++ 协议层；安卓端是 Kotlin 重写，靠同一套测试向量保证行为一致")
    return slide


def slide_features(prs):
    slide = add_slide(prs, "功能一览", "三端都有的功能，以及各自特有的")
    rows = [
        ["功能", "Windows", "安卓", "Linux"],
        ["连接 / 注册 / 登录 / 改密码", "有", "有", "有"],
        ["群聊 + 多行消息", "有", "有", "有"],
        ["彩色文字（&a 快捷色 / #rrggbb）", "有 + 取色盘", "有 + 取色盘", "有"],
        ["文件上传下载（含进度、重名处理）", "有", "有", "有"],
        ["语音消息（录制 / 播放 / 自动下载）", "有", "有", "有"],
        ["表情与贴纸", "有", "有", "贴纸按附件收发"],
        ["指令 + Tab 补全 / 历史", "有", "有", "有"],
        ["图片与视频缩略图预览", "有（系统缩略图）", "有", "无"],
        ["深色模式", "跟随系统", "跟随系统", "跟随终端"],
        ["界面形态", "Win32 自绘", "Compose", "终端 TUI"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.8, rows, col_widths=[5.2, 2.3, 2.3, 2.3],
              font_size=12)
    note(slide, "Linux 端是终端界面，因此没有图片预览；贴纸按普通附件收发，不做内联渲染")
    return slide


def slide_screens(prs):
    slide = add_slide(prs, "界面实拍", "左边是桌面端的界面效果图（离屏渲染，复用真界面的配色与字号）")
    add_image_fit(slide, os.path.join(ASSETS, "ui-proposal-a.png"), 0.5, 1.25, 7.3, 4.5)
    add_image_fit(slide, os.path.join(REPO, "android", "app", "build", "screenshots",
                                      "chat-phone-393x851-dark.png"),
                  8.0, 1.25, 4.85, 4.5)
    note(slide, "安卓截图由 Robolectric 在真实布局上渲染得出，不是手绘示意图")
    return slide


def slide_protocol(prs):
    slide = add_slide(prs, "协议设计：为什么选行式文本", "简单到能用肉眼调试，是这个项目最重要的一个决定")
    bullets(slide, 0.62, 1.3, 6.2, 5.0, [
        ("每行一条命令，UTF-8 编码，单行 4096 字节上限", 0, NAVY),
        ("例：SAY 小明 你好、MSG 你好、FILE_SEND F1 名字 大小 voice", 1),
        ("好处一：能用 telnet / nc 直接手工测，不用抓包解密", 1),
        ("好处二：加字段只要追加在末尾，老客户端读到旧的格数就停", 1),
        ("好处三：三端实现都短，安卓 Kotlin 和 C++ 各写一遍也不累", 1),
        ("", 0),
        ("踩过的坑：字段是「按位置」解析的", 0, RED),
        ("附件的种类放在第 7 格（追加），绝不能插到中间", 1),
        ("一旦插错位置，老客户端会把标记当成字节数去解析", 1),
        ("所以连注释里都写死了：只追加、不重排", 1),
    ], size=14)
    card_text(slide, 7.05, 1.3, 5.65, 2.5, "附件种类字段（三端一致）", [
        "0 / file    → 普通文件",
        "1 / sticker → 贴纸",
        "voice       → 语音",
        "",
        "判定只看这一格，不看扩展名——安卓录的是 .m4a、",
        "桌面端是 .wav，扩展名根本不可靠。",
    ], accent=BLUE, fill=BLUE_L, body_size=13)
    card_text(slide, 7.05, 4.0, 5.65, 2.35, "彩色文字的语法", [
        "&a        快捷色（十六个，三端颜色表完全相同）",
        "&#ff8800  真彩色",
        "&&        一个字面量 &",
        "",
        "服务器关掉彩色聊天时，色码原样显示而不是被吃掉",
        "——否则用户会以为自己的字被吞了。",
    ], accent=AMBER, fill=AMBER_L, body_size=13)
    return slide


def slide_crypto(prs):
    slide = add_slide(prs, "加密设计", "ECDH P-256 握手 → HKDF-SHA256 派生 → AES-256-GCM 会话加密")
    add_image_fit(slide, os.path.join(ASSETS, "diagram-handshake.png"), 0.5, 1.15, 12.4, 4.5)
    card_text(slide, 0.5, 5.75, 6.05, 1.35, "保护什么", [
        "被动窃听：抓包看不到密码和聊天内容",
        "篡改：GCM 自带认证，改一个字节就解不开",
    ], accent=GREEN, fill=GREEN_L, body_size=12)
    card_text(slide, 6.85, 5.75, 6.05, 1.35, "不保护什么（写进代码注释了）", [
        "主动中间人：裸 ECDH 没有身份认证",
        "所以有 TOFU：第一次记住指纹，之后每次比对，变了就警告",
    ], accent=RED, fill=RED_L, body_size=12)
    return slide


def slide_three_contracts(prs):
    slide = add_slide(prs, "三端一致：三处必须字节级相同的约定",
                      "这三条任何一条错了，都是「不报错、只是永远连不上」的那种 bug")
    bullets(slide, 0.62, 1.35, 12.1, 5.0, [
        ("约定一：ECDH 共享密钥用大端", 0, NAVY),
        ("Windows 的 CNG 返回小端，必须翻转；OpenSSL 本来就是大端，绝不能翻", 1),
        ("约定二：公钥在线路上是裸的 X‖Y（各 32 字节）", 0, NAVY),
        ("不能带 BCRYPT_ECCKEY_BLOB 那种平台专有的头部", 1),
        ("约定三：AES-GCM 的输出是「密文在前、16 字节标签在后」", 0, NAVY),
        ("必须和 Java 的 Cipher.doFinal 一致，否则安卓端解不开", 1),
        ("", 0),
        ("为什么反复强调：错了不会崩、不会报错，只会「握手看起来成功了但消息是乱码」", 0, RED),
    ], size=15)
    return slide


def slide_server(prs):
    slide = add_slide(prs, "服务端", "Windows 和 Linux 跑同一份源码，只换 socket 层和加密后端")
    bullets(slide, 0.62, 1.3, 6.1, 5.0, [
        ("并发模型：每个连接一个线程", 0, NAVY),
        ("聊天是低并发 IO，线程模型足够，也不需要 epoll 的复杂度", 1),
        ("", 0),
        ("账号与安全", 0, NAVY),
        ("PBKDF2-HMAC-SHA256 加盐，落盘的只有哈希和盐", 1),
        ("身份密钥落盘（标量 + 公钥的十六进制），TOFU 才有意义", 1),
        ("公网加固：连接数上限、单 IP 上限、登录失败封禁、握手超时", 1),
        ("可调规则：限速、文本长度与行数、暂存附件保留时长", 1),
        ("", 0),
        ("控制台指令：/ban /kick /op /say /chatrule /ip 等", 0, NAVY),
    ], size=14)
    card_text(slide, 7.0, 1.3, 5.7, 2.3, "跨平台移植只动了两个地方", [
        "socket 层：Winsock → POSIX（封在 socket_util.h 后面）",
        "加密后端：CNG → OpenSSL（封在 crypto_backend.h 后面）",
        "",
        "业务代码一个字没改——移植点越少，越不容易改出新 bug。",
    ], accent=BLUE, fill=BLUE_L, body_size=13)
    card_text(slide, 7.0, 3.85, 5.7, 2.5, "被 socket 层挡住的两个坑", [
        "SIGPIPE：Linux 上往已关闭的连接写，默认直接杀掉整个进程",
        "→ 所有发送都走带 MSG_NOSIGNAL 的封装，绝不裸调 send",
        "",
        "keepalive：Windows 用 WSAIoctl 一次设好，Linux 要三个独立",
        "setsockopt → 数值定成同一组，两端发现死连接的时间才一致",
    ], accent=RED, fill=RED_L, body_size=12)
    return slide


def slide_timeline(prs):
    slide = add_slide(prs, "开发过程梗概", "53 次提交、9 天，按提交信息可以分成八个阶段")
    rows = [
        ["阶段", "做了什么", "关键产物"],
        ["第 1 天  起步", "局域网聊天室：C++ 客户端 + 服务端，先跑通再说", "协议层、自定义气泡界面"],
        ["第 1 天  第二个端", "安卓客户端（Kotlin + Compose）", "三端格局成型"],
        ["第 2 天  公网加固", "限速、文本限制、多行消息、连接数上限、防爆破、握手超时", "可调规则系统"],
        ["第 3 天  加密", "ECDH + HKDF + AES-GCM，从协议层一路接到三端", "TOFU 指纹闭环"],
        ["第 4~5 天  功能", "表情、贴纸、语音（录制/播放/自动下载）", "附件种类字段"],
        ["第 6 天  界面返工", "重新设计桌面端界面：菜单栏、齿轮设置、＋菜单", "离屏渲染效果图"],
        ["第 7 天  彩蛋与修复", "彩色文字代码端到端；修 6 处用户反馈 + 1 处自己引入的回归", "取色盘"],
        ["第 8~9 天  第三个端", "Linux：加密后端抽象、服务端移植、终端客户端、预编译包", "741 项 Linux 检查"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 5.0, rows, col_widths=[2.2, 7.4, 3.2], font_size=11.5)
    note(slide, "时间按提交日期归并，不是精确工时；阶段划分依据是提交信息里能看出的功能边界")
    return slide


def slide_phases(prs):
    slide = add_slide(prs, "开发过程的几个转折点", "哪些决定改变了后面的走向")
    items = [
        ("加密放在第 3 天，而不是第 1 天", GREEN,
         "先把聊天跑通、把协议摸熟，再套加密。反过来做的话，每次调协议都要先怀疑是不是加密错了。"),
        ("先把「种类字段」立起来，再加功能", BLUE,
         "表情、贴纸、语音三个功能都靠附件这一条路径。先定好那一格怎么扩，后面三个功能都是小改动。"),
        ("界面被退回重做两次", AMBER,
         "第一版太朴素、第二版太密集。用户直接说「太丑」「太挤」，于是先用离屏渲染出三套方案，选完再写。"),
        ("Linux 端放最后，并且先做密码学", RED,
         "真正的风险不是界面，是「三端算出来的字节不一样」。所以第一步就是抽象加密后端 + 跑同一套测试向量。"),
    ]
    y = 1.3
    for title, color, body in items:
        card(slide, 0.62, y, 12.1, 1.22, WHITE, color)
        # 标题单独一行、给足宽度：中文没有词边界，靠自动换行会出现
        # 「标题折成两行压住正文」这种排版事故，所以宁可留白
        textbox(slide, 0.85, y + 0.08, 11.6, 0.42, [title], size=15, color=color, spacing=0)
        textbox(slide, 0.85, y + 0.56, 11.6, 0.6, [body], size=12.5, color=TEXT, spacing=0)
        y += 1.34
    return slide


def slide_verification(prs):
    slide = add_slide(prs, "怎么证明它是对的", "这个项目里「验证」和「写代码」花的力气差不多")
    rows = [
        ["验证手段", "规模 / 结果"],
        ["Windows 端 16 个测试程序", "966 项检查，全部通过；构建 0 告警"],
        ["Linux 端 10 个测试程序", "785 项检查，全部通过"],
        ["安卓端单元测试", "328 个测试、19 个测试类，全部通过"],
        ["密码学测试向量", "HKDF 对 RFC 5869、AES-GCM 对 NIST、ECDH 固定向量、与 Kotlin 跨语言比对"],
        ["三端互通实测", "Linux 客户端 ↔ Windows 服务端、Windows 协议层 ↔ Linux 服务端，双向加密握手成功"],
        ["文件传输", "200 KB 随机文件往返，MD5 逐字节一致"],
        ["语音", "虚拟机里真录音 3 秒 → 上传成功；WAV 时长解析在真实录音上 1/2/5 秒全对"],
        ["预编译包", "从 tar 包解出来直接跑，全程无 cmake、不装任何依赖"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.9, rows, col_widths=[4.2, 7.9], font_size=12)
    note(slide, "测试能跑起来，是后面所有「修 bug」动作的前提——没有测试就只能靠肉眼猜")
    return slide



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




def slide_chart_conn(prs):
    slide = add_slide(prs, "攻击对比曲线（一）：连接上限",
                      "目标连接数从 20 加到 300，存活数在 60 处硬性封顶")
    add_image_fit(slide, os.path.join(ASSETS, "chart-conn-limit.png"), 0.5, 1.18, 12.4, 4.35)
    card_text(slide, 0.6, 5.68, 6.0, 1.42, "读这张图要看什么", [
        "· 左侧蓝线在 60 处变成水平——上限是硬封顶，不是「大概拦一拦」",
        "· 右侧红线严格 1:1：超出多少就拒多少，一条不漏也不多拒",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11.5)
    card_text(slide, 6.85, 5.68, 6.0, 1.42, "数据来源", [
        "· 8 个测试点各有实测记录（20/40/60/80/100/150/200/300）",
        "· 复现：tools/loadtest.cpp flood <条数>，配置见 linux/README.md",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11.5)
    return slide


def slide_chart_timeout(prs):
    slide = add_slide(prs, "攻击对比曲线（二）：慢速耗尽的清理过程",
                      "30 条连上不登录的连接，在三种超时配置下全部被按时清掉")
    add_image_fit(slide, os.path.join(ASSETS, "chart-timeout.png"), 0.5, 1.18, 12.4, 4.35)
    card_text(slide, 0.6, 5.68, 6.0, 1.42, "读这张图要看什么", [
        "· 三条阶跃线精确落在设定值上（5 / 10 / 20 秒），没有延迟漂移",
        "· 无论配置多长最终都清零——攻击者拿不到「一直占着」的结果",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11.5)
    card_text(slide, 6.85, 5.68, 6.0, 1.42, "服务端的处理方式", [
        "· 关闭前先回一条 ERROR「太久没有登录，连接已关闭」，用户看得懂",
        "· 每次清理都进日志：实测一轮出现 72 次 handshake timeout",
    ], accent=RED, fill=RED_L, title_size=13, body_size=11.5)
    return slide


def slide_perf_boundary(prs):
    """把「吞吐与稳定性」和「安全边界」合并成一页：数字和边界放一起才不会被误读。"""
    slide = add_slide(prs, "性能实测与安全边界",
                      "左边是能做到的，右边是明确做不到的——两栏都要看")
    card_text(slide, 0.62, 1.28, 6.0, 2.65, "性能与稳定性（实测）", [
        "· 16 客户端持续压测：处理 474,296 条消息，墙钟 2.4 秒",
        "· 服务端 CPU 17 秒（多核并行，广播式分发）",
        "· 空载内存 9.2 MB；攻击 + 压测全程 9.8–10.8 MB",
        "· 全程零崩溃、零重启；单文件静态二进制，无外部依赖",
        "· 200 条并发建连 6 ms（本机回环，非广域网指标）",
        "· 吞吐与服务端日志逐条计数核对过，不是写缓冲假象",
    ], accent=GREEN, fill=GREEN_L, title_size=15, body_size=11.5)
    card_text(slide, 6.72, 1.28, 6.0, 2.65, "安全边界（明确做不到）", [
        "· 主动中间人：裸 ECDH 没有身份认证，能劫持线路的人理论上",
        "  可以分别和两端握手——要挡它需要证书或预共享密钥",
        "· 网络层大流量 DDoS：本项目只做应用层防护，流量清洗要靠",
        "  上游运营商 / 云清洗 / CDN",
        "· 端到端加密：服务端能解密消息（它要转发、要存历史）",
        "· 密码找回、多设备同步、消息签名——都没有",
    ], accent=RED, fill=RED_L, title_size=15, body_size=11.5)
    card_text(slide, 0.62, 4.12, 12.1, 2.2, "为什么两栏要放在同一页", [
        "· 单看左边会以为「这服务器打不死」，单看右边会以为「防护很弱」——都不对",
        "· 实测数字证明的是「规则按设计生效」，不是「扛得住任何流量」",
        "· 安全材料里最危险的不是「防护少」，而是「你以为防护了很多」；",
        "  使用者按错误假设去用，风险比明说大得多",
        "· 这不是事后补的：项目从第一版加密起就在 crypto.h 里写着「不保护主动中间人」，README 照抄同一句",
    ], accent=AMBER, fill=AMBER_L, title_size=15, body_size=12)
    return slide



def slide_reg_defense(prs):
    """注册路径防护：从"发现缺口"到"修好并实测"的完整一页。"""
    slide = add_slide(prs, "补上注册路径：从发现缺口到实测修复",
                      "有人问「疯狂注册换 IP 是不是防不住」——实测确实防不住，于是补掉了")
    add_image_fit(slide, os.path.join(ASSETS, "chart-registration.png"), 0.45, 1.15, 12.5, 4.3)
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
    return slide


def slide_rules(prs):
    slide = add_slide(prs, "可调防护规则", "17 条规则全部是运行时参数：改一行、重启即生效")
    rows = [
        ["规则", "作用", "默认值"],
        ["maxconns", "同时连接总数上限", "0（不限）"],
        ["maxconnsperip", "同一 IP 的同时连接上限", "0（不限）"],
        ["loginfails", "同 IP 每 5 分钟允许的登录失败次数，超出即封", "0（不限）"],
        ["handshaketimeout", "连上后多少秒内必须登录，否则断开", "30 秒"],
        ["uploadrate / downloadrate", "单客户端上传 / 下载限速（KB/s）", "0（不限）"],
        ["maxtextlen / maxtextlines", "单条消息最大字符数 / 行数", "0（不限）"],
        ["chatinterval", "发言最小间隔，防刷屏", "0（不限）"],
        ["registerinterval", "同一 IP 两次注册的最小间隔", "0（不限）"],
        ["maxaccounts", "账号总数上限（换 IP 也绕不过）", "0（不限）"],
        ["documentsize / maxservertemp", "单文件上限 / 暂存总量上限", "64 MB / 1024 MB"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.4, rows, col_widths=[3.7, 5.9, 2.5], font_size=11.5)
    note(slide, "默认只开握手超时是刻意的：先保证「拿下来就能用」，要上公网再按需开启——加固后的实测见前两页")
    return slide





def slide_interop(prs):
    """四端互通：实测过的组合 + 这次真机验证补齐了什么。"""
    slide = add_slide(prs, "四端互通：实测通过",
                      "Windows · Linux · macOS · Android 同一个服务器，互相都能看到消息")
    add_image_fit(slide, os.path.join(ASSETS, "chart-interop.png"), 0.45, 1.05, 12.5, 4.2)
    card_text(slide, 0.6, 5.45, 4.0, 1.7, "这次补上了什么", [
        "· macOS 首次真机编译：零报错",
        "· 原先只做过静态验证（交叉编译）",
        "· 现在补上了运行与互通验证",
        "· 那张「不夸大」图已改成「已实测」",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 4.75, 5.45, 4.0, 1.7, "已验证的功能", [
        "· 登录 / 收发消息（与三个平台双向互发）",
        "· 四端同时在线，互相都看得到",
        "· 附件（文件）在端之间传过",
        "· 彩色文字在 mac 上显示正常",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.45, 3.85, 1.7, "为什么不全部涂绿", [
        "· 语音消息没测，那一行标红写「未测」",
        "· 灰色格是这一轮没测的组合",
        "· 标出来比统一涂绿更有用：",
        "  下一个要补的地方一目了然",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide

def slide_unified(prs):
    """四端统一：会话逻辑只有一处实现 + 终端界面的真机测试。"""
    slide = add_slide(prs, "四端统一：会话逻辑只有一处实现",
                      "Linux 终端界面也接上了共用的 chat_core —— 界面只负责「怎么显示」")
    add_image_fit(slide, os.path.join(ASSETS, "chart-layers.png"), 0.45, 1.05, 12.5, 4.1)
    card_text(slide, 0.6, 5.35, 4.0, 1.8, "Linux 界面这次改了什么", [
        "· 原先自己解析协议、自己分派 9 个指令",
        "· 现在实现 ChatCoreDelegate，不再持有连接",
        "· 渲染抽成纯函数 cli_render（可单测）",
        "· 界面只剩：读键 / 画输入行 / 打输出",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 4.75, 5.35, 4.0, 1.8, "界面终于能自动化测了", [
        "· 用伪终端（pty）驱动真客户端",
        "· 35 项：收发 / 补全 / 色码 / 退格删中文",
        "· 还有附件（下载内容逐字节核对）与语音",
        "· 以前这块完全没有自动化覆盖",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.35, 3.85, 1.8, "一个值得记的教训", [
        "· 这批测试第一版报了 6 项失败",
        "· 逐条查下来，6 项全是测试自己写错",
        "· client_core 新加 35 项渲染单测",
        "· 测试失败先怀疑测试，别急着改代码",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide


def slide_macos(prs):
    """macOS 端：做到了什么、验证到哪一步（如实标注）。"""
    slide = add_slide(prs, "macOS 端：原生 Cocoa 界面 + 如实标注的验证边界",
                      "开发机上没有 Mac，所以「能编的都编了、能查的都查了」，但从未真机运行")
    add_image_fit(slide, os.path.join(ASSETS, "chart-macos-verify.png"), 0.45, 1.1, 12.5, 4.4)
    # 三条、每条约 20 字以内 —— 四条会把卡片撑破（第一版就是这么溢出的）
    card_text(slide, 0.6, 5.65, 4.0, 1.5, "为 macOS 改了什么", [
        "· 没有 MSG_NOSIGNAL → 用 SO_NOSIGPIPE",
        "· TCP_KEEPIDLE 改叫 TCP_KEEPALIVE",
        "· htons 是宏：加了 :: 反而编不过",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11)
    card_text(slide, 4.75, 5.65, 4.0, 1.5, "客户端分了层", [
        "· client_core/ 可移植核心：网络/信任/附件/颜色/语音/会话逻辑",
        "· linux/client/ 与 macos/main.mm 只剩界面",
        "· 界面能编不了也不怕：逻辑都在能验证的核心层",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.65, 3.85, 1.5, "怎么在没有 Mac 时查错", [
        "· zig 交叉编译成真 Mach-O 二进制",
        "· 18 个文件 × 两个架构全通过",
        "· 文本层查 selector / 括号 / 声明",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide


def slide_bugs(prs):
    slide = add_slide(prs, "几个真实的 bug（和它们为什么难查）",
                      "共同点：不报错、不崩，只是「结果悄悄不对」")
    rows = [
        ["现象", "真正的原因"],
        ["桌面端所有自绘按钮突然不显示",
         "SetWindowLongPtrW 的返回值被丢掉了，导致 CallWindowProcW 收到空指针——按钮画不出来，窗口还被永久置为无效"],
        ["附件卡片完全不显示",
         "FILE_OFFER 第一格是上传者昵称，被当成了附件 ID；又漏剥了消息里的 hh:mm 时间戳"],
        ["三端握手永远失败，但没有任何报错",
         "CNG 的私钥 blob 里标量是小端，OpenSSL 的 BIGNUM 是大端。不翻转的话「同一个标量」其实是两个数"],
        ["取色盘点某个位置出来的是别的颜色",
         "SV 方块的四角伸进了色环，命中测试先命中了色环——单元测试按采样点比对才抓出来"],
        ["上传完不知道自己文件的附件 ID",
         "服务端只广播给别人，把上传者排除了；而 ID 是服务端分配的，客户端无从猜测"],
    ]
    add_table(slide, 0.62, 1.3, 12.1, 4.7, rows, col_widths=[4.6, 7.5], font_size=11.5)
    note(slide, "这五个里有四个是靠测试或服务端日志定位的，只有一个（按钮不显示）是用户先发现、再回头查的")
    return slide


def slide_ai_intro(prs):
    slide = add_slide(prs, "AI 在项目里做了什么", "公开说明：这个项目从第一行代码起就是 AI 结对开发的")
    card_text(slide, 0.62, 1.3, 5.95, 2.6, "AI 承担的部分", [
        "· 全部代码的第一版：C++ / Kotlin / Python 都一样",
        "· 设计并撰写中文注释（这个项目的注释密度很高）",
        "· 跑构建、跑测试、读日志、定位失败原因",
        "· 操作 Kali 虚拟机做跨平台验证（装工具链、传文件、跑 ctest）",
        "· 写提交信息、README、以及这份 PPT",
    ], accent=BLUE, fill=BLUE_L, body_size=13)
    card_text(slide, 6.78, 1.3, 5.95, 2.6, "人承担的部分", [
        "· 决定先做什么、后做什么（优先级全由人定）",
        "· 在真机上试、给出界面反馈（「太挤」「太丑」「这按钮呢」）",
        "· 定下项目约定：中文注释、不用 #define、避免 Python、先问再做安全相关写入",
        "· 仓库治理与发布决策（什么时候推 GitHub、发不发二进制）",
    ], accent=GREEN, fill=GREEN_L, body_size=13)
    card_text(slide, 0.62, 3.95, 6.0, 2.4, "AI 最有效的三个用法", [
        "① 写可被测试的纯逻辑：解析、判定、格式化全部抽成纯函数，",
        "   一有测试，后面每次修改都能立刻验证",
        "② 交叉核对而不是相信「应该对」：写探针程序把中间量打出来",
        "   逐字节 diff，三端一致的三个坑就是这么找出来的",
        "③ 顺着日志往下挖：加临时日志、看服务端记录、用 strace 看",
        "   实际发出去的字节，好几个 bug 几分钟内定位",
    ], accent=GREEN, fill=GREEN_L, title_size=15, body_size=11.5)
    card_text(slide, 6.78, 3.95, 5.95, 2.4, "一个坦诚的比例估计", [
        "· 代码、注释、测试、文档、构建与验证脚本：绝大部分由 AI 产出",
        "· 方向、取舍、验收标准、以及「什么算做完了」：全部由人决定",
        "· AI 更像执行力很强的实现者 + 不会累的验证者，而不是决策者。",
        "  决策错了的时候（比如界面改版方向），返工成本也是真实的。",
    ], accent=AMBER, fill=AMBER_L, title_size=15, body_size=11.5)
    return slide



def slide_ai_failures(prs):
    slide = add_slide(prs, "AI 真实搞砸的地方",
                      "这一页比上一页更有用——都是这个项目里实际发生的")
    rows = [
        ["翻车方式", "具体发生了什么", "后来的做法"],
        ["过度宽泛的批量替换",
         "用词边界正则替换 SOCKET，把 SOL_SOCKET 改成了 SOL_sock::Handle，整个文件编不过，只能回退重来",
         "替换改成按整行精确匹配，并写成脚本执行"],
        ["对运行环境想当然",
         "服务端进程还占着 exe，链接直接失败报 Permission denied，一度以为是代码问题",
         "先看错误正文，再动手改代码"],
        ["忽视自己写过的规则",
         "测试脚本给服务端传位置参数 5599，实际参数是 --port，端口没生效，白折腾一轮",
         "跑之前先确认命令行语法"],
        ["工具链知识缺失",
         "std::string 会间接引入 windows.h，把 winsock2.h 挡掉，报错却是「sock 未声明」，很误导",
         "记住 winsock2 必须在任何标准库头之前"],
        ["自造的测试也写错过",
         "把「时长未知」的 -1 当成非法值去断言，测试红了但产品是对的",
         "先想清楚语义，再写断言"],
    ]
    add_table(slide, 0.62, 1.25, 12.1, 4.9, rows, col_widths=[2.5, 6.3, 3.3], font_size=11)
    note(slide, "这一页的价值在于：知道 AI 会怎么错，才知道该在哪些地方加检查")
    return slide


def slide_lessons(prs):
    slide = add_slide(prs, "从这次开发里得到的结论",
                      "如果你也想用 AI 做这种规模的项目")
    bullets(slide, 0.62, 1.3, 12.1, 5.0, [
        ("一、测试不是「有空再写」，是让 AI 能干活的前提", 0, NAVY),
        ("没有测试的代码，改一处要重跑整个程序靠肉眼确认；有测试的模块，改完一条命令就知道对不对", 1),
        ("这个项目里几乎每个真 bug 最后都变成了一条新的断言", 1),
        ("", 0),
        ("二、跨平台 / 跨语言的项目，先立「一致性契约」再写业务", 0, NAVY),
        ("三端一致性不是靠小心，是靠同一套测试向量 + 明确的书面约定", 1),
        ("字节序、字段顺序、输出布局这类事，一定要写进注释并配测试", 1),
        ("", 0),
        ("三、AI 写的注释质量直接决定后期能不能维护", 0, NAVY),
        ("这个项目要求注释写「为什么」，包括被否决的方案和每个防护挡住的具体故障", 1),
        ("回头看，正是这些注释让后面几轮的修改没有把前面的修复推翻", 1),
        ("", 0),
        ("四、人必须守住「验收标准」", 0, RED),
        ("AI 很容易把「编译通过 + 测试通过」当成做完了，但界面好不好看、体验顺不顺，只有人能判断", 1),
    ], size=13.5, spacing=6)
    return slide


def slide_end(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    bg = slide.shapes.add_shape(1, Inches(0), Inches(0), Inches(SW), Inches(SH))
    bg.fill.solid()
    bg.fill.fore_color.rgb = NAVY
    bg.line.fill.background()
    bg.shadow.inherit = False

    textbox(slide, 1.15, 1.5, 11, 1.0, ["现在就能试"], size=40, color=WHITE, spacing=0)

    box = card(slide, 1.15, 2.7, 11.0, 2.35, RGBColor(0x0E, 0x1C, 0x36))
    textbox(slide, 1.45, 2.9, 10.5, 1.9, [
        "# 下载免编译的 Linux 包，解包就能跑",
        "tar -xzf dchat-linux-x86_64.tar.gz && cd dchat-linux-x86_64",
        "chmod +x dchat_client dchat_server",
        "./dchat_client --host 服务器地址 --port 5555 --user 名字 --pass 密码 --register",
        "",
        "# 或者自己开一台",
        "./dchat_server --port 5555",
    ], size=13.5, color=RGBColor(0xBF, 0xD4, 0xF2), spacing=3)

    textbox(slide, 1.15, 5.2, 11, 1.8, [
        "github.com/dongfang20101113/DChat",
        "Windows / 安卓 / Linux 三端 · 端到端加密 · 990+ 项测试检查",
        "实测：连接上限精确生效 · 握手超时 40/40 断开 · 暴破 12/12 拦下 · 内存稳定 10 MB",
        "",
        "欢迎试用、提问题、或者直接拿去改",
    ], size=15, color=RGBColor(0xA9, 0xC2, 0xE8), spacing=6)
    return slide


def main():
    prs = new_deck()
    slide_cover(prs)
    slide_overview(prs)
    slide_arch(prs)
    slide_features(prs)
    slide_screens(prs)
    slide_protocol(prs)
    slide_crypto(prs)
    slide_three_contracts(prs)
    slide_server(prs)
    slide_timeline(prs)
    slide_phases(prs)
    slide_verification(prs)
    slide_sec_design(prs)
    slide_ddos(prs)
    slide_chart_conn(prs)
    slide_chart_timeout(prs)
    slide_reg_defense(prs)
    slide_perf_boundary(prs)
    slide_rules(prs)
    slide_unified(prs)
    slide_interop(prs)
    slide_macos(prs)
    slide_bugs(prs)
    slide_ai_intro(prs)
    slide_ai_failures(prs)
    slide_lessons(prs)
    slide_end(prs)
    prs.save(OUT)
    print("已生成", OUT, "共", len(prs.slides.__iter__.__self__._sldIdLst), "页")


if __name__ == "__main__":
    main()
