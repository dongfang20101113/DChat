// 服务端控制台 / 管理员聊天指令的解析（纯逻辑，便于单元测试）
//
//   /ban <昵称> [时长]   加入黑名单（不给时长 = 永久）；到时自动解封
//   /kick <昵称>         暂时踢出房间（不封禁，可立刻重新加入）
//   /unban <昵称>        移出黑名单
//   /op <昵称>           授予管理员权限（之后可在聊天框里用 / 开头的指令）
//   /deop <昵称>         取消管理员权限
//   /say <文本>          发一条全服公告（客户端会大字居中、特殊颜色显示）
//   /bans                查看黑名单
//   /ops                 查看管理员名单
//   /changepassword <新密码>            改自己的密码（聊天框 / 控制台都能用）
//   /changepassword <昵称> <新密码>      改别人的密码（**只能**在服务器控制台用）
//   /help                帮助
//
// 判定规则（和 Minecraft 一样）：聊天内容只要**以 '/' 开头**就当作命令尝试——
// 即使指令名打错了（例如 /bann），也只在发送者本地报错，不会当成聊天消息广播出去。
// 不以 '/' 开头（包括 '/' 出现在句子中间）才是普通聊天消息。
#pragma once

#include <string>
#include <vector>

namespace dchat {

struct ServerCommand {
    enum class Kind {
        Unknown, Help, Ban, Kick, Unban, Op, Deop, Say, ListBans, ListOps, ChangePassword
    };
    Kind kind = Kind::Unknown;
    std::string name;    // 目标昵称
    std::string text;    // /say 的公告内容
    std::string password;  // /changepassword 的新密码
    int seconds = 0;     // /ban 的封禁时长（0 且 permanent=true 表示永久）
    bool permanent = false;
    bool knownCommand = false;  // 指令名是否是可识别的（用来区分"打错指令名"和"参数不对"）
    // 归一化后的指令名（小写、不含 '/'）。日志里用它，避免把 /changepassword 的密码写进日志
    std::string keyword;
    // 这次调用是否只允许来自服务器控制台（例如"给别的账号改密码"）
    bool consoleOnly = false;
    // 是否需要管理员权限。默认 true（和以前一样）；改自己的密码是自助操作，不需要管理员
    bool opRequired = true;
    std::string error;   // 解析失败的原因（中文）
};

// 解析一行指令，例如 "/ban alice 1h30m"、"/ban bob"（永久）
ServerCommand ParseServerCommand(const std::string& line);

// 这条聊天内容是否算"命令尝试"：以 '/' 开头就算（Minecraft 的规则）
bool IsCommandAttempt(const std::string& text);

// 这行是不是"改密码"指令尝试（客户端用它来避免把带密码的那一行记进输入历史）。
// 只按第一个词判断，所以参数写错、密码不合规则也能识别出来。
bool LooksLikePasswordCommand(const std::string& text);

// 记进输入框历史（按 ↑ 能翻回来）的内容。
// 普通聊天和普通指令原样返回；只有 /changepassword 会把密码那段去掉，
// 只留 "/changepassword " —— 这样指令能翻回来，密码不会又被显示在屏幕上。
std::string HistoryTextFor(const std::string& text);

// ---- Tab 指令补全（类似 Minecraft）----
// 输入以 '/' 开头、并且还在打指令名（还没有空格）时，按 Tab 依次循环补全；
// 只输入 "/" 就从第一个指令开始循环。指令名的补全之外，还支持**第一个参数是昵称**
// 的那几条指令（/ban /kick /unban /op /deop）按在线昵称补全。
struct CompletionResult {
    std::string text;                  // 补全后的输入框内容（Suggest 时就是原文本）
    std::string head;                  // 正在补的那个词之前的内容（含分隔符）
    std::vector<std::string> matches;  // 本轮的全部候选（指令带 '/'，昵称原样）
    std::vector<std::string> hints;    // 和 matches 一一对应：灰色的用法说明（可能为空）
    std::vector<std::string> groups;   // 和 matches 一一对应：分组名（候选按分组连续排列）
    int picked = -1;                   // 这次 Tab 选中了第几个候选（-1 = 没选）
    bool isArgument = false;           // true = 正在补参数（昵称）
    bool firstOfRound = false;         // 本轮第一次按 Tab
};

class TabCompleter {
public:
    // 按一次 Tab：text 是输入框当前内容，nickNames 是当前在线昵称（补参数时用）
    CompletionResult Next(const std::string& text, const std::vector<std::string>& nickNames);

private:
    std::string start_;       // 这一轮补全开始时的文本（用户改了内容就重新开始）
    std::string lastOutput_;  // 上一次补出来的结果
    int index_ = -1;
};

// 只算候选、不循环也不改文本：界面用它实时刷新"输入框上方的候选浮层"
CompletionResult Suggest(const std::string& text, const std::vector<std::string>& nickNames);

// 所有指令名（不含 '/'），按分组顺序：Tab 循环和帮助都用它
const std::vector<std::string>& AllCommandNames();

// 时长解析：支持 w/d/h/m/s，可组合；不带单位的数字按秒算
bool ParseDuration(const std::string& text, int* outSeconds);

// 秒数格式化成人话：5400 -> "1 小时 30 分"；0 -> "永久"
std::string FormatBanDuration(int seconds, bool permanent);

// 控制台帮助文本
std::string ServerCommandHelp();

}  // namespace dchat
