"""第 7 项：文件断点续传。

设计（三处，协议向后兼容）：
  1. protocol：加 ParseUint64（服务端与客户端都要解析 offset）
  2. 服务端：FILE_GET <id> [已有字节数] -> FILE_BEGIN 带上起点，从那里接着发
  3. 客户端：下载先写 `.part` 中间文件；中断后重试时把 `.part` 的长度当作起点；
     下完再改名成最终文件名

为什么客户端要引入 `.part` 而不是直接续写到目标文件：
  原来的实现每次下载都用 O_TRUNC + 取一个"不重名"的新路径（文件 (2).txt）。
  续传要求"同一个文件、同一个 fd、写指针在中间" —— 直接写目标文件的话，
  用户会在目录里看到半截文件，而且重试又生成一个新名字，永远拼不上。
  `.part` 是标准做法：没下完就不出现在最终位置，下完一次改名。
"""
import io
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTO_H = os.path.join(REPO, "src", "protocol.h")
PROTO_CPP = os.path.join(REPO, "src", "protocol.cpp")
SERVER = os.path.join(REPO, "src", "server.cpp")
FILES_H = os.path.join(REPO, "client_core", "files.h")
FILES_CPP = os.path.join(REPO, "client_core", "files.cpp")


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


# --------------------------------------------------------------------- 1) ParseUint64
PROTO_H_OLD = '''std::string NowTimeString();                   // 本机当前时间'''
PROTO_H_NEW = '''std::string NowTimeString();                   // 本机当前时间

/**
 * 解析十进制无符号整数。
 *
 * 为什么单独有这个函数：断点续传的起点是从网络上来的字符串，
 * 直接 std::stoull 遇到非数字会**抛异常**（服务器上抛异常等于整条连接崩掉），
 * 而 atoll 会把垃圾解析成 0 —— 那更糟：客户端说"我从 100MB 处续传"，
 * 服务器理解成"从头发"，两边的写指针就此错位，最后拼出一个坏文件。
 * 所以必须"解析失败就明确失败"，让调用方去拒绝这次请求。
 */
bool ParseUint64(const std::string& text, unsigned long long* out);'''

PROTO_CPP_OLD = '''std::string NowTimeString() {'''
PROTO_CPP_NEW = '''bool ParseUint64(const std::string& text, unsigned long long* out) {
    if (!out || text.empty()) return false;
    unsigned long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;  // 含正负号/空格/字母一律拒绝
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        // 溢出就失败：回绕出来的小数字会让续传位置跳到文件开头附近，
        // 后果是"悄悄拼出一个坏文件"，比直接拒绝严重得多
        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
        value = value * 10ull + digit;
    }
    *out = value;
    return true;
}

std::string NowTimeString() {'''

# --------------------------------------------------------------------- 2) 服务端
SERVER_OLD_A = '''        if (words.size() != 1 || !IsValidTransferId(words[0])) {
            client->SendLine(Timed("ERROR", "用法：FILE_GET <文件ID>"));
            return true;
        }'''

SERVER_NEW_A = '''        // FILE_GET <文件ID> [已有字节数]
        // 第二个参数是断点续传用的：客户端说"我这儿已经有前 N 字节了"，服务器从 N 接着发。
        // **参数可选**，老客户端不带就还是从头发 —— 协议向后兼容。
        if ((words.size() != 1 && words.size() != 2) || !IsValidTransferId(words[0])) {
            client->SendLine(Timed("ERROR", "用法：FILE_GET <文件ID> [已下载字节数]"));
            return true;
        }
        unsigned long long resumeFrom = 0;
        if (words.size() == 2 && !dchat::ParseUint64(words[1], &resumeFrom)) {
            client->SendLine(Timed("ERROR", "断点位置不是合法数字：" + words[1]));
            return true;
        }'''

SERVER_OLD_B = '''        Log("download: " + client->nick + " 下载 " + file->id + "（由 " + file->owner +
            " 上传，" + dchat::FormatBytes(file->size) + "）");
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size)));
        for (unsigned long long offset = 0; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {'''

SERVER_NEW_B = '''        // 续传起点必须落在文件范围内。超出说明客户端手里是**旧文件**的大小
        // （同一 ID 的文件被重新上传过）。这时必须拒绝，绝不能"从头再发一遍" ——
        // 那会让客户端把新旧两份内容拼成一个坏文件，而且它会以为下载成功了。
        if (resumeFrom > file->size) {
            client->SendLine(dchat::BuildLine(
                "FILE_FAIL", file->id + " 续传位置超出文件大小（文件可能已被重新上传过）"));
            Log("download resume rejected: " + client->nick + " offset=" +
                std::to_string(resumeFrom) + " size=" + std::to_string(file->size));
            return true;
        }
        Log("download: " + client->nick + " 下载 " + file->id + "（由 " + file->owner +
            " 上传，" + dchat::FormatBytes(file->size) + "）" +
            (resumeFrom > 0 ? "，从 " + dchat::FormatBytes(resumeFrom) + " 处续传" : ""));
        // FILE_BEGIN 第 4 格把续传起点回给客户端，它据此定位写指针。
        // 老客户端只读前三格，多出来的第 4 格会被忽略 —— 依旧向后兼容。
        bool ok = client->SendLine(dchat::BuildLine(
            "FILE_BEGIN", file->id + " " + file->nameB64 + " " + std::to_string(file->size) +
                              " " + std::to_string(resumeFrom)));
        for (unsigned long long offset = resumeFrom; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {'''

# --------------------------------------------------------------------- 3) 客户端数据结构
FILES_H_OLD = '''struct DownloadJob {
    std::string id;
    std::string name;
    std::string path;  // 落到磁盘的完整路径'''
FILES_H_NEW = '''struct DownloadJob {
    std::string id;
    std::string name;
    std::string path;      // 最终落到磁盘的完整路径（下完才有效）
    std::string partPath;  // 下载中的中间文件；下完改名成 path'''

FILES_H_OLD2 = '''    bool FinishDownload(DownloadJob* job, std::string* error);'''
FILES_H_NEW2 = '''    bool FinishDownload(DownloadJob* job, std::string* error);

    /** 断点续传用的中间文件路径（按附件 ID 定，所以重试能找回上次的进度）。 */
    std::string PartialPath(const std::string& id) const;'''

# --------------------------------------------------------------------- 4) 客户端实现
FILES_CPP_HELPERS_OLD = '''bool EnsureDir(const std::string& dir) {'''
FILES_CPP_HELPERS_NEW = '''/** 文件大小；不存在或不是普通文件时返回 0。 */
unsigned long long FileSizeOrZero(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    if (!S_ISREG(info.st_mode)) return 0;
    return static_cast<unsigned long long>(info.st_size);
}

bool EnsureDir(const std::string& dir) {'''

FILES_CPP_PARTIAL_OLD = '''bool FileTransfers::RequestDownload(const std::string& id, std::string* error) {
    if (id.empty()) {
        if (error) *error = "附件 ID 是空的";
        return false;
    }
    // 服务器收到 FILE_GET 就会回 FILE_BEGIN，之后的数据由 HandleLine 落盘
    if (!connection_->SendLine(BuildLine("FILE_GET", id))) {
        if (error) *error = "发不出 FILE_GET（连接可能已断开）";
        return false;
    }
    return true;
}'''

FILES_CPP_PARTIAL_NEW = '''std::string FileTransfers::PartialPath(const std::string& id) const {
    // 只保留字母数字：ID 是服务器给的（形如 F1），但万一以后变了也不能让它
    // 通过路径分隔符跑到别的目录去 —— 这是"收别人的字符串当文件名"的经典坑。
    std::string safe;
    for (char ch : id) {
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            safe.push_back(ch);
        }
    }
    if (safe.empty()) safe = "unknown";
    return downloadDir_ + "/.dchat-part-" + safe;
}

bool FileTransfers::RequestDownload(const std::string& id, std::string* error) {
    if (id.empty()) {
        if (error) *error = "附件 ID 是空的";
        return false;
    }
    // 断点续传：上次没下完的话，磁盘上留着 `.part`，从它的长度接着要。
    // 没有 `.part`（或长度为 0）就还是从头下 —— 服务器端 offset 是可选的。
    const unsigned long long have = FileSizeOrZero(PartialPath(id));
    const std::string request = have > 0 ? (id + " " + std::to_string(have)) : id;
    // 服务器收到 FILE_GET 就会回 FILE_BEGIN，之后的数据由 HandleLine 落盘
    if (!connection_->SendLine(BuildLine("FILE_GET", request))) {
        if (error) *error = "发不出 FILE_GET（连接可能已断开）";
        return false;
    }
    return true;
}'''

FILES_CPP_BEGIN_OLD = '''        auto job = std::make_shared<DownloadJob>();
        job->id = fields[0];
        job->name = name;
        job->total = total;
        job->path = UniquePath(downloadDir_, name);
        job->fd = ::open(job->path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (job->fd < 0) {
            progress("打不开要保存的文件：" + job->path + "（" + std::strerror(errno) + "）");
            return true;
        }
        downloads_[job->id] = job;
        progress("开始接收 " + name + "（" + FormatBytes(total) + "）");
        return true;'''

FILES_CPP_BEGIN_NEW = '''        // 第 4 格是服务器回的续传起点（老服务器不发，缺省按 0 处理）
        unsigned long long resumeFrom = 0;
        if (fields.size() >= 4 && !ParseUint64(fields[3], &resumeFrom)) resumeFrom = 0;
        if (resumeFrom > total) resumeFrom = 0;  // 不合法就当从头下，别信这个数

        const std::string partPath = PartialPath(fields[0]);
        const unsigned long long existing = FileSizeOrZero(partPath);
        if (existing != resumeFrom) {
            // 磁盘上的 `.part` 和服务器说的起点对不上（被外部改过、或是上一个文件的残留）。
            // **不能硬着头皮往 existing 处续写** —— 那样拼出来的文件大小是对的、
            // 内容是坏的，而且客户端会报"下载成功"。丢掉重来，并且重新请求一次完整下载。
            progress("续传起点不一致，重新完整下载：" + name);
            ::unlink(partPath.c_str());
            connection_->SendLine(BuildLine("FILE_GET", fields[0]));
            return true;  // 不注册 job：这轮旧流的数据会被忽略，等新的 FILE_BEGIN
        }

        auto job = std::make_shared<DownloadJob>();
        job->id = fields[0];
        job->name = name;
        job->total = total;
        job->partPath = partPath;
        job->received = resumeFrom;
        // **不带 O_TRUNC**：续传要保留已有内容
        job->fd = ::open(partPath.c_str(), O_WRONLY | O_CREAT, 0600);
        if (job->fd < 0) {
            progress("打不开要保存的文件：" + partPath + "（" + std::strerror(errno) + "）");
            return true;
        }
        if (resumeFrom > 0 && ::lseek(job->fd, static_cast<off_t>(resumeFrom), SEEK_SET) < 0) {
            ::close(job->fd);
            progress("定位续传位置失败：" + name);
            return true;
        }
        downloads_[job->id] = job;
        progress(resumeFrom > 0
                     ? ("继续接收 " + name + "（已 " + FormatBytes(resumeFrom) + " / " +
                        FormatBytes(total) + "）")
                     : ("开始接收 " + name + "（" + FormatBytes(total) + "）"));
        return true;'''

FILES_CPP_FINISH_OLD = '''bool FileTransfers::FinishDownload(DownloadJob* job, std::string* error) {
    if (job->fd >= 0) {
        ::close(job->fd);
        job->fd = -1;
    }
    if (job->received != job->total) {
        if (error) {
            *error = "文件不完整（还差 " + FormatBytes(job->total - job->received) + "）";
        }
        job->failed = true;
        return false;
    }
    return true;
}'''

FILES_CPP_FINISH_NEW = '''bool FileTransfers::FinishDownload(DownloadJob* job, std::string* error) {
    if (job->fd >= 0) {
        ::close(job->fd);
        job->fd = -1;
    }
    if (job->received != job->total) {
        if (error) {
            *error = "文件不完整（还差 " + FormatBytes(job->total - job->received) + "）";
        }
        // **保留 .part**：下次 RequestDownload 会从它的长度续上，不用白下一遍
        job->failed = true;
        return false;
    }
    // 收齐了才改名到最终位置。重名时取"名字 (2).扩展名"，和 Windows 端一致。
    job->path = UniquePath(downloadDir_, job->name);
    if (::rename(job->partPath.c_str(), job->path.c_str()) != 0) {
        if (error) {
            *error = "保存失败（改名）：" + std::string(std::strerror(errno));
        }
        job->failed = true;
        return false;
    }
    return true;
}'''


def main():
    print("第 7 项：断点续传")
    patch(PROTO_H, [(PROTO_H_OLD, PROTO_H_NEW)], "protocol.h")
    patch(PROTO_CPP, [(PROTO_CPP_OLD, PROTO_CPP_NEW)], "protocol.cpp")
    patch(SERVER, [(SERVER_OLD_A, SERVER_NEW_A), (SERVER_OLD_B, SERVER_NEW_B)], "server.cpp")
    patch(FILES_H, [(FILES_H_OLD, FILES_H_NEW), (FILES_H_OLD2, FILES_H_NEW2)], "files.h")
    patch(FILES_CPP, [
        (FILES_CPP_HELPERS_OLD, FILES_CPP_HELPERS_NEW),
        (FILES_CPP_PARTIAL_OLD, FILES_CPP_PARTIAL_NEW),
        (FILES_CPP_BEGIN_OLD, FILES_CPP_BEGIN_NEW),
        (FILES_CPP_FINISH_OLD, FILES_CPP_FINISH_NEW),
    ], "files.cpp")


if __name__ == "__main__":
    main()
