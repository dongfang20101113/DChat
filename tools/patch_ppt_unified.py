# 给 PPT 加"四端统一"一页（Linux 界面也接上共用核心之后）。
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

NEW_SLIDE = '''

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
        "· 24 项：收发 / Tab 补全 / 色码 / 退格删中文",
        "· 还覆盖 Ctrl+U、↑ 翻历史、/clear、/quit",
        "· 以前这块完全没有自动化覆盖",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11)
    card_text(slide, 8.9, 5.35, 3.85, 1.8, "一个值得记的教训", [
        "· 这批测试第一版报了 6 项失败",
        "· 逐条查下来，6 项全是测试自己写错",
        "· client_core 新加 35 项渲染单测",
        "· 测试失败先怀疑测试，别急着改代码",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11)
    return slide

'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "slide_unified" in text:
        raise SystemExit("四端统一页已存在")

    anchor = "\ndef slide_macos(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_macos 锚点")
    text = text.replace(anchor, NEW_SLIDE + "\ndef slide_macos(prs):", 1)

    # 放在 macOS 页之前：先讲"现在四端统一了"，再讲 macOS 的验证边界
    old_order = "    slide_rules(prs)\n    slide_macos(prs)"
    new_order = "    slide_rules(prs)\n    slide_unified(prs)\n    slide_macos(prs)"
    if old_order not in text:
        raise SystemExit("找不到装配顺序锚点")
    text = text.replace(old_order, new_order, 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已加入四端统一页，行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
