// 输入框的历史记录（类似 Minecraft 聊天：上键翻出自己刚发过的内容）
// 纯逻辑，不依赖任何 Windows API，方便单元测试。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dchat {

class InputHistory {
public:
    static constexpr std::size_t kMaxEntries = 200;

    // 发送成功后记录一条（空字符串会被忽略）
    void Add(const std::string& text);

    // 开始翻历史之前，把输入框里正在编辑的内容存为草稿（只在第一次按上键时生效）
    void SetDraft(const std::string& text);

    // 上键：返回 true 表示取到了更早的一条（写入 out）；
    // 历史为空、或已经在最旧一条时返回 false（调用方什么都不做）
    bool Up(std::string* out);

    // 下键：返回 true 表示取到了更新的一条；越过最新一条时恢复草稿
    bool Down(std::string* out);

    // 结束翻历史状态（例如发送之后），下一次按上键从最新一条重新开始
    void ResetBrowse();

    std::size_t Count() const { return entries_.size(); }
    bool Browsing() const { return browsing_; }

private:
    std::vector<std::string> entries_;
    std::size_t cursor_ = 0;  // == entries_.size() 表示"没在翻历史"
    bool browsing_ = false;
    std::string draft_;
};

}  // namespace dchat
