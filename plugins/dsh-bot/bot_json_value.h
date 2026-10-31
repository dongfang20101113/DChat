// 一个够用的 JSON 值解析器（对象/数组/字符串/数字/布尔/null）
//
// 为什么不用 bot_json.h 里那个：那个只解析「顶层扁对象」，是给管理界面的
// POST 体用的。OpenAI 兼容接口的响应是嵌套的（choices[].message.tool_calls[]），
// 扁解析器根本拿不到。这里写完整值类型，代价是这一百多行 + 单测。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                          // String 的值
    std::vector<JsonValue> items;              // Array 的元素
    std::vector<std::pair<std::string, JsonValue>> members;  // Object 的键值（保序）

    bool IsNull() const { return type == Type::Null; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }

    /** 取对象成员；没有就返回 nullptr。 */
    const JsonValue* Find(const std::string& key) const;
    /** 便捷取值：不是对象/没有这个键/类型不对就返回 fallback。 */
    std::string GetString(const std::string& key, const std::string& fallback = std::string()) const;
    int GetInt(const std::string& key, int fallback = 0) const;
    bool GetBool(const std::string& key, bool fallback = false) const;
    /** 取数组形式的字符串（比如 content parts）。不是数组就返回空。 */
    std::vector<JsonValue> GetArray(const std::string& key) const;
};

/**
 * 解析一段 JSON。允许前后有空白。失败返回 false 并给出行/列附近的提示。
 * 不支持：注释、尾逗号、NaN/Infinity（服务端不会发这些）。
 */
bool ParseJsonValue(const std::string& text, JsonValue* out, std::string* error);

}  // namespace dshbot
}  // namespace dchat
