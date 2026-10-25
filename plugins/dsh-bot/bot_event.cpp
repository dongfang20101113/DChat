#include "bot_event.h"

#include <cstddef>
#include <cstdint>

namespace dchat {
namespace dshbot {

namespace {

void SkipSpaces(const std::string& s, std::size_t* i) {
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n')) ++*i;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void AppendUtf8(std::uint32_t code, std::string* out) {
    if (code < 0x80) {
        out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (code >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

/** 读一个 JSON 字符串（调用方已经确认 s[*i] == '"'）。 */
bool ReadString(const std::string& s, std::size_t* i, std::string* out) {
    if (*i >= s.size() || s[*i] != '"') return false;
    ++*i;
    out->clear();
    while (*i < s.size()) {
        const char ch = s[*i];
        if (ch == '"') {
            ++*i;
            return true;
        }
        if (ch != '\\') {
            out->push_back(ch);
            ++*i;
            continue;
        }
        ++*i;
        if (*i >= s.size()) return false;
        const char esc = s[*i];
        ++*i;
        switch (esc) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (*i + 4 > s.size()) return false;
                std::uint32_t code = 0;
                for (int k = 0; k < 4; ++k) {
                    const int value = HexValue(s[*i + static_cast<std::size_t>(k)]);
                    if (value < 0) return false;
                    code = (code << 4) | static_cast<std::uint32_t>(value);
                }
                *i += 4;
                // 代理对：D800-DBFF 后面必须跟 DC00-DFFF
                if (code >= 0xD800 && code <= 0xDBFF && *i + 6 <= s.size() && s[*i] == '\\' &&
                    s[*i + 1] == 'u') {
                    std::uint32_t low = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        const int value = HexValue(s[*i + 2 + static_cast<std::size_t>(k)]);
                        if (value < 0) {
                            ok = false;
                            break;
                        }
                        low = (low << 4) | static_cast<std::uint32_t>(value);
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                        *i += 6;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                AppendUtf8(code, out);
                break;
            }
            default: return false;
        }
    }
    return false;
}

/** 跳过一个值（对象/数组整段跳过，字符串跳转义，数字/字面量跳到分隔符）。 */
bool SkipValue(const std::string& s, std::size_t* i) {
    SkipSpaces(s, i);
    if (*i >= s.size()) return false;
    const char ch = s[*i];
    if (ch == '"') {
        std::string scratch;
        return ReadString(s, i, &scratch);
    }
    if (ch == '{' || ch == '[') {
        const char open = ch;
        const char close = (ch == '{') ? '}' : ']';
        int depth = 0;
        while (*i < s.size()) {
            const char current = s[*i];
            if (current == '"') {
                std::string scratch;
                if (!ReadString(s, i, &scratch)) return false;
                continue;
            }
            if (current == open) {
                ++depth;
            } else if (current == close) {
                --depth;
                if (depth == 0) {
                    ++*i;
                    return true;
                }
            }
            ++*i;
        }
        return false;
    }
    while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != ']') ++*i;
    return true;
}

}  // namespace

bool ParseDshEvent(const std::string& line, DshEvent* out) {
    if (out == nullptr) return false;
    DshEvent event;
    std::size_t i = 0;
    SkipSpaces(line, &i);
    if (i >= line.size() || line[i] != '{') return false;
    ++i;
    while (true) {
        SkipSpaces(line, &i);
        if (i >= line.size()) return false;
        if (line[i] == '}') break;
        if (line[i] != '"') return false;
        std::string key;
        if (!ReadString(line, &i, &key)) return false;
        SkipSpaces(line, &i);
        if (i >= line.size() || line[i] != ':') return false;
        ++i;
        SkipSpaces(line, &i);
        if (i >= line.size()) return false;

        const bool wantString = (key == "type" || key == "text" || key == "sessionId" ||
                                 key == "phase" || key == "error");
        if (wantString && line[i] == '"') {
            std::string value;
            if (!ReadString(line, &i, &value)) return false;
            if (key == "type") {
                event.type = value;
            } else if (key == "text") {
                event.text = value;
            } else if (key == "sessionId") {
                event.sessionId = value;
            } else if (key == "phase") {
                event.phase = value;
            } else {
                event.error = value;
            }
        } else {
            if (!SkipValue(line, &i)) return false;
        }

        SkipSpaces(line, &i);
        if (i >= line.size()) return false;
        if (line[i] == ',') {
            ++i;
            continue;
        }
        if (line[i] == '}') break;
        return false;
    }
    if (event.type.empty()) return false;
    *out = event;
    return true;
}

}  // namespace dshbot
}  // namespace dchat
