#include "files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "files_parse.h"
#include "file_transfer.h"
#include "protocol.h"

namespace dchat {
namespace {

/** 从完整路径里取文件名。 */
std::string BaseName(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool EnsureDir(const std::string& dir) {
    struct stat info {};
    if (::stat(dir.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    return ::mkdir(dir.c_str(), 0700) == 0;
}

/** 重名就加 " (2)"、" (3)"……，和 Windows 端的行为一致。 */
std::string UniquePath(const std::string& dir, const std::string& name) {
    std::string candidate = dir + "/" + name;
    struct stat info {};
    if (::stat(candidate.c_str(), &info) != 0) return candidate;

    std::string stem = name;
    std::string extension;
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
    }
    for (int index = 2; index < 1000; ++index) {
        candidate = dir + "/" + stem + " (" + std::to_string(index) + ")" + extension;
        if (::stat(candidate.c_str(), &info) != 0) return candidate;
    }
    return dir + "/" + name;
}

/** 去掉行首的 hh:mm 时间戳，只留参数。 */
std::vector<std::string> FieldsOf(const Message& message) {
    std::vector<std::string> words = message.Words();
    if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());
    return words;
}

}  // namespace

FileTransfers::FileTransfers(ClientConnection* connection) : connection_(connection) {}

FileTransfers::~FileTransfers() {
    for (auto& item : downloads_) {
        if (item.second->fd >= 0) ::close(item.second->fd);
    }
}

void FileTransfers::SetDownloadDir(const std::string& dir) {
    downloadDir_ = dir;
    EnsureDir(downloadDir_);
}

std::string FileTransfers::ServerIdFor(const std::string& localId) const {
    const auto it = serverIds_.find(localId);
    return it == serverIds_.end() ? std::string() : it->second;
}

bool FileTransfers::RequestDownload(const std::string& id, std::string* error) {
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
}

std::string FileTransfers::Upload(const std::string& path, const std::string& kind,
                                  std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "打不开文件：" + path;
        return std::string();
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0, std::ios::beg);
    if (length <= 0) {
        if (error) *error = "文件是空的（协议不接受 0 字节附件）";
        return std::string();
    }
    if (static_cast<unsigned long long>(length) > kMaxFileBytes) {
        if (error) {
            *error = "文件太大（上限 " + FormatBytes(kMaxFileBytes) + "）";
        }
        return std::string();
    }

    const std::string displayName = SanitizeFileName(BaseName(path));
    if (displayName.empty()) {
        if (error) *error = "文件名不合法";
        return std::string();
    }

    const std::string id = "L" + std::to_string(::getpid()) + "-" +
                           std::to_string(nextUploadId_++);
    serverIds_[id] = std::string();  // 先占位，等 FILE_OFFER 回来填服务器 ID
    const unsigned long long total = static_cast<unsigned long long>(length);
    const std::string rest = BuildFileSendRest(
        id, Base64Encode(displayName), total, kind, /*hasThumbnail=*/false);
    if (!connection_->SendLine(BuildLine("FILE_SEND", rest))) {
        if (error) *error = "发不出 FILE_SEND（连接可能已断开）";
        return std::string();
    }

    std::vector<char> buffer(kFileChunkBytes);
    unsigned long long sent = 0;
    while (sent < total) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<unsigned long long>(kFileChunkBytes, total - sent));
        in.read(buffer.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            // 文件在读的过程中被截短了：告诉对方取消，别留个半个文件在服务器上
            connection_->SendLine(BuildLine("FILE_CANCEL", id));
            if (error) *error = "文件读不完整（可能被其他程序改动了）";
            return std::string();
        }
        const std::string piece = Base64Encode(
            reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(got));
        if (!connection_->SendLine(BuildLine("FILE_CHUNK", id + " " + piece))) {
            if (error) *error = "发送中断：连接已断开（已发 " + FormatBytes(sent) + "）";
            return std::string();
        }
        sent += static_cast<unsigned long long>(got);
        // 每块之间让出一下：一路占着 send 锁会把聊天消息和心跳全挡住
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!connection_->SendLine(BuildLine("FILE_END", id))) {
        if (error) *error = "收尾失败：连接已断开";
        return std::string();
    }
    return id;
}

bool FileTransfers::FinishDownload(DownloadJob* job, std::string* error) {
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
}

bool FileTransfers::HandleLine(const std::string& line,
                               const std::function<void(const std::string&)>& progress) {
    const Message message = ParseLine(line);
    const std::string& command = message.command;
    if (command != "FILE_OFFER" && command != "FILE_BEGIN" && command != "FILE_DATA" &&
        command != "FILE_END" && command != "FILE_CANCEL" && command != "FILE_FAIL") {
        return false;
    }
    const std::vector<std::string> fields = FieldsOf(message);

    if (command == "FILE_OFFER") {
        // 有人上传好了：让用户自己决定要不要下载（和 Windows 端一样不自动下）
        std::string rest;
        for (const std::string& field : fields) {
            if (!rest.empty()) rest += " ";
            rest += field;
        }
        // 传**完整行**：解析器自己会剥掉命令名和时间戳
        const FileOffer offer = ParseFileOffer(line);
        if (!offer.valid) return true;
        // 如果是我们自己上传的，这里把服务器分配的附件 ID 记下来。
        // FILE_OFFER 里**不带**客户端的传输 ID，所以只能按顺序配对：
        // 上传是在界面线程里同步做完的（一次只有一个在传），
        // 因此第一个还没对上号的本地 ID 就是它。
        for (auto& item : serverIds_) {
            if (item.second.empty()) {
                item.second = offer.id;
                break;
            }
        }
        lastOfferId_ = offer.id;
        // 这条一定要带上 id：上传者拿到的就是这一条（服务器现在会把附件 ID 回给上传者），
        // 界面上的 "等服务器确认附件 id…" 就靠它转成真实 ID。
        progress("📎 " + offer.Describe() + "  id=" + offer.id + "   （/get " + offer.id +
                 " 下载）");
        return true;
    }

    if (command == "FILE_FAIL") {
        // 下载失败时服务器回这条（附件不存在 / 已过期）。**必须告诉用户**，
        // 否则他会一直等一个永远不来的文件。
        std::string body;
        for (const std::string& field : fields) {
            if (!body.empty()) body += " ";
            body += field;
        }
        progress("\x1b[31m附件下载失败：" + body + "\x1b[0m");
        return true;
    }

    if (command == "FILE_BEGIN") {
        if (fields.size() < 3) return true;
        std::vector<unsigned char> decoded;
        if (!Base64Decode(fields[1], &decoded)) return true;
        const std::string name =
            SanitizeFileName(std::string(decoded.begin(), decoded.end()));
        unsigned long long total = 0;
        for (char ch : fields[2]) {
            if (ch < '0' || ch > '9') return true;
            total = total * 10 + static_cast<unsigned long long>(ch - '0');
        }
        if (total == 0 || total > kMaxFileBytes) {
            progress("附件大小不合法，已取消：" + FormatBytes(total));
            return true;
        }
        if (!EnsureDir(downloadDir_)) {
            progress("建不了下载目录：" + downloadDir_);
            return true;
        }

        auto job = std::make_shared<DownloadJob>();
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
        return true;
    }

    if (command == "FILE_DATA") {
        if (fields.size() < 2) return true;
        const auto it = downloads_.find(fields[0]);
        if (it == downloads_.end()) return true;  // 不是我们在下的，忽略
        DownloadJob* job = it->second.get();
        std::vector<unsigned char> chunk;
        if (!Base64Decode(fields[1], &chunk)) {
            ::close(job->fd);
            job->fd = -1;
            job->failed = true;
            progress("数据损坏，已放弃：" + job->name);
            downloads_.erase(it);
            return true;
        }
        // 对方给的数据比声明的多：直接拒掉，别把磁盘写爆
        if (job->received + chunk.size() > job->total) {
            ::close(job->fd);
            job->fd = -1;
            job->failed = true;
            progress("数据超出声明大小，已放弃：" + job->name);
            downloads_.erase(it);
            return true;
        }
        if (!chunk.empty()) {
            std::size_t written = 0;
            while (written < chunk.size()) {
                const ssize_t got =
                    ::write(job->fd, chunk.data() + written, chunk.size() - written);
                if (got <= 0) {
                    ::close(job->fd);
                    job->fd = -1;
                    job->failed = true;
                    progress("写文件失败：" + job->name);
                    downloads_.erase(it);
                    return true;
                }
                written += static_cast<std::size_t>(got);
            }
            job->received += chunk.size();
        }
        const int percent = static_cast<int>(job->received * 100 / job->total);
        if (percent != job->lastPercent) {
            job->lastPercent = percent;
            progress("接收 " + job->name + " " + std::to_string(percent) + "%");
        }
        return true;
    }

    if (command == "FILE_END") {
        if (fields.empty()) return true;
        const auto it = downloads_.find(fields[0]);
        if (it == downloads_.end()) return true;
        DownloadJob* job = it->second.get();
        std::string error;
        const bool ok = FinishDownload(job, &error);
        if (ok) {
            progress("✅ 已保存：" + job->path + "（" + FormatBytes(job->total) + "）");
        } else {
            progress("❌ " + job->name + "：" + error);
        }
        downloads_.erase(it);
        return true;
    }

    if (command == "FILE_CANCEL") {
        if (fields.empty()) return true;
        const auto it = downloads_.find(fields[0]);
        if (it != downloads_.end()) {
            if (it->second->fd >= 0) ::close(it->second->fd);
            progress("对端取消了传输：" + it->second->name);
            downloads_.erase(it);
        }
        return true;
    }
    return true;
}

}  // namespace dchat
