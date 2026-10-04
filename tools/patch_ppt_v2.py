"""把三项新功能（历史落盘 / 离线消息 / 断点续传）写进 PPT。

只改必要的地方：
  - 规则条数 17 -> 18，规则表补一行 offlinemessages
  - 验证页的测试数字按实测更新（Windows 966->1025 / 17 个程序；Linux 785->892 / 12 个程序）
  - 总览页的合计 2115 -> 2231
  - 新增一页专讲这三项，含"为什么这样设计"的取舍
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_ppt.py")

NEW_SLIDE = '''

def slide_v2(prs):
    """三项新能力：历史落盘 / 离线消息 / 断点续传。"""
    slide = add_slide(prs, "补上三项能力：历史落盘 · 离线消息 · 断点续传",
                      "都是「以前直接丢掉」的东西：重启后记录没了、你不在线消息没了、断了要重下")
    card_text(slide, 0.62, 1.25, 3.95, 2.9, "① 聊天历史落盘", [
        "· 原来只在内存（std::deque），重启即丢",
        "· 改成追加写 + 上限压实 + 每秒节流 flush",
        "· **崩溃写了一半的那行会被丢弃**（宁可少一条，",
        "   不能凭空多一条残缺消息）",
        "· 序号持久化：离线补发靠它判断谁还没看过",
    ], accent=BLUE, fill=BLUE_L, title_size=13, body_size=10.5)
    card_text(slide, 4.72, 1.25, 3.95, 2.9, "② 离线消息", [
        "· 每用户记「读到第几条」，存在 dchat-seen.txt",
        "· 重新登录只补他错过的，条数由规则限制",
        "· 与 keepchathistory **不重复**：有进度的只补差额，",
        "   新用户仍看完整历史",
        "· 默认关闭，升级后行为完全不变",
    ], accent=GREEN, fill=GREEN_L, title_size=13, body_size=10.5)
    card_text(slide, 8.82, 1.25, 3.95, 2.9, "③ 文件断点续传", [
        "· FILE_GET <id> [已有字节数]，服务器从那儿接着发",
        "· 客户端先写 .part 中间文件，下完才改名",
        "· 起点对不上就丢弃重来——绝不硬续",
        "   （那会拼出大小对、内容坏的文件）",
        "· 超出文件大小直接拒绝，不从头再发一遍",
    ], accent=AMBER, fill=AMBER_L, title_size=13, body_size=10.5)
    card_text(slide, 0.62, 4.3, 12.1, 1.5, "实测（真机，不是估算）", [
        "· 历史：kill -9 强杀后重启，记录仍在（loaded 1 history line …末序号 1）；"
        "手工塞入半行记录被正确丢弃",
        "· 离线：A 下线期间 B 发两条，A 重登收到「你不在的时候有 2 条消息」，"
        "且**不重复**自己在线时那条；默认规则下不补发",
        "· 续传：64 KB 文件下到 46.0 KB 时掐断，重连后服务端日志 "
        "「从 46.0 KB 处续传」，**最终文件 md5 与源文件完全一致**",
    ], accent=NAVY, fill=(0xF2, 0xF5, 0xFA), title_size=13, body_size=11)
    card_text(slide, 0.62, 5.95, 12.1, 1.3, "一个差点造成事故的兼容性问题（值得记）", [
        "· 我原本让服务器在 FILE_BEGIN 后追加一格回显续传起点，看起来完全合理",
        "· 但 Windows 客户端写的是 `fields.size() != 3` —— **要求恰好 3 格**，"
        "多一格会让它静默忽略 FILE_BEGIN、下载整个失效且不报错",
        "· 改成不回显（客户端自己记着请求的 offset），并顺手把那个 != 3 放宽成 < 3 —— "
        "否则**下一个追加字段的人还会踩**",
    ], accent=RED, fill=RED_L, title_size=13, body_size=11)
    return slide

'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()

    if "slide_v2" in text:
        raise SystemExit("v2 页已存在")

    # 规则条数
    text = text.replace('["规则（共 17 条，全部是运行时参数）", "作用", "默认"],',
                        '["规则（共 18 条，全部是运行时参数）", "作用", "默认"],', 1)
    text = text.replace('slide = add_slide(prs, "可调防护规则", "17 条规则全部是运行时参数：改一行、重启即生效")',
                        'slide = add_slide(prs, "可调防护规则", "18 条规则全部是运行时参数：改一行、重启即生效")', 1)

    # 规则表补一行
    text = text.replace(
        '        ["maxtextlen / maxtextlines / chatinterval", "文本长度 / 行数 / 发言间隔", "0"],',
        '        ["maxtextlen / maxtextlines / chatinterval", "文本长度 / 行数 / 发言间隔", "0"],\n'
        '        ["offlinemessages", "重登录时最多补发多少条离线消息", "0（不补发）"],', 1)

    # 验证页数字
    text = text.replace('["Windows 端 16 个测试程序", "966 项检查，全部通过；构建 0 告警"],',
                        '["Windows 端 17 个测试程序", "1025 项检查，全部通过；构建 0 告警"],', 1)
    text = text.replace('["Linux 端 10 个测试程序", "785 项检查，全部通过"],',
                        '["Linux 端 12 个测试程序", "892 项检查，全部通过"],', 1)
    text = text.replace('"· 有测试：四端合计 2115 项检查（C++ 1787 + 安卓 328）",',
                        '"· 有测试：四端合计 2231 项检查（C++ 1903 + 安卓 328）",', 1)
    # 封面与结尾的合计
    text = text.replace('2115 项测试全过', '2231 项测试全过')
    text = text.replace('2115 项测试检查', '2231 项测试检查')

    # 装配：插在"四端互通"之前（功能讲完再讲验证）
    anchor = "\ndef slide_interop(prs):"
    if anchor not in text:
        raise SystemExit("找不到 slide_interop 锚点")
    text = text.replace(anchor, NEW_SLIDE + anchor, 1)
    order_old = "    slide_reg_defense(prs)      # 12 注册路径"
    if order_old not in text:
        raise SystemExit("找不到装配顺序锚点")
    text = text.replace(order_old,
                        "    slide_v2(prs)              # 补上的三项能力\n" + order_old, 1)

    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("PPT 已更新：新增一页 + 规则 18 条 + 测试数字")


if __name__ == "__main__":
    main()
