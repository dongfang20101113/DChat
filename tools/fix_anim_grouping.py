"""修动画：组里第一个应是 clickEffect（占掉这次点击），其余才是 withEffect。

第一版把整组都写成 withEffect，结果**一页 0 次点击、动画自动全播完**。
用户要的是"分组，一次点击出一组"，不是"全自动" —— 这个区别很关键。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "add_ppt_animation.py")

OLD = '''    parts = []
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
            '<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst>'
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
    return "".join(parts), node_id'''

NEW = '''    parts = []
    for offset, shape_id in enumerate(shape_ids):
        delay = offset * GROUP_STAGGER_MS
        # 组里第一个用 clickEffect：它"吃掉"这次点击；
        # 其余用 withEffect：跟着第一个一起出现，不再各自等一次点击。
        node_type = "clickEffect" if offset == 0 else "withEffect"
        parts.append(
            "<p:par>"
            '<p:cTn id="%d" presetID="%s" presetClass="entr" presetSubtype="0" '
            'fill="hold" grpId="0" nodeType="%s">'
            '<p:stCondLst><p:cond delay="%d"/></p:stCondLst>'
            "<p:childTnLst>"
            "<p:set>"
            "<p:cBhvr>"
            '<p:cTn id="%d" dur="1" fill="hold">'
            '<p:stCondLst><p:cond delay="0"/></p:stCondLst>'
            "</p:cTn>"
            '<p:tgtEl><p:spTgt spid="%s"/></p:tgtEl>'
            '<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst>'
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
            % (node_id, FADE_PRESET_ID, node_type, delay, node_id + 1, shape_id,
               node_id + 2, shape_id)
        )
        node_id += 3
    return "".join(parts), node_id'''


def main():
    with io.open(TARGET, encoding="utf-8") as handle:
        text = handle.read()
    if OLD not in text:
        raise SystemExit("找不到锚点（可能已改过）")
    text = text.replace(OLD, NEW, 1)
    # 顺手把 docstring 补上这次踩的坑
    text = text.replace(
        '"""一组内所有形状的淡入，**并行**（同一层 par）—— 这就是"同时出来"。"""',
        '"""一组内所有形状的淡入，**并行**（同一层 par）—— 这就是"同时出来"。\n\n'
        '    只有组里第一个是 clickEffect；整组都写 withEffect 的话会变成\n'
        '    "一页 0 次点击、动画自动全播完"，那不是"分组"而是"全自动"。\n'
        '    """', 1)
    with io.open(TARGET, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("已修：组内第一个改成 clickEffect")


if __name__ == "__main__":
    main()
