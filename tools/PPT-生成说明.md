# PPT 的生成流水线

这份 PPT 不是手工拖出来的，是脚本生成的——**数据和图都来自仓库里的实测**，
改一个字重新跑一遍即可，不会出现"PPT 里的数字和代码对不上"。

## 四步生成

```bash
py=tools/../python    # 用 python-pptx 跑，见下面的依赖说明

python tools/make_ppt_diagrams.py    # 1. 两张示意图（三端架构、加密握手时序）
python tools/make_charts.py          # 2. 两张对比曲线（连接上限、慢速耗尽清理）
python tools/make_ppt.py             # 3. 生成 PPT 正文（23 页）
python tools/add_ppt_animation.py \
    dchat-项目介绍.pptx dchat-项目介绍-动画版.pptx   # 4. 注入切换与逐条动画
```

依赖：`python-pptx`（生成）、`Pillow`（画图）。两个都在工作区自带的运行时里。

## 为什么动画要单独一步

**python-pptx 的 API 不提供动画**——动画是 PowerPoint 的扩展内容，在 OPC 包里就是
`slideN.xml` 末尾的一段 `<p:timing>`。所以流程拆成两段：先正常生成内容，
再由 `add_ppt_animation.py` 以"读改写 zip"的方式把动画 XML 注入每个 slide。

注入的内容：

| 元素 | 效果 |
| --- | --- |
| `<p:transition><p:fade/>` | 每页翻页时淡出淡入（0.5 秒），不再硬切 |
| `<p:timing>` 里的 `presetClass="entr"` | 标题和每个内容块**点击一次出现一个**，淡入 0.4 秒 |

脚本会跳过纯装饰形状（标题栏、卡片底板、色块）——只让**有文字或图片**的形状
逐个出现，否则一页要点十几次。判断依据是形状里有没有 `<a:t>`。

脚本可重复执行：注入前会先清掉旧的 `<p:timing>` / `<p:transition>`。

## 图表的数据从哪来

`make_charts.py` 里的数字是**手抄自实测结果**的常量（不是从服务端动态读的）：

- 连接上限曲线：8 个测试点，目标连接数 20/40/60/80/100/150/200/300，
  对应存活 20/40/60/60/60/60/60/60、被拒 0/0/0/20/40/90/140/240
- 慢速耗尽曲线：`handshaketimeout` 分别设 5/10/20 秒，每次 30 条未登录连接，
  三组配置下都是 30/30 被按时清掉

复现这两组数据的命令在 `linux/README.md` 的「安全与压力测试」章节。
**如果改了服务端的防护逻辑，这两张图必须重新采集、手工更新常量**——
脚本不会自己去测（那会让生成 PPT 变成一次压测，太重）。

## 已知的坑

1. **PowerPoint 开着文件时写不进去**（PermissionError）。生成到临时文件再拷，
   或者先关掉 PowerPoint。`make_ppt.py` 支持 `DCHAT_PPT_OUT` 环境变量指定输出路径。
2. `make_ppt.py` 里的素材路径是**相对脚本自身**定位仓库根的，不是相对当前目录——
   之前从这个坑里掉过一次（从别的目录调用就找不到图）。
3. 用 PowerShell 的 `Replace` 改这些脚本时小心：`.NET` 的替换会把 `$1` 当反向引用，
   我就是这样把一行代码改成 `\1` 过。改完立刻 `python -m py_compile` 检查一次。
