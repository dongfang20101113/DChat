// 验证"你是不是这个 dchat 账号的主人" —— 管理界面登录用
//
// 为什么要真去服务器验：管理界面要给你改配置，改配置等于改机器人能干什么。
// 自己发明一套密码只会多一个要被记住、要泄露的口令；直接用 dchat 账号密码，
// 和 /admin 是同一套身份。
//
// 一个关键细节：账号**可能正连着**（手机/电脑在线），这时服务器会回
// 「昵称已被占用」。那句话是在**校验密码之后**才发的 —— 所以它同样证明
// 密码是对的，不能当成失败，否则用户在线时就永远登不进管理界面。
#pragma once

#include <string>

namespace dchat {
namespace dshbot {

struct PasswordCheck {
    bool ok = false;          // 密码对不对
    bool nickTaken = false;   // 密码对，但这个昵称正连在线
    std::string error;        // 技术上的失败（连不上、握手失败…）
};

/** 开一条临时连接去服务器上验一次账号密码，验完立刻断开（不影响在线的客户端）。 */
PasswordCheck VerifyDchatPassword(const std::string& host, int port, const std::string& nick,
                                  const std::string& password, int timeoutMs);

}  // namespace dshbot
}  // namespace dchat
