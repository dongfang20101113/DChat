# 给 PPTX 注入切换动画与逐条淡入动画。
#
# 为什么需要这个脚本：python-pptx 的 API **不提供**动画（这是 PowerPoint 的
# 扩展内容，OPC 包里就是 slideN.xml 末尾的一段 <p:timing>）。所以流程是
# 先用 python-pptx 生成内容，再把这个脚本跑一遍，读改写 zip 里的 slide XML。
#
# 做两件事：
#   1. 每页加一个"淡出"切换（p:transition），翻页时不会硬切
#   2. 每页把标题和内容块按顺序做成"点击一次、出现一个"的淡入效果
#
# 只动 slideN.xml，不碰别的关系（rels / content types 都不需要改——
# p:transition 和 p:timing 都是 slide 自己的子元素）。
import os
import re
import shutil
import zipfile

NS_DECL = (
    'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
    'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" '
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
)

# 淡入：presetID 10 = Fade，presetClass = entr（进入）
FADE_PRESET_ID = "10"


def shape_ids_with_text(slide_xml):
    """按文档顺序取出"有文字或图片"的形状 id，跳过纯装饰的色块与背景。

    判断依据：<p:sp> 里有 <a:t> 的算内容；<p:pic> 一律算内容。
    纯色装饰块（标题栏、卡片底板）不应该逐个淡入——那会让每页点很多次。
    """
    ids = []
    # 按出现顺序扫所有 sp / pic
    for match in re.finditer(r"<p:(sp|pic)>.*?</p:\1>", slide_xml, re.S):
        block = match.group(0)
        id_match = re.search(r'<p:cNvPr id="(\d+)"', block)
        if not id_match:
            continue
        shape_id = id_match.group(1)
        if match.group(1) == "pic":
            ids.append(shape_id)
            continue
        # sp：有实际文字才算内容
        if "<a:t>" in block:
            ids.append(shape_id)
    return ids


def build_timing(shape_ids):
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
            "<p:to><p:strVal val=\"visible\"/></p:to>"
            "</p:set>"
            "<p:animEffect transition=\"in\" filter=\"fade\">"
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

    return (
        "<p:timing>"
        '<p:tnLst>'
        "<p:par>"
        '<p:cTn id="1" dur="indefinite" restart="never" nodeType="tmRoot">'
        "<p:childTnLst>"
        '<p:seq concurrent="1" nextAc="seek">'
        "<p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\">"
        "<p:childTnLst>"
        + "".join(click_groups)
        + "</p:childTnLst>"
        "</p:cTn>"
        '<p:prevCondLst><p:cond evt="onPrev" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst>'
        '<p:nextCondLst><p:cond evt="onNext" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst>'
        "</p:seq>"
        "</p:childTnLst>"
        "</p:cTn>"
        "</p:par>"
        "</p:tnLst>"
        '<p:bldLst>' + bld + "</p:bldLst>"
        "</p:timing>"
    )


TRANSITION = (
    '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
    '<mc:Choice xmlns:p14="http://schemas.microsoft.com/office/powerpoint/2010/main" Requires="p14">'
    '<p:transition spd="med" p14:dur="500">'
    '<p:fade/>'
    "</p:transition>"
    "</mc:Choice>"
    '<mc:Fallback>'
    '<p:transition spd="med"><p:fade/></p:transition>'
    "</mc:Fallback>"
    "</mc:AlternateContent>"
)


def inject(input_path, output_path, animate=True, transition=True):
    with zipfile.ZipFile(input_path) as source:
        names = source.namelist()
        payload = {name: source.read(name) for name in names}

    slide_names = sorted(
        [n for n in names if re.match(r"ppt/slides/slide\d+\.xml$", n)],
        key=lambda n: int(re.search(r"(\d+)", n.split("/")[-1]).group(1)),
    )

    animated = 0
    for name in slide_names:
        xml = payload[name].decode("utf-8")
        # 先去掉可能存在的旧动画，保证脚本可重复执行
        xml = re.sub(r"<p:timing>.*?</p:timing>", "", xml, flags=re.S)
        xml = re.sub(r"<p:transition[^>]*/>", "", xml)
        xml = re.sub(r"<p:transition.*?</p:transition>", "", xml, flags=re.S)

        prefix = ""
        if transition:
            prefix += TRANSITION
        if animate:
            ids = shape_ids_with_text(xml)
            prefix += build_timing(ids)
            if ids:
                animated += 1

        if prefix:
            # 必须插在 </p:sld> 之前；p:transition / p:timing 在 sld 里是有序的
            xml = xml.replace("</p:sld>", prefix + "</p:sld>")
        payload[name] = xml.encode("utf-8")

    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as target:
        for name in names:
            target.writestr(name, payload[name])
    return len(slide_names), animated


if __name__ == "__main__":
    import sys
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "dchat-项目介绍.pptx")
    dst = sys.argv[2] if len(sys.argv) > 2 else src
    total, animated = inject(src, dst)
    print("已注入动画：%d 页，其中 %d 页有逐条淡入" % (total, animated))
