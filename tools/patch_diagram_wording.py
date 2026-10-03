# 架构图与对应幻灯片里剩下的"三端/两处"措辞收尾。
import io
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIAGRAM = os.path.join(REPO, "tools", "make_ppt_diagrams.py")
PPT = os.path.join(REPO, "tools", "make_ppt.py")

DIAGRAM_EDITS = [
    # 三处约定是协议层面的概念，但现在实现的端是四个
    ("三端必须字节级一致的三处约定", "四端必须字节级一致的三处约定"),
    # 密码学后端枚举里补上 macOS（它和 Linux 共用 OpenSSL 实现）
    ("密码学原语一律不自己发明：Windows 用 CNG、安卓用 JCE、Linux 用 OpenSSL",
     "密码学原语一律不自己发明：Windows 用 CNG、安卓用 JCE、Linux 与 macOS 用 OpenSSL"),
]

PPT_EDITS = [
    ('slide = add_slide(prs, "整体架构", "三个客户端共用一份协议层源码，服务端两头都能跑")',
     'slide = add_slide(prs, "整体架构", "四个客户端共用一份协议层源码，服务端三端都能跑")'),
    ('note(slide, "Linux 端和服务端共用同一份 C++ 协议层；安卓端是 Kotlin 重写，靠同一套测试向量保证行为一致")',
     'note(slide, "Windows / Linux / macOS 三个客户端与服务端共用同一份 C++ 协议层；'
     '安卓端是 Kotlin 重写，靠同一套测试向量保证行为一致")'),
]


def apply(path, edits):
    with io.open(path, encoding="utf-8") as handle:
        text = handle.read()
    changed = 0
    for old, new in edits:
        if old not in text:
            print("  ⚠ 未匹配:", old[:50])
            continue
        text = text.replace(old, new, 1)
        changed += 1
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("  %s: %d / %d 处" % (os.path.basename(path), changed, len(edits)))


def main():
    print("改架构图与幻灯片：")
    apply(DIAGRAM, DIAGRAM_EDITS)
    apply(PPT, PPT_EDITS)


if __name__ == "__main__":
    main()
