#include "bot_file.h"

#include <filesystem>
#include <system_error>

namespace dchat {
namespace dshbot {

namespace fs = std::filesystem;

namespace {

std::string TrimSpaces(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

FileDirective ExtractFileDirectives(const std::string& text) {
    FileDirective result;
    const std::string open = "[[file:";
    const std::string close = "]]";
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t at = text.find(open, i);
        if (at == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        const std::size_t end = text.find(close, at + open.size());
        if (end == std::string::npos) {
            // 没闭合：当成普通文字，原样留着
            out.append(text, i, std::string::npos);
            break;
        }
        out.append(text, i, at - i);
        const std::string path = TrimSpaces(text.substr(at + open.size(), end - at - open.size()));
        if (!path.empty()) result.files.push_back(path);
        i = end + close.size();
    }
    // 指令占了一整行时会留下空行：把连续空行收一收，别让群里出现一串空消息
    std::string compact;
    compact.reserve(out.size());
    int newlines = 0;
    for (char ch : out) {
        if (ch == '\n') {
            ++newlines;
            if (newlines > 2) continue;
        } else {
            newlines = 0;
        }
        compact.push_back(ch);
    }
    result.text = TrimSpaces(compact);
    return result;
}

bool IsInsideDirectory(const std::string& directory, const std::string& path) {
    if (directory.empty() || path.empty()) return false;
    std::error_code code;
    const fs::path base = fs::weakly_canonical(fs::path(directory), code);
    if (code) return false;
    const fs::path target = fs::weakly_canonical(fs::path(path), code);
    if (code) return false;
    // 按路径元素逐个比较：字符串前缀比较会把 C:\work2 当成 C:\work 里面
    auto baseIt = base.begin();
    auto targetIt = target.begin();
    for (; baseIt != base.end(); ++baseIt, ++targetIt) {
        if (targetIt == target.end()) return false;
        if (*baseIt != *targetIt) return false;
    }
    return targetIt != target.end();  // 必须比 base 更深（不接受"就是目录本身"）
}

std::string ResolveInsideWorkspace(const std::string& workspace, const std::string& path) {
    if (workspace.empty() || path.empty()) return std::string();
    std::error_code code;
    fs::path candidate(path);
    if (candidate.is_relative()) candidate = fs::path(workspace) / candidate;
    // 先确认在目录里面，再看它到底是不是一个普通文件
    const std::string resolved = candidate.lexically_normal().string();
    if (!IsInsideDirectory(workspace, resolved)) return std::string();
    if (!fs::is_regular_file(fs::path(resolved), code) || code) return std::string();
    return resolved;
}

std::string ResolveSendablePath(const std::vector<std::string>& roots, const std::string& path) {
    // 绝对路径：只要落在任意一个允许的目录里就放行
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        const std::string resolved = ResolveInsideWorkspace(root, path);
        if (!resolved.empty()) return resolved;
    }
    return std::string();
}

std::string BaseNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace dshbot
}  // namespace dchat
