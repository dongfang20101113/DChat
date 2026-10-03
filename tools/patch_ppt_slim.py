# PPT 精简 + 加入攻击对比曲线页。
#
# 精简原则：**合并同主题页，不丢内容**。删页很容易，但把实测数字或边界说明
# 删掉就是硬伤——那些是这份材料里最有价值的部分。
#
#   - "AI 最有效的三个用法" -> 整页并入 "AI 做了什么"（本来就在讲同一件事）
#   - "吞吐与稳定性" + "边界说明" -> 合并成一页"性能实测与安全边界"
#   - 新增两页攻击对比曲线（连接数 vs 存活数 / 时间 vs 断开数）
import io
import os
import re

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

REMOVE_FUNCS = ["slide_ai_strength", "slide_throughput", "slide_claims"]

NEW_SLIDES = '''

def slide_chart_conn(prs):
    slide = add_slide(prs, "攻击对比曲线（一）：连接上限",
                      "目标连接数从 20 加到 300，存活数在 60 处硬性封顶")
    add_image_fit(slide, os.path.join(ASSETS, "chart-conn-limit.png"), 0.5, 1.18, 12.4, 4.35)
    card_text(slide, 0.6, 5.68, 6.0, 1.42, "读这张图要看什么", [
        "· 左侧蓝线在 60 处变成水平——上限是硬封顶，不是「大概拦一拦」",
        "· 右侧红线严格 1:1：超出多少就拒多少，一条不漏也不多拒",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=11.5)
    card_text(slide, 6.85, 5.68, 6.0, 1.42, "数据来源", [
        "· 8 个测试点各有实测记录（20/40/60/80/100/150/200/300）",
        "· 复现：tools/loadtest.cpp flood <条数>，配置见 linux/README.md",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=11.5)
    return slide


def slide_chart_timeout(prs):
    slide = add_slide(prs, "攻击对比曲线（二）：慢速耗尽的清理过程",
                      "30 条连上不登录的连接，在三种超时配置下全部被按时清掉")
    add_image_fit(slide, os.path.join(ASSETS, "chart-timeout.png"), 0.5, 1.18, 12.4, 4.35)
    card_text(slide, 0.6, 5.68, 6.0, 1.42, "读这张图要看什么", [
        "· 三条阶跃线精确落在设定值上（5 / 10 / 20 秒），没有延迟漂移",
        "· 无论配置多长最终都清零——攻击者拿不到「一直占着」的结果",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=11.5)
    card_text(slide, 6.85, 5.68, 6.0, 1.42, "服务端的处理方式", [
        "· 关闭前先回一条 ERROR「太久没有登录，连接已关闭」，用户看得懂",
        "· 每次清理都进日志：实测一轮出现 72 次 handshake timeout",
    ], accent=RED, fill=RED_L, title_size=13, body_size=11.5)
    return slide


def slide_perf_boundary(prs):
    """把「吞吐与稳定性」和「安全边界」合并成一页：数字和边界放一起才不会被误读。"""
    slide = add_slide(prs, "性能实测与安全边界",
                      "左边是能做到的，右边是明确做不到的——两栏都要看")
    card_text(slide, 0.62, 1.28, 6.0, 2.65, "性能与稳定性（实测）", [
        "· 16 客户端持续压测：处理 474,296 条消息，墙钟 2.4 秒",
        "· 服务端 CPU 17 秒（多核并行，广播式分发）",
        "· 空载内存 9.2 MB；攻击 + 压测全程 9.8–10.8 MB",
        "· 全程零崩溃、零重启；单文件静态二进制，无外部依赖",
        "· 200 条并发建连 6 ms（本机回环，非广域网指标）",
        "· 吞吐与服务端日志逐条计数核对过，不是写缓冲假象",
    ], accent=GREEN, fill=GREEN_L, title_size=15, body_size=11.5)
    card_text(slide, 6.72, 1.28, 6.0, 2.65, "安全边界（明确做不到）", [
        "· 主动中间人：裸 ECDH 没有身份认证，能劫持线路的人理论上",
        "  可以分别和两端握手——要挡它需要证书或预共享密钥",
        "· 网络层大流量 DDoS：本项目只做应用层防护，流量清洗要靠",
        "  上游运营商 / 云清洗 / CDN",
        "· 端到端加密：服务端能解密消息（它要转发、要存历史）",
        "· 密码找回、多设备同步、消息签名——都没有",
    ], accent=RED, fill=RED_L, title_size=15, body_size=11.5)
    card_text(slide, 0.62, 4.12, 12.1, 2.2, "为什么两栏要放在同一页", [
        "· 单看左边会以为「这服务器打不死」，单看右边会以为「防护很弱」——都不对",
        "· 实测数字证明的是「规则按设计生效」，不是「扛得住任何流量」",
        "· 安全材料里最危险的不是「防护少」，而是「你以为防护了很多」；",
        "  使用者按错误假设去用，风险比明说大得多",
        "· 这不是事后补的：项目从第一版加密起就在 crypto.h 里写着「不保护主动中间人」，README 照抄同一句",
\1
    return slide

'''

OLD_ORDER = """    slide_sec_design(prs)
    slide_ddos(prs)
    slide_throughput(prs)
    slide_rules(prs)
    slide_claims(prs)
    slide_bugs(prs)
    slide_ai_intro(prs)
    slide_ai_strength(prs)
    slide_ai_failures(prs)"""

NEW_ORDER = """    slide_sec_design(prs)
    slide_ddos(prs)
    slide_chart_conn(prs)
    slide_chart_timeout(prs)
    slide_rules(prs)
    slide_bugs(prs)
    slide_ai_intro(prs)
    slide_ai_failures(prs)"""


def drop_function(text, name):
    pattern = re.compile(r"\ndef " + re.escape(name) + r"\(prs\):.*?(?=\ndef |\Z)", re.S)
    new_text, count = pattern.subn("\n", text)
    if count == 0:
        raise SystemExit("找不到函数 " + name)
    return new_text


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "slide_chart_conn" in text:
        raise SystemExit("曲线页已存在，无需重复打补丁")

    for name in REMOVE_FUNCS:
        text = drop_function(text, name)
        print("已移除整页函数:", name)

    # 合并页 + 两页曲线，插在 slide_ddos 之后
    anchor = "\ndef slide_rules(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_rules 锚点")
    text = text.replace(anchor, NEW_SLIDES + "\ndef slide_rules(prs):", 1)

    if OLD_ORDER not in text:
        raise SystemExit("找不到 main() 装配顺序锚点")
    text = text.replace(OLD_ORDER, NEW_ORDER, 1)

    # 合并页要进装配顺序（放在规则页之前）
    text = text.replace("    slide_chart_timeout(prs)\n    slide_rules(prs)",
                        "    slide_chart_timeout(prs)\n    slide_perf_boundary(prs)\n"
                        "    slide_rules(prs)", 1)

    body = text.split("def main()")[1]
    for required in ["slide_rules(prs)", "slide_perf_boundary(prs)", "slide_chart_conn(prs)",
                     "slide_chart_timeout(prs)", "slide_bugs(prs)", "slide_ai_failures(prs)"]:
        if required not in body:
            raise SystemExit("装配顺序里缺了 " + required)

    # 把「AI 最有效的三个用法」的内容并入 AI 概述页的最后一张卡片
    old_card = '''    card_text(slide, 0.62, 4.1, 12.1, 2.25, "一个坦诚的比例估计", [
        "· 代码字数、注释、测试、文档、构建与验证脚本：绝大部分由 AI 产出",
        "· 方向、取舍、验收标准、以及「什么算做完了」：全部由人决定",
        "· 这个项目里 AI 更像一个执行力很强的实现者 + 一个不会累的验证者，",
        "  而不是决策者。决策错了的时候（比如界面改版方向），返工成本也是真实的。",
    ], accent=AMBER, fill=AMBER_L, body_size=13)'''
    new_card = '''    card_text(slide, 0.62, 3.95, 6.0, 2.4, "AI 最有效的三个用法", [
        "① 写可被测试的纯逻辑：解析、判定、格式化全部抽成纯函数，",
        "   一有测试，后面每次修改都能立刻验证",
        "② 交叉核对而不是相信「应该对」：写探针程序把中间量打出来",
        "   逐字节 diff，三端一致的三个坑就是这么找出来的",
        "③ 顺着日志往下挖：加临时日志、看服务端记录、用 strace 看",
        "   实际发出去的字节，好几个 bug 几分钟内定位",
    ], accent=GREEN, fill=GREEN_L, title_size=15, body_size=11.5)
    card_text(slide, 6.78, 3.95, 5.95, 2.4, "一个坦诚的比例估计", [
        "· 代码、注释、测试、文档、构建与验证脚本：绝大部分由 AI 产出",
        "· 方向、取舍、验收标准、以及「什么算做完了」：全部由人决定",
        "· AI 更像执行力很强的实现者 + 不会累的验证者，而不是决策者。",
        "  决策错了的时候（比如界面改版方向），返工成本也是真实的。",
    ], accent=AMBER, fill=AMBER_L, title_size=15, body_size=11.5)'''
    if old_card in text:
        text = text.replace(old_card, new_card, 1)
        print("已把「AI 三个用法」并入 AI 概述页")
    else:
        raise SystemExit("找不到 AI 概述页的卡片锚点")

    # AI 概述页原来那张卡占 0.62~4.62，现在两栏从 3.95 开始，标题区够放
    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("补丁完成，当前行数:", len(text.splitlines()))


if __name__ == "__main__":
    main()
