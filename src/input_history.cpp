#include "input_history.h"

namespace dchat {

void InputHistory::Add(const std::string& text) {
    if (text.empty()) return;
    entries_.push_back(text);
    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin(), entries_.begin() + (entries_.size() - kMaxEntries));
    }
    ResetBrowse();
}

void InputHistory::SetDraft(const std::string& text) {
    if (!browsing_) draft_ = text;
}

bool InputHistory::Up(std::string* out) {
    if (entries_.empty()) return false;  // 没有历史：按上键无效
    if (!browsing_) {                    // 第一次按上键：从最新一条开始
        browsing_ = true;
        cursor_ = entries_.size() - 1;
        if (out) *out = entries_[cursor_];
        return true;
    }
    if (cursor_ == 0) return false;  // 已经在最旧的一条：再按无效
    --cursor_;
    if (out) *out = entries_[cursor_];
    return true;
}

bool InputHistory::Down(std::string* out) {
    if (!browsing_) return false;
    if (cursor_ + 1 < entries_.size()) {
        ++cursor_;
        if (out) *out = entries_[cursor_];
        return true;
    }
    // 越过最新一条：回到进来之前正在写的内容
    browsing_ = false;
    cursor_ = entries_.size();
    if (out) *out = draft_;
    return true;
}

void InputHistory::ResetBrowse() {
    browsing_ = false;
    cursor_ = entries_.size();
    draft_.clear();
}

}  // namespace dchat
