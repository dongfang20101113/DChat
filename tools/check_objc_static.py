# 对无法编译的文件做能做的静态检查。
#
# 背景：macOS 客户端的 main.mm 在本机编不了（没有 Foundation/AppKit 头，zig 也不带
# framework 头）。但"编不了"不等于"什么都查不了"——下面这些错误类型是可以在
# 文本层面查出来的，而且恰好是最容易犯、最难在 Mac 上一次性发现的：
#
#   1. 调用了自己没有实现的 selector（编译报 warning，运行直接 unrecognized selector 崩）
#   2. 括号不配对（我这种长文件最容易漏）
#   3. 用了 C++ 侧不存在的类型/函数名（对照 chat_core.h 的声明）
#   4. 混进 GNU 扩展写法（clang 在 macOS 上未必接受）
#
# 这个脚本不是编译器，只能减少错误，不能替代在 Mac 上真编一次。
import io
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MM = os.path.join(REPO, "macos", "main.mm")
CORE_H = os.path.join(REPO, "client_core", "chat_core.h")


def read(path):
    with io.open(path, encoding="utf-8") as handle:
        return handle.read()


def main():
    if not os.path.exists(MM):
        print("找不到", MM)
        return 1
    text = read(MM)
    problems = []

    # ---- 1) selector 实现 vs 调用 ----
    # 实现：^[-+] (返回类型)? (selector 各部分)  取选择器时把参数占位去掉
    defined = set()
    for match in re.finditer(r"^\s*[-+]\s*\([^)]*\)\s*([A-Za-z_][A-Za-z0-9_]*)", text, re.M):
        defined.add(match.group(1))
    # 多段选择器：继续抓后面的 "label:(...)"
    for match in re.finditer(r"^\s*[-+]\s*\([^)]*\)\s*((?:[A-Za-z_][A-Za-z0-9_]*\s*:\s*\([^)]*\)"
                             r"[A-Za-z_][A-Za-z0-9_]*\s*)+)", text, re.M):
        parts = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*:", match.group(1))
        if parts:
            defined.add(parts[0])
            defined.add("".join(p.capitalize() if i else p for i, p in enumerate(parts)))

    # 调用：[obj selector:...] 或 [obj selector]
    called = set()
    for match in re.finditer(r"\[\s*[A-Za-z_][A-Za-z0-9_.\[\]\s]*?\s+([A-Za-z_][A-Za-z0-9_]*)\s*[:\]]",
                             text):
        called.add(match.group(1))

    # Cocoa / Foundation 自带的 selector 白名单。
    # 只列**本文件真正用到**的那些 —— 白名单越长越容易把"漏实现的自定义方法"
    # 也一起放过，那就失去意义了。
    FRAMEWORK_OK = {
        # 构造
        "alloc", "init", "initWithFrame", "initWithContentRect", "initWithString",
        "initWithAttributedString", "checkboxWithTitle", "buttonWithTitle",
        "stringWithFormat", "stringWithUTF8String", "stringByAppendingString",
        "stringByAppendingPathComponent", "addItem", "addItemWithTitle",
        # 应用与窗口
        "sharedApplication", "setActivationPolicy", "activateIgnoringOtherApps", "run",
        "terminate", "setMainMenu", "setSubmenu", "center", "makeKeyAndOrderFront",
        "makeFirstResponder", "orderOut", "contentView", "setContentView",
        "setFrame", "setFrameOrigin", "setFrameSize", "setTitle",
        # 视图
        "addSubview", "setAutoresizingMask", "setHasVerticalScroller", "setDocumentView",
        "setEditable", "setRichText", "setSelectable", "setBezeled", "setDrawsBackground",
        "setFont", "setTextColor", "setTextContainerInset", "setLineBreakMode",
        "setStringValue", "setPlaceholderString", "setEnabled", "setKeyEquivalent",
        "setState", "state", "setDelegate", "setString", "appendAttributedString",
        "addAttributes", "scrollRangeToVisible", "setUserInfo", "objectForKey",
        # 字体与颜色（这些正是第一版误报的那批）
        "boldSystemFontOfSize", "systemFontOfSize", "monospacedSystemFontOfSize",
        "labelColor", "secondaryLabelColor", "systemRedColor", "systemTealColor",
        "systemOrangeColor", "colorWithSRGBRed",
        # 文件
        "defaultManager", "createDirectoryAtPath", "firstObject",
        # 取值
        "intValue", "length", "unsignedIntegerValue",
    }
    KNOWN_OK = {*defined, *FRAMEWORK_OK}
    for name in sorted(called - KNOWN_OK):
        # 只有当名字看起来像自定义方法（不是 Cocoa 动词开头）时才提醒，
        # 避免把框架方法误报成问题
        if name[0].islower() and not name.startswith(("set", "is", "add", "string", "int",
                                                      "unsigned", "object", "scroll", "append",
                                                      "init", "make", "order", "run", "terminate",
                                                      "create", "content", "first", "length",
                                                      "state", "checkbox", "button", "shared",
                                                      "activate")):
            problems.append("调用了可能未实现的 selector: -%s" % name)

    # ---- 2) 括号平衡 ----
    for open_ch, close_ch, label in (("{", "}", "花括号"), ("[", "]", "方括号"), ("(", ")", "圆括号")):
        if text.count(open_ch) != text.count(close_ch):
            problems.append("%s 不配对：%d 个 %s vs %d 个 %s"
                            % (label, text.count(open_ch), open_ch, text.count(close_ch), close_ch))

    # ---- 3) 用到的 chat_core 成员是否在头文件里声明 ----
    if os.path.exists(CORE_H):
        header = read(CORE_H)
        for member in ("Connect", "Disconnect", "SendMessage", "SubmitInput", "Complete",
                       "SetKnownServersPath", "ColorEnabled", "SetColorEnabled", "Files",
                       "SelfNick", "OnlineNicks", "Host", "Port", "Connected"):
            if member not in header:
                problems.append("chat_core.h 里没有声明成员: %s（main.mm 用到了）" % member)
        for type_name in ("ChatCore", "ChatCoreDelegate", "ChatMessage", "ConnectOptions",
                          "TrustDecision", "ColorSegment"):
            if type_name not in header and not os.path.exists(
                    os.path.join(REPO, "client_core", "chat_color.h")):
                problems.append("chat_core.h 里没有类型: %s" % type_name)

    # ---- 4) GNU 扩展写法 ----
    if re.search(r"\?:\s*\"", text):
        problems.append("用了 GNU 的 ?: 省略中间操作数写法，改成显式判断更稳")
    if "#import <GNUstep" in text:
        problems.append("引了 GNUstep 头，macOS 上应该是 Cocoa 头")

    # ---- 5) 必须有的东西 ----
    for required in ("int main(", "NSApplication", "@autoreleasepool", "dispatch_async",
                     "dispatch_get_main_queue"):
        if required not in text:
            problems.append("缺少 %s —— 检查是不是漏了界面主循环/线程切换" % required)

    print("检查文件:", os.path.relpath(MM, REPO))
    print("  实现的 selector 数:", len(defined))
    print("  调用点 selector 数:", len(called))
    print()
    if problems:
        print("发现 %d 处需要人工确认：" % len(problems))
        for item in problems:
            print("  ⚠", item)
        return 1
    print("✅ 文本层面没查出问题（注意：这只减少错误，不能替代在 Mac 上真编一次）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
