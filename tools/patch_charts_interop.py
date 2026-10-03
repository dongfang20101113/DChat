# 改写「macOS 验证到了哪一步」那张图（用户已在 Mac 上真机跑通），并新增互通矩阵图。
#
# 为什么必须改：那张图是上一轮特意画的「不夸大」图，上面写着
# "没有 Mac，未运行"和"缺 macOS 头，未编译"。用户实测之后这些标注已经**与事实不符**，
# 留着反而是另一种不诚实。
#
# 改写原则：**只写用户实际测过的**。用户勾选的实测范围是：
#   能登录发消息 / 与 Windows·Linux·Android 互发 / 四端同时在线互通 /
#   附件传输 / 彩色文字在 mac 上正常
# 用户**没有**勾语音，所以语音那一行如实写「未测」。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_charts.py")

OLD_DATA = '''MAC_VERIFY = [
    ("服务端 + 协议 + 加密", "真交叉编译成 Mach-O", 2),
    ("客户端核心 18 个文件", "两个 macOS 架构各编一遍", 2),
    ("Cocoa 界面 main.mm", "只做了文本层静态检查", 1),
    ("OpenSSL 后端", "缺 macOS 头，未编译", 0),
    ("运行行为（收发/文件/语音）", "没有 Mac，未运行", 0),
]
MAC_LEVEL_TEXT = {2: "已编译验证", 1: "部分检查", 0: "未验证"}'''

NEW_DATA = '''# 0 = 未验证　1 = 部分验证　2 = 已验证
MAC_VERIFY = [
    ("Cocoa 界面真机编译 + 运行", "首次编译零报错，直接跑起来", 2),
    ("登录 / 收发消息", "在 Mac 上与三个平台互发消息", 2),
    ("四端同时在线互通", "Windows · Linux · macOS · Android 同服务器", 2),
    ("附件传输", "文件在端之间传过", 2),
    ("彩色文字", "色码在 mac 上显示正常", 2),
    ("语音消息", "这一项**没测**，不做假设", 0),
]
MAC_LEVEL_TEXT = {2: "已验证", 1: "部分验证", 0: "未测"}'''

OLD_TITLE = '''    d.text((90, 44), "macOS 端验证到了哪一步（不夸大）", font=f_title, fill=NAVY)
    d.text((90, 96), "同一张图里的每一项都在仓库里有对应证据：脚本、错误信息或缺失的头文件",
           font=f_note, fill=GRAY)'''

NEW_TITLE = '''    d.text((90, 44), "macOS 端验证到了哪一步（真机实测之后）", font=f_title, fill=NAVY)
    d.text((90, 96), "原先只做了静态验证，现已在 Mac 上真机跑通；下表按实际测过的范围标注",
           font=f_note, fill=GRAY)'''

OLD_TAIL = '''    d.text((90, 860),
           "结论：能编的都编过了、能查的都查了，但 macOS 版本第一次在真机上编译大概率还需要修",
           font=f_note, fill=TEXT)'''

NEW_TAIL = '''    d.text((90, 862),
           "结论：首次真机编译零报错，四端互通已验证；语音是唯一还没测的一项",
           font=f_note, fill=TEXT)'''

# 互通矩阵：行=发起方，列=接收方。2 = 用户实测验证过，0 = 这一轮没测，1 = 其它会话里测过
INTEROP_ROWS = ["Windows", "Linux", "macOS", "Android"]
INTEROP_COLS = ["Windows", "Linux", "macOS", "Android"]
INTEROP = {
    ("Windows", "macOS"): 2, ("macOS", "Windows"): 2,
    ("Linux", "macOS"): 2, ("macOS", "Linux"): 2,
    ("Android", "macOS"): 2, ("macOS", "Android"): 2,
}

INTEROP_CHART = '''

# ---------------------------------------------------------------------------
# 四端互通矩阵（用户实测：四端同时在线、互相都能看到消息，还传了附件）
# ---------------------------------------------------------------------------
def make_interop_chart(path):
    """互通矩阵：行=谁发，列=谁收。绿=实测互通，灰=这一轮没测（不做假设）。"""
    W, H = 2000, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load_font(34, bold=True)
    f_note = load_font(21)
    f_cell = load_font(23, bold=True)
    f_axis = load_font(24, bold=True)

    d.text((80, 40), "四端互通矩阵：实测过的组合", font=f_title, fill=NAVY)
    d.text((80, 92), "四端同时在同一个服务器上，互相都能看到消息；附件也能在端之间传",
           font=f_note, fill=GRAY)

    left, top = 360, 200
    cell_w, cell_h = 330, 130

    for col_index, name in enumerate(INTEROP_COLS):
        cx = left + col_index * cell_w + cell_w // 2
        d.text((cx, top - 46), name, font=f_axis, fill=NAVY, anchor="mm")
    d.text((left + 2 * cell_w + cell_w // 2, top - 108), "谁收 →", font=f_note, fill=GRAY,
           anchor="mm")

    for row_index, row in enumerate(INTEROP_ROWS):
        cy = top + row_index * cell_h + cell_h // 2
        d.text((left - 30, cy), row, font=f_axis, fill=NAVY, anchor="rm")
        for col_index, col in enumerate(INTEROP_COLS):
            x0 = left + col_index * cell_w
            y0 = top + row_index * cell_h
            if row == col:
                d.rounded_rectangle((x0 + 8, y0 + 8, x0 + cell_w - 8, y0 + cell_h - 8),
                                    radius=12, fill=(246, 247, 250), outline=GRID, width=2)
                d.text((x0 + cell_w // 2, cy), "—", font=f_cell, fill=GRAY, anchor="mm")
                continue
            known = INTEROP.get((row, col), 0)
            if known == 2:
                d.rounded_rectangle((x0 + 8, y0 + 8, x0 + cell_w - 8, y0 + cell_h - 8),
                                    radius=12, fill=GREEN_L, outline=GREEN, width=3)
                d.text((x0 + cell_w // 2, cy - 12), "✓ 互通", font=f_cell, fill=GREEN,
                       anchor="mm")
                d.text((x0 + cell_w // 2, cy + 20), "已实测", font=f_note, fill=GREEN,
                       anchor="mm")
            else:
                d.rounded_rectangle((x0 + 8, y0 + 8, x0 + cell_w - 8, y0 + cell_h - 8),
                                    radius=12, fill=(246, 247, 250), outline=GRID, width=2)
                d.text((x0 + cell_w // 2, cy), "这一轮没测", font=f_note, fill=GRAY,
                       anchor="mm")

    d.text((80, 790),
           "✓ 已实测：macOS ↔ Windows / Linux / Android 双向互发，四端同时在线",
           font=f_note, fill=GREEN)
    d.text((80, 830),
           "灰色格表示这一轮没测 —— 标出来比统一涂绿更有用：下一个要补的地方一目了然",
           font=f_note, fill=GRAY)
    img.save(path)
    return path
'''

MAIN_ADD = '''    print(make_interop_chart(os.path.join(out, "chart-interop.png")))'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "make_interop_chart" in text:
        raise SystemExit("互通图已存在")

    for old, new, label in ((OLD_DATA, NEW_DATA, "验证图数据"),
                            (OLD_TITLE, NEW_TITLE, "验证图标题"),
                            (OLD_TAIL, NEW_TAIL, "验证图结论")):
        if old not in text:
            raise SystemExit("找不到锚点：" + label)
        text = text.replace(old, new, 1)

    marker = '\n\nif __name__ == "__main__":'
    if marker not in text:
        raise SystemExit("找不到 __main__ 锚点")
    text = text.replace(marker, INTEROP_CHART + marker, 1)

    anchor = '    print(make_layers_chart(os.path.join(out, "chart-layers.png")))'
    if anchor not in text:
        raise SystemExit("找不到生成调用锚点")
    text = text.replace(anchor, anchor + "\n" + MAIN_ADD, 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已改写验证图并加入互通矩阵图，行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
