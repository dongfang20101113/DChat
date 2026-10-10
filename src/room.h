// 多会话（房间）模型：大厅 / 群聊 / 私聊。
//
// 为什么要有这一层：dchat 以前**只有一个全局房间** —— Broadcast() 发给所有连接，
// 聊天记录、文件池、成员名单都是单份全局的，协议里根本没有"房间"这个概念。
// 私聊和群聊要落地，第一步就必须把"单房间"变成"多会话"，否则后面每一个功能
// （群公告、群文件、加群审核、免打扰、未读……）都会写成一堆针对全局状态的特例。
//
// 这个文件**刻意不碰网络、不碰文件系统**，只做三件事：
//   1. 数据结构：一个房间长什么样、成员有哪些角色；
//   2. 规则判定：谁能改哪个设置、谁能进群、谁能踢人 —— 全部是纯函数；
//   3. 序列化：dchat-rooms.txt 的读写（同样是纯字符串进出）。
// 这样"权限判错"这种最危险、最难在真机上复现的问题，可以用单测钉死。
//
// ---------------------------------------------------------------------------
// 权限分两栏（用户明确要求区分，代码里也物理隔开，避免哪天混着混着就串了）
// ---------------------------------------------------------------------------
//   群管理员可设（存服务端、广播全群）：群名、最大人数、加群方式、是否公开可搜、
//                                     群公告、服务端缓存预算、重要提醒开关、
//                                     成员群昵称与角色、踢人、审核入群。
//   成员本地设置（只在各自客户端，**永不上传**）：免打扰、置顶、本地缓存上限、
//                                     删除本地记录、重要消息是否仍然提醒。
// 后者放在 RoomPrefs 里，服务端代码一行都不会引用它 —— 这就是"物理隔开"。
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dchat {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/** 大厅的房间 ID。**保留字**：任何群都不许占用，见 NormalizeGroupId。 */
inline constexpr const char* kHallId = "hall";

/**
 * 私聊房间 ID 的前缀。
 *
 * 为什么用 `dm-` 而不是别的：群 ID 的合法字符集是 `[a-z0-9_-]`，`dm-` 落在里面，
 * 所以必须**显式**把 `dm-` 开头列为保留前缀，否则用户能自己建一个叫 `dm-1` 的
 * 群，跟服务端分配的私聊房间撞号。加保留前缀是最省事且不会漏的写法。
 */
inline constexpr const char* kDmIdPrefix = "dm-";

inline constexpr std::size_t kMinGroupIdChars = 3;
inline constexpr std::size_t kMaxGroupIdChars = 16;
inline constexpr std::size_t kMaxRoomNameChars = 24;      // 群名上限（Unicode 码点）
inline constexpr std::size_t kMaxRoomAnnounceChars = 500; // 群公告上限
inline constexpr std::size_t kMaxMemberDisplayChars = 12; // 群昵称上限，和账号昵称一致

inline constexpr int kMinGroupMembers = 2;
inline constexpr int kMaxGroupMembers = 2000;
inline constexpr int kDefaultGroupMembers = 200;

// 服务端"总缓存最大大小"（该群历史消息 + 群文件在服务器上的保留预算，单位 MB）
inline constexpr unsigned long long kMinRoomCacheMb = 1;
inline constexpr unsigned long long kMaxRoomCacheMb = 64 * 1024;  // 64 GB
inline constexpr unsigned long long kDefaultGroupCacheMb = 256;
/** 大厅的缓存预算：它承载全服公共历史，给得比普通群宽松。 */
inline constexpr unsigned long long kDefaultHallCacheMb = 1024;

// 成员本地缓存上限（客户端自己的磁盘占用，单位 MB）
inline constexpr unsigned long long kMinLocalCacheMb = 8;
inline constexpr unsigned long long kMaxLocalCacheMb = 64 * 1024;
inline constexpr unsigned long long kDefaultLocalCacheMb = 128;

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------

/**
 * 房间种类。
 *
 * Hall 在数据结构上就是**一个群聊**（有成员表、有群公告、有群文件、有管理员），
 * 唯一的区别是三条硬规则：不可删除、不可退出、新成员默认免打扰。
 * 把它单独列一个枚举值，是为了让这三条规则在代码里只写一次（HasFixedMembership）。
 */
enum class RoomKind { Hall, Group, Direct };

const char* RoomKindText(RoomKind kind);  // "hall" / "group" / "dm"
bool ParseRoomKind(const std::string& text, RoomKind* out);

/** 房间里的角色。Owner 一个房间只有一个（群主）。 */
enum class RoomRole { Owner, Admin, Member };

const char* RoomRoleText(RoomRole role);  // "owner" / "admin" / "member"
bool ParseRoomRole(const std::string& text, RoomRole* out);
/** 群主或管理员 = 能改群设置、能踢人、能审核入群。 */
bool RoomRoleCanAdminister(RoomRole role);

/**
 * 加群方式。
 *
 * 三个值的差别必须说清楚，否则"直接添加"和"自动审核"看起来是一回事：
 *   Direct   任何人知道 ID 就能直接进来，服务端不拦。
 *   Approval 申请后进待审队列，**必须**管理员点同意才进得来。
 *   Auto     申请后进待审队列，但服务端**立刻自动批准**。刻意保留那条申请记录：
 *            管理员能在入群记录里看到谁什么时候进来的，只是不用他动手。
 * 换句话说 Auto = Approval 的"免人工"版本，两者都不放行"连申请都不发就进来"。
 */
enum class JoinMode { Direct, Approval, Auto };

const char* JoinModeText(JoinMode mode);  // "direct" / "approval" / "auto"
bool ParseJoinMode(const std::string& text, JoinMode* out);
std::string JoinModeLabel(JoinMode mode);  // 中文标签，界面直接用

// ---------------------------------------------------------------------------
// 房间 ID
// ---------------------------------------------------------------------------

enum class RoomIdError { None, Empty, TooShort, TooLong, IllegalChar, Reserved };

/**
 * 规范化并校验用户自填的群 ID。
 *
 * 规则：长度 3-16，只允许 `[a-z0-9_-]`，**统一转小写**，且不能占用保留名。
 *
 * 为什么强制小写：ID 是「不可更改」的，如果允许大小写，`MyGroup` 和 `mygroup`
 * 就是两个不同的群 —— 用户搜不到自己刚建的群、复制粘贴多一个大写字母就进不去，
 * 而因为不可改，他连补救的机会都没有。转小写这一步把这类问题在源头消掉；
 * 界面上会**先显示规范化后的 ID 再让用户确认**，不做静默改名。
 *
 * 为什么不许中文：ID 要跨三端、要能出现在 `/group join <id>` 这种命令行里、
 * 还要能当文件名（每个房间的历史与文件目录按 ID 命名）。限定 ASCII 让这三件事
 * 都不会出意外；想用中文，那是「群名」的活，群名随便起。
 *
 * @return 规范化后的 ID；不合法时返回空串，原因写入 error
 */
std::string NormalizeGroupId(const std::string& raw, RoomIdError* error = nullptr);
const char* RoomIdErrorText(RoomIdError error);

/** 已经是规范化形式的 ID 是否合法（群 ID 和私聊 ID 都能过）。 */
bool IsValidRoomId(const std::string& id);
bool IsHallId(const std::string& id);
bool IsDmId(const std::string& id);

/** 服务端分配私聊房间号：dm-000001、dm-000002…… 纯格式化，分配逻辑在服务端。 */
std::string FormatDmId(unsigned long long serial);
/** 解析 FormatDmId 生成的号；不是私聊 ID 或格式不对返回 false。 */
bool ParseDmSerial(const std::string& id, unsigned long long* out);

// ---------------------------------------------------------------------------
// 房间数据
// ---------------------------------------------------------------------------

struct RoomMember {
    std::string nick;         // 账号名，房间内唯一
    std::string displayName;  // 群昵称，空 = 用账号名
    RoomRole role = RoomRole::Member;

    /** 在群里显示的名字：有群昵称就用群昵称。 */
    const std::string& Display() const { return displayName.empty() ? nick : displayName; }
};

/**
 * 一个房间的全部状态。
 *
 * 注意 members / pending 是**有序的**：入群顺序就是加入时间顺序，
 * "群主退群时把群主转给最早加入的管理员"这条规则靠它实现（见 LeaveRoom）。
 */
struct Room {
    std::string id;
    RoomKind kind = RoomKind::Group;
    std::string name;   // 群名 / 私聊时为空（界面显示对方昵称）
    std::string owner;  // 群主账号名；私聊为空
    std::vector<RoomMember> members;
    std::vector<std::string> pending;  // 待审队列（昵称，按申请顺序）

    // ---- 以下是群管理员可改的设置项 ----
    int maxMembers = kDefaultGroupMembers;
    JoinMode joinMode = JoinMode::Approval;
    bool listed = false;  // 勾选后才会出现在群搜索里
    std::string announcement;
    unsigned long long cacheMaxMb = kDefaultGroupCacheMb;  // 服务端保留预算
    bool importantNotice = true;  // 是否允许在本群发"重要提醒"（@全体）

    bool IsHall() const { return kind == RoomKind::Hall; }
    bool IsDirect() const { return kind == RoomKind::Direct; }
    bool IsGroup() const { return kind == RoomKind::Group; }
};

/** 新建一个大殿（服务端启动时创建一个，全局唯一）。 */
Room MakeHallRoom();

/**
 * 新建一个群：建群者成为群主，同时是第一个成员。
 * 不做 ID 合法性校验（调用方先用 NormalizeGroupId 过一遍），
 * 但会保证 owner 与 members 一致 —— 这两者不一致是后患无穷的状态。
 */
Room MakeGroupRoom(const std::string& id, const std::string& name, const std::string& owner);

/** 新建立/打开一个私聊房间：两个成员，没有群主、没有群设置。 */
Room MakeDmRoom(const std::string& id, const std::string& a, const std::string& b);

// ---- 成员查询 ----
const RoomMember* FindMember(const Room& room, const std::string& nick);
RoomMember* FindMember(Room& room, const std::string& nick);
bool IsRoomMember(const Room& room, const std::string& nick);
/** 取角色；不是成员返回 false（此时 *out 不动）。 */
bool TryRoleOf(const Room& room, const std::string& nick, RoomRole* out);
/** 不是成员时按 Member 算 —— 只在"确定是成员"的分支之后用，别拿它当成员判定。 */
RoomRole RoleOf(const Room& room, const std::string& nick);

/**
 * 这个房间的成员关系是不是**不可变更**的。
 *
 * 大厅为真：不可退出、不可被踢、也不参与"人数上限"检查（它是全服公共房间，
 * 上限对它没有意义）。私聊为假但另有规则（私聊不能"加入"，只能被建立）。
 */
bool HasFixedMembership(const Room& room);

// ---------------------------------------------------------------------------
// 加入 / 退出 / 审核 / 踢人
// ---------------------------------------------------------------------------

enum class JoinResult {
    Joined,         // Direct 模式：直接成为成员
    AutoJoined,     // Auto 模式：申请即自动放行。**服务端要额外给管理员一条通知**，
                    // 否则"自动审核"和"直接添加"对管理员来说完全没差别，等于白设。
    Queued,         // Approval 模式：进了待审队列，等管理员点同意
    AlreadyMember,  // 已经在群里
    Full,           // 人数已满
    NotJoinable,    // 这个房间不接受加入（大厅/私聊）
    AlreadyPending, // 已经在待审队列里了，别重复申请
};

const char* JoinResultText(JoinResult result);

/** 有人申请加群。Direct 直接进；Approval 进队列；Auto 进队列并立刻自动放行。 */
JoinResult RequestJoin(Room* room, const std::string& nick);

/** 管理员审核。accept=false 时从队列里去掉（拒绝）。返回 false 表示队列里没有这个人。 */
bool ResolveJoinRequest(Room* room, const std::string& nick, bool accept);

bool IsPending(const Room& room, const std::string& nick);

/**
 * 退出群聊。
 *
 * 群主退群的处理：自动把群主转给**最早加入的管理员**，没有管理员就转给最早加入的
 * 其他成员。为什么不拒绝群主退群、要求他先手动转让：多一条"你还得先做 X"的规则，
 * 用户就要多学一件事，而自动转让的结果是可预测的（转给资历最老的管理者）。
 * 群里只剩群主一人时返回 false —— 那时候没人可转，直接退掉会让群变成没有群主的
 * 孤儿房间。
 *
 * @param newOwner 群主转让时写入新群主昵称（可为 nullptr）
 */
bool LeaveRoom(Room* room, const std::string& nick, std::string* newOwner = nullptr);

/**
 * 踢人。权限检查由调用方做（服务端知道谁在操作），这里只管数据一致性：
 * 不能踢群主、不能踢不存在的人、大厅不能踢。
 */
bool KickMember(Room* room, const std::string& target, std::string* error);

/** 改群昵称。校验长度与字符（和账号昵称同一套规则）。空串 = 恢复用账号名。 */
bool SetMemberDisplayName(Room* room, const std::string& nick, const std::string& display,
                          std::string* error);

/**
 * 改成员角色。群主可以设/撤管理员；**不能把自己降级**（否则群会没有群主），
 * 也不能改别的群主（一个房间只有一个群主）。
 */
bool SetMemberRole(Room* room, const std::string& nick, RoomRole role, std::string* error);

// ---------------------------------------------------------------------------
// 群设置：谁能改、怎么改
// ---------------------------------------------------------------------------

enum class RoomSettingKey {
    Name,             // 群名
    MaxMembers,       // 最大人数
    JoinMode,         // 加群方式
    Listed,           // 向公众展示（可被搜索到）
    CacheMaxMb,       // 总缓存最大大小（服务端保留预算）
    Announcement,     // 群公告
    ImportantNotice,  // 重要提醒开关
};

bool ParseRoomSettingKey(const std::string& text, RoomSettingKey* out);
const char* RoomSettingKeyText(RoomSettingKey key);
/** 界面和 /group help 用的中文说明。 */
std::string RoomSettingLabel(RoomSettingKey key);
/** 取值范围说明（报错时附上，省得用户猜）。 */
std::string RoomSettingRangeText(RoomSettingKey key);

/**
 * 改这一项需要什么角色。
 *
 * 目前全部是 Admin（群主或管理员）。单独留这个函数是因为"哪些只有群主能改"这件事
 * 迟早会变（比如以后加"解散群聊"），到时候只改这一处，不用去翻每个调用点。
 */
RoomRole RoomSettingRequiredRole(RoomSettingKey key);
bool CanChangeRoomSetting(RoomRole role, RoomSettingKey key);

struct RoomSettingChange {
    bool ok = false;       // 指令本身合法（值能解析、在范围内）
    bool changed = false;  // 值是否真的变了（没变就不用广播）
    std::string text;      // 给操作者看的中文说明
    std::string value;     // 变更后的规范化值（广播给客户端用）
};

/**
 * 应用一次设置修改。**不做权限检查** —— 权限是"谁在操作"决定的，
 * 调用方先用 CanChangeRoomSetting 判过再进来。
 */
RoomSettingChange ApplyRoomSetting(Room* room, RoomSettingKey key, const std::string& value);

/** 把房间当前设置打包成若干行，用于登录/进群时同步给客户端。 */
std::string RoomSettingLine(const Room& room, RoomSettingKey key);
std::vector<RoomSettingKey> AllRoomSettingKeys();

// ---------------------------------------------------------------------------
// 群搜索
// ---------------------------------------------------------------------------

/**
 * 这个房间该不该出现在群搜索里。
 *
 * 只有群、且管理员勾了"向公众展示"才会被搜到。大厅不进搜索（人人都在里面，
 * 搜它没有意义），私聊更不能被别人搜出来。
 */
bool IsSearchable(const Room& room);

/**
 * 正文里有没有「@全体成员」。
 *
 * 为什么这个判定要放在协议层、由服务端和客户端共用：它是**唯一能越过所有人
 * 免打扰设置**的东西（见 ContainsAllMention 在 server.cpp 里的用法）。如果只靠
 * 客户端自我约束，任何改过的客户端都能随便发；所以服务端必须自己认得出这一段文本。
 *
 * 认三种写法：`@全体成员`、`@all`（大小写不敏感）、`@所有人`。
 * 多认几种的理由是用户手打的习惯不统一，而"我明明 @ 了全体却只有管理员收到"
 * 这种不一致比多认一个别名危险得多。
 */
bool ContainsAllMention(const std::string& text);

/**
 * 校验一段"给人看的文本"（群名、群昵称之类）：非空、不超长、不含换行或控制字符。
 *
 * 控制字符那一条不是洁癖：这些文本会被 Base64 之后塞进按行分隔的协议与
 * dchat-rooms.txt，换行会让"一行一条"的格式直接错位，而错位之后
 * 受影响的不是一个字段、是整份文件。
 */
bool ValidateRoomText(const std::string& text, std::size_t maxChars, std::string* error,
                      const char* what);

/**
 * 关键词匹配。**大小写不敏感**，同时匹配群名和群 ID —— 用户可能记得 ID 也可能
 * 记得群名，只匹配其中一个都会让人觉得"搜不到"。
 */
bool RoomMatchesKeyword(const Room& room, const std::string& keyword);

// ---------------------------------------------------------------------------
// 成员本地设置（**只在客户端，永不上传**）
// ---------------------------------------------------------------------------
//
// 这一整块服务端代码一行都不引用。放在同一个头文件里只是因为它属于同一个
// "房间"概念，客户端（桌面端 + Linux 终端版）可以共用同一份纯逻辑。

struct RoomPrefs {
    bool muted = false;           // 免打扰：新消息不提示、不闪任务栏、不计数
    bool pinned = false;          // 置顶：会话列表里排在最上面
    bool alertImportant = true;   // 免打扰时，"重要提醒"是否仍然提示我
    unsigned long long localCacheMb = kDefaultLocalCacheMb;  // 本地缓存上限
};

/**
 * 一个房间**还没被用户改过设置**时的默认值。
 *
 * 大厅默认免打扰 —— 这是用户明确要求的：它是全服公共房间，消息最多，
 * 默认每来一条就提示一次会把人吵死。
 */
RoomPrefs DefaultRoomPrefs(RoomKind kind);

/** 本地设置的落盘格式：一行一个房间，`<房间ID>\t<muted>\t<pinned>\t<alertImportant>\t<localCacheMb>`。 */
std::string SerializePrefs(const std::string& roomId, const RoomPrefs& prefs);
bool ParsePrefsLine(const std::string& line, std::string* roomId, RoomPrefs* prefs);
/** 解析整个本地设置文件；返回的是"房间ID -> 设置"。 */
std::vector<std::pair<std::string, RoomPrefs>> ParsePrefsFile(const std::string& text);

// ---------------------------------------------------------------------------
// dchat-rooms.txt 的读写
// ---------------------------------------------------------------------------
//
// 格式：一行一条记录，字段用**制表符**分隔。三种记录：
//   R <id> <kind> <群名B64> <群主> <最大人数> <加群方式> <listed> <缓存MB> <重要提醒> <公告B64>
//   M <id> <昵称> <角色> <群昵称B64>
//   P <id> <昵称>
// R 必须先于它自己的 M/P 出现（解析时按顺序归属）。
//
// 为什么群名和公告用 Base64：它们可以含空格、制表符、换行，而协议和文件都是
// 行式的。Base64 之后只剩 `[A-Za-z0-9+/=]`，怎么切都不会切错。
//
// 大厅不写进文件：它是服务端启动时凭空建的，写进去反而多一份可能与代码不一致的
// 状态。私聊要写 —— 不写的话重启后私聊房间就没了，那两个用户的历史记录会变成
// 无人认领的孤儿文件。

std::string SerializeRooms(const std::vector<Room>& rooms);
std::vector<Room> ParseRooms(const std::string& text);

}  // namespace dchat
