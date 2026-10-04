"""第 5 项：离线消息。先加规则 offlinemessages（第 17 → 18 条）。"""
import io
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = os.path.join(REPO, "src", "server_rules.h")
CPP = os.path.join(REPO, "src", "server_rules.cpp")

H_OLD = '''    int registerIntervalSec = 0;  // registerinterval：同一 IP 两次注册的最小间隔（秒），0 = 不限制
    int maxAccounts = 0;          // maxaccounts：账号总数上限，0 = 不限制
};'''

H_NEW = '''    int registerIntervalSec = 0;  // registerinterval：同一 IP 两次注册的最小间隔（秒），0 = 不限制
    int maxAccounts = 0;          // maxaccounts：账号总数上限，0 = 不限制

    // ---- 2026-10 新增：离线消息 ----
    // 别人说话时你不在线，那些消息以前是**直接丢掉**的。现在服务端把聊天记录
    // 落盘并记住每个人读到第几条，重新登录时把错过的补上。
    //
    // 默认 0 = 不补发：这是刻意的，升级后行为和以前完全一样（本项目一贯的约定）。
    // 要开就 /chatrule offlinemessages set 100。
    int offlineMessages = 0;  // offlinemessages：登录时最多补发多少条，0 = 不补发
};'''

CPP_EDITS = [
    # 取值范围表
    ('''    {"maxaccounts", 0, 10000000, "个", "账号总数上限（0 = 不限制）"},''',
     '''    {"maxaccounts", 0, 10000000, "个", "账号总数上限（0 = 不限制）"},
    // ---- 2026-10 新增：离线消息 ----
    {"offlinemessages", 0, 1000, "条", "登录时最多补发多少条离线消息（0 = 不补发）"},'''),

    # 规则信息（Tab 补全用）
    ('''        {"maxaccounts", "<个> 账号总数上限，0 = 不限", false},''',
     '''        {"maxaccounts", "<个> 账号总数上限，0 = 不限", false},
        {"offlinemessages", "<条> 登录时最多补发多少条离线消息，0 = 不补", false},'''),

    # 描述
    ('''    if (lower == "maxaccounts") {
        return "maxaccounts = " + std::to_string(rules.maxAccounts) +
               " 个（账号总数上限，0 = 不限制）";
    }''',
     '''    if (lower == "maxaccounts") {
        return "maxaccounts = " + std::to_string(rules.maxAccounts) +
               " 个（账号总数上限，0 = 不限制）";
    }
    if (lower == "offlinemessages") {
        return "offlinemessages = " + std::to_string(rules.offlineMessages) +
               " 条（重新登录时最多补发多少条离线期间错过的消息，0 = 不补发）";
    }'''),

    # 取当前值（add/remove 语义要用）
    ('''    if (lower == "maxaccounts") current = rules->maxAccounts;''',
     '''    if (lower == "maxaccounts") current = rules->maxAccounts;
    if (lower == "offlinemessages") current = rules->offlineMessages;'''),

    # 赋值
    ('''    if (lower == "maxaccounts") rules->maxAccounts = static_cast<int>(next);''',
     '''    if (lower == "maxaccounts") rules->maxAccounts = static_cast<int>(next);
    if (lower == "offlinemessages") rules->offlineMessages = static_cast<int>(next);'''),

    # 文件解析
    ('''        if (name == "maxaccounts") parsed.maxAccounts = static_cast<int>(number);''',
     '''        if (name == "maxaccounts") parsed.maxAccounts = static_cast<int>(number);
        if (name == "offlinemessages") parsed.offlineMessages = static_cast<int>(number);'''),

    # 序列化
    ('''    out += "maxaccounts " + std::to_string(rules.maxAccounts) +
           "      # 账号总数上限，0 = 不限制（换 IP 也绕不过这道闸）\\n";''',
     '''    out += "maxaccounts " + std::to_string(rules.maxAccounts) +
           "      # 账号总数上限，0 = 不限制（换 IP 也绕不过这道闸）\\n";
    out += "offlinemessages " + std::to_string(rules.offlineMessages) +
           "      # 登录时最多补发多少条离线消息，0 = 不补发（要先开 keepchathistory）\\n";'''),
]


def patch(path, edits):
    with io.open(path, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    ok = 0
    for old, new in edits:
        if old not in text:
            print("  ⚠ 未匹配:", old.strip().splitlines()[0][:58])
            continue
        text = text.replace(old, new, 1)
        ok += 1
    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("  %s: %d/%d" % (os.path.basename(path), ok, len(edits)))


def main():
    print("加 offlinemessages 规则：")
    patch(H, [(H_OLD, H_NEW)])
    patch(CPP, CPP_EDITS)


if __name__ == "__main__":
    main()
