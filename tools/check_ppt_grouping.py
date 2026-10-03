"""核对动画分组：每页几次点击、组内几个元素同时出。

为什么要专门核对：第一版把整组都写成 withEffect，
结果**一页 0 次点击、动画自动全播完** —— 那不是"分组"而是"全自动"。
光看"动画效果总数"发现不了这个问题，必须看 nodeType 的分布。
"""
import re
import sys
import zipfile

DECK = sys.argv[1] if len(sys.argv) > 1 else r"D:\codes\dchat\dchat-项目介绍.pptx"
CLICK = 'nodeType="clickEffect"'
WITH = 'nodeType="withEffect"'


def main():
    with zipfile.ZipFile(DECK) as archive:
        names = sorted(
            [n for n in archive.namelist() if re.match(r"ppt/slides/slide\d+\.xml$", n)],
            key=lambda s: int(re.search(r"(\d+)", s.split("/")[-1]).group(1)))

        print("文件:", DECK.split("\\")[-1])
        print("页数:", len(names))
        print()
        print("%-6s %-8s %-10s %-10s %s" % ("页", "点击组", "跟随效果", "要按几次", "说明"))
        total_clicks = 0
        worst = 0
        for name in names:
            xml = archive.read(name).decode("utf-8")
            clicks = xml.count(CLICK)
            follows = xml.count(WITH)
            index = int(re.search(r"(\d+)", name.split("/")[-1]).group(1))
            total_clicks += clicks
            # 第一组随页面切换自动开始，所以用户实际要按 clicks - 1 次
            presses = max(0, clicks - 1)
            worst = max(worst, presses)
            note = "第 1 组自动" if clicks >= 1 else "⚠ 没有任何点击组"
            print("%-6d %-8d %-10d %-10d %s" % (index, clicks, follows, presses, note))

        print()
        print("全 deck 点击组总数:", total_clicks)
        print("最坏一页要按:", worst, "次")
        if total_clicks == 0:
            print()
            print("⚠ 有问题：一个 clickEffect 都没有 —— 动画会全自动播完，"
                  "用户要的是「分组、一次点击出一组」")
            return 1
        if worst > 2:
            print()
            print("⚠ 偏多：最坏一页要按 %d 次，建议 ≤2" % worst)
            return 1
        print()
        print("✅ 分组正常：每页最多按 %d 次，组内元素同时淡入" % worst)
        return 0


if __name__ == "__main__":
    sys.exit(main())
