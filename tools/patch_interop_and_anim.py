# ①互通矩阵：全部 6 个组合都已实测；②动画：从"每个元素一次点击"改成"分组出"。
#
# 两处都要改，原因不同：
#   矩阵：用户澄清"其他组合也测过了"，留灰格现在是**少写了**而不是诚实。
#   动画：用户说"不要每一个都是单个出来的" —— 逐条出来要按十几次点击，
#         演示时很烦。改成每组一次点击、组内并行淡入。
import io
import os

CHARTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_charts.py")
ANIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "add_ppt_animation.py")

# ---------------- ① 互通矩阵 ----------------
OLD_INTEROP = '''# 行 = 谁发，列 = 谁收。2 = 实测互通；这一轮没测的组合一律留灰，**不做假设**。
INTEROP_ROWS = ["Windows", "Linux", "macOS", "Android"]
INTEROP_COLS = ["Windows", "Linux", "macOS", "Android"]
INTEROP = {
    ("Windows", "macOS"): 2, ("macOS", "Windows"): 2,
    ("Linux", "macOS"): 2, ("macOS", "Linux"): 2,
    ("Android", "macOS"): 2, ("macOS", "Android"): 2,
}'''

NEW_INTEROP = '''# 行 = 谁发，列 = 谁收。用户实测：**四端两两组合全部互通**，所以不再留灰格。
# （早先只有 macOS 那几个格子是绿的，是因为当时只确认了那几个；
#   用户后来澄清"其他组合也测过了"，按事实补全。）
INTEROP_ROWS = ["Windows", "Linux", "macOS", "Android"]
INTEROP_COLS = ["Windows", "Linux", "macOS", "Android"]
INTEROP = {
    (row, col): 2
    for row in INTEROP_ROWS
    for col in INTEROP_COLS
    if row != col
}'''

OLD_NOTE = '''    d.text((80, 790),
           "已实测：macOS ←→ Windows / Linux / Android 双向互发，四端同时在线",
           font=f_note, fill=GREEN)
    d.text((80, 830),
           "灰色格表示这一轮没测 —— 标出来比统一涂绿更有用：下一个要补的地方一目了然",
           font=f_note, fill=GRAY)'''

NEW_NOTE = '''    d.text((80, 790),
           "四端两两组合全部实测互通：消息、附件都能双向送达，四端同时在线",
           font=f_note, fill=GREEN)
    d.text((80, 830),
           "同一个服务器、同一份协议与加密：四端任意两两之间都能直接对话",
           font=f_note, fill=GRAY)'''

OLD_TITLE = '    d.text((80, 40), "四端互通矩阵：实测过的组合", font=f_title, fill=NAVY)'
NEW_TITLE = '    d.text((80, 40), "四端互通矩阵：全部组合实测通过", font=f_title, fill=NAVY)'

# ---------------- ② 动画：分组出 ----------------
OLD_BUILD = '''def build_timing(shape_ids):
    """生成 <p:timing>：每个形状一次点击触发的淡入。"""
    if not shape_ids:
        return ""

    # 构建 build 列表：按段落级别动画需要 bldP，这里按整形状，用 bldGraphic 不需要
    bld = "".join(
        '<p:bldP spid="%s" grpId="0"/>' % shape_id for shape_id in shape_ids
    )

    # 每一层：一个点击触发的 par -> seq -> 一个动画 par
    click_groups = []
    node_id = 2  # 1 留给根节点
    for shape_id in shape_ids:
        click_groups.append(
            "<p:par>"
            '<p:cTn id="%d" fill="hold">'
            '<p:stCondLst><p:cond delay="indefinite"/></p:stCondLst>'
            "<p:childTnLst>"
            "<p:par>"
            '<p:cTn id="%d" fill="hold">'
            '<p:stCondLst><p:cond delay="0"/></p:stCondLst>'
            "<p:childTnLst>"
            "<p:par>"
            '<p:cTn id="%d" presetID="%s" presetClass="entr" presetSubtype="0" '
            'fill="hold" grpId="0" nodeType="clickEffect">'
            '<p:stCondLst><p:cond delay="0"/></p:stCondLst>'
            "<p:childTnLst>"
            "<p:set>"
            '<p:cBhvr>'
            '<p:cTn id="%d" dur="1" fill="hold">'
            '<p:stCondLst><p:cond delay="0"/></p:stCondLst>'
            "</p:cTn>"
            '<p:tgtEl><p:spTgt spid="%s"/></p:tgtEl>'
            '<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst>'
            "</p:cBhvr>"
            "<p:to><p:strVal val=\\"visible\\"/></p:to>"
            "</p:set>"
            "<p:animEffect transition=\\"in\\" filter=\\"fade\\">"
            '<p:cBhvr>'
            '<p:cTn id="%d" dur="400"/>'
            '<p:tgtEl><p:spTgt spid="%s"/></p:tgtEl>'
            "</p:cBhvr>"
            "</p:animEffect>"
            "</p:childTnLst>"
            "</p:cTn>"
            "</p:par>"
            "</p:childTnLst>"
            "</p:cTn>"
            "</p:par>"
            "</p:childTnLst>"
            "</p:cTn>"
            "</p:par>"
            % (node_id, node_id + 1, node_id + 2, FADE_PRESET_ID, node_id + 3, shape_id,
               node_id + 4, shape_id)
        )
        node_id += 5

    return ('''

NEW_BUILD = '''# 每页最多几次点击。多了演示时要按十几次，很烦；分组之后 2~3 次就出完。
MAX_CLICKS_PER_SLIDE = 3

# 组内每个形状的淡入依次错开一点点（毫秒）。问的是"不要一个个单独出来"，
# 所以组内**同时**开始，只留很小的错位让眼睛能跟上，不至于糊成一片。
GROUP_STAGGER_MS = 120


def _effect_nodes(shape_ids, node_id):
    """一组内所有形状的淡入，**并行**（同一层 par）—— 这就是"同时出来"。"""
    parts = []
    for offset, shape_id in enumerate(shape_ids):
        delay = offset * GROUP_STAGGER_MS
        parts.append(
            "<p:par>"
            '<p:cTn id="%d" presetID="%s" presetClass="entr" presetSubtype="0" '
            'fill="hold" grpId="0" nodeType="withEffect">'
            '<p:stCondLst><p:cond delay="%d"/></p:stCondLst>'
            "<p:childTnLst>"
            "<p:set>"
            "<p:cBhvr>"
            '<p:cTn id="%d" dur="1" fill="hold">'
            '<p:stCondLst><p:cond delay="0"/></p:stCondLst>'
            "</p:cTn>"
            '<p:tgtEl><p:spTgt spid="%s"/></p:tgtEl>'
            "<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst>"
            "</p:cBhvr>"
            '<p:to><p:strVal val="visible"/></p:to>'
            "</p:set>"
            '<p:animEffect transition="in" filter="fade">'
            "<p:cBhvr>"
            '<p:cTn id="%d" dur="400"/>'
            '<p:tgtEl><p:spTgt spid="%s"/></p:tgtEl>'
            "</p:cBhvr>"
            "</p:animEffect>"
            "</p:childTnLst>"
            "</p:cTn>"
            "</p:par>"
            % (node_id, FADE_PRESET_ID, delay, node_id + 1, shape_id, node_id + 2, shape_id)
        )
        node_id += 3
    return "".join(parts), node_id


def _split_evenly(items, groups):
    """尽量均匀地切成 groups 组（组数不能超过元素数）。"""
    groups = max(1, min(groups, len(items)))
    base = len(items) // groups
    extra = len(items) % groups
    out = []
    start = 0
    for index in range(groups):
        size = base + (1 if index < extra else 0)
        out.append(items[start:start + size])
        start += size
    return [group for group in out if group]


def build_timing(shape_ids, max_clicks=MAX_CLICKS_PER_SLIDE):
    """生成 <p:timing>：**分组**淡入 —— 每组一次点击，组内同时出现。

    为什么要分组：原先每个形状一次点击，一页十几条就要点十几次，
    演示和讲解都被打断。现在按顺序均分成最多 max_clicks 组。
    """
    if not shape_ids:
        return ""

    bld = "".join(
        '<p:bldP spid="%s" grpId="0"/>' % shape_id for shape_id in shape_ids
    )

    groups = _split_evenly(list(shape_ids), max_clicks)
    click_groups = []
    node_id = 3  # 1 = 根，2 = mainSeq
    for group_index, group in enumerate(groups):
        # 第一组随页面切换自动开始，后面每组等一次点击
        start_cond = ('<p:cond delay="0"/>' if group_index == 0
                      else '<p:cond delay="indefinite"/>')
        effects, node_id = _effect_nodes(group, node_id + 1)
        click_groups.append(
            "<p:par>"
            '<p:cTn id="%d" fill="hold">'
            '<p:stCondLst>%s</p:stCondLst>'
            "<p:childTnLst>"
            "<p>%s</p>"
            "</p:childTnLst>"
            "</p:cTn>"
            "</p:par>"
            % (node_id, start_cond, effects)
        )
        node_id += 1

    return ('''

# 老版本最后返回时的收尾（续接上面的 return (）


def main():
    # ---- ① 互通矩阵 ----
    with io.open(CHARTS, encoding="utf-8") as handle:
        charts = handle.read()
    for old, new, label in ((OLD_INTEROP, NEW_INTEROP, "矩阵数据"),
                            (OLD_NOTE, NEW_NOTE, "矩阵说明"),
                            (OLD_TITLE, NEW_TITLE, "矩阵标题")):
        if old not in charts:
            print("⚠ 图上锚点未匹配:", label)
        else:
            charts = charts.replace(old, new, 1)
            print("  ✅ 图:", label)
    with io.open(CHARTS, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(charts)

    # ---- ② 动画分组 ----
    with io.open(ANIM, encoding="utf-8") as handle:
        anim = handle.read()
    if OLD_BUILD not in anim:
        print("⚠ 动画脚本锚点未匹配 —— 可能已经改过")
    else:
        anim = anim.replace(OLD_BUILD, NEW_BUILD, 1)
        with io.open(ANIM, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(anim)
        print("  ✅ 动画已改成分组")


if __name__ == "__main__":
    main()
