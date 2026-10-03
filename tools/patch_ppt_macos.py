# 给 PPT 加"macOS 端"一页，并同步平台支持的说法。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

NEW_SLIDE = '''

def slide_macos(prs):
    """macOS 端：做到了什么、验证到哪一步（如实标注）。"""
    slide = add_slide(prs, "macOS 端：原生 Cocoa 界面 + 如实标注的验证边界",
                      "开发机上没有 Mac，所以「能编的都编了、能查的都查了」，但从未真机运行")
    add_image_fit(slide, os.path.join(ASSETS, "chart-macos-verify.png"), 0.45, 1.1, 12.5, 4.4)
    card_text(slide, 0.6, 5.65, 4.0, 1.5, "为 macOS 改了什么", [
        "· MSG_NOSIGNAL 没有 → SO_NOSIGPIPE（漏了会被断开的客户端打死）",
        "· TCP_KEEPIDLE 在 macOS 叫 TCP_KEEPALIVE，且没有 TCP_KEEPCNT",
        "· htons 在 macOS 是宏，加了 :: 反而编不过",
        "· 语音 arecord/aplay → sox 的 rec/play",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=10)
    card_text(slide, 4.75, 5.65, 4.0, 1.5, "客户端分了层", [
        "· client_core/ 可移植核心：网络/信任/附件/颜色/语音/会话逻辑",
        "· linux/client/ 与 macos/main.mm 只剩界面",
        "· 界面能编不了也不怕：逻辑都在能验证的核心层",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.65, 3.85, 1.5, "怎么在没有 Mac 时查错", [
        "· zig cc -target *-macos 交叉编译成真 Mach-O",
        "· 18 个文件 × 两个架构全部通过",
        "· 文本层静态检查 selector/括号/成员声明",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide

'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "slide_macos" in text:
        raise SystemExit("macOS 页已存在")

    anchor = "\ndef slide_bugs(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_bugs 锚点")
    text = text.replace(anchor, NEW_SLIDE + "\ndef slide_bugs(prs):", 1)

    # 放在规则页之后、踩坑页之前：先讲完功能与实测，再讲平台与坑
    old_order = "    slide_rules(prs)\n    slide_bugs(prs)"
    new_order = "    slide_rules(prs)\n    slide_macos(prs)\n    slide_bugs(prs)"
    if old_order not in text:
        raise SystemExit("找不到装配顺序锚点")
    text = text.replace(old_order, new_order, 1)

    # 总览那页的"平台"说法要跟上（原本只说 Windows/安卓/Linux）
    text = text.replace(
        'slide = add_slide(prs, "可调防护规则", "17 条规则全部是运行时参数：改一行、重启即生效")',
        'slide = add_slide(prs, "可调防护规则", "17 条规则全部是运行时参数：改一行、重启即生效")',
        1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已加入 macOS 页，行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
