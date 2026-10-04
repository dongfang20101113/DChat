"""修正续传实现里的一个兼容性事故。

问题：服务端在 FILE_BEGIN 之后追加了第 4 格（续传起点），但 Windows 客户端的
解析写的是 `if (fields.size() != 3) return;` —— **要求恰好 3 格**。
结果：Windows 端会静默忽略 FILE_BEGIN，下载功能整个失效，而且不报错。

这还违反了协议自己写明的约定："加字段只要追加在末尾，老客户端读到旧的格数就停"。
Windows 端那个 != 3 属于潜在违规 —— 下一个想追加字段的人还会踩。

两处都修：
  1. 服务端**不再追加第 4 格**。客户端本来就知道自己请求的是哪个 offset
     （是它自己算出来发过去的），不需要服务器回显；而"服务器是否接受了这个
     offset"由 FILE_FAIL 负责表达（超出文件大小时会拒）。
  2. 客户端改成记住自己请求的 offset。
  3. 顺手把 Windows 端的 != 3 放宽成 >= 3，免得下次追加字段又静默坏掉。
"""
import io
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(REPO, "src", "server.cpp")
FILES_H = os.path.join(REPO, "client_core", "files.h")
FILES_CPP = os.path.join(REPO, "client_core", "files.cpp")
CLIENT = os.path.join(REPO, "src", "client.cpp")


def patch(path, edits, label):
    with io.open(path, encoding="utf-8", newline="") as handle:
        text = handle.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    ok = 0
    for old, new in edits:
        if old not in text:
            print("  ⚠ [%s] 未匹配: %s" % (label, old.strip().splitlines()[0][:56]))
            continue
        text = text.replace(old, new, 1)
        ok += 1
    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("  %s: %d/%d" % (os.path.basename(path), ok, len(edits)))


SERVER_OLD = '''        // FILE_BEGIN 第 4 格把续传起点回给客户端，它据此定位写指针。
        // 老客户端只读前三格，多出来的第 4 格会被忽略 —— 依旧向后兼容。
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size) +
                              " " + std::to_string(resumeFrom)));'''

SERVER_NEW = '''        // FILE_BEGIN **保持 3 格不变**。
        // 这里原本想追加第 4 格把续传起点回给客户端，但 Windows 客户端的解析是
        // `fields.size() != 3 -> return`（要求恰好 3 格），多一格会让它静默忽略
        // FILE_BEGIN、下载整个失效 —— 而且不报错，极难查。
        // 客户端本来就知道自己请求的 offset（是它自己算出来发过来的），不需要回显；
        // "服务器是否接受这个 offset" 由 FILE_FAIL 表达。
        // 教训：协议文档写着"加字段只追加在末尾、老客户端读到旧格数就停"，
        // 但**实现里可能有 `!= N` 这种更严的判断**，加字段前必须去客户端确认。
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size)));'''

FILES_H_OLD = '''    /** 断点续传用的中间文件路径（按附件 ID 定，所以重试能找回上次的进度）。 */
    std::string PartialPath(const std::string& id) const;'''

FILES_H_NEW = '''    /** 断点续传用的中间文件路径（按附件 ID 定，所以重试能找回上次的进度）。 */
    std::string PartialPath(const std::string& id) const;

    // 请求下载时算出的续传起点（附件 ID -> 字节数）。
    // 服务器不回显这个值，所以客户端自己记着；FILE_BEGIN 到达时取走。
    std::map<std::string, unsigned long long> pendingResume_;'''

FILES_CPP_REQ_OLD = '''    const unsigned long long have = FileSizeOrZero(PartialPath(id));
    const std::string request = have > 0 ? (id + " " + std::to_string(have)) : id;'''

FILES_CPP_REQ_NEW = '''    const unsigned long long have = FileSizeOrZero(PartialPath(id));
    // 记下来：服务器不会在 FILE_BEGIN 里回显这个值，FILE_BEGIN 到达时要靠它定位写指针
    pendingResume_[id] = have;
    const std::string request = have > 0 ? (id + " " + std::to_string(have)) : id;'''

FILES_CPP_BEGIN_OLD = '''        // 第 4 格是服务器回的续传起点（老服务器不发，缺省按 0 处理）
        unsigned long long resumeFrom = 0;
        if (fields.size() >= 4 && !ParseUint64(fields[3], &resumeFrom)) resumeFrom = 0;
        if (resumeFrom > total) resumeFrom = 0;  // 不合法就当从头下，别信这个数
'''

FILES_CPP_BEGIN_NEW = '''        // 续传起点用**我们自己请求时记下的值**，而不是服务器回显的 ——
        // FILE_BEGIN 保持 3 格不变，多一格会弄坏老客户端（见服务端那段注释）。
        unsigned long long resumeFrom = 0;
        {
            const auto pending = pendingResume_.find(fields[0]);
            if (pending != pendingResume_.end()) {
                resumeFrom = pending->second;
                pendingResume_.erase(pending);
            }
        }
        if (resumeFrom > total) resumeFrom = 0;  // 不合法就当从头下，别信这个数
'''

FILES_CPP_MISMATCH_OLD = '''            progress("续传起点不一致，重新完整下载：" + name);
            ::unlink(partPath.c_str());
            connection_->SendLine(BuildLine("FILE_GET", fields[0]));
            return true;  // 不注册 job：这轮旧流的数据会被忽略，等新的 FILE_BEGIN'''
FILES_CPP_MISMATCH_NEW = '''            progress("续传起点不一致，重新完整下载：" + name);
            ::unlink(partPath.c_str());
            pendingResume_[fields[0]] = 0;
            connection_->SendLine(BuildLine("FILE_GET", fields[0]));
            return true;  // 不注册 job：这轮旧流的数据会被忽略，等新的 FILE_BEGIN'''

CLIENT_OLD = '''        if (msg.command == "FILE_BEGIN") {
        // 服务器开始发数据：把文件建在 received\\ 里
        if (fields.size() != 3) return;'''
CLIENT_NEW = '''        if (msg.command == "FILE_BEGIN") {
        // 服务器开始发数据：把文件建在 received\\ 里
        // 用 >= 而不是 ==：协议约定"新增字段只追加在末尾，老客户端读到旧格数就停"，
        // 写成 == 3 的话，将来任何一次追加字段都会让这里静默失效（下载不报错地坏掉）。
        if (fields.size() < 3) return;'''


def main():
    print("修正续传的兼容性问题：")
    patch(SERVER, [(SERVER_OLD, SERVER_NEW)], "server.cpp")
    patch(FILES_H, [(FILES_H_OLD, FILES_H_NEW)], "files.h")
    patch(FILES_CPP, [
        (FILES_CPP_REQ_OLD, FILES_CPP_REQ_NEW),
        (FILES_CPP_BEGIN_OLD, FILES_CPP_BEGIN_NEW),
        (FILES_CPP_MISMATCH_OLD, FILES_CPP_MISMATCH_NEW),
    ], "files.cpp")
    patch(CLIENT, [(CLIENT_OLD, CLIENT_NEW)], "client.cpp")


if __name__ == "__main__":
    main()
