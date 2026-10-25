// DeepSeek Harness ↔ dchat 机器人插件：配置文件
//
// 格式刻意做得极其简单：一行一条 `键 = 值`，`#` 开头是注释，值里的换行用 \n 转义。
// 为什么不用 JSON/YAML：这个文件是要**手改**的（机器人账号、进哪些群、谁能 @ 它），
// 而配置解析出错的表现是"机器人在线但不说话"，很难查。键值对里多一个逗号少一个引号
// 不该让人对着日志猜十分钟。键名写错会**直接报错并指出行号**，不是静默忽略。
//
// 这份解析是纯逻辑（不碰网络、不碰文件系统），所以能单测 ——
// 见 tests/test_dsh_bot.cpp。
#pragma once

#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

/** 触发条件：什么情况下才算"叫了机器人"。 */
struct TriggerConfig {
    /** 额外别名（不带 @）。主名就是 bot.nick，不用在这里重复写。 */
    std::vector<std::string> aliases;
    /**
     * true = 必须 @ 才回；false = 房间里说的每句话都回（调试/陪聊用）。
     *
     * 默认 true：一个会插话的机器人比一个不理人的机器人更烦人。
     */
    bool mentionOnly = true;
};

enum class ReplyMode {
    All,        // 谁都回
    Whitelist,  // 只回名单里的人
    Blacklist,  // 名单里的人不回
};

/** 谁能让它开口。 */
struct ReplyPolicy {
    ReplyMode mode = ReplyMode::All;
    /** 名单（昵称，比较时大小写不敏感）。 */
    std::vector<std::string> nicks;
};

/** 回答怎么切成一条条发出去。 */
struct TypingConfig {
    /** 单条最多几个码点。发太长一条，手机端一屏全是它。 */
    int maxChars = 45;
    /** 合并的下限：比这短的片段并到下一条，免得"嗯。""好。"刷屏。 */
    int minChars = 10;
    /** 一条回答最多切成几条。超了后面并到最后一条里，避免长篇刷屏。 */
    int maxChunks = 24;
    /** 两条之间的基础间隔（毫秒）。 */
    int minGapMs = 350;
    /** 每多一个码点多等几毫秒（模拟打字节奏）。 */
    int perCharMs = 55;
    /** 间隔上限。再长就不像打字，像卡死了。 */
    int maxGapMs = 1500;
    /** 把 Markdown 记号（**、`、# 标题）抹平成聊天语气。 */
    bool stripMarkdown = true;
};

/** 大脑（DeepSeek Harness）怎么调。 */
struct BrainConfig {
    /**
     * 每行一个参数，支持单/双引号；`{prompt}` 会被替换成一个参数，
     * `{session}` 展开成 `--session-id <id>`（没有会话时整个消失）。
     *
     * 默认 `dsh headless --json {session} -`：提示词从 **stdin** 喂进去，不走命令行
     * 参数 —— 聊天内容里什么引号、换行都可能有，塞进命令行参数迟早被转义规则坑一次。
     */
    std::vector<std::string> command{"dsh", "headless", "--json", "{session}", "-"};
    /** 子进程工作目录（空 = 当前目录）。 */
    std::string cwd;
    /**
     * 权限模式，写到子进程的 DSH_PERMISSION_MODE 环境变量。
     *
     * 默认 read-only：机器人在群里，群里任何人都能 @ 它。**绝不能**让一句
     * "帮我 rm -rf /" 变成真的命令。要放开必须显式改这里。
     */
    std::string permissionMode = "read-only";
    /**
     * 额外传给 dsh 的 `--patch` 文件（关掉工具插件用，见 dsh-chat-only.yml）。
     *
     * 只靠 read-only 还不够：只读沙箱仍然允许**读**本机文件，群里的人可以
     * 让机器人把 /etc/passwd 念出来。关掉工具才是干净的隔离。
     */
    std::string patch;
    /** 一次回答最多等多久（毫秒）。超时就杀掉子进程、回一句"想太久了"。 */
    int timeoutMs = 120000;
    /**
     * 每个房间一个 DSH 会话（`--session-id`），这样它能记住上文。
     *
     * 关掉的话每次都从零开始，只能靠 promptTemplate 里塞的最近对话。
     */
    bool sessionPerRoom = true;
    /** 同时最多跑几个 DSH 进程。 */
    int maxConcurrent = 1;
    /** 排队上限。超了就明确告诉被 @ 的人"忙不过来"，而不是默默吞掉。 */
    int maxQueue = 8;
    /**
     * 提示词模板。占位符：{room} 房间显示名、{roomId} 房间 ID、{nick} 说话的人、
     * {text} 去掉 @ 之后的内容。
     */
    std::string promptTemplate =
        "你在 dchat 的群聊「{room}」里被 {nick} @ 了。下面是要你回的话。\n"
        "请用简体中文、口语化、简短地回复；这是聊天消息，不要用 Markdown 标题、\n"
        "表格或代码块，不要分成很多小标题。\n"
        "\n"
        "{nick} 说：{text}";
};

/** 整个插件的配置。 */
struct Config {
    // ---- 连接 ----
    std::string host = "127.0.0.1";
    int port = 5555;
    /**
     * 传输加密：auto（能握手就握手，老服务器退回明文）| off | require。
     *
     * 默认 auto。公网服务器必须落在加密上 —— 明文 LOGIN 会把机器人密码
     * 直接暴露在网络上。
     */
    std::string encrypt = "auto";
    /** 钉住服务器公钥指纹（留空 = 只记日志不校验）。 */
    std::string fingerprint;

    // ---- 账号 ----
    std::string nick;
    std::string password;
    /** 账号不存在时自动 REGISTER 一次（首启方便；之后建议改回 false）。 */
    bool registerFirst = false;

    // ---- 接入哪些会话 ----
    /**
     * 房间 ID 列表。`hall` 表示大厅。
     *
     * 只回这里列出的房间 —— 这是"接入特定群聊"的落点：机器人是普通成员，
     * 它没加入的群根本收不到消息，再叠加这一层白名单，双保险。
     */
    std::vector<std::string> rooms;
    /** 不在群里时自动 GJOIN（需要审核的群会排队，等群主同意）。 */
    bool autoJoin = true;

    TriggerConfig trigger;
    ReplyPolicy reply;
    TypingConfig typing;
    BrainConfig brain;

    /** 被 @ 之后先说一句话（空 = 不说）。DSH 起进程要几秒，没反馈像卡死了。 */
    std::string ackText = "（正在想…）";

    /**
     * 登录后多久之内收到的聊天消息**当成历史回放丢掉**（毫秒）。
     *
     * 线上服务器一般开着 keepchathistory / offlinemessages：登录那一刻服务端会把
     * 错过的历史**用原始 SAY/RMSG 行重放一遍**，里面完全可能有人 @ 过机器人。
     * 不丢的话，机器人一上线就对着几个小时前的 10 条老消息逐条回答 —— 群里看到的是
     * 一串答非所问。历史回放没法从协议上区分出来（没有序号也没有标记行），
     * 只能靠"登录后这一小段时间内的不算"。
     *
     * 服务端发"以下是加入之前的聊天记录"这类提示时会自动把这个窗口往后延，
     * 所以慢网络下也能盖住整段回放。
     */
    int historySettleMs = 2000;

    // ---- 日志与状态 ----
    std::string logPath = "dchat-dsh-bot.log";
    bool verbose = false;
    /** 每个房间的 DSH 会话 id 存这里，重启后还能接上话。 */
    std::string sessionFile = "dchat-dsh-bot-sessions.txt";

    // ---- 重连 ----
    int reconnectMinMs = 3000;
    int reconnectMaxMs = 60000;
};

/**
 * 解析配置文本。失败时返回 false 并把"第几行、错在哪"写进 error。
 * 未知的键**一律报错**：写错键名的配置如果静默生效一半，排查成本极高。
 */
bool ParseConfig(const std::string& text, Config* out, std::string* error);

/** 读文件再解析。 */
bool LoadConfigFile(const std::string& path, Config* out, std::string* error);

/** 注释齐全的示例配置（`--example` 打印它，部署时直接重定向成文件）。 */
std::string ExampleConfig();

/** 大小写不敏感地比较两个昵称。 */
bool SameNick(const std::string& a, const std::string& b);

}  // namespace dshbot
}  // namespace dchat
