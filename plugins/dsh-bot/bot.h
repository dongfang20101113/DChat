// dchat × DeepSeek Harness 机器人：把 DSH 作为一个"群成员"接进 dchat
//
// 形态是**独立进程**（sidecar），不是编进 dchat_server 的模块，理由有三条：
//
//   1. 可选装：不部署它，服务端一个字节都不变 —— 这是"插件"这个词的实际含义；
//   2. 崩得起：模型调用、子进程、网络任何一处出问题，只影响机器人自己，
//      服务端不受牵连，重启一下机器人就恢复；
//   3. 位置自由：DSH 装在哪台机器上都能用。它默认连本机 5555，但也可以指向
//      任何一台 dchat 服务器（配置里改 server.host 就行）。
//
// 机器人用**普通账号**登录，说话就是普通消息。所以它天然受群权限约束：
// 没加入的群收不到消息，被踢出去就再也收不到，不需要在服务端开任何后门。
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "bot_config.h"

namespace dchat {
namespace dshbot {

/** 极简日志：时间 + 级别 + 内容，同时写文件和控制台（systemd 收 stderr）。 */
class Logger {
public:
    ~Logger();

    bool Open(const std::string& path, bool verbose, std::string* error);
    void SetVerbose(bool verbose) { verbose_ = verbose; }

    void Info(const std::string& text);
    void Warn(const std::string& text);
    void Error(const std::string& text);
    /** 只有 verbose 打开时才输出。 */
    void Debug(const std::string& text);

    /** 最近 count 行（管理界面直接显示，省得再教人去找日志文件）。 */
    std::vector<std::string> Tail(std::size_t count) const;

private:
    void Write(const char* level, const std::string& text);

    mutable std::mutex mutex_;
    void* file_ = nullptr;  // FILE*（头文件里不拉 <cstdio>）
    bool verbose_ = false;
    std::deque<std::string> tail_;
};

/** 跑起来直到进程被要求退出（Ctrl+C / systemd stop）。返回进程退出码。
 *  configPath 用来在管理界面保存时把改动写回原文件。 */
int RunBot(const Config& config, const std::string& configPath);

}  // namespace dshbot
}  // namespace dchat
