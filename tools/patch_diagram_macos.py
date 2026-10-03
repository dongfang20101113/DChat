# 架构图加 macOS 客户端：从三个框变四个框。
#
# 加框不只是多写一行 —— 尺寸也要跟着改：原先是 3 × 560，放不下 4 个，
# 收窄到 4 × 440，间距从 4 等分改成 5 等分，箭头数量也从 3 变 4。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt_diagrams.py")

EDITS = [
    ('    """三端架构图：三个客户端 + 一个服务端 + 共享协议层。"""',
     '    """四端架构图：四个客户端 + 一个服务端 + 共享协议层。"""'),

    ('''    # ---- 顶部三个客户端 ----
    clients = [
        ("Windows 客户端", "C++17 + Win32/GDI+", "系统自带 CNG 加密", BLUE, BLUE_L),
        ("安卓客户端", "Kotlin + Compose", "系统自带 JCE 加密", GREEN, GREEN_L),
        ("Linux 客户端", "C++17 终端界面", "OpenSSL 3 加密", AMBER, AMBER_L),
    ]
    box_w, box_h = 560, 250
    gap = (W - 3 * box_w) / 4''',
     '''    # ---- 顶部四个客户端 ----
    # 加 macOS 之后要放下四个框：原先 3 × 560 放不下，收窄到 4 × 440，
    # 间距从 4 等分改成 5 等分（4 个框之间是 5 段空隙）。
    clients = [
        ("Windows 客户端", "C++17 + Win32/GDI+", "系统自带 CNG 加密", BLUE, BLUE_L),
        ("安卓客户端", "Kotlin + Compose", "系统自带 JCE 加密", GREEN, GREEN_L),
        ("Linux 客户端", "C++17 终端界面", "OpenSSL 3 加密", AMBER, AMBER_L),
        ("macOS 客户端", "ObjC++ + Cocoa", "OpenSSL 3 加密", (150, 60, 160), (248, 238, 250)),
    ]
    box_w, box_h = 440, 250
    gap = (W - 4 * box_w) / 5'''),

    ('''    # ---- 箭头 ----
    for i in range(3):''',
     '''    # ---- 箭头 ----
    for i in range(4):'''),

    ('"服务端（Windows / Linux 同一份源码）"',
     '"服务端（Windows / Linux / macOS 同一份源码）"'),
]


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    changed = 0
    for old, new in EDITS:
        if old not in text:
            print("⚠ 未匹配:", old.strip().splitlines()[0][:60])
            continue
        text = text.replace(old, new, 1)
        changed += 1

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("改了 %d / %d 处" % (changed, len(EDITS)))


if __name__ == "__main__":
    main()
