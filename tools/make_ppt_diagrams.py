# 画 PPT 里要用的两张示意图：三端架构、加密握手时序。
# 用 PIL 精确控制位置，避免 PPT 里用形状拼图对不齐。
from PIL import Image, ImageDraw, ImageFont

# 找中文字体
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\simsun.ttc",
]


def load_font(size, bold=False):
    order = FONT_CANDIDATES[1:2] + FONT_CANDIDATES[:1] + FONT_CANDIDATES[2:] if bold else FONT_CANDIDATES
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
AMBER = (176, 108, 12)
AMBER_L = (253, 244, 226)
GRAY = (108, 117, 132)
GRAY_L = (243, 245, 248)
TEXT = (32, 41, 56)


def rrect(draw, box, radius, fill, outline=None, width=2):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def center_text(draw, box, text, font, fill):
    x0, y0, x1, y1 = box
    bbox = draw.textbbox((0, 0), text, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    draw.text((x0 + (x1 - x0 - w) / 2 - bbox[0], y0 + (y1 - y0 - h) / 2 - bbox[1]),
              text, font=font, fill=fill)


def make_architecture(path):
    """四端架构图：四个客户端 + 一个服务端 + 共享协议层。"""
    W, H = 2000, 1120
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    f_title = load_font(40, bold=True)
    f_box = load_font(34, bold=True)
    f_sub = load_font(25)
    f_small = load_font(23)
    f_tiny = load_font(21)

    # ---- 顶部四个客户端 ----
    # 加 macOS 之后要放下四个框：原先 3 × 560 放不下，收窄到 4 × 440，
    # 间距从 4 等分改成 5 等分（4 个框之间是 5 段空隙）。
    clients = [
        ("Windows 客户端", "C++17 + Win32/GDI+", "系统自带 CNG 加密", BLUE, BLUE_L),
        ("安卓客户端", "Kotlin + Compose", "系统自带 JCE 加密", GREEN, GREEN_L),
        ("Linux 客户端", "C++17 终端界面", "OpenSSL 3 加密", AMBER, AMBER_L),
        ("macOS 客户端", "ObjC++ + Cocoa", "OpenSSL 3 加密", (150, 60, 160), (248, 238, 250)),
    ]
    box_w, box_h = 440, 250
    gap = (W - 4 * box_w) / 5
    y = 60
    for i, (name, stack, crypto, color, light) in enumerate(clients):
        x = gap + i * (box_w + gap)
        box = (x, y, x + box_w, y + box_h)
        rrect(d, box, 22, light, color, 3)
        d.rectangle((x, y, x + box_w, y + 8), fill=color)
        center_text(d, (x, y + 30, x + box_w, y + 92), name, f_box, color)
        center_text(d, (x, y + 100, x + box_w, y + 140), stack, f_sub, TEXT)
        center_text(d, (x, y + 150, x + box_w, y + 190), crypto, f_small, GRAY)
        center_text(d, (x, y + 196, x + box_w, y + 232), "连接 · 登录 · 聊天 · 文件 · 语音",
                    f_tiny, GRAY)

    # ---- 中间：协议层 ----
    proto_y = 400
    proto_box = (gap, proto_y, W - gap, proto_y + 210)
    rrect(d, proto_box, 22, (240, 244, 250), NAVY, 3)
    center_text(d, (gap, proto_y + 22, W - gap, proto_y + 82),
                "共享协议层（同一份源码 / 同一套约定）", f_title, NAVY)

    items = [
        ("行式 UTF-8 协议", "每行一条命令，4096 字节上限"),
        ("附件种类字段", "file / sticker / voice，不看扩展名"),
        ("彩色文字", "&a 快捷色 + #rrggbb 真彩色"),
        ("端到端加密", "ECDH P-256 → HKDF → AES-256-GCM"),
    ]
    iw = (W - 2 * gap - 3 * 24) / 4
    for i, (head, body) in enumerate(items):
        x = gap + 18 + i * (iw + 24)
        b = (x, proto_y + 100, x + iw, proto_y + 190)
        rrect(d, b, 14, (255, 255, 255), (200, 210, 225), 2)
        center_text(d, (b[0], b[1] + 8, b[2], b[1] + 48), head, f_small, NAVY)
        center_text(d, (b[0] + 6, b[1] + 46, b[2] - 6, b[3] - 8), body, f_tiny, GRAY)

    # ---- 箭头 ----
    for i in range(4):
        x = gap + box_w / 2 + i * (box_w + gap)
        d.line((x, y + box_h, x, proto_y), fill=GRAY, width=4)
        d.polygon([(x - 12, proto_y - 18), (x + 12, proto_y - 18), (x, proto_y)], fill=GRAY)

    # ---- 底部：服务端 ----
    srv_y = 690
    srv = (gap * 2, srv_y, W - gap * 2, srv_y + 175)
    rrect(d, srv, 22, (250, 240, 240), (176, 60, 60), 3)
    center_text(d, (srv[0], srv_y + 20, srv[2], srv_y + 76), "服务端（Windows / Linux / macOS 同一份源码）",
                f_title, (150, 42, 42))
    feats = ["每连接一个线程", "账号 PBKDF2 加盐哈希", "限速与文本限制",
             "防爆破 / 连接数上限", "身份密钥持久化 + TOFU"]
    fw = (srv[2] - srv[0] - 40) / len(feats)
    for i, f in enumerate(feats):
        x = srv[0] + 20 + i * fw
        center_text(d, (x, srv_y + 90, x + fw, srv_y + 150), f, f_small, TEXT)

    d.line(((W / 2), proto_y + 210, (W / 2), srv_y), fill=GRAY, width=4)
    d.polygon([(W / 2 - 12, srv_y - 18), (W / 2 + 12, srv_y - 18), (W / 2, srv_y)], fill=GRAY)

    # ---- 底部注释 ----
    # 注意：PIL 不认 markdown，**不要**在文字里写 `**`，那会原样画出来
    center_text(d, (0, H - 180, W, H - 130),
                "四端必须字节级一致的三处约定：ECDH 共享密钥大端 · 公钥裸 X‖Y · AES-GCM 密文在前、标签在后",
                f_small, (150, 42, 42))
    center_text(d, (0, H - 120, W, H - 70),
                "密码学原语一律不自己发明：Windows 用 CNG、安卓用 JCE、Linux 与 macOS 用 OpenSSL",
                f_small, GRAY)
    img.save(path)
    return path


def make_handshake(path):
    """加密握手时序图。"""
    W, H = 1900, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_lane = load_font(32, bold=True)
    f_msg = load_font(25)
    f_note = load_font(22)
    f_tiny = load_font(20)

    left_x, right_x = 300, W - 300
    d.rectangle((left_x - 150, 40, left_x + 150, 110), fill=BLUE_L, outline=BLUE, width=3)
    d.rectangle((right_x - 150, 40, right_x + 150, 110), fill=(250, 240, 240),
                outline=(176, 60, 60), width=3)
    center_text(d, (left_x - 150, 40, left_x + 150, 110), "客户端", f_lane, BLUE)
    center_text(d, (right_x - 150, 40, right_x + 150, 110), "服务端", f_lane, (150, 42, 42))
    d.line((left_x, 110, left_x, H - 60), fill=(200, 210, 225), width=2)
    d.line((right_x, 110, right_x, H - 60), fill=(200, 210, 225), width=2)

    steps = [
        ("HELLO  版本 + 客户端公钥 + 客户端随机数", "→", "明文"),
        ("HELLO_OK  服务器公钥 + 服务器随机数", "←", "明文"),
        ("双方各自算：shared = ECDH(自己私钥, 对方公钥)", "↕", "本地"),
        ("c2s = HKDF(shared, salt=两个随机数, info=\"dchat-v1-c2s\")", "↕", "本地"),
        ("s2c = HKDF(shared, salt=两个随机数, info=\"dchat-v1-s2c\")", "↕", "本地"),
        ("ENC base64(密文 ‖ 16 字节标签)", "→", "加密"),
        ("ENC base64(密文 ‖ 16 字节标签)", "←", "加密"),
    ]
    y = 160
    for text, arrow, tag in steps:
        if arrow == "↕":
            box = (left_x - 130, y, right_x + 130, y + 62)
            rrect(d, box, 12, GRAY_L, (200, 210, 225), 2)
            center_text(d, box, text, f_msg, TEXT)
            center_text(d, (left_x - 300, y, left_x - 140, y + 62), tag, f_tiny, GRAY)
        elif arrow == "→":
            d.line((left_x, y + 31, right_x, y + 31), fill=BLUE, width=4)
            d.polygon([(right_x - 20, y + 20), (right_x - 20, y + 42), (right_x, y + 31)],
                      fill=BLUE)
            d.text((left_x + 30, y), text, font=f_msg, fill=BLUE)
            center_text(d, (left_x - 300, y, left_x - 140, y + 62), tag, f_tiny, GRAY)
        else:
            d.line((right_x, y + 31, left_x, y + 31), fill=(176, 60, 60), width=4)
            d.polygon([(left_x + 20, y + 20), (left_x + 20, y + 42), (left_x, y + 31)],
                      fill=(176, 60, 60))
            bbox = d.textbbox((0, 0), text, font=f_msg)
            d.text((right_x - 30 - (bbox[2] - bbox[0]), y), text, font=f_msg,
                   fill=(150, 42, 42))
            center_text(d, (left_x - 300, y, left_x - 140, y + 62), tag, f_tiny, GRAY)
        y += 88

    center_text(d, (0, H - 105, W, H - 55),
                "密钥分方向派生：两个方向共用一把密钥会让两个计数器都从 0 开始，第 0 条消息的 nonce 就撞了（GCM 下这是致命的）",
                f_note, (150, 42, 42))
    img.save(path)
    return path


if __name__ == "__main__":
    import os
    out = os.path.join("build", "ppt-assets")
    os.makedirs(out, exist_ok=True)
    print(make_architecture(os.path.join(out, "diagram-arch.png")))
    print(make_handshake(os.path.join(out, "diagram-handshake.png")))
