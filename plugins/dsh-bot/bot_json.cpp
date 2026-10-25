#include "bot_json.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace dchat {
namespace dshbot {

namespace {

void SkipSpaces(const std::string& text, std::size_t* i) {
    while (*i < text.size() &&
           (text[*i] == ' ' || text[*i] == '\t' || text[*i] == '\r' || text[*i] == '\n')) {
        ++*i;
    }
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

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadString(const std::string& text, std::size_t* i, std::string* out) {
    if (*i >= text.size() || text[*i] != '"') return false;
    ++*i;
    out->clear();
    while (*i < text.size()) {
        const char ch = text[*i];
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
        if (*i >= text.size()) return false;
        const char esc = text[*i];
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
                if (*i + 4 > text.size()) return false;
                std::uint32_t code = 0;
                for (int k = 0; k < 4; ++k) {
                    const int value = HexValue(text[*i + static_cast<std::size_t>(k)]);
                    if (value < 0) return false;
                    code = (code << 4) | static_cast<std::uint32_t>(value);
                }
                *i += 4;
                AppendUtf8(code, out);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool ReadNumber(const std::string& text, std::size_t* i, double* out) {
    const std::size_t start = *i;
    while (*i < text.size() &&
           ((text[*i] >= '0' && text[*i] <= '9') || text[*i] == '-' || text[*i] == '+' ||
            text[*i] == '.' || text[*i] == 'e' || text[*i] == 'E')) {
        ++*i;
    }
    if (*i == start) return false;
    try {
        *out = std::stod(text.substr(start, *i - start));
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

bool FlatJson::Has(const std::string& key) const {
    return strings.count(key) > 0 || numbers.count(key) > 0 || bools.count(key) > 0 ||
           arrays.count(key) > 0;
}

std::string FlatJson::GetString(const std::string& key, const std::string& fallback) const {
    const auto it = strings.find(key);
    return it == strings.end() ? fallback : it->second;
}

bool FlatJson::GetBool(const std::string& key, bool fallback) const {
    const auto it = bools.find(key);
    return it == bools.end() ? fallback : it->second;
}

int FlatJson::GetInt(const std::string& key, int fallback) const {
    const auto it = numbers.find(key);
    if (it == numbers.end()) return fallback;
    return static_cast<int>(it->second);
}

bool FlatJson::GetArray(const std::string& key, std::vector<std::string>* out) const {
    const auto it = arrays.find(key);
    if (it == arrays.end() || out == nullptr) return false;
    *out = it->second;
    return true;
}

bool ParseFlatJson(const std::string& text, FlatJson* out, std::string* error) {
    if (out == nullptr) return false;
    FlatJson parsed;
    std::size_t i = 0;
    SkipSpaces(text, &i);
    if (i >= text.size() || text[i] != '{') {
        if (error != nullptr) *error = "请求体不是一个 JSON 对象";
        return false;
    }
    ++i;
    while (true) {
        SkipSpaces(text, &i);
        if (i >= text.size()) {
            if (error != nullptr) *error = "JSON 提前结束";
            return false;
        }
        if (text[i] == '}') break;
        std::string key;
        if (!ReadString(text, &i, &key)) {
            if (error != nullptr) *error = "键名不合法（第 " + std::to_string(i) + " 个字符附近）";
            return false;
        }
        SkipSpaces(text, &i);
        if (i >= text.size() || text[i] != ':') {
            if (error != nullptr) *error = "键 " + key + " 后面少了冒号";
            return false;
        }
        ++i;
        SkipSpaces(text, &i);
        if (i >= text.size()) {
            if (error != nullptr) *error = "JSON 提前结束";
            return false;
        }
        const char ch = text[i];
        if (ch == '"') {
            std::string value;
            if (!ReadString(text, &i, &value)) {
                if (error != nullptr) *error = "键 " + key + " 的字符串值不合法";
                return false;
            }
            parsed.strings[key] = value;
        } else if (ch == '[') {
            ++i;
            std::vector<std::string> values;
            SkipSpaces(text, &i);
            if (i < text.size() && text[i] == ']') {
                ++i;
            } else {
                while (true) {
                    SkipSpaces(text, &i);
                    if (i >= text.size()) {
                        if (error != nullptr) *error = "键 " + key + " 的数组没有结束";
                        return false;
                    }
                    if (text[i] == '"') {
                        std::string value;
                        if (!ReadString(text, &i, &value)) return false;
                        values.push_back(value);
                    } else {
                        double number = 0;
                        if (!ReadNumber(text, &i, &number)) {
                            if (error != nullptr) *error = "键 " + key + " 的数组元素不合法";
                            return false;
                        }
                        values.push_back(std::to_string(static_cast<long long>(number)));
                    }
                    SkipSpaces(text, &i);
                    if (i < text.size() && text[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < text.size() && text[i] == ']') {
                        ++i;
                        break;
                    }
                    if (error != nullptr) *error = "键 " + key + " 的数组格式不对";
                    return false;
                }
            }
            parsed.arrays[key] = values;
        } else if (ch == 't' || ch == 'f') {
            const bool truth = (ch == 't');
            const std::string literal = truth ? "true" : "false";
            if (text.compare(i, literal.size(), literal) != 0) {
                if (error != nullptr) *error = "键 " + key + " 的布尔值不合法";
                return false;
            }
            i += literal.size();
            parsed.bools[key] = truth;
        } else {
            double number = 0;
            if (!ReadNumber(text, &i, &number)) {
                if (error != nullptr) *error = "键 " + key + " 的值类型不支持";
                return false;
            }
            parsed.numbers[key] = number;
        }
        SkipSpaces(text, &i);
        if (i >= text.size()) {
            if (error != nullptr) *error = "JSON 提前结束";
            return false;
        }
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') break;
        if (error != nullptr) *error = "键 " + key + " 后面既不是逗号也不是右括号";
        return false;
    }
    *out = parsed;
    return true;
}

}  // namespace dshbot
}  // namespace dchat
