// 附件解析与格式化（纯逻辑，不碰网络和文件系统）。
//
// 单独拆出来是为了能单测：FILE_OFFER 的字段是**按位置**解析的，
// 而解析错了的表现是"附件不显示"或者"名字成了乱码"——不会报错，只会让人困惑。
#include "files_parse.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "file_transfer.h"
#include "protocol.h"

namespace dchat {
namespace {

/**
 * 附件 ID 长什么样：字母数字加 `-`/`_`，长度合理，**而且必须含至少一个数字**。
 *
 * "必须含数字"这一条是必需的：服务器发的是 F1/L123-4 这类，而昵称经常是纯字母
 * （alice、bob）。第一版只判字符集，于是 "alice" 也被当成了合法 ID，
 * 结果把上传者昵称当成了附件 ID，附件卡片直接不显示。
 */
bool LooksLikeTransferId(const std::string& text) {
    if (text.empty() || text.size() > 64) return false;
    bool hasDigit = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            hasDigit = true;
            continue;
        }
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' ||
                        ch == '_';
        if (!ok) return false;
    }
    return hasDigit;
}

bool TryDecodeSize(const std::string& text, unsigned long long* out) {
    if (text.empty() || text.size() > 20) return false;
    unsigned long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<unsigned long long>(ch - '0');
    }
    *out = value;
    return true;
}

/** 名字是不是合法的 Base64 且解出来像文件名。 */
bool TryDecodeName(const std::string& text, std::string* out) {
    if (text.empty()) return false;
    std::vector<unsigned char> decoded;
    if (!Base64Decode(text, &decoded)) return false;
    // 解出来必须是合法文件名（过滤掉路径分隔符等），否则多半是字段错位了
    const std::string name = SanitizeFileName(std::string(decoded.begin(), decoded.end()));
    if (name.empty()) return false;
    *out = name;
    return true;
}

}  // namespace

bool FileOffer::IsVoice() const { return IsVoiceKind(kind); }

std::string FileOffer::Describe() const {
    std::string text = IsVoice() ? "🎤 语音 " : "📎 文件 ";
    text += name;
    text += " (";
    text += FormatBytes(size);
    text += ")";
    return text;
}

FileOffer ParseFileOffer(const std::string& rest) {
    FileOffer offer;
    // 入参三种都接受：完整行、带 hh:mm 时间戳的完整行、或者只有参数部分。
    // 让调用方不必记住传哪种——**这正是第一版踩的坑**：一边加了命令名前缀、
    // 一边又传了完整行，命令名被当成昵称；后来漏剥时间戳，又把 "02:32" 当成了
    // 附件 ID。统一在这里处理，外面随便传。
    std::vector<std::string> words = ParseLine(rest).Words();
    if (!words.empty() && words[0] == "FILE_OFFER") words.erase(words.begin());
    if (!words.empty() && LooksLikeTime(words[0])) words.erase(words.begin());

    // 字段布局：**按位置**，先试服务器实际用的标准格式，失败再退回老格式。
    //
    // 为什么不"按内容猜哪一格是上传者"（试过，不行）：昵称里完全可能带数字，
    // 比如 "fr5" 和附件 ID "F1" 长得一样像，猜不出来。格式是服务器定死的，
    // 直接按位置解才是对的；只有解不通时才说明是别的写法。
    const std::size_t candidates[2] = {1, 0};
    for (std::size_t attempt = 0; attempt < 2; ++attempt) {
        const std::size_t idAt = candidates[attempt];
        if (words.size() < idAt + 3) continue;

        FileOffer candidate;
        candidate.id = words[idAt];
        if (!LooksLikeTransferId(candidate.id)) continue;
        if (!TryDecodeName(words[idAt + 1], &candidate.name)) continue;
        if (!TryDecodeSize(words[idAt + 2], &candidate.size)) continue;
        if (candidate.size == 0) continue;

        if (idAt == 1) candidate.owner = words[0];
        // 后面两格：缩略图标记、种类。种类可能是 "0"/"file"/"voice"/"1"/"sticker"。
        for (std::size_t i = idAt + 3; i < words.size(); ++i) {
            const std::string& field = words[i];
            if (field == "1" || field == "true") {
                candidate.hasThumbnail = true;
                continue;
            }
            if (field == "0" || field == "false") continue;
            if (candidate.kind.empty()) candidate.kind = field;
        }
        candidate.valid = true;
        return candidate;
    }
    return offer;
}

}  // namespace dchat
