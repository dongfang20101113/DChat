# 给 PPT 加"注册路径防护"页，并同步规则条数等因新规则而变的数字。
import io
import os
import re

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

NEW_SLIDE = '''

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

'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "slide_reg_defense" in text:
        raise SystemExit("注册防护页已存在")

    anchor = "\ndef slide_rules(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_rules 锚点")
    text = text.replace(anchor, NEW_SLIDE + "\ndef slide_rules(prs):", 1)

    # 装配顺序：放在两页曲线之后、规则页之前
    old_order = "    slide_chart_timeout(prs)\n    slide_perf_boundary(prs)"
    new_order = ("    slide_chart_timeout(prs)\n    slide_reg_defense(prs)\n"
                 "    slide_perf_boundary(prs)")
    if old_order not in text:
        raise SystemExit("找不到装配顺序锚点")
    text = text.replace(old_order, new_order, 1)

    # 规则表里补上新规则（第 16 页那张表）
    old_rows = '''        ["chatinterval", "发言最小间隔，防刷屏", "0（不限）"],
        ["documentsize / maxservertemp", "单文件上限 / 暂存总量上限", "64 MB / 1024 MB"],'''
    new_rows = '''        ["chatinterval", "发言最小间隔，防刷屏", "0（不限）"],
        ["registerinterval", "同一 IP 两次注册的最小间隔", "0（不限）"],
        ["maxaccounts", "账号总数上限（换 IP 也绕不过）", "0（不限）"],
        ["documentsize / maxservertemp", "单文件上限 / 暂存总量上限", "64 MB / 1024 MB"],'''
    if old_rows not in text:
        raise SystemExit("找不到规则表行锚点")
    text = text.replace(old_rows, new_rows, 1)

    # 规则表那一页的副标题说明也要提一句注册防护
    text = text.replace(
        'slide = add_slide(prs, "可调防护规则", "所有防护都是运行时参数：改一行、重启即生效")',
        'slide = add_slide(prs, "可调防护规则", "17 条规则全部是运行时参数：改一行、重启即生效")',
        1)

    # 第 12 页（怎么证明它是对的）的测试数字要带上新增的 44 项
    text = text.replace('["Linux 端 9 个测试程序", "741 项检查，全部通过"]',
                        '["Linux 端 10 个测试程序", "785 项检查，全部通过"]', 1)
    text = text.replace('["Windows 端 15 个测试程序", "921 项检查，全部通过；构建 0 告警"]',
                        '["Windows 端 16 个测试程序", "966 项检查，全部通过；构建 0 告警"]', 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已加入注册防护页并同步规则条数与测试数字，行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
