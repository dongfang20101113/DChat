// 交互式聊天界面（终端）。
//
// 这一层只做**终端相关**的事：读键、画输入行、把渲染好的行打出去。
// 会话逻辑全在 client_core/chat_core 里 —— 和 macOS 的 Cocoa 界面共用同一份，
// 所以两个界面不会各写一套指令解析。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "chat_core.h"
#include "chat_color.h"
#include "cli_render.h"
#include "input_history.h"
#include "terminal.h"
#include "voice.h"

namespace dchat {

/**
 * 终端界面。实现 ChatCoreDelegate，把核心的通知画到终端上。
 *
 * 注意 ChatUi **不再直接持有 ClientConnection**：连接归 ChatCore 管，
 * 界面要发消息就调 core_->SubmitInput() / SendMessage()。
 * 这样"哪一步该发什么"只有一处实现。
 */
class ChatUi : public ChatCoreDelegate {
public:
    explicit ChatUi(Terminal* terminal);
    ~ChatUi() override;

    ChatUi(const ChatUi&) = delete;
    ChatUi& operator=(const ChatUi&) = delete;

    /** 连接之前要拿它设参数（主机、端口、账号、指纹文件路径）。 */
    ChatCore* Core();

    /** 跑主循环，返回进程退出码。 */
    int Run();

    // ---- ChatCoreDelegate（都在接收线程上被调用）----
    void OnChatMessage(const ChatMessage& message) override;
    void OnConnected(bool encrypted, const std::string& fingerprint,
                     const TrustDecision& trust) override;
    void OnLoggedIn(const std::string& nick) override;
    void OnDisconnected(const std::string& reason) override;
    void OnOnlineNicks(const std::vector<std::string>& nicks) override;
    void OnTransferProgress(const std::string& text) override;
    void OnColorSettingChanged(bool enabled, const std::string& rawRest) override;
    void OnVoiceCommand(const std::string& arg) override;

private:
    void PrintLine(const std::string& text);
    void PrintWrapped(const std::string& text);
    void ShowHint();
    void SendRecordedVoice(const std::string& path);
    void CompleteInput();
    bool HandleTerminalOnlyCommand(const std::string& text);

    Terminal* terminal_ = nullptr;
    std::unique_ptr<ChatCore> core_;
    std::string input_;
    MicRecorder recorder_;
    // 登录成功之前不让发文件/语音：服务器会直接拒绝，而用户看到的只是
    // 一句莫名其妙的失败。本地先拦，提示更直白。
    bool loggedIn_ = false;
    bool wantQuit_ = false;
};

}  // namespace dchat
