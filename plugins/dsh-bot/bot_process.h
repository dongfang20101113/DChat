// 调用"大脑"：起一个子进程（DeepSeek Harness），把提示词喂进去，边读边吐事件行
//
// 为什么是子进程而不是把 DSH 链接进来：DSH 是一个会自己起 Node 运行时、自己管
// 会话和插件的完整程序，它和 dchat 服务端之间没有可以链接的库接口。用子进程
// 还有个实际好处 —— **崩了只崩一次回答**，机器人本身还在，连接都不用重连。
//
// 两个平台各写一套，但对外只有 RunBrain 一个函数：
//   POSIX    fork / execve / poll
//   Windows  CreateProcess + 匿名管道 + PeekNamedPipe
//
// 超时是硬要求：模型卡住、子进程挂死、网络断了都可能让 `dsh` 永远不出结果。
// 到点必须杀掉，否则线程被永久占住，下一个 @ 就再也排不上了。
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dchat {
namespace dshbot {

struct ProcessSpec {
    /** argv（已做占位符替换）。 */
    std::vector<std::string> argv;
    /** 子进程工作目录（空 = 继承当前目录）。 */
    std::string cwd;
    /** 额外环境变量（覆盖同名）。 */
    std::vector<std::pair<std::string, std::string>> env;
    /** 等多久（毫秒）。到点杀进程。 */
    int timeoutMs = 120000;
};

struct BrainRunResult {
    bool started = false;   // 子进程成功起来
    bool timedOut = false;  // 被超时杀掉
    int exitCode = -1;      // 退出码（超时/没起来时是 -1）
    std::string error;      // 起不来 / 超时的说明
    std::string stderrTail; // 子进程 stderr 的最后一段（失败时写日志用）
};

/**
 * 跑一次。`prompt` 写进子进程的 stdin 后关闭（默认命令就是从 stdin 读任务）。
 *
 * `onLine` 每读到 stdout 的一行就回调一次；返回 false 表示"别再读了"
 * （比如机器人已经断线，没人收答案了，继续跑只是浪费）。
 *
 * 返回 false 表示子进程压根没起来（找不到程序、权限不对），此时 error 有说明。
 * 子进程起来了但退出码非 0 时**返回 true** —— 那是"大脑自己报错"，不是调用失败，
 * 上层要把它当成一次"答不上来"而不是"插件坏了"。
 */
bool RunBrain(const ProcessSpec& spec, const std::string& prompt,
              const std::function<bool(const std::string&)>& onLine, BrainRunResult* result,
              std::string* error);

/** 从配置里的命令模板生成 argv：{prompt} / {session} 展开。
 *  `session` 为空时 {session} 整个消失（命令里不该留下空参数）。 */
std::vector<std::string> BuildArgv(const std::vector<std::string>& command,
                                   const std::string& prompt, const std::string& session);

}  // namespace dshbot
}  // namespace dchat
