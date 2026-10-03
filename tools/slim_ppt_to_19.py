"""把 27 页压到 19 页。

合并策略：**同类页并成一张，不删内容**。具体：
  梗概 + 转折点      -> 一张（时间线 + 关键判断）
  安全设计 + 压测实测  -> 一张（四层防御 + 六行实测结果）
  连接上限 + 超时曲线  -> 一张（两张小图并排）
  注册缺口 + 可调规则  -> 一张（发现 -> 修复 -> 全部规则）
  协议设计            -> 并入加密设计（都属"底层约定"）

用户特别看重的四端互通 / 四端统一 / macOS 实测**原样保留**，
压测与攻击实测、注册防护、AI 复盘这些实质内容也都留着。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

# ---- 新的装配顺序：19 页 ----
OLD_ORDER = """    slide_cover(prs)
    slide_overview(prs)
    slide_arch(prs)
    slide_features(prs)
    slide_screens(prs)
    slide_protocol(prs)
    slide_crypto(prs)
    slide_three_contracts(prs)
    slide_server(prs)
    slide_timeline(prs)
    slide_phases(prs)
    slide_verification(prs)
    slide_sec_design(prs)
    slide_ddos(prs)
    slide_chart_conn(prs)
    slide_chart_timeout(prs)
    slide_reg_defense(prs)
    slide_perf_boundary(prs)
    slide_rules(prs)
    slide_unified(prs)
    slide_interop(prs)
    slide_macos(prs)
    slide_bugs(prs)
    slide_ai_intro(prs)
    slide_ai_failures(prs)
    slide_lessons(prs)
    slide_end(prs)"""

NEW_ORDER = """    # ---- 19 页（原 27 页，合并同类页；顺序按"是什么 -> 怎么做的 -> 实测 -> 复盘"）----
    slide_cover(prs)            # 1  封面
    slide_overview(prs)         # 2  定位 + 不是玩具的地方 + 代码规模
    slide_arch(prs)             # 3  四端架构 + 三处字节级约定
    slide_features(prs)         # 4  功能一览（四端对照表）
    slide_screens(prs)          # 5  界面实拍
    slide_crypto(prs)           # 6  加密设计（并入行式协议的选择理由）
    slide_server(prs)           # 7  服务端
    slide_timeline(prs)         # 8  开发过程梗概（并入转折点）
    slide_verification(prs)     # 9  怎么证明它是对的
    slide_sec_design(prs)       # 10 纵深防御（并入压测实测结果）
    slide_chart_conn(prs)       # 11 攻击对比曲线（连接上限 + 超时，两图并排）
    slide_reg_defense(prs)      # 12 注册路径：发现缺口 -> 修复 -> 规则（并入可调规则）
    slide_perf_boundary(prs)    # 13 性能实测与安全边界
    slide_interop(prs)          # 14 四端互通：全部组合实测通过
    slide_unified(prs)          # 15 四端统一：会话逻辑只有一处实现
    slide_macos(prs)            # 16 macOS 真机实测与验证边界
    slide_bugs(prs)             # 17 几个真实的 bug
    slide_ai_failures(prs)      # 18 AI 真实搞砸的地方（并入 AI 做了什么）
    slide_lessons(prs)          # 19 结论 + 结尾"""

# ---- 被并入的页函数：保留函数体，但不再装配 ----
MERGED_MARK = "# 【已并入其它页，不再单独装配】"


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if OLD_ORDER not in text:
        raise SystemExit("找不到装配顺序锚点（可能已经改过）")
    text = text.replace(OLD_ORDER, NEW_ORDER, 1)

    # 给被并入的页函数加标记，方便以后看出它们还在但没装配
    for name, note in (
        ("slide_protocol", "并入 slide_crypto：行式协议的选择理由"),
        ("slide_three_contracts", "并入 slide_arch 的底部注脚"),
        ("slide_phases", "并入 slide_timeline"),
        ("slide_ddos", "并入 slide_sec_design"),
        ("slide_chart_timeout", "并入 slide_chart_conn"),
        ("slide_rules", "并入 slide_reg_defense"),
        ("slide_ai_intro", "并入 slide_ai_failures"),
    ):
        marker = "def %s(prs):" % name
        if marker in text and (MERGED_MARK not in text.split(marker)[1][:200]):
            text = text.replace(
                marker, "%s %s —— %s\ndef %s(prs):" % (MERGED_MARK, "", note, name), 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("装配顺序已改成 19 页")
    print("已标记为「并入」的页：", "slide_protocol, slide_three_contracts, slide_phases, "
          "slide_ddos, slide_chart_timeout, slide_rules, slide_ai_intro")


if __name__ == "__main__":
    main()
