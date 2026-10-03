"""收尾修三处排版/文字问题（合并后暴露出来的）。

  1. 第 11 页标题还写"攻击对比曲线（一）：连接上限"，但它现在同时包含超时曲线
  2. 第 12 页（注册防护）规则表溢出到幻灯片外，最后一行被切
  3. 第 10 页（安全设计）底部注释太长换行
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

EDITS = [
    # ---- 1. 曲线页标题 ----
    ('slide = add_slide(prs, "攻击对比曲线（一）：连接上限",\n'
     '                      "目标连接数从 20 加到 300，存活数在 60 处硬性封顶")',
     'slide = add_slide(prs, "攻击对比曲线：连接上限与慢速耗尽",\n'
     '                      "左边是「建太多」、右边是「占着不动」——两类攻击都被硬性拦住")'),

    # ---- 2. 注册页整体上收，给规则表腾出空间 ----
    ('add_image_fit(slide, os.path.join(ASSETS, "chart-registration.png"), 0.45, 1.08, 12.5, 3.35)',
     'add_image_fit(slide, os.path.join(ASSETS, "chart-registration.png"), 0.45, 1.02, 12.5, 2.75)'),
    ('    card_text(slide, 0.6, 4.55, 4.0, 1.25, "缺口是什么", [',
     '    card_text(slide, 0.6, 3.95, 4.0, 1.3, "缺口是什么", ['),
    ('    card_text(slide, 4.75, 4.55, 4.0, 1.25, "怎么修的", [',
     '    card_text(slide, 4.75, 3.95, 4.0, 1.3, "怎么修的", ['),
    ('    card_text(slide, 8.9, 4.55, 3.85, 1.25, "不误伤验证", [',
     '    card_text(slide, 8.9, 3.95, 3.85, 1.3, "不误伤验证", ['),
    ('''    rule_rows = [
        ["规则", "作用", "默认"],
        ["maxconns / maxconnsperip", "连接总数 / 同一 IP 上限", "0"],
        ["loginfails", "同 IP 每 5 分钟登录失败次数", "0"],
        ["handshaketimeout", "连上后多久必须登录", "30 秒"],
        ["registerinterval / maxaccounts", "注册冷却 / 账号总量上限（本次新增）", "0"],
        ["uploadrate / downloadrate", "单客户端上传 / 下载限速", "0"],
        ["maxtextlen / maxtextlines / chatinterval", "文本长度 / 行数 / 发言间隔", "0"],
        ["documentsize / maxservertemp", "单文件 / 暂存总量上限", "64MB / 1024MB"],
    ]
    add_table(slide, 0.62, 5.92, 12.1, 1.75, rule_rows, col_widths=[4.5, 6.1, 1.5],
              font_size=9.5)''',
     '''    # 规则表压缩成 5 行：原先 7 行 + 表头会溢出到幻灯片外（最后一行被切）。
    # 合并成同类项既省高度，读起来也更像"旋钮分类"而不是一堆散条目。
    rule_rows = [
        ["规则（共 17 条，全部是运行时参数）", "作用", "默认"],
        ["maxconns / maxconnsperip", "连接总数 / 同一 IP 上限", "0"],
        ["loginfails / handshaketimeout", "登录失败封禁 / 握手超时", "0 / 30 秒"],
        ["registerinterval / maxaccounts", "注册冷却 / 账号总量上限（本次新增）", "0"],
        ["uploadrate / downloadrate / chatinterval", "上传下载限速 / 发言间隔", "0"],
        ["maxtextlen / maxtextlines / documentsize / maxservertemp",
         "文本长度 / 行数 / 单文件 / 暂存总量", "0 / 64MB / 1024MB"],
    ]
    add_table(slide, 0.62, 5.42, 12.1, 1.55, rule_rows, col_widths=[4.9, 5.6, 1.6],
              font_size=10)'''),

    # ---- 3. 安全页底部注释缩短到一行 ----
    ('''    note(slide, "四层互相独立（加密被绕过仍有指纹，指纹被忽略仍有连接上限，上限被绕开仍有凭据哈希）；"
                "右栏是每一层的实测证据，全部在真服务端上打出来。攻击后内存稳定在 9.3–10.3 MB、"
                "新客户端照样能注册登录收发")''',
     '''    note(slide, "四层互相独立：加密被绕过仍有指纹，指纹被忽略仍有连接上限，上限被绕开仍有凭据哈希。"
                "右栏是每层的实测证据（真服务端），攻击后内存稳定 9.3–10.3 MB")'''),
]


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()
    changed = 0
    for old, new in EDITS:
        if old not in text:
            print("  ⚠ 未匹配:", old.strip().splitlines()[0][:60])
            continue
        text = text.replace(old, new, 1)
        changed += 1
    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("改了 %d / %d 处" % (changed, len(EDITS)))


if __name__ == "__main__":
    main()
