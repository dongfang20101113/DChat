// 聊天记录的持久化存储（纯逻辑，不碰文件系统，方便单测）。
//
// 为什么单独抽一层：落盘最容易出的两类问题是"错了不报错"的那种 ——
//   1. 进程被强杀时最后一行只写了一半，下次启动把它当成合法记录，
//      于是房间里凭空多出一条残缺消息（甚至解析成别的命令）；
//   2. 上限只在内存里守，磁盘文件却无限增长。
// 所以这里把"解析/追加/裁剪/序列化"全做成纯函数，用测试把这两条钉死；
// 真正的文件读写留在服务端（它才知道路径与生命周期的开关）。
//
// 文件格式：每行 `<序号>\t<原始协议行>`。
//   - 用 **制表符** 分隔：协议行本身以时间戳开头、且正文可能含空格，
//     用空格分隔会把时间戳当成序号的一部分。按**第一个**制表符切分即可，
//     正文里就算有制表符也不影响。
//   - 存**原始协议行**（而不是重新拼一遍）：回放时原样发回去，
//     保证重启前后客户端看到的内容逐字节一致。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dchat {

/** 历史里的一条：序号 + 原始协议行。序号用于"离线补发"判断谁还没看过。 */
struct HistoryEntry {
    unsigned long long seq = 0;
    std::string line;
};

/**
 * 历史存储：内存里保留最近若干条，并支持按字节预算裁剪。
 *
 * 序号**单调递增且不会回退**：裁剪只丢最旧的，LastSeq() 始终是最大值。
 * 这一点是离线补发的前提 —— 如果裁剪后序号重置，客户端记的"我看到第几条"
 * 就会突然指向另一条消息。
 */
class HistoryStore {
public:
    HistoryStore(std::size_t maxLines, unsigned long long maxBytes);

    /** 追加一条，返回它的序号。超出上限时从最旧的开始丢。 */
    unsigned long long Append(const std::string& line);

    /**
     * 解析历史文件内容（用于启动时加载）。
     *
     * **容忍最后一行被截断**：没有结尾换行、或解析不出序号的那一行会被丢弃。
     * 这正是"进程被强杀"时磁盘上的样子 —— 宁可少一条，也不能凭空多一条残缺消息。
     * @return 成功读入的条数
     */
    std::size_t LoadFromText(const std::string& text);

    /** 当前内存里的记录（旧 -> 新）。 */
    std::vector<HistoryEntry> Snapshot() const;

    /**
     * 取序号大于 afterSeq 的记录，最多 limit 条（用于离线补发）。
     * limit 为 0 时返回空 —— 调用方用 0 表示"这个功能没开"。
     */
    std::vector<HistoryEntry> Since(unsigned long long afterSeq, std::size_t limit) const;

    /** 序列化成文件内容（用于压实重写）。 */
    std::string Serialize() const;

    std::size_t Size() const { return entries_.size(); }
    unsigned long long Bytes() const { return bytes_; }
    unsigned long long LastSeq() const { return lastSeq_; }

    /** 启动至今累计追加过多少条（含已被裁掉的），服务端用它决定何时压实。 */
    unsigned long long TotalAppended() const { return totalAppended_; }

    /** 按字节预算再裁一次（服务端把它和文件占用放在同一个预算里）。 */
    void TrimToBytes(unsigned long long maxBytes);

    void Clear();

private:
    std::vector<HistoryEntry> entries_;
    std::size_t maxLines_;
    unsigned long long maxBytes_;
    unsigned long long bytes_ = 0;
    unsigned long long lastSeq_ = 0;
    unsigned long long totalAppended_ = 0;
};

/** 拼一行文件记录（序号 + 制表符 + 原始行 + 换行）。 */
std::string FormatHistoryFileLine(const HistoryEntry& entry);

/** 解析一行文件记录；格式不对返回 false（调用方据此丢弃该行）。 */
bool ParseHistoryFileLine(const std::string& text, HistoryEntry* out);

// ---------------------------------------------------------------------------
// 每用户"已读到第几条"——离线补发靠它判断该补哪些
// ---------------------------------------------------------------------------
// 单独一个文件存（不塞进账号文件）的理由：账号文件是"用户凭据"，格式改动会
// 牵连四端的登录解析；而这是**服务端自己的阅读进度**，客户端根本不需要知道。
// 文件同样是追加写 + 同名以后出现的为准，所以进程被强杀时最多丢最后一条进度，
// 后果只是下次多补几条（重复），不会漏消息。

struct SeenRecord {
    std::string nick;
    unsigned long long seq = 0;
};

std::string FormatSeenFileLine(const SeenRecord& record);
bool ParseSeenFileLine(const std::string& text, SeenRecord* out);

/**
 * 解析整个进度文件。
 *
 * **同名以最后出现的为准**：文件是追加写的，同一个用户会有多行历史。
 * 序号取最大值而不是最后一行的值 —— 文件可能被外部工具打乱过顺序，
 * 取最大值才能保证"进度只前进、不后退"（后退会导致重复补发已看过的消息）。
 */
std::vector<SeenRecord> ParseSeenFile(const std::string& text);

}  // namespace dchat
