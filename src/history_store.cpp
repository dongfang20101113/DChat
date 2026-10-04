#include "history_store.h"

#include <cstdlib>

namespace dchat {
namespace {

/** 一条记录在内存和文件里占的字节数（正文 + 分隔符 + 换行 + 序号几位）。 */
unsigned long long EntryBytes(const HistoryEntry& entry) {
    // 序号按十进制位数算，和文件里实际写入的长度一致，这样预算才对得上
    unsigned long long digits = 1;
    for (unsigned long long v = entry.seq; v >= 10; v /= 10) ++digits;
    return static_cast<unsigned long long>(entry.line.size()) + digits + 2;  // \t 和 \n
}

/** 从十进制文本解析无符号数；有空字符或非数字就失败。 */
bool ParseUnsigned(const std::string& text, unsigned long long* out) {
    if (text.empty()) return false;
    unsigned long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        // 溢出保护：超了就当解析失败，宁可不加载也不能回绕成一个假序号
        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
        value = value * 10ull + digit;
    }
    *out = value;
    return true;
}

}  // namespace

HistoryStore::HistoryStore(std::size_t maxLines, unsigned long long maxBytes)
    : maxLines_(maxLines == 0 ? 1 : maxLines), maxBytes_(maxBytes) {}

unsigned long long HistoryStore::Append(const std::string& line) {
    HistoryEntry entry;
    // 序号从一个非零值开始：0 留给"这个用户从没看过任何消息"
    entry.seq = lastSeq_ + 1;
    entry.line = line;
    lastSeq_ = entry.seq;
    bytes_ += EntryBytes(entry);
    entries_.push_back(std::move(entry));
    ++totalAppended_;

    while (!entries_.empty() &&
           (entries_.size() > maxLines_ || bytes_ > maxBytes_)) {
        bytes_ -= EntryBytes(entries_.front());
        entries_.erase(entries_.begin());
    }
    return lastSeq_;
}

void HistoryStore::TrimToBytes(unsigned long long maxBytes) {
    while (!entries_.empty() && bytes_ > maxBytes) {
        bytes_ -= EntryBytes(entries_.front());
        entries_.erase(entries_.begin());
    }
}

std::size_t HistoryStore::LoadFromText(const std::string& text) {
    // 启动加载是唯一的"从头来"场景：这里才允许把序号归零。
    entries_.clear();
    bytes_ = 0;
    lastSeq_ = 0;
    totalAppended_ = 0;
    const std::vector<std::string> lines = [&text] {
        std::vector<std::string> out;
        std::size_t begin = 0;
        while (begin < text.size()) {
            std::size_t end = text.find('\n', begin);
            if (end == std::string::npos) end = text.size();
            out.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        return out;
    }();

    std::size_t loaded = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        // 兼容 CRLF：文件被别处工具碰过也不会整份读不出来
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        HistoryEntry entry;
        if (!ParseHistoryFileLine(line, &entry)) {
            // 解析不了的直接丢。**最后一行尤其常见**：上次是被强杀的，
            // 那一行只写了一半。绝不能把它当成合法记录塞进历史。
            continue;
        }
        // 序号必须严格递增：文件被手工编辑过时可能出现乱序/重复，
        // 那种情况按"以文件里的序号为准、但必须大于当前最大"来收敛，
        // 否则 Since() 的语义会被破坏。
        if (entry.seq <= lastSeq_) continue;
        lastSeq_ = entry.seq;
        bytes_ += EntryBytes(entry);
        entries_.push_back(std::move(entry));
        ++loaded;
    }
    // 加载完也要守上限：文件可能比当前配置的上限还大（规则被调小过）
    while (!entries_.empty() && (entries_.size() > maxLines_ || bytes_ > maxBytes_)) {
        bytes_ -= EntryBytes(entries_.front());
        entries_.erase(entries_.begin());
    }
    return loaded;
}

std::vector<HistoryEntry> HistoryStore::Snapshot() const { return entries_; }

std::vector<HistoryEntry> HistoryStore::Since(unsigned long long afterSeq,
                                              std::size_t limit) const {
    std::vector<HistoryEntry> out;
    if (limit == 0) return out;
    for (const HistoryEntry& entry : entries_) {
        if (entry.seq <= afterSeq) continue;
        out.push_back(entry);
        if (out.size() >= limit) break;
    }
    return out;
}

std::string HistoryStore::Serialize() const {
    std::string out;
    for (const HistoryEntry& entry : entries_) out += FormatHistoryFileLine(entry);
    return out;
}

void HistoryStore::Clear() {
    // **故意不重置 lastSeq_**：运行中清空历史（比如管理员想清屏）之后，
    // 序号必须继续往前走。否则新消息会复用旧序号，而客户端记着"我看到第 120 条"，
    // 那些新消息就会被当成"已经看过"而永远补发不到 —— 这类 bug 极难查。
    entries_.clear();
    bytes_ = 0;
}

std::string FormatHistoryFileLine(const HistoryEntry& entry) {
    return std::to_string(entry.seq) + "\t" + entry.line + "\n";
}

bool ParseHistoryFileLine(const std::string& text, HistoryEntry* out) {
    if (!out) return false;
    const std::size_t tab = text.find('\t');
    if (tab == std::string::npos || tab == 0) return false;
    unsigned long long seq = 0;
    if (!ParseUnsigned(text.substr(0, tab), &seq)) return false;
    const std::string line = text.substr(tab + 1);
    if (line.empty()) return false;
    out->seq = seq;
    out->line = line;
    return true;
}

std::string FormatSeenFileLine(const SeenRecord& record) {
    return record.nick + "\t" + std::to_string(record.seq) + "\n";
}

bool ParseSeenFileLine(const std::string& text, SeenRecord* out) {
    if (!out) return false;
    const std::size_t tab = text.find('\t');
    if (tab == std::string::npos || tab == 0) return false;
    const std::string nick = text.substr(0, tab);
    unsigned long long seq = 0;
    if (!ParseUnsigned(text.substr(tab + 1), &seq)) return false;
    if (nick.empty()) return false;
    out->nick = nick;
    out->seq = seq;
    return true;
}

std::vector<SeenRecord> ParseSeenFile(const std::string& text) {
    std::vector<SeenRecord> out;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        begin = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        SeenRecord record;
        if (!ParseSeenFileLine(line, &record)) continue;  // 坏行直接丢（强杀时的半行）
        // 同名取最大值：保证进度只前进不后退
        bool merged = false;
        for (SeenRecord& existing : out) {
            if (existing.nick != record.nick) continue;
            if (record.seq > existing.seq) existing.seq = record.seq;
            merged = true;
            break;
        }
        if (!merged) out.push_back(record);
    }
    return out;
}

}  // namespace dchat
