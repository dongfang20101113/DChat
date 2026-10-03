"""修第 8 页（开发过程梗概）的遮挡：卡片压住了表格底部。

数值账：
  表格   y=1.3  高 5.0  -> 到 6.3
  卡片   y=5.55 高 1.75 -> 从 5.55 开始  => 重叠 0.75 英寸
幻灯片可用高度约 7.5，所以把表格收到 3.55、卡片下移到 4.95（到 7.05），
既不再重叠，也不越出页面底边。

顺带：合并了两行阶段之后只剩 7 个阶段，副标题还写着"八个阶段"。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

EDITS = [
    ('slide = add_slide(prs, "开发过程梗概", "69 次提交、9 天，按提交信息可以分成八个阶段")',
     'slide = add_slide(prs, "开发过程梗概", "69 次提交、9 天，按提交信息可以分成七个阶段")'),

    ('    add_table(slide, 0.62, 1.3, 12.1, 5.0, rows, col_widths=[2.2, 7.4, 3.2], font_size=11.5)',
     '    # 表格高度必须收到 3.55：原先写 5.0（到 6.3），会把下面两张卡片的顶部盖住\n'
     '    add_table(slide, 0.62, 1.25, 12.1, 3.55, rows, col_widths=[2.2, 7.4, 3.2],\n'
     '              font_size=11)'),

    ('    card_text(slide, 0.62, 5.55, 5.95, 1.75, "几个改变了走向的决定", [',
     '    card_text(slide, 0.62, 4.95, 5.95, 2.1, "几个改变了走向的决定", ['),
    ('    card_text(slide, 6.78, 5.55, 5.95, 1.75, "吃过亏的地方", [',
     '    card_text(slide, 6.78, 4.95, 5.95, 2.1, "吃过亏的地方", ['),
]


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()
    changed = 0
    for old, new in EDITS:
        if old not in text:
            print("  ⚠ 未匹配:", old.strip()[:55])
            continue
        text = text.replace(old, new, 1)
        changed += 1
    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("改了 %d / %d 处" % (changed, len(EDITS)))


if __name__ == "__main__":
    main()
