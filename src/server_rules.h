// 服务器运行规则（用 /chatrule 指令修改，只能从服务器控制台改）
//
//   /chatrule                     列出所有规则和当前值
//   /chatrule <规则>               只看某条规则
//   /chatrule <规则> set <值>      设成某个值
//   /chatrule <规则> add <值>      在当前值上加
//   /chatrule <规则> remove <值>   在当前值上减
//   /chatrule <规则> true|false   布尔规则（keepchathistory）的写法
#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dchat {

struct ServerRules {
    int chatIntervalMs = 0;        // chatinterval：两条消息之间最少间隔（毫秒），0 = 不限制
    int documentSizeMb = 64;       // documentsize：单个文件最大 MB（默认 64，和以前一致）
    bool keepChatHistory = false;  // keepchathistory
    bool chatColor = true;         // chatcolor：允不允许聊天里用彩色代码（#RRGGBB / &a）。
                                   // 关掉后客户端的色码**原样显示**，不是把它删掉
    int maxServerTempMb = 1048;    // maxservertemp：服务端保存文件 + 聊天记录缓存的总上限（MB）

    // ---- 2026-10 新增：公网接入需要的限速与文本限制 ----
    // 全部遵循上面 chatinterval 的约定：**0 = 不限制**，这样升级后行为不变，
    // 由服务器管理员按需开启（公网服务器建议都设上）。
    int uploadRateKbps = 0;    // uploadrate：单个客户端上传限速（KB/s），0 = 不限制
    int downloadRateKbps = 0;  // downloadrate：单个客户端下载限速（KB/s），0 = 不限制
    int maxTextLength = 0;     // maxtextlen：单条消息最大字符数（Unicode 码点），0 = 不限制
    int maxTextLines = 0;      // maxtextlines：单条消息最大行数，0 = 不限制

    // ---- 2026-10 新增：公网加固 ----
    // 这几条是**给公网服务器用的**：不设的话，任何人都能开几千个连接把线程耗光，
    // 或者对着账号无限次试密码。局域网里不用管（默认值就是"不限制"）。
    int maxConnections = 0;        // maxconns：同时连接总数上限，0 = 不限制
    int maxConnectionsPerIp = 0;   // maxconnsperip：同一 IP 的同时连接数上限，0 = 不限制
    int loginFailLimit = 0;        // loginfails：同一 IP 在 5 分钟内允许的登录失败次数，0 = 不限制
    int handshakeTimeoutSec = 30;  // handshaketimeout：连上后多少秒内必须登录，0 = 不限制

    // ---- 2026-10 新增：注册路径防护 ----
    // 上面那一组防护全部作用在"已有账号"的路径上（登录失败、连接数），
    // 而**注册路径之前完全没有限制**。实测（tools/regflood.cpp）：开了全部规则之后，
    // 12 线程并发注册仍能跑到 363 个/秒、720 个全部成功、服务端零拦截，
    // 并且把正常用户的消息往返从 105 ms 拖到 10 秒超时。
    //
    // 换 IP 就能绕开所有基于 IP 的限制，所以这里刻意做成**两层**：
    //   registerinterval 抬高出单个 IP 的注册成本（换 IP 就失效）
    //   maxaccounts      兜住总量（换多少 IP 都没用，这是最后一道闸）
    int registerIntervalSec = 0;  // registerinterval：同一 IP 两次注册的最小间隔（秒），0 = 不限制
    int maxAccounts = 0;          // maxaccounts：账号总数上限，0 = 不限制

    // ---- 2026-10 新增：离线消息 ----
    // 别人说话时你不在线，那些消息以前是**直接丢掉**的。现在服务端把聊天记录
    // 落盘并记住每个人读到第几条，重新登录时把错过的补上。
    //
    // 默认 0 = 不补发：这是刻意的，升级后行为和以前完全一样（本项目一贯的约定）。
    // 要开就 /chatrule offlinemessages set 100。
    int offlineMessages = 0;  // offlinemessages：登录时最多补发多少条，0 = 不补发
};

/**
 * 登录失败窗口的固定长度（秒）。
 *
 * 刻意做成常量而不是规则：窗口长度调来调去对实际防护没什么帮助，
 * 反而多一条要记的规则。5 分钟是常见取值。
 */
inline constexpr int kLoginFailWindowSec = 300;

/**
 * 按 IP 统计登录失败次数，用于防爆破。
 *
 * 规则：同一个 IP 在 [kLoginFailWindowSec] 秒内失败达到 `loginfails` 次，
 * 就**在剩余窗口内直接拒绝登录**（连密码都不比对），窗口过期自动清零。
 * 登录成功也会立刻清零，免得正常用户偶尔打错几次被自己锁住。
 */
class LoginFailTracker {
public:
    /** 这个 IP 现在是不是被锁了（只在 limit > 0 时有意义）。 */
    bool IsBlocked(const std::string& ip, int limit) const;

    /** 记一次登录失败。 */
    void NoteFailure(const std::string& ip);

    /** 登录成功：把这个 IP 的记录清掉。 */
    void Clear(const std::string& ip);

    /** 顺手清理过期的记录，避免长期运行时 map 无限增长。 */
    void Sweep();

    std::size_t TrackedCount() const;

private:
    struct Record {
        int failures = 0;
        std::chrono::steady_clock::time_point windowStart{};
    };
    mutable std::mutex mutex_;
    std::map<std::string, Record> records_;
};

// 令牌桶限速器：**每个客户端一个**，桶容量等于 1 秒的量（允许 1 秒的突发）。
//
// 用法是"先记账、再按返回的毫秒数 Sleep"，用阻塞形成天然背压——
// 比丢包或断连都友好：发送方只是变慢，数据不会损坏。
struct RateLimiter {
    int kbps = 0;  // 0 = 不限制
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last{};

    void Configure(int rateKbps) {
        kbps = rateKbps;
        tokens = 0.0;
        last = std::chrono::steady_clock::time_point{};
    }

    // 记入 bytes 字节，返回**调用方应该等待的毫秒数**（0 = 不用等）
    int Consume(std::size_t bytes);
};

enum class RuleAction { Show, Set, Add, Remove, SetBool };

struct RuleChange {
    bool ok = false;        // 指令本身合法
    bool changed = false;   // 值是否真的被改了
    std::string rule;       // 规则名（小写）
    std::string message;    // 给用户看的中文说明
};

bool IsKnownRule(const std::string& name);
const std::vector<std::string>& AllRuleNames();

// 规则元数据：名字 + Tab 候选浮层里显示的灰色说明 + 是不是 true/false 的布尔规则。
// /chatrule 的补全用它，避免把规则名和说明写两遍。
struct RuleInfo {
    const char* name;
    const char* hint;
    bool isBool;
};
const std::vector<RuleInfo>& AllRuleInfos();
bool IsBoolRule(const std::string& name);

// 规则取值范围的文字说明（错误提示里要用）
std::string RuleRangeText(const std::string& name);
// 一条规则的当前值说明，例如 "chatinterval = 500 ms（两条消息之间最少间隔）"
std::string DescribeRule(const ServerRules& rules, const std::string& name);
// 所有规则的多行说明
std::string DescribeAllRules(const ServerRules& rules);

// 应用一次修改。value 用于数值规则，boolValue 用于布尔规则
RuleChange ApplyRule(ServerRules* rules, const std::string& name, RuleAction action, long long value,
                     bool boolValue);

// 打包成发给客户端的一行（客户端据此调整自己的"单文件上限"等本地检查）
std::string RulesLineForClient(const ServerRules& rules);

// ---- dchat-rules.txt 的读写（一行一条：规则名 值；以 # 开头的是注释）----
std::string SerializeRules(const ServerRules& rules);
// 解析规则文件：非法行跳过、超出范围的夹到范围内。
// 返回成功读到的规则条数；rules 就地更新（文件里没写的规则保持原值）
int ParseRules(const std::string& text, ServerRules* rules);

// 数值上限辅助（MB -> 字节）
unsigned long long RuleMbToBytes(int mb);

}  // namespace dchat
