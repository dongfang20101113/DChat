# 验证动画注入结果：每页是否有切换动画、有几个逐条淡入效果。
import re
import sys
import zipfile

path = sys.argv[1] if len(sys.argv) > 1 else r"D:\codes\dchat\build\ppt-anim.pptx"
z = zipfile.ZipFile(path)
names = z.namelist()
slides = sorted(
    [n for n in names if re.match(r"ppt/slides/slide\d+\.xml$", n)],
    key=lambda n: int(re.search(r"(\d+)", n.split("/")[-1]).group(1)),
)

ENTR = 'presetClass="entr"'
with_transition = 0
with_timing = 0
total = 0
print("%-6s %-10s %-8s %s" % ("页码", "切换动画", "淡入条数", "状态"))
for index, name in enumerate(slides, 1):
    xml = z.read(name).decode("utf-8")
    has_transition = "<p:transition" in xml
    effects = xml.count(ENTR)
    ok = has_transition and effects > 0
    if has_transition:
        with_transition += 1
    if "<p:timing>" in xml:
        with_timing += 1
    total += effects
    if index <= 3 or index >= len(slides) - 1 or not ok:
        print("%-6d %-10s %-8d %s" % (index, "有" if has_transition else "无", effects,
                                      "OK" if ok else "**缺**"))

print()
print("总页数        : %d" % len(slides))
print("有切换动画    : %d" % with_transition)
print("有逐条动画    : %d" % with_timing)
print("动画效果总数  : %d" % total)
print("平均每页条数  : %.1f" % (total / max(1, len(slides))))
