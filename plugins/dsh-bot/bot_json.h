// 极小的 JSON 输入/输出辅助
//
// 输出侧（JsonQuote）见头文件；这里只做管理界面用得到的**扁平对象**解析：
// 顶层一个对象，值只可能是字符串 / 数字 / 布尔 / 字符串数组。
// 不上完整的 JSON 解析器是因为这一页提交的就是这几个字段，多写两百行解析器
// 只会多两百行要维护、要测的代码。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

/** 转成一个带引号的 JSON 字符串字面量。 */
inline std::string JsonQuote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[ch >> 4]);
                    out.push_back(kHex[ch & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
    return out;
}

/** 把字符串数组拼成 JSON 数组。 */
inline std::string JsonQuoteArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += JsonQuote(items[i]);
    }
    out += "]";
    return out;
}

/** 扁平对象：四种值各存一份，键名区分大小写（和 JSON 一致）。 */
struct FlatJson {
    std::map<std::string, std::string> strings;
    std::map<std::string, double> numbers;
    std::map<std::string, bool> bools;
    std::map<std::string, std::vector<std::string>> arrays;

    bool Has(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& fallback) const;
    bool GetBool(const std::string& key, bool fallback) const;
    int GetInt(const std::string& key, int fallback) const;
    /** 有就给 out 赋值并返回 true；没有就返回 false（out 不动）。 */
    bool GetArray(const std::string& key, std::vector<std::string>* out) const;
};

/** 解析。失败返回 false 并写 error（尽量指出第几个字符）。 */
bool ParseFlatJson(const std::string& text, FlatJson* out, std::string* error);

}  // namespace dshbot
}  // namespace dchat
