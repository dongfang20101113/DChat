#include "bot_chunk.h"

#include <algorithm>
#include <cstddef>

#include "bot_config.h"

namespace dchat {
namespace dshbot {

namespace {

/**
 * 把一个 UTF-8 字节串解码成"一个字一个元素"。
 *
 * 不追求把非法字节修好：解不出来就当单字节字符，原样往下传。
 * 聊天内容里偶尔出现的坏字节不该让机器人把整段话丢掉。
 */
std::vector<std::string> DecodeChars(const std::string& text) {
    std::vector<std::string> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0x80) == 0x00) {
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        }
        if (i + length > text.size()) length = 1;
        out.push_back(text.substr(i, length));
        i += length;
    }
    return out;
}

bool IsAsciiSpace(const std::string& cp) {
    return cp == " " || cp == "\t" || cp == "\r";
}

bool IsSpaceOrEnd(const std::string& cp) { return IsAsciiSpace(cp) || cp == "\n"; }

/** 句末：这几类后面必须断。 */
bool IsStrongEnder(const std::string& cp) {
    return cp == "。" || cp == "！" || cp == "？" || cp == "；" || cp == "…" || cp == "!" ||
           cp == "?" || cp == ";" || cp == "\u2026";
}

/** 句内停顿：太长的时候在这些地方断更自然。 */
bool IsWeakEnder(const std::string& cp) {
    return cp == "，" || cp == "、" || cp == "：" || cp == "," || cp == ":" || cp == "\u3001" ||
           cp == "\uFF0C" || cp == "\uFF1A";
}

std::string Join(const std::vector<std::string>& chars, std::size_t begin, std::size_t end) {
    std::string out;
    for (std::size_t i = begin; i < end; ++i) out += chars[i];
    return out;
}

std::string TrimSpaces(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

}  // namespace

std::size_t CodePointCount(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        }
        if (i + length > text.size()) length = 1;
        i += length;
        ++count;
    }
    return count;
}

std::string TruncateCodePoints(const std::string& text, std::size_t maxCodePoints) {
    if (maxCodePoints == 0) return std::string();
    const std::vector<std::string> chars = DecodeChars(text);
    if (chars.size() <= maxCodePoints) return text;
    return Join(chars, 0, maxCodePoints);
}

std::string FlattenMarkdown(const std::string& text) {
    // 逐行处理：围栏代码块的 ``` 行整行丢掉（里面是代码，不是给人念的），
    // 其余行去掉行首的 # / > 记号。最后再把 ** __ ` 这几个成对记号当字符删掉。
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(current);

    std::string out;
    bool firstKept = true;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 围栏代码块标记：整行只有 ``` 或 ```lang
        const std::string trimmed = TrimSpaces(line);
        if (trimmed.size() >= 3 && trimmed.compare(0, 3, "```") == 0) continue;

        // 行首的 # 标题 / > 引用
        std::size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
        std::size_t hash = begin;
        while (hash < line.size() && line[hash] == '#') ++hash;
        if (hash > begin && (hash == line.size() || line[hash] == ' ')) {
            begin = hash;
            while (begin < line.size() && line[begin] == ' ') ++begin;
        }
        while (begin < line.size() && (line[begin] == '>' || line[begin] == ' ')) ++begin;
        line = line.substr(begin);

        if (!firstKept) out += '\n';
        firstKept = false;
        out += line;
    }

    // 成对记号：**粗体** / __粗体__ / `代码`
    std::string cleaned;
    cleaned.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '*') {
            ++i;
            continue;
        }
        if (out[i] == '_' && i + 1 < out.size() && out[i + 1] == '_') {
            ++i;
            continue;
        }
        if (out[i] == '`') continue;
        cleaned.push_back(out[i]);
    }

    // 连续 3 个以上换行收成 2 个（空行留一个就够，多了只是空白）
    std::string compact;
    compact.reserve(cleaned.size());
    int newlines = 0;
    for (char ch : cleaned) {
        if (ch == '\n') {
            ++newlines;
            if (newlines > 2) continue;
        } else {
            newlines = 0;
        }
        compact.push_back(ch);
    }
    return TrimSpaces(compact);
}

std::vector<std::string> SplitForChat(const std::string& text, const TypingConfig& config) {
    std::string body = config.stripMarkdown ? FlattenMarkdown(text) : text;
    // 统一换行
    std::string normalized;
    normalized.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\r') {
            if (i + 1 < body.size() && body[i + 1] == '\n') continue;
            normalized.push_back('\n');
            continue;
        }
        normalized.push_back(body[i]);
    }

    const int limit = std::max(8, config.maxChars);
    const std::vector<std::string> chars = DecodeChars(normalized);
    std::vector<std::string> segments;

    std::vector<std::string> current;
    auto flush = [&]() {
        if (current.empty()) return;
        const std::string joined = TrimSpaces(Join(current, 0, current.size()));
        if (!joined.empty()) segments.push_back(joined);
        current.clear();
    };

    for (std::size_t i = 0; i < chars.size(); ++i) {
        const std::string& cp = chars[i];
        if (cp == "\n") {
            flush();
            continue;
        }
        current.push_back(cp);

        bool strong = IsStrongEnder(cp);
        // 英文句点只有在"后面是空白或到头"时才算句末：否则 3.14、a.com、v1.2 都会被切开
        if (!strong && cp == ".") {
            strong = (i + 1 >= chars.size()) || IsSpaceOrEnd(chars[i + 1]);
        }
        if (strong) {
            // 把紧随其后的同类标点一起吞掉（"……" "？！" 不该断成两条）
            while (i + 1 < chars.size()) {
                const std::string& next = chars[i + 1];
                if (!(IsStrongEnder(next) || next == "." || next == "…")) break;
                current.push_back(next);
                ++i;
            }
            flush();
            continue;
        }

        if (static_cast<int>(current.size()) >= limit) {
            // 超长了：回头找最近的弱标点断（至少要在后半段，否则等于没前进）
            std::size_t cut = 0;
            for (std::size_t k = current.size(); k > current.size() / 2; --k) {
                if (IsWeakEnder(current[k - 1])) {
                    cut = k;
                    break;
                }
            }
            if (cut == 0) {
                cut = current.size();
                flush();
            } else {
                const std::string head = TrimSpaces(Join(current, 0, cut));
                if (!head.empty()) segments.push_back(head);
                current.erase(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(cut));
            }
        }
    }
    flush();

    // 太短的并到下一句：避免"嗯。""好。"刷屏
    std::vector<std::string> merged;
    std::string pending;
    for (const std::string& segment : segments) {
        if (pending.empty()) {
            pending = segment;
            continue;
        }
        const std::size_t pendingCount = CodePointCount(pending);
        const std::size_t segmentCount = CodePointCount(segment);
        if (pendingCount < static_cast<std::size_t>(std::max(1, config.minChars)) &&
            static_cast<int>(pendingCount + segmentCount) <= limit) {
            pending += segment;
            continue;
        }
        merged.push_back(pending);
        pending = segment;
    }
    if (!pending.empty()) merged.push_back(pending);

    // 条数上限：超了就把尾巴并进最后一条并截断。
    const std::size_t maxChunks = static_cast<std::size_t>(std::max(1, config.maxChunks));
    if (merged.size() > maxChunks) {
        std::string tail;
        for (std::size_t i = maxChunks - 1; i < merged.size(); ++i) tail += merged[i];
        const std::string kept = TruncateCodePoints(tail, static_cast<std::size_t>(limit) * 2);
        const bool truncated = kept.size() < tail.size();
        merged.resize(maxChunks - 1);
        merged.push_back(truncated ? (kept + "…") : kept);
    }
    return merged;
}

int ChunkDelayMs(const std::string& chunk, const TypingConfig& config) {
    const int chars = static_cast<int>(CodePointCount(chunk));
    int delay = config.minGapMs + chars * config.perCharMs;
    if (delay > config.maxGapMs) delay = config.maxGapMs;
    if (delay < config.minGapMs) delay = config.minGapMs;
    return delay;
}

}  // namespace dshbot
}  // namespace dchat
