// 多会话（房间）模型的实现。全部是纯逻辑：不碰网络、不碰文件系统。
// 设计理由见 room.h 顶部的注释。
#include "room.h"

#include <algorithm>
#include <cctype>

#include "file_transfer.h"  // Base64Encode / Base64Decode
#include "protocol.h"       // Utf8CharCount / Utf8Truncate / ParseUint64 / NormalizeNick

namespace dchat {
namespace {

/** ASCII 小写化。中文没有大小写，所以按字节处理是安全的（非 ASCII 字节原样保留）。 */
std::string AsciiLower(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/** 解析十进制正整数；空串、有非数字、超范围都返回 false。 */
bool ParseInt(const std::string& text, long long* out) {
    if (text.empty()) return false;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = (text[0] == '-');
        index = 1;
        if (text.size() == 1) return false;
    }
    long long value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > 1000000000LL) return false;  // 这些设置项都远小于 10 亿，超了就是乱填
    }
    if (out) *out = negative ? -value : value;
    return true;
}

bool ParseBool(const std::string& text, bool* out) {
    const std::string lower = AsciiLower(text);
    if (lower == "true" || lower == "1" || lower == "on" || lower == "yes" || lower == "是") {
        if (out) *out = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "off" || lower == "no" || lower == "否") {
        if (out) *out = false;
        return true;
    }
    return false;
}

/** 群名 / 群公告这类"给人看的文本"的通用校验：不能空、不能有控制字符。 */
bool ValidateHumanText(const std::string& text, std::size_t maxChars, std::string* error,
                       const char* what) {
    if (text.empty()) {
        if (error) *error = std::string(what) + "不能为空";
        return false;
    }
    if (Utf8CharCount(text) > maxChars) {
        if (error) *error = std::string(what) + "太长（最多 " + std::to_string(maxChars) + " 个字符）";
        return false;
    }
    for (unsigned char c : text) {
        // 换行和制表符会破坏"一行一条"的协议与文件格式；其余控制字符更是没理由出现
        if (c < 0x20 || c == 0x7f) {
            if (error) *error = std::string(what) + "不能包含换行或控制字符";
            return false;
        }
    }
    return true;
}

std::string TrimAsciiSpace(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举文本
// ---------------------------------------------------------------------------

const char* RoomKindText(RoomKind kind) {
    switch (kind) {
        case RoomKind::Hall:
            return "hall";
        case RoomKind::Group:
            return "group";
        case RoomKind::Direct:
            return "dm";
    }
    return "group";
}

bool ParseRoomKind(const std::string& text, RoomKind* out) {
    const std::string lower = AsciiLower(text);
    if (lower == "hall") {
        if (out) *out = RoomKind::Hall;
        return true;
    }
    if (lower == "group") {
        if (out) *out = RoomKind::Group;
        return true;
    }
    if (lower == "dm" || lower == "direct") {
        if (out) *out = RoomKind::Direct;
        return true;
    }
    return false;
}

const char* RoomRoleText(RoomRole role) {
    switch (role) {
        case RoomRole::Owner:
            return "owner";
        case RoomRole::Admin:
            return "admin";
        case RoomRole::Member:
            return "member";
    }
    return "member";
}

bool ParseRoomRole(const std::string& text, RoomRole* out) {
    const std::string lower = AsciiLower(text);
    if (lower == "owner") {
        if (out) *out = RoomRole::Owner;
        return true;
    }
    if (lower == "admin") {
        if (out) *out = RoomRole::Admin;
        return true;
    }
    if (lower == "member") {
        if (out) *out = RoomRole::Member;
        return true;
    }
    return false;
}

bool RoomRoleCanAdminister(RoomRole role) {
    return role == RoomRole::Owner || role == RoomRole::Admin;
}

const char* JoinModeText(JoinMode mode) {
    switch (mode) {
        case JoinMode::Direct:
            return "direct";
        case JoinMode::Approval:
            return "approval";
        case JoinMode::Auto:
            return "auto";
    }
    return "approval";
}

bool ParseJoinMode(const std::string& text, JoinMode* out) {
    const std::string lower = AsciiLower(text);
    if (lower == "direct" || lower == "0") {
        if (out) *out = JoinMode::Direct;
        return true;
    }
    if (lower == "approval" || lower == "admin" || lower == "1") {
        if (out) *out = JoinMode::Approval;
        return true;
    }
    if (lower == "auto" || lower == "2") {
        if (out) *out = JoinMode::Auto;
        return true;
    }
    return false;
}

std::string JoinModeLabel(JoinMode mode) {
    switch (mode) {
        case JoinMode::Direct:
            return "直接添加";
        case JoinMode::Approval:
            return "管理员审核";
        case JoinMode::Auto:
            return "自动审核";
    }
    return "管理员审核";
}

// ---------------------------------------------------------------------------
// 房间 ID
// ---------------------------------------------------------------------------

std::string NormalizeGroupId(const std::string& raw, RoomIdError* error) {
    auto fail = [&](RoomIdError kind) {
        if (error) *error = kind;
        return std::string();
    };
    if (error) *error = RoomIdError::None;

    if (raw.empty()) return fail(RoomIdError::Empty);
    // 先转小写再判长度：ASCII 转换不改变字节数，顺序其实无所谓，
    // 但先转能保证后面所有判断看到的都是同一个形式
    const std::string id = AsciiLower(raw);

    if (id.size() < kMinGroupIdChars) return fail(RoomIdError::TooShort);
    if (id.size() > kMaxGroupIdChars) return fail(RoomIdError::TooLong);

    for (char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'z';
        if (!digit && !lower && c != '_' && c != '-') return fail(RoomIdError::IllegalChar);
    }

    // 保留名。注意是在字符集校验**之后**判，否则 "HALL" 会因为先撞保留字而报错，
    // 提示就不准确了（转小写后它确实是保留字，报 Reserved 是对的）
    if (id == kHallId) return fail(RoomIdError::Reserved);
    if (id.rfind(kDmIdPrefix, 0) == 0) return fail(RoomIdError::Reserved);

    return id;
}

const char* RoomIdErrorText(RoomIdError error) {
    switch (error) {
        case RoomIdError::None:
            return "";
        case RoomIdError::Empty:
            return "群聊 ID 不能为空";
        case RoomIdError::TooShort:
            return "群聊 ID 太短（至少 3 个字符）";
        case RoomIdError::TooLong:
            return "群聊 ID 太长（最多 16 个字符）";
        case RoomIdError::IllegalChar:
            return "群聊 ID 只能用英文字母、数字、下划线和减号";
        case RoomIdError::Reserved:
            return "这个 ID 是系统保留的（hall 和 dm- 开头不能用作群聊 ID）";
    }
    return "群聊 ID 不合法";
}

bool IsHallId(const std::string& id) { return id == kHallId; }

bool IsDmId(const std::string& id) { return id.rfind(kDmIdPrefix, 0) == 0; }

bool IsValidRoomId(const std::string& id) {
    if (IsHallId(id)) return true;
    if (IsDmId(id)) {
        unsigned long long serial = 0;
        return ParseDmSerial(id, &serial);
    }
    RoomIdError error = RoomIdError::None;
    const std::string normalized = NormalizeGroupId(id, &error);
    // 规范化结果必须和输入**逐字节相同**：否则说明这个 ID 不是规范形式
    // （比如含大写），而"不规范但能通过"的 ID 会在查表时找不到自己
    return error == RoomIdError::None && normalized == id;
}

std::string FormatDmId(unsigned long long serial) {
    std::string digits = std::to_string(serial);
    // 补到 6 位：dm-000001。补零是为了让 ID 长度固定，界面列出来是齐的
    while (digits.size() < 6) digits.insert(digits.begin(), '0');
    return std::string(kDmIdPrefix) + digits;
}

bool ParseDmSerial(const std::string& id, unsigned long long* out) {
    const std::string prefix = kDmIdPrefix;
    if (id.size() <= prefix.size() || id.rfind(prefix, 0) != 0) return false;
    unsigned long long value = 0;
    if (!ParseUint64(id.substr(prefix.size()), &value)) return false;
    if (out) *out = value;
    return true;
}

// ---------------------------------------------------------------------------
// 房间构造
// ---------------------------------------------------------------------------

Room MakeHallRoom() {
    Room room;
    room.id = kHallId;
    room.kind = RoomKind::Hall;
    room.name = "大厅";
    room.owner.clear();  // 大厅没有群主：它的规则由服务端写死，不归任何人管
    room.maxMembers = 0;  // 0 = 不限人数（大厅本来就不该有人数上限）
    room.joinMode = JoinMode::Direct;
    room.listed = false;
    room.cacheMaxMb = kDefaultHallCacheMb;
    room.importantNotice = true;
    return room;
}

Room MakeGroupRoom(const std::string& id, const std::string& name, const std::string& owner) {
    Room room;
    room.id = id;
    room.kind = RoomKind::Group;
    room.name = name;
    room.owner = owner;
    if (!owner.empty()) {
        RoomMember member;
        member.nick = owner;
        member.role = RoomRole::Owner;
        room.members.push_back(member);
    }
    return room;
}

Room MakeDmRoom(const std::string& id, const std::string& a, const std::string& b) {
    Room room;
    room.id = id;
    room.kind = RoomKind::Direct;
    room.name.clear();
    room.owner.clear();
    room.maxMembers = 2;
    room.joinMode = JoinMode::Approval;  // 私聊不开放加入，这个值只是个占位
    room.listed = false;
    RoomMember first;
    first.nick = a;
    room.members.push_back(first);
    if (b != a) {
        RoomMember second;
        second.nick = b;
        room.members.push_back(second);
    }
    return room;
}

// ---------------------------------------------------------------------------
// 成员查询
// ---------------------------------------------------------------------------

const RoomMember* FindMember(const Room& room, const std::string& nick) {
    for (const RoomMember& member : room.members) {
        if (member.nick == nick) return &member;
    }
    return nullptr;
}

RoomMember* FindMember(Room& room, const std::string& nick) {
    for (RoomMember& member : room.members) {
        if (member.nick == nick) return &member;
    }
    return nullptr;
}

bool IsRoomMember(const Room& room, const std::string& nick) {
    return FindMember(room, nick) != nullptr;
}

bool TryRoleOf(const Room& room, const std::string& nick, RoomRole* out) {
    const RoomMember* member = FindMember(room, nick);
    if (!member) return false;
    if (out) *out = member->role;
    return true;
}

RoomRole RoleOf(const Room& room, const std::string& nick) {
    RoomRole role = RoomRole::Member;
    TryRoleOf(room, nick, &role);
    return role;
}

bool HasFixedMembership(const Room& room) { return room.kind == RoomKind::Hall; }

// ---------------------------------------------------------------------------
// 加入 / 退出 / 审核 / 踢人
// ---------------------------------------------------------------------------

const char* JoinResultText(JoinResult result) {
    switch (result) {
        case JoinResult::Joined:
            return "已加入";
        case JoinResult::AutoJoined:
            return "已自动通过审核，加入成功";
        case JoinResult::Queued:
            return "入群申请已提交，等待管理员审核";
        case JoinResult::AlreadyMember:
            return "你已经在这个群里了";
        case JoinResult::Full:
            return "群人数已满，加不进去了";
        case JoinResult::NotJoinable:
            return "这个会话不能加入";
        case JoinResult::AlreadyPending:
            return "你已经申请过了，等管理员审核就行";
    }
    return "";
}

JoinResult RequestJoin(Room* room, const std::string& nick) {
    if (!room || nick.empty()) return JoinResult::NotJoinable;
    // 大厅人人都在里面（登录时自动成为成员），私聊只能由服务端建立 —— 两者都不接受申请
    if (room->kind != RoomKind::Group) return JoinResult::NotJoinable;
    if (IsRoomMember(*room, nick)) return JoinResult::AlreadyMember;
    if (IsPending(*room, nick)) return JoinResult::AlreadyPending;
    if (room->maxMembers > 0 && static_cast<int>(room->members.size()) >= room->maxMembers) {
        return JoinResult::Full;
    }

    if (room->joinMode == JoinMode::Approval) {
        room->pending.push_back(nick);
        return JoinResult::Queued;
    }

    RoomMember member;
    member.nick = nick;
    member.role = RoomRole::Member;
    room->members.push_back(member);
    // Auto 模式刻意返回一个不同的结果：服务端据此给管理员发一条通知。
    // 如果这里也返回 Joined，"自动审核"就退化成了"直接添加"，管理员永远不知道
    // 有谁进来过 —— 那这个选项就没有存在意义了。
    return room->joinMode == JoinMode::Auto ? JoinResult::AutoJoined : JoinResult::Joined;
}

bool IsPending(const Room& room, const std::string& nick) {
    return std::find(room.pending.begin(), room.pending.end(), nick) != room.pending.end();
}

bool ResolveJoinRequest(Room* room, const std::string& nick, bool accept) {
    if (!room) return false;
    const auto it = std::find(room->pending.begin(), room->pending.end(), nick);
    if (it == room->pending.end()) return false;
    room->pending.erase(it);
    if (!accept) return true;
    if (IsRoomMember(*room, nick)) return true;  // 期间已经被别的管理员放进来了
    if (room->maxMembers > 0 && static_cast<int>(room->members.size()) >= room->maxMembers) {
        // 审核通过的瞬间人满了：把这个申请**放回队列**而不是默默丢掉，
        // 否则申请人那边看到"申请没了、人也没进来"，管理员也说不清发生了什么
        room->pending.insert(room->pending.begin(), nick);
        return false;
    }
    RoomMember member;
    member.nick = nick;
    member.role = RoomRole::Member;
    room->members.push_back(member);
    return true;
}

bool LeaveRoom(Room* room, const std::string& nick, std::string* newOwner) {
    if (!room) return false;
    // 大厅不可退出（用户明确要求）。这里返回 false，服务端会给出"大厅不能退出"的提示。
    if (HasFixedMembership(*room)) return false;
    // 私聊没有"退出"这一说：会话一直在，删掉只是删本地记录
    if (room->kind != RoomKind::Group) return false;

    RoomMember* member = FindMember(*room, nick);
    if (!member) return false;

    if (member->role == RoomRole::Owner) {
        // 群主退群：先把群主交出去，再走人
        RoomMember* successor = nullptr;
        for (RoomMember& candidate : room->members) {
            if (candidate.nick == nick) continue;
            if (candidate.role == RoomRole::Admin) {
                successor = &candidate;
                break;  // members 是有序的（入群顺序），第一个就是资历最老的管理员
            }
        }
        if (!successor) {
            for (RoomMember& candidate : room->members) {
                if (candidate.nick == nick) continue;
                successor = &candidate;
                break;  // 没有管理员就交给资历最老的普通成员
            }
        }
        // 群里只剩自己：没人可接，允许退出会让这个群变成没有群主的孤儿房间。
        // 让它退不掉，是这里唯一不会产生坏状态的选择。
        if (!successor) return false;
        successor->role = RoomRole::Owner;
        room->owner = successor->nick;
        if (newOwner) *newOwner = successor->nick;
    }

    const auto it = std::find_if(room->members.begin(), room->members.end(),
                                 [&](const RoomMember& m) { return m.nick == nick; });
    if (it != room->members.end()) room->members.erase(it);
    // 顺手把待审队列里的自己也清掉：不然退了群还挂着一条入群申请
    room->pending.erase(std::remove(room->pending.begin(), room->pending.end(), nick),
                        room->pending.end());
    return true;
}

bool KickMember(Room* room, const std::string& target, std::string* error) {
    if (!room) return false;
    if (HasFixedMembership(*room)) {
        if (error) *error = "大厅不能踢人";
        return false;
    }
    if (room->kind != RoomKind::Group) {
        if (error) *error = "私聊没有踢人这回事";
        return false;
    }
    RoomMember* member = FindMember(*room, target);
    if (!member) {
        if (error) *error = "这个人不在群里";
        return false;
    }
    if (member->role == RoomRole::Owner) {
        if (error) *error = "不能踢群主";
        return false;
    }
    const auto it = std::find_if(room->members.begin(), room->members.end(),
                                 [&](const RoomMember& m) { return m.nick == target; });
    room->members.erase(it);
    room->pending.erase(std::remove(room->pending.begin(), room->pending.end(), target),
                        room->pending.end());
    return true;
}

bool SetMemberDisplayName(Room* room, const std::string& nick, const std::string& display,
                          std::string* error) {
    if (!room) return false;
    RoomMember* member = FindMember(*room, nick);
    if (!member) {
        if (error) *error = "你不在这个群里";
        return false;
    }
    if (room->kind == RoomKind::Direct) {
        if (error) *error = "私聊没有群昵称";
        return false;
    }
    const std::string trimmed = TrimAsciiSpace(display);
    if (trimmed.empty()) {
        member->displayName.clear();  // 清空 = 恢复显示账号名
        return true;
    }
    if (Utf8CharCount(trimmed) > kMaxMemberDisplayChars) {
        if (error) *error = "群昵称太长（最多 " + std::to_string(kMaxMemberDisplayChars) + " 个字符）";
        return false;
    }
    // 复用账号昵称那一套校验：群昵称和账号昵称在界面上是同一个位置显示的，
    // 规则不一致只会让用户困惑（"为什么这个名字能起那个不能"）
    NickError nickError = NickError::None;
    if (NormalizeNick(trimmed, &nickError).empty()) {
        if (error) *error = NickErrorText(nickError);
        return false;
    }
    member->displayName = trimmed;
    return true;
}

bool SetMemberRole(Room* room, const std::string& nick, RoomRole role, std::string* error) {
    if (!room) return false;
    if (room->kind != RoomKind::Group) {
        if (error) *error = "只有群聊才有管理员";
        return false;
    }
    RoomMember* member = FindMember(*room, nick);
    if (!member) {
        if (error) *error = "这个人不在群里";
        return false;
    }
    if (role == RoomRole::Owner) {
        // 转让群主要同时改 owner 字段和两个人的角色，是个独立操作，不走这里
        if (error) *error = "群主只能转让，不能直接设置";
        return false;
    }
    if (member->role == RoomRole::Owner) {
        if (error) *error = "不能改群主的角色";
        return false;
    }
    member->role = role;
    return true;
}

// ---------------------------------------------------------------------------
// 群设置
// ---------------------------------------------------------------------------

bool ParseRoomSettingKey(const std::string& text, RoomSettingKey* out) {
    const std::string lower = AsciiLower(text);
    RoomSettingKey key = RoomSettingKey::Name;
    if (lower == "name") {
        key = RoomSettingKey::Name;
    } else if (lower == "maxmembers" || lower == "max") {
        key = RoomSettingKey::MaxMembers;
    } else if (lower == "joinmode" || lower == "join") {
        key = RoomSettingKey::JoinMode;
    } else if (lower == "listed" || lower == "public") {
        key = RoomSettingKey::Listed;
    } else if (lower == "cachemaxmb" || lower == "cache") {
        key = RoomSettingKey::CacheMaxMb;
    } else if (lower == "announcement" || lower == "ann") {
        key = RoomSettingKey::Announcement;
    } else if (lower == "importantnotice" || lower == "important") {
        key = RoomSettingKey::ImportantNotice;
    } else {
        return false;
    }
    if (out) *out = key;
    return true;
}

const char* RoomSettingKeyText(RoomSettingKey key) {
    switch (key) {
        case RoomSettingKey::Name:
            return "name";
        case RoomSettingKey::MaxMembers:
            return "maxmembers";
        case RoomSettingKey::JoinMode:
            return "joinmode";
        case RoomSettingKey::Listed:
            return "listed";
        case RoomSettingKey::CacheMaxMb:
            return "cachemaxmb";
        case RoomSettingKey::Announcement:
            return "announcement";
        case RoomSettingKey::ImportantNotice:
            return "importantnotice";
    }
    return "name";
}

std::string RoomSettingLabel(RoomSettingKey key) {
    switch (key) {
        case RoomSettingKey::Name:
            return "群聊名称";
        case RoomSettingKey::MaxMembers:
            return "最大人数";
        case RoomSettingKey::JoinMode:
            return "加群方式";
        case RoomSettingKey::Listed:
            return "向公众展示";
        case RoomSettingKey::CacheMaxMb:
            return "总缓存最大大小";
        case RoomSettingKey::Announcement:
            return "群公告";
        case RoomSettingKey::ImportantNotice:
            return "消息重要提醒";
    }
    return "";
}

std::string RoomSettingRangeText(RoomSettingKey key) {
    switch (key) {
        case RoomSettingKey::Name:
            return "1-" + std::to_string(kMaxRoomNameChars) + " 个字符";
        case RoomSettingKey::MaxMembers:
            return std::to_string(kMinGroupMembers) + "-" + std::to_string(kMaxGroupMembers) + " 人";
        case RoomSettingKey::JoinMode:
            return "direct（直接添加）/ approval（管理员审核）/ auto（自动审核）";
        case RoomSettingKey::Listed:
            return "true / false";
        case RoomSettingKey::CacheMaxMb:
            return std::to_string(kMinRoomCacheMb) + "-" + std::to_string(kMaxRoomCacheMb) + " MB";
        case RoomSettingKey::Announcement:
            return "最多 " + std::to_string(kMaxRoomAnnounceChars) + " 个字符";
        case RoomSettingKey::ImportantNotice:
            return "true / false";
    }
    return "";
}

RoomRole RoomSettingRequiredRole(RoomSettingKey key) {
    // 目前群主和管理员权限相同。留成函数是为了以后加"只有群主能解散/改 ID"这类项时
    // 只改这里一处，不用去翻所有调用点。
    (void)key;
    return RoomRole::Admin;
}

bool CanChangeRoomSetting(RoomRole role, RoomSettingKey key) {
    return RoomRoleCanAdminister(role) && RoomRoleCanAdminister(RoomSettingRequiredRole(key));
}

RoomSettingChange ApplyRoomSetting(Room* room, RoomSettingKey key, const std::string& value) {
    RoomSettingChange change;
    if (!room) {
        change.text = "房间不存在";
        return change;
    }
    if (room->kind == RoomKind::Direct) {
        change.text = "私聊没有群设置";
        return change;
    }

    auto reject = [&](const std::string& reason) {
        change.ok = false;
        change.changed = false;
        change.text = reason + "（" + RoomSettingLabel(key) + "：" + RoomSettingRangeText(key) + "）";
        return change;
    };
    auto accept = [&](const std::string& text, const std::string& normalized) {
        change.ok = true;
        change.text = text;
        change.value = normalized;
        return change;
    };

    switch (key) {
        case RoomSettingKey::Name: {
            // 群名是从命令行/界面来的，可能带用户手滑打的首尾空格；先修掉再校验长度，
            // 免得"多打了一个空格"变成"名字太长"
            const std::string trimmed = TrimAsciiSpace(value);
            std::string error;
            if (!ValidateHumanText(trimmed, kMaxRoomNameChars, &error, "群名")) {
                return reject(error);
            }
            const bool same = (trimmed == room->name);
            room->name = trimmed;
            change.changed = !same;
            return accept("群名已改为「" + trimmed + "」", trimmed);
        }
        case RoomSettingKey::MaxMembers: {
            long long parsed = 0;
            if (!ParseInt(value, &parsed)) return reject("最大人数只能是数字");
            if (parsed < kMinGroupMembers || parsed > kMaxGroupMembers) {
                return reject("最大人数超出范围");
            }
            // 只许调大不许调小到现有成员数以下：否则"设置成功"的下一秒群里就有
            // 一批超编成员，谁该被踢出去是个没人能回答的问题
            if (parsed < static_cast<long long>(room->members.size())) {
                return reject("最大人数不能小于当前成员数（现在有 " +
                              std::to_string(room->members.size()) + " 人）");
            }
            const bool same = (static_cast<int>(parsed) == room->maxMembers);
            room->maxMembers = static_cast<int>(parsed);
            change.changed = !same;
            return accept("最大人数已设为 " + std::to_string(parsed) + " 人",
                          std::to_string(parsed));
        }
        case RoomSettingKey::JoinMode: {
            JoinMode mode = JoinMode::Approval;
            if (!ParseJoinMode(value, &mode)) return reject("加群方式只能是上面三种之一");
            const bool same = (mode == room->joinMode);
            room->joinMode = mode;
            change.changed = !same;
            return accept("加群方式已设为「" + JoinModeLabel(mode) + "」", JoinModeText(mode));
        }
        case RoomSettingKey::Listed: {
            bool listed = false;
            if (!ParseBool(value, &listed)) return reject("向公众展示只能是 true 或 false");
            const bool same = (listed == room->listed);
            room->listed = listed;
            change.changed = !same;
            return accept(listed ? "已勾选「向公众展示」，其他人可以在群搜索里找到这个群"
                                 : "已取消「向公众展示」，群搜索里不再出现这个群",
                          listed ? "true" : "false");
        }
        case RoomSettingKey::CacheMaxMb: {
            unsigned long long parsed = 0;
            if (!ParseUint64(value, &parsed)) return reject("总缓存最大大小只能是数字（MB）");
            if (parsed < kMinRoomCacheMb || parsed > kMaxRoomCacheMb) {
                return reject("总缓存最大大小超出范围");
            }
            const bool same = (parsed == room->cacheMaxMb);
            room->cacheMaxMb = parsed;
            change.changed = !same;
            return accept("总缓存最大大小已设为 " + std::to_string(parsed) + " MB（超出后自动淘汰最旧的消息和文件）",
                          std::to_string(parsed));
        }
        case RoomSettingKey::Announcement: {
            // 公告允许清空（"撤销公告"是个正常操作，和群名不同）
            if (value.empty()) {
                const bool same = room->announcement.empty();
                room->announcement.clear();
                change.changed = !same;
                return accept("群公告已清空", "");
            }
            std::string error;
            if (!ValidateHumanText(value, kMaxRoomAnnounceChars, &error, "群公告")) {
                return reject(error);
            }
            const bool same = (value == room->announcement);
            room->announcement = value;
            change.changed = !same;
            return accept("群公告已更新", value);
        }
        case RoomSettingKey::ImportantNotice: {
            bool allowed = false;
            if (!ParseBool(value, &allowed)) return reject("消息重要提醒只能是 true 或 false");
            const bool same = (allowed == room->importantNotice);
            room->importantNotice = allowed;
            change.changed = !same;
            return accept(allowed ? "已开启「消息重要提醒」（管理员可以发 @全体成员 的重要消息）"
                                  : "已关闭「消息重要提醒」",
                          allowed ? "true" : "false");
        }
    }
    return reject("未知的设置项");
}

std::string RoomSettingLine(const Room& room, RoomSettingKey key) {
    // 线上格式：RSET <房间ID> <设置名> <值>
    // 群名和公告可能含空格，所以这两项的值是 **Base64**；其余项是纯 ASCII 字面量。
    // 客户端按设置名就知道该不该解码 —— 设置名是固定集合，不存在"猜"的问题。
    std::string value;
    switch (key) {
        case RoomSettingKey::Name:
            value = Base64Encode(room.name);
            break;
        case RoomSettingKey::MaxMembers:
            value = std::to_string(room.maxMembers);
            break;
        case RoomSettingKey::JoinMode:
            value = JoinModeText(room.joinMode);
            break;
        case RoomSettingKey::Listed:
            value = room.listed ? "true" : "false";
            break;
        case RoomSettingKey::CacheMaxMb:
            value = std::to_string(room.cacheMaxMb);
            break;
        case RoomSettingKey::Announcement:
            value = Base64Encode(room.announcement);
            break;
        case RoomSettingKey::ImportantNotice:
            value = room.importantNotice ? "true" : "false";
            break;
    }
    return BuildLine("RSET", room.id + " " + RoomSettingKeyText(key) + " " + value);
}

std::vector<RoomSettingKey> AllRoomSettingKeys() {
    return {RoomSettingKey::Name,       RoomSettingKey::MaxMembers,
            RoomSettingKey::JoinMode,   RoomSettingKey::Listed,
            RoomSettingKey::CacheMaxMb, RoomSettingKey::Announcement,
            RoomSettingKey::ImportantNotice};
}

// ---------------------------------------------------------------------------
// 群搜索
// ---------------------------------------------------------------------------

bool IsSearchable(const Room& room) {
    // 大厅不进搜索：所有人都在里面，搜出来也没有意义，反而每次都占满结果第一屏。
    // 私聊更不能被搜到。
    return room.kind == RoomKind::Group && room.listed;
}

bool ContainsAllMention(const std::string& text) {
    if (text.find("@全体成员") != std::string::npos) return true;
    if (text.find("@所有人") != std::string::npos) return true;
    return AsciiLower(text).find("@all") != std::string::npos;
}

bool ValidateRoomText(const std::string& text, std::size_t maxChars, std::string* error,
                      const char* what) {
    return ValidateHumanText(text, maxChars, error, what);
}

bool RoomMatchesKeyword(const Room& room, const std::string& keyword) {
    const std::string needle = AsciiLower(TrimAsciiSpace(keyword));
    if (needle.empty()) return false;
    if (AsciiLower(room.id).find(needle) != std::string::npos) return true;
    if (AsciiLower(room.name).find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 成员本地设置
// ---------------------------------------------------------------------------

RoomPrefs DefaultRoomPrefs(RoomKind kind) {
    RoomPrefs prefs;
    // 大厅默认免打扰：它是全服公共房间，消息量最大，默认每条都提示会把人吵死。
    // 这是用户明确要求的行为（"聊天室最大的房间……默认开启免打扰"）。
    prefs.muted = (kind == RoomKind::Hall);
    return prefs;
}

std::string SerializePrefs(const std::string& roomId, const RoomPrefs& prefs) {
    return roomId + "\t" + (prefs.muted ? "1" : "0") + "\t" + (prefs.pinned ? "1" : "0") + "\t" +
           (prefs.alertImportant ? "1" : "0") + "\t" + std::to_string(prefs.localCacheMb);
}

bool ParsePrefsLine(const std::string& line, std::string* roomId, RoomPrefs* prefs) {
    if (line.empty() || line[0] == '#') return false;
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    if (fields.size() < 5) return false;

    const std::string id = fields[0];
    if (!IsValidRoomId(id)) return false;

    RoomPrefs parsed;
    parsed.muted = (fields[1] == "1");
    parsed.pinned = (fields[2] == "1");
    parsed.alertImportant = (fields[3] == "1");
    unsigned long long cacheMb = kDefaultLocalCacheMb;
    if (ParseUint64(fields[4], &cacheMb)) {
        // 夹到合法范围内：这个文件是本地的、可能被手工改过或被旧版本写过，
        // 越界值不该让整个设置项失效
        if (cacheMb < kMinLocalCacheMb) cacheMb = kMinLocalCacheMb;
        if (cacheMb > kMaxLocalCacheMb) cacheMb = kMaxLocalCacheMb;
    } else {
        cacheMb = kDefaultLocalCacheMb;
    }
    parsed.localCacheMb = cacheMb;

    if (roomId) *roomId = id;
    if (prefs) *prefs = parsed;
    return true;
}

std::vector<std::pair<std::string, RoomPrefs>> ParsePrefsFile(const std::string& text) {
    std::vector<std::pair<std::string, RoomPrefs>> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string id;
        RoomPrefs prefs;
        if (ParsePrefsLine(line, &id, &prefs)) out.emplace_back(id, prefs);
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// dchat-rooms.txt
// ---------------------------------------------------------------------------

namespace {

/** 把一行按制表符切成字段。 */
std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::string DecodeBase64Text(const std::string& text) {
    std::vector<unsigned char> bytes;
    if (!Base64Decode(text, &bytes)) return std::string();
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

std::string SerializeRooms(const std::vector<Room>& rooms) {
    std::string out;
    out += "# dchat 房间名单（群聊 + 私聊），由服务端自动写回。\n";
    out += "# 手工编辑也可以，改完需要重启服务。大厅不在这里，它是服务端启动时建的。\n";
    out += "# R = 房间, M = 成员, P = 待审申请人；字段之间是制表符。\n";
    for (const Room& room : rooms) {
        if (room.kind == RoomKind::Hall) continue;  // 大厅由代码建，不落盘
        out += "R\t" + room.id + "\t" + RoomKindText(room.kind) + "\t" + Base64Encode(room.name) +
               "\t" + room.owner + "\t" + std::to_string(room.maxMembers) + "\t" +
               JoinModeText(room.joinMode) + "\t" + (room.listed ? "1" : "0") + "\t" +
               std::to_string(room.cacheMaxMb) + "\t" + (room.importantNotice ? "1" : "0") + "\t" +
               Base64Encode(room.announcement) + "\n";
        for (const RoomMember& member : room.members) {
            out += "M\t" + room.id + "\t" + member.nick + "\t" + RoomRoleText(member.role) + "\t" +
                   Base64Encode(member.displayName) + "\n";
        }
        for (const std::string& nick : room.pending) {
            out += "P\t" + room.id + "\t" + nick + "\n";
        }
    }
    return out;
}

std::vector<Room> ParseRooms(const std::string& text) {
    std::vector<Room> rooms;

    auto findRoom = [&](const std::string& id) -> Room* {
        for (Room& room : rooms) {
            if (room.id == id) return &room;
        }
        return nullptr;
    };

    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (end == text.size() && line.empty() && start >= text.size()) break;

        const std::vector<std::string> fields = SplitTabs(line);
        if (!fields.empty() && !fields[0].empty() && fields[0][0] != '#') {
            if (fields[0] == "R" && fields.size() >= 11) {
                Room room;
                room.id = fields[1];
                if (IsValidRoomId(room.id) && !IsHallId(room.id)) {
                    RoomKind kind = RoomKind::Group;
                    JoinMode mode = JoinMode::Approval;
                    unsigned long long cacheMb = kDefaultGroupCacheMb;
                    ParseRoomKind(fields[2], &kind);
                    room.kind = (kind == RoomKind::Hall) ? RoomKind::Group : kind;
                    room.name = DecodeBase64Text(fields[3]);
                    room.owner = fields[4];
                    long long maxMembers = kDefaultGroupMembers;
                    if (ParseInt(fields[5], &maxMembers)) room.maxMembers = static_cast<int>(maxMembers);
                    if (ParseJoinMode(fields[6], &mode)) room.joinMode = mode;
                    room.listed = (fields[7] == "1");
                    if (ParseUint64(fields[8], &cacheMb)) room.cacheMaxMb = cacheMb;
                    room.importantNotice = (fields[9] == "1");
                    room.announcement = DecodeBase64Text(fields[10]);
                    rooms.push_back(room);
                }
            } else if (fields[0] == "M" && fields.size() >= 5) {
                Room* room = findRoom(fields[1]);
                if (room && !fields[2].empty()) {
                    RoomMember member;
                    member.nick = fields[2];
                    RoomRole role = RoomRole::Member;
                    ParseRoomRole(fields[3], &role);
                    member.role = role;
                    member.displayName = DecodeBase64Text(fields[4]);
                    if (!FindMember(*room, member.nick)) room->members.push_back(member);
                }
            } else if (fields[0] == "P" && fields.size() >= 3) {
                Room* room = findRoom(fields[1]);
                if (room && !fields[2].empty() && !IsPending(*room, fields[2])) {
                    room->pending.push_back(fields[2]);
                }
            }
        }

        if (end == text.size()) break;
        start = end + 1;
    }

    // 收尾修正：文件可能被手工编辑成自相矛盾的状态，这里统一掰回一致。
    // 这些修正看着啰嗦，但"群主不在成员表里"或"没有群主"的房间一旦进了内存，
    // 后面每一次 LeaveRoom / SetMemberRole 都会走进没人预料到的分支。
    for (Room& room : rooms) {
        if (room.kind != RoomKind::Group) {
            room.owner.clear();
            continue;
        }
        RoomMember* owner = FindMember(room, room.owner);
        if (owner) {
            owner->role = RoomRole::Owner;
        } else if (!room.members.empty()) {
            // 群主字段指向一个不在群里的人（或为空）：把最早的成员扶正
            room.owner = room.members.front().nick;
            room.members.front().role = RoomRole::Owner;
        } else {
            room.owner.clear();
        }
        // 一个群里只能有一个 owner
        for (RoomMember& member : room.members) {
            if (member.nick != room.owner && member.role == RoomRole::Owner) {
                member.role = RoomRole::Admin;
            }
        }
    }

    return rooms;
}

}  // namespace dchat
