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

#include <map>
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
     * 用哪个大脑：
     *   "dsh" —— 起子进程跑本机的 `dsh headless`（工具全、但必须有 DSH）
     *   "api" —— 直接调 OpenAI 兼容的 HTTP 接口（DeepSeek 等），
     *            **工具由机器人自己实现**，所以不依赖任何本机程序。
     *            api 模式可以整个跑在服务器上，改的也就是服务器上的文件。
     */
    std::string mode = "dsh";
    /** api 模式用的接口（OpenAI 兼容的 chat/completions）。 */
    std::string apiUrl = "https://api.deepseek.com/chat/completions";
    /** api 模式的模型名。 */
    std::string apiModel = "deepseek-chat";
    /** 默认 API Key（可以按房间单独覆盖，见 roomApiKeys）。 */
    std::string apiKey;
    /**
     * 每个房间单独的 API Key（房间 ID -> key）。空 = 用默认的。
     * 用户要求"每个对话可以在网页里填不同的 key"，就落在这里。
     */
    std::map<std::string, std::string> roomApiKeys;
    /** api 模式下最多来回几轮工具调用（防止模型自己绕死）。 */
    int apiMaxRounds = 6;
    /** 单次 HTTP 请求超时（毫秒）。 */
    int apiTimeoutMs = 120000;
    /** 额外的系统提示（空 = 用内置的，见 bot_api.cpp）。 */
    std::string apiSystemPrompt;
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
     * {text} 去掉 @ 之后的内容、{workdir} 它能动手的工作目录、
     * {fileroots} 额外允许发送（只读）的目录。
     */
    std::string promptTemplate =
        "你在 dchat 的群聊「{room}」里被 {nick} @ 了。\n"
        "你能改动的目录是 {workdir}（读写、改名、建目录、跑命令都在这里）。\n"
        "另外这些目录里的文件**可以直接发**给对方（只读，不能改）：{fileroots}\n"
        "想发任何文件，就在回答最后单独写一行：[[file: 完整路径或相对路径]]\n"
        "（机器人会自己读文件上传，**不需要**你先复制到工作目录。）\n"
        "用简体中文、口语化、简短地回复；这是聊天消息，不要用 Markdown 标题、表格或代码块。\n"
        "但**命令、路径、报错原文、工具名一律照抄，不要翻译**；对方用英文问就用英文答。\n"
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
     * 允许机器人发到群里的单个文件上限（MB）。0 = 不许发文件。
     *
     * 机器人自己发的文件是**从本机读出来再传到服务器**（不是链接），
     * 所以这个上限同时是"一次回答最多能把多少本机数据带出去"的闸门。
     * 还会再受服务器 rules 里 documentsize 的限制，两者取小。
     */
    int fileMaxMb = 32;

    /**
     * 额外允许**发送**（读）的目录，分号分隔。
     *
     * 和 brain.cwd 的区别是权限方向：brain.cwd 里它能**改**文件，
     * 这里只是能**读出来发出去**。用户想把桌面上的安装包发到群里时，
     * 不该为此把"能在桌面改文件"的权限也一起给出去。
     */
    std::vector<std::string> fileRoots;

    /**
     * 判定"历史回放结束"的安静时长（毫秒）：服务端开始回放后，只要还在收数据就
     * 一直推迟结束，连续这么久没新行才认为回放完了。
     *
     * 线上服务器一般开着 keepchathistory / offlinemessages：登录那一刻服务端会把
     * 错过的历史**用原始 SAY/RMSG 行重放一遍**，里面完全可能有人 @ 过机器人。
     * 不丢的话，机器人一上线就对着几个小时前的 10 条老消息逐条回答 —— 群里看到的
     * 是一串答非所问。历史行没有序号，协议上分不出来，只能靠这段"回放窗口"。
     *
     * 默认 300 毫秒：回放是一口气写完的，行与行之间不会有这么长的空档；
     * 而调太大（比如 2 秒）会让**刚上线的头两秒完全不理人**。
     */
    int historySettleMs = 300;

    // ---- 日志与状态 ----
    std::string logPath = "dchat-dsh-bot.log";
    bool verbose = false;
    /** 每个房间的 DSH 会话 id 存这里，重启后还能接上话。 */
    std::string sessionFile = "dchat-dsh-bot-sessions.txt";

    /**
     * 心跳：每隔多久发一次 PING（毫秒），以及多久没收到 PONG 就认定连接已死。
     *
     * 为什么必须有：TCP 连接可以在**两端都不报错**的情况下死掉（NAT/代理/运营商
     * 设备把空闲连接的表项清了）。这时 recv 既不返回 0 也不报错，机器人还以为自己
     * 在线 —— 群里 @ 它毫无反应，而日志里连"断开"都没有。实测在用户网络上
     * 空闲约 10 分钟就会这样。
     *
     * 超时之后主动断开重连：服务端那边还挂着旧连接，重连会把它顶掉。
     */
    int pingIntervalMs = 30000;
    int pingTimeoutMs = 90000;

    /**
     * 本机管理界面的端口（127.0.0.1，0 = 不开）。
     *
     * 只监听回环：它是"改机器人设置"的入口，里面没有密码，绝不能给外网看。
     */
    int uiPort = 8731;
    /**
     * 管理界面绑哪个地址。默认只听本机；要在公网打开就写 0.0.0.0
     * —— 界面本身有登录（用 dchat 账号，且必须 op 或所在群的群主）。
     */
    std::string uiBind = "127.0.0.1";
    /**
     * op 名单文件（一行一个昵称，# 开头是注释）。默认指向 dchat 服务端用的那份，
     * 机器人跑在同一台机器上时直接复用，不用再维护第二份。
     */
    std::string uiOpFile =
#ifdef _WIN32
        "";
#else
        "/var/lib/dchat/dchat-admins.txt";
#endif

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

/**
 * 交叉校验（单行都合法、合起来不成立的组合）。
 * 解析和"管理界面保存"两条路都走它，免得两边规则各写一套。
 */
bool ValidateConfig(Config* config, std::string* error);

/**
 * 把几个键的新值写回配置文本，**保留注释和别的行**。
 *
 * 为什么不是"整份重写"：配置文件里那些注释是给以后改它的人看的，
 * 从界面点一下保存就把注释全抹掉，等于把文档一起删了。
 * 键不存在的追加到末尾（append 顺序 = changes 的顺序）。
 */
std::string UpdateConfigText(const std::string& text,
                             const std::vector<std::pair<std::string, std::string>>& changes);

/** 把值转成能写进配置文件的形式（换行/制表符/反斜杠要转义）。 */
std::string EscapeConfigValue(const std::string& value);

/** 注释齐全的示例配置（`--example` 打印它，部署时直接重定向成文件）。 */
std::string ExampleConfig();

/** 大小写不敏感地比较两个昵称。 */
bool SameNick(const std::string& a, const std::string& b);

}  // namespace dshbot
}  // namespace dchat
