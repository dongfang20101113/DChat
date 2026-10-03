# 给 PPT 加"四端分层"一页。
#
# 注意：make_charts.py 上一轮被我误截掉了 __main__ 块，这个脚本会一并恢复。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_charts.py")

CHARTS = '''

# ---------------------------------------------------------------------------
# 四端分层：谁和谁共用代码
# ---------------------------------------------------------------------------
# 这张图的作用：一眼看出"加一个新平台要写多少代码"。答案只有最上面那一层。
LAYERS = [
    ("界面层", "各自实现 —— 只做「怎么显示」",
     ["Windows   Win32 / GDI+ 图形界面",
      "Linux     终端 TUI（ui.cpp 只读键、画输入行）",
      "macOS     Cocoa / ObjC++（main.mm）",
      "Android   Kotlin / Compose"], BLUE),
    ("会话核心", "终端版与 Cocoa 版共用同一份 client_core",
     ["chat_core    登录注册 · 收发 · 指令派发 · 附件 · TOFU 判定",
      "cli_render   终端渲染：ANSI 上色、按宽度折行（纯函数，有单测）",
      "net · trust · chat_color · files · voice"], GREEN),
    ("协议与加密", "四端共用同一份 src/（dchat_protocol 静态库）",
     ["protocol · crypto（ECDH P-256 + AES-256-GCM）· auth",
      "server_rules · server_command · socket_util"], AMBER),
    ("服务端", "同一份源码，Windows / Linux / macOS 三端可构建",
     ["多线程 TCP · 登录失败封禁 · 连接数上限 · 限速",
      "注册防护：registerinterval / maxaccounts"], GRAY),
]


def make_layers_chart(path):
    """四端分层图：每层一个横条，越靠上越是「每个平台自己写」。"""
    W, H = 2000, 980
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(33, bold=True)
    f_layer = load_font(26, bold=True)
    f_note = load_font(20)
    f_item = load_font(21)

    d.text((80, 38), "四个平台，代码分四层：加新平台只需要写最上面那一层",
           font=f_title, fill=NAVY)
    d.text((80, 88), "会话逻辑只有一处实现，所以界面不会各写一套指令解析与协议时序",
           font=f_note, fill=GRAY)

    y = 140
    for name, note, items, color in LAYERS:
        height = 78 + 32 * len(items)
        # 浅底色：把该层颜色往白里调；边框仍用本色，文字才看得清
        light = tuple(c + (255 - c) * 9 // 10 for c in color)
        d.rounded_rectangle((80, y, 1920, y + height), radius=16, fill=light,
                            outline=color, width=3)
        d.text((112, y + 18), name, font=f_layer, fill=color)
        d.text((340, y + 24), note, font=f_note, fill=GRAY)
        item_y = y + 56
        for item in items:
            d.text((132, item_y), "·  " + item, font=f_item, fill=TEXT)
            item_y += 30
        y += height + 18

    img.save(path)
    return path
'''

MAIN_BLOCK = '''

if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "build", "ppt-assets")
    os.makedirs(out, exist_ok=True)
    print(make_conn_chart(os.path.join(out, "chart-conn-limit.png")))
    print(make_timeout_chart(os.path.join(out, "chart-timeout.png")))
    print(make_registration_chart(os.path.join(out, "chart-registration.png")))
    print(make_macos_verify_chart(os.path.join(out, "chart-macos-verify.png")))
    print(make_layers_chart(os.path.join(out, "chart-layers.png")))
'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "make_layers_chart" in text:
        raise SystemExit("分层图已存在")

    if "__main__" not in text:
        # 上一轮把 __main__ 块一起截掉了，补回来（保留已生成的图）
        text = text.rstrip() + "\n" + CHARTS + MAIN_BLOCK
    else:
        marker = '\n\nif __name__ == "__main__":'
        if marker not in text:
            raise SystemExit("找不到 __main__ 锚点")
        text = text.replace(marker, CHARTS + marker, 1)
        text = text.replace(
            '    print(make_macos_verify_chart(os.path.join(out, "chart-macos-verify.png")))',
            '    print(make_macos_verify_chart(os.path.join(out, "chart-macos-verify.png")))\n'
            '    print(make_layers_chart(os.path.join(out, "chart-layers.png")))')

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已加入分层图，行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
