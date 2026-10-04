// 文件传输（上传 / 下载）。
//
// 协议（和 Windows 客户端完全一致）：
//   上传：FILE_SEND <id> <文件名B64> <字节数> [种类]  ->  N 个 FILE_CHUNK <id> <B64>
//         -> FILE_END <id>
//   下载：FILE_GET <id>  ->  服务器回 FILE_BEGIN <id> <文件名B64> <字节数>
//         -> N 个 FILE_DATA <id> <B64>  -> FILE_END <id>
//
// 每块 2048 字节（kFileChunkBytes）。**块大小两端必须一致**吗？其实不必——
// 协议里每块自带长度，收端按到达的字节数写就行。但保持一致能让两端的进度条
// 走法一样，排查问题时心算也方便。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "files_parse.h"
#include "net.h"

namespace dchat {

/** 上传任务。 */
struct UploadJob {
    std::string id;
    std::string displayName;
    std::string path;
    std::string kind;
    unsigned long long total = 0;
    unsigned long long sent = 0;
    int lastPercent = -1;
};

/** 下载任务（服务器推过来的）。 */
struct DownloadJob {
    std::string id;
    std::string name;
    std::string path;      // 最终落到磁盘的完整路径（下完才有效）
    std::string partPath;  // 下载中的中间文件；下完改名成 path
    unsigned long long total = 0;
    unsigned long long received = 0;
    int lastPercent = -1;
    int fd = -1;  // POSIX 文件描述符
    bool failed = false;
};

class FileTransfers {
public:
    FileTransfers(ClientConnection* connection);
    ~FileTransfers();

    /** 下载保存目录（默认 ./dchat-downloads，不存在就建）。 */
    void SetDownloadDir(const std::string& dir);
    const std::string& DownloadDir() const { return downloadDir_; }

    /**
     * 上传一个本地文件。返回**本地**传输 ID（空串表示失败）。
     * **会阻塞**：整个过程是同步发完的（每块之间让出 CPU）。
     *
     * 返回的**不是**服务器上的附件 ID：服务器收到 FILE_END 后自己分配一个
     * （形如 F1），再广播 FILE_OFFER。所以想拿到能用来 /get 的 ID，要么等那份
     * 广播回来，要么用 ServerIdFor 查。
     */
    std::string Upload(const std::string& path, const std::string& kind, std::string* error);

    /** 本地传输 ID 对应的服务器附件 ID；还没收到 FILE_OFFER 时返回空串。 */
    std::string ServerIdFor(const std::string& localId) const;

    /** 最近一次广播出来的附件 ID（服务器分配的）。 */
    const std::string& LastOfferId() const { return lastOfferId_; }

    /** 请求下载某个附件。 */
    bool RequestDownload(const std::string& id, std::string* error);

    /** 附件落地后的本地路径（用于播放语音）；没下过或已删返回空。 */
    std::string LocalPathFor(const std::string& id) const;

    /** 自动下载的开关（语音默认开，普通文件默认关——和另外两端一致）。 */
    void SetAutoDownloadPid(const std::string& id, bool on) { (void)id; autoDownloadOwner_ = on; }
    bool autoDownloadOwner() const { return autoDownloadOwner_; }

    /**
     * 处理服务器来的一行。返回 true 表示这行被文件模块吃掉了（界面不用管）。
     * 参数 progress 用来把进度回调给界面。
     */
    bool HandleLine(const std::string& line,
                    const std::function<void(const std::string&)>& progress);

    /** 当前在下载的任务数。 */
    std::size_t ActiveDownloads() const { return downloads_.size(); }

private:
    bool FinishDownload(DownloadJob* job, std::string* error);

    /** 断点续传用的中间文件路径（按附件 ID 定，所以重试能找回上次的进度）。 */
    std::string PartialPath(const std::string& id) const;

    // 请求下载时算出的续传起点（附件 ID -> 字节数）。
    // 服务器不回显这个值，所以客户端自己记着；FILE_BEGIN 到达时取走。
    std::map<std::string, unsigned long long> pendingResume_;

    ClientConnection* connection_ = nullptr;
    std::string downloadDir_ = "dchat-downloads";
    std::map<std::string, std::shared_ptr<DownloadJob>> downloads_;
    // 本地传输 ID -> 服务器附件 ID。服务器收到 FILE_END 才分配自己的 ID，
    // 并在随后广播的 FILE_OFFER 里把两个 ID 都带着，我们从那里学。
    std::map<std::string, std::string> serverIds_;
    // 最近一次广播出来的附件（供 UI 显示"刚上传的是哪个 id"）
    std::string lastOfferId_;
    // 附件 ID -> 本地落盘路径（语音要能点一下/自动就开始放）
    std::map<std::string, std::string> localPaths_;
    bool autoDownloadOwner_ = true;
    std::uint64_t nextUploadId_ = 1;
};

}  // namespace dchat
