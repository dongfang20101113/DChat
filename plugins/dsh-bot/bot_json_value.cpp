#include "bot_json_value.h"

#include <cstddef>
#include <cstdlib>

namespace dchat {
namespace dshbot {
namespace {

constexpr int kMaxDepth = 64;  // 防止恶意/损坏的响应把栈打穿

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool Parse(JsonValue* out, std::string* error) {
        SkipSpace();
        if (!ParseValue(out, 0)) {
            if (error != nullptr) *error = error_ + "（位置 " + std::to_string(pos_) + "）";
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            if (error != nullptr) *error = "JSON 后面还有多余内容（位置 " + std::to_string(pos_) + "）";
            return false;
        }
        return true;
    }

private:
    void SkipSpace() {
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Fail(const std::string& why) {
        error_ = why;
        return false;
    }

    bool Literal(const char* word) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool ParseValue(JsonValue* out, int depth) {
        if (depth > kMaxDepth) return Fail("嵌套太深");
        SkipSpace();
        if (pos_ >= text_.size()) return Fail("内容提前结束");
        const char ch = text_[pos_];
        if (ch == '{') return ParseObject(out, depth);
        if (ch == '[') return ParseArray(out, depth);
        if (ch == '"') {
            out->type = JsonValue::Type::String;
            return ParseString(&out->text);
        }
        if (ch == 't') {
            if (!Literal("true")) return Fail("不是合法的 true");
            out->type = JsonValue::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (ch == 'f') {
            if (!Literal("false")) return Fail("不是合法的 false");
            out->type = JsonValue::Type::Bool;
            out->boolean = false;
            return true;
        }
        if (ch == 'n') {
            if (!Literal("null")) return Fail("不是合法的 null");
            out->type = JsonValue::Type::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseObject(JsonValue* out, int depth) {
        ++pos_;  // '{'
        out->type = JsonValue::Type::Object;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("对象的键必须是字符串");
            std::string key;
            if (!ParseString(&key)) return false;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("键后面缺冒号");
            ++pos_;
            JsonValue value;
            if (!ParseValue(&value, depth + 1)) return false;
            out->members.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            if (pos_ >= text_.size()) return Fail("对象没有闭合");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("对象里期望逗号或右花括号");
        }
    }

    bool ParseArray(JsonValue* out, int depth) {
        ++pos_;  // '['
        out->type = JsonValue::Type::Array;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!ParseValue(&value, depth + 1)) return false;
            out->items.push_back(std::move(value));
            SkipSpace();
            if (pos_ >= text_.size()) return Fail("数组没有闭合");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("数组里期望逗号或右方括号");
        }
    }

    static void AppendUtf8(std::string* out, unsigned int code) {
        if (code <= 0x7F) {
            out->push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
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

    bool ParseHex4(unsigned int* out) {
        if (pos_ + 4 > text_.size()) return Fail("\\u 后面不够 4 位");
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[pos_ + static_cast<std::size_t>(i)];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return Fail("\\u 后面不是十六进制");
            }
        }
        pos_ += 4;
        *out = value;
        return true;
    }

    bool ParseString(std::string* out) {
        ++pos_;  // '"'
        out->clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return true;
            if (ch != '\\') {
                out->push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) return Fail("转义符后面没内容");
            const char esc = text_[pos_++];
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
                    unsigned int code = 0;
                    if (!ParseHex4(&code)) return false;
                    // 成对的 \uD800-\uDBFF + \uDC00-\uDFFF 才算一个完整字符
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        const std::size_t save = pos_;
                        pos_ += 2;
                        unsigned int low = 0;
                        if (ParseHex4(&low) && low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default: return Fail("不认识的转义符");
            }
        }
        return Fail("字符串没有闭合");
    }

    bool ParseNumber(JsonValue* out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
                digits = true;
            }
        }
        if (digits && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (!digits) return Fail("这里应该是一个值");
        out->type = JsonValue::Type::Number;
        out->number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& member : members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

std::string JsonValue::GetString(const std::string& key, const std::string& fallback) const {
    const JsonValue* value = Find(key);
    if (value == nullptr) return fallback;
    if (value->type == Type::String) return value->text;
    if (value->type == Type::Null) return fallback;
    return fallback;
}

int JsonValue::GetInt(const std::string& key, int fallback) const {
    const JsonValue* value = Find(key);
    if (value == nullptr || value->type != Type::Number) return fallback;
    return static_cast<int>(value->number);
}

bool JsonValue::GetBool(const std::string& key, bool fallback) const {
    const JsonValue* value = Find(key);
    if (value == nullptr || value->type != Type::Bool) return fallback;
    return value->boolean;
}

std::vector<JsonValue> JsonValue::GetArray(const std::string& key) const {
    const JsonValue* value = Find(key);
    if (value == nullptr || value->type != Type::Array) return {};
    return value->items;
}

bool ParseJsonValue(const std::string& text, JsonValue* out, std::string* error) {
    if (out == nullptr) return false;
    *out = JsonValue();
    Parser parser(text);
    return parser.Parse(out, error);
}

}  // namespace dshbot
}  // namespace dchat
