#include "trust.h"

#include <sstream>
#include <vector>

namespace dchat {
namespace {

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

TrustDecision DecideTrust(const std::string& known, const std::string& current) {
    TrustDecision decision;
    decision.known = known;
    decision.current = current;
    if (current.empty()) {
        decision.kind = TrustKind::None;
        return decision;
    }
    if (known.empty()) {
        decision.kind = TrustKind::FirstUse;
        return decision;
    }
    decision.kind = (known == current) ? TrustKind::Unchanged : TrustKind::Changed;
    return decision;
}

std::string DescribeTrust(const TrustDecision& decision) {
    switch (decision.kind) {
        case TrustKind::FirstUse:
            return "第一次连这台服务器，指纹已记住：" + decision.current;
        case TrustKind::Unchanged:
            return "服务器指纹没变：" + decision.current;
        case TrustKind::Changed:
            // 这条要显眼：密钥变了要么是管理员换了密钥，要么是有人在中间劫持
            return "⚠ 服务器指纹变了！\n  记住的是：" + decision.known + "\n  现在收到：" +
                   decision.current +
                   "\n  如果是管理员换过密钥就没事；否则**不要输入密码**，先找管理员确认。";
        case TrustKind::None:
        default:
            return "这次连接没有加密，无法比对服务器指纹。";
    }
}

std::string KnownServerKey(const std::string& host, int port) {
    return host + ":" + std::to_string(port);
}

std::string LookupKnownFingerprint(const std::string& fileContent, const std::string& key) {
    std::istringstream stream(fileContent);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        // 格式：`键 指纹`（键里没有空格，指纹是 E1:2D:... 形式）
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        if (Trim(line.substr(0, space)) == key) return Trim(line.substr(space + 1));
    }
    return std::string();
}

std::string UpsertKnownFingerprint(const std::string& fileContent, const std::string& key,
                                   const std::string& fingerprint) {
    std::vector<std::string> lines;
    std::istringstream stream(fileContent);
    std::string line;
    bool replaced = false;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            const std::size_t space = trimmed.find(' ');
            if (space != std::string::npos && Trim(trimmed.substr(0, space)) == key) {
                lines.push_back(key + " " + fingerprint);
                replaced = true;
                continue;
            }
        }
        lines.push_back(line);
    }
    if (!replaced) lines.push_back(key + " " + fingerprint);

    std::string out;
    for (const std::string& item : lines) {
        out += item;
        out += '\n';
    }
    return out;
}

}  // namespace dchat
