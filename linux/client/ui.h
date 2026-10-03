// 交互式聊天界面。接口很小：给它一个连接和一个终端，调 Run()。
#pragma once

#include <string>
#include <vector>

#include "net.h"
#include "server_command.h"
#include "terminal.h"

namespace dchat {

class ChatUi {
public:
    ChatUi(ClientConnection* connection, Terminal* terminal);

    /** 登录后服务器会告诉我们昵称（用于 @我 高亮和 Tab 补全排除自己）。 */
    void SetSelfNick(const std::string& nick);

    /** 接收线程每收到一行就调它。**必须在接收线程里调用**，内部会加锁。 */
    void HandleServerLine(const std::string& line);

    void SetOnlineNicks(const std::vector<std::string>& nicks);

    /** 跑主循环，返回进程退出码。 */
    int Run();

private:
    void PrintLine(const std::string& text);
    std::string RenderServerLine(const std::string& line);
    void CompleteInput();
    bool HandleLocalCommand(const std::string& text);

    ClientConnection* connection_ = nullptr;
    Terminal* terminal_ = nullptr;
    std::string selfNick_;
    std::string input_;
    std::vector<std::string> onlineNicks_;
    std::vector<std::string> knownNicks_;
    TabCompleter completer_;
    bool colorEnabled_ = true;
    bool wantQuit_ = false;
};

}  // namespace dchat
