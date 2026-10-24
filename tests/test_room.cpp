// 房间模型单元测试：ID 规则、加入/退出/审核/踢人、群设置权限、搜索、序列化。
//
// 这一层是全项目里**最值得测得狠**的地方：权限判错、群主交接错、房间状态不一致，
// 这些问题在真机上要么复现不了、要么复现时已经是"群没了"这种不可逆的后果。
// 所以下面的用例刻意覆盖了大量**边界与拒绝路径**，而不是只测顺利情况。
#include <cstdio>
#include <string>
#include <vector>

#include "file_transfer.h"  // Base64Encode（核对群名/公告的上线编码）
#include "protocol.h"       // NickErrorText（核对群昵称复用了账号昵称的报错文案）
#include "room.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

void checkText(const std::string& actual, const std::string& expected, const char* what) {
    ++g_checks;
    if (actual == expected) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s（期望「%s」，实际「%s」）\n", what, expected.c_str(), actual.c_str());
    }
}

/** 造一个测试用群：群主 owner，另有 listed 里的成员（按顺序，全部是普通成员）。 */
dchat::Room MakeTestGroup(const std::string& owner, const std::vector<std::string>& others) {
    dchat::Room room = dchat::MakeGroupRoom("testgroup", "测试群", owner);
    for (const std::string& nick : others) {
        dchat::RoomMember member;
        member.nick = nick;
        room.members.push_back(member);
    }
    return room;
}

}  // namespace

int main() {
    std::printf("== dchat room model tests ==\n");

    // -----------------------------------------------------------------------
    std::printf("[1] 群聊 ID 规则：用户自填，填完不可改\n");
    {
        dchat::RoomIdError error = dchat::RoomIdError::IllegalChar;

        checkText(dchat::NormalizeGroupId("abc", &error), "abc", "普通小写 ID 通过");
        check(error == dchat::RoomIdError::None, "合法 ID 不报错");

        // 大写统一转小写：ID 不可改，如果允许大小写，MyGroup 和 mygroup 就是两个群，
        // 用户复制粘贴多一个大写字母就永远进不去自己建的群
        checkText(dchat::NormalizeGroupId("MyGroup", &error), "mygroup", "大写自动转小写");
        checkText(dchat::NormalizeGroupId("ABC-123_x", &error), "abc-123_x",
                  "字母数字下划线减号都允许");

        checkText(dchat::NormalizeGroupId("ab", &error), "", "太短（2 字符）被拒");
        check(error == dchat::RoomIdError::TooShort, "太短报 TooShort");
        checkText(dchat::NormalizeGroupId("abcdefghijklmnopq", &error), "", "太长（17 字符）被拒");
        check(error == dchat::RoomIdError::TooLong, "太长报 TooLong");
        checkText(dchat::NormalizeGroupId("abcdefghijklmnop", &error), "abcdefghijklmnop",
                  "正好 16 字符通过");
        checkText(dchat::NormalizeGroupId("abc", &error), "abc", "正好 3 字符通过");

        checkText(dchat::NormalizeGroupId("", &error), "", "空 ID 被拒");
        check(error == dchat::RoomIdError::Empty, "空报 Empty");
        checkText(dchat::NormalizeGroupId("ab c", &error), "", "含空格被拒");
        checkText(dchat::NormalizeGroupId("中文群", &error), "", "中文 ID 被拒（那是群名的活）");
        checkText(dchat::NormalizeGroupId("ab.c", &error), "", "含点号被拒");
        check(error == dchat::RoomIdError::IllegalChar, "非法字符报 IllegalChar");

        // 保留名：hall 是全局大厅，dm- 是服务端分配的私聊号段。
        // 不保留的话用户能建出 dm-1 这种群，跟私聊房间撞号。
        checkText(dchat::NormalizeGroupId("hall", &error), "", "hall 是保留字");
        check(error == dchat::RoomIdError::Reserved, "hall 报 Reserved");
        checkText(dchat::NormalizeGroupId("HALL", &error), "", "HALL 转小写后同样是保留字");
        checkText(dchat::NormalizeGroupId("dm-001", &error), "", "dm- 开头是保留前缀");
        checkText(dchat::NormalizeGroupId("dmgroup", &error), "dmgroup",
                  "dmgroup 不是保留前缀（只保留 dm- 带减号）");
    }

    // -----------------------------------------------------------------------
    std::printf("[2] 房间 ID「是否已是规范形式」的判定\n");
    {
        check(dchat::IsValidRoomId("hall"), "hall 是合法房间 ID");
        check(dchat::IsHallId("hall"), "IsHallId 认得 hall");
        check(!dchat::IsHallId("hall2"), "hall2 不是大厅");

        check(dchat::IsValidRoomId("mygroup"), "规范群 ID 合法");
        check(!dchat::IsValidRoomId("MyGroup"), "非规范形式（含大写）不算合法");
        check(!dchat::IsValidRoomId("ab"), "太短的群 ID 不合法");

        check(dchat::IsValidRoomId("dm-000001"), "私聊 ID 合法");
        check(dchat::IsDmId("dm-000001"), "IsDmId 认得私聊 ID");
        check(!dchat::IsDmId("mygroup"), "普通群 ID 不是私聊");
        check(!dchat::IsValidRoomId("dm-abc"), "dm- 后面必须是数字");
        checkText(dchat::FormatDmId(1), "dm-000001", "私聊号补零到 6 位");
        checkText(dchat::FormatDmId(123456), "dm-123456", "私聊号 6 位不补");
        checkText(dchat::FormatDmId(1234567), "dm-1234567", "超过 6 位照常展开");

        unsigned long long serial = 0;
        check(dchat::ParseDmSerial("dm-000042", &serial) && serial == 42, "私聊号解析正确");
        check(!dchat::ParseDmSerial("dm-", &serial), "只有前缀不算私聊号");
        check(!dchat::ParseDmSerial("hall", &serial), "大厅不是私聊号");
    }

    // -----------------------------------------------------------------------
    std::printf("[3] 房间构造：大厅 / 群聊 / 私聊\n");
    {
        const dchat::Room hall = dchat::MakeHallRoom();
        check(hall.IsHall(), "大厅 kind 是 Hall");
        check(hall.members.empty(), "大厅初始没有成员（登录时逐个加入）");
        check(hall.owner.empty(), "大厅没有群主");
        check(hall.maxMembers == 0, "大厅不设人数上限");
        check(!dchat::IsSearchable(hall), "大厅不进群搜索");
        check(dchat::HasFixedMembership(hall), "大厅是固定成员关系（不可退）");

        const dchat::Room group = dchat::MakeGroupRoom("mygroup", "我的群", "alice");
        check(group.IsGroup(), "群 kind 是 Group");
        check(group.members.size() == 1, "建群后只有一个成员");
        check(group.owner == "alice", "建群者是群主");
        check(group.members[0].role == dchat::RoomRole::Owner, "建群者的角色是 Owner");
        check(!dchat::HasFixedMembership(group), "普通群成员关系可变");
        check(!dchat::IsSearchable(group), "新建群默认不向公众展示");

        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        check(dm.IsDirect(), "私聊 kind 是 Direct");
        check(dm.members.size() == 2, "私聊有两个成员");
        check(dm.owner.empty(), "私聊没有群主");
        check(!dchat::IsSearchable(dm), "私聊不能被搜到");
        check(dm.maxMembers == 2, "私聊上限是 2");

        // 自己跟自己私聊（理论上不该发生，但真发生了不能多出一个成员）
        dchat::Room self = dchat::MakeDmRoom("dm-000002", "alice", "alice");
        check(self.members.size() == 1, "自己跟自己私聊只有一个成员，不会重复");
    }

    // -----------------------------------------------------------------------
    std::printf("[4] 成员与角色查询\n");
    {
        dchat::Room group = MakeTestGroup("alice", {"bob", "carol"});
        group.members[2].role = dchat::RoomRole::Admin;

        check(dchat::IsRoomMember(group, "alice"), "群主是成员");
        check(dchat::IsRoomMember(group, "carol"), "管理员是成员");
        check(!dchat::IsRoomMember(group, "dave"), "没进群的人不是成员");
        check(dchat::FindMember(group, "dave") == nullptr, "查不到的人返回空指针");

        dchat::RoomRole role = dchat::RoomRole::Member;
        check(dchat::TryRoleOf(group, "alice", &role) && role == dchat::RoomRole::Owner,
              "群主角色正确");
        check(dchat::TryRoleOf(group, "carol", &role) && role == dchat::RoomRole::Admin,
              "管理员角色正确");
        check(dchat::TryRoleOf(group, "bob", &role) && role == dchat::RoomRole::Member,
              "普通成员角色正确");
        check(!dchat::TryRoleOf(group, "dave", &role), "非成员取角色返回 false");
        check(role == dchat::RoomRole::Member, "取角色失败时不改动出参");

        check(dchat::RoomRoleCanAdminister(dchat::RoomRole::Owner), "群主能管理");
        check(dchat::RoomRoleCanAdminister(dchat::RoomRole::Admin), "管理员能管理");
        check(!dchat::RoomRoleCanAdminister(dchat::RoomRole::Member), "普通成员不能管理");

        // 群昵称：有就用，没有就用账号名
        checkText(group.members[0].Display(), "alice", "没设群昵称时显示账号名");
        group.members[0].displayName = "群主大人";
        checkText(group.members[0].Display(), "群主大人", "设了群昵称就显示群昵称");
    }

    // -----------------------------------------------------------------------
    std::printf("[5] 加群：三种加群方式\n");
    {
        // 直接添加：申请即入群
        dchat::Room direct = MakeTestGroup("alice", {});
        direct.joinMode = dchat::JoinMode::Direct;
        check(dchat::RequestJoin(&direct, "bob") == dchat::JoinResult::Joined,
              "直接添加：申请即入群");
        check(dchat::IsRoomMember(direct, "bob"), "直接添加后确实是成员");

        // 管理员审核：进待审队列，人还没进来
        dchat::Room approval = MakeTestGroup("alice", {});
        approval.joinMode = dchat::JoinMode::Approval;
        check(dchat::RequestJoin(&approval, "bob") == dchat::JoinResult::Queued,
              "管理员审核：进待审队列");
        check(!dchat::IsRoomMember(approval, "bob"), "待审期间还不是成员");
        check(dchat::IsPending(approval, "bob"), "待审队列里能查到");
        check(dchat::RequestJoin(&approval, "bob") == dchat::JoinResult::AlreadyPending,
              "重复申请被识别");

        // 自动审核：立刻进来，但结果值和"直接添加"不同 —— 服务端据此通知管理员
        dchat::Room autoRoom = MakeTestGroup("alice", {});
        autoRoom.joinMode = dchat::JoinMode::Auto;
        check(dchat::RequestJoin(&autoRoom, "bob") == dchat::JoinResult::AutoJoined,
              "自动审核：返回 AutoJoined 而不是 Joined");
        check(dchat::IsRoomMember(autoRoom, "bob"), "自动审核后已经是成员");

        // 已经在群里的人再申请
        check(dchat::RequestJoin(&direct, "bob") == dchat::JoinResult::AlreadyMember,
              "已在群里再申请被识别");

        // 人数上限
        dchat::Room small = MakeTestGroup("alice", {"bob"});
        small.maxMembers = 2;
        small.joinMode = dchat::JoinMode::Direct;
        check(dchat::IsRoomMember(small, "bob"), "满员群里已有 bob");
        check(dchat::RequestJoin(&small, "carol") == dchat::JoinResult::Full, "人数满了拒绝加入");

        // 大厅与私聊不接受"申请加入"
        dchat::Room hall = dchat::MakeHallRoom();
        check(dchat::RequestJoin(&hall, "bob") == dchat::JoinResult::NotJoinable,
              "大厅不接受申请加入");
        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        check(dchat::RequestJoin(&dm, "carol") == dchat::JoinResult::NotJoinable,
              "私聊不接受申请加入");
    }

    // -----------------------------------------------------------------------
    std::printf("[6] 入群审核\n");
    {
        dchat::Room room = MakeTestGroup("alice", {});
        room.joinMode = dchat::JoinMode::Approval;
        dchat::RequestJoin(&room, "bob");

        check(!dchat::ResolveJoinRequest(&room, "carol", true), "审核一个没申请的人失败");
        check(dchat::ResolveJoinRequest(&room, "bob", true), "同意 bob 的申请");
        check(dchat::IsRoomMember(room, "bob"), "同意后 bob 是成员");
        check(!dchat::IsPending(room, "bob"), "同意后从待审队列移除");

        // 拒绝路径
        dchat::RequestJoin(&room, "carol");
        check(dchat::ResolveJoinRequest(&room, "carol", false), "拒绝 carol 的申请");
        check(!dchat::IsRoomMember(room, "carol"), "被拒绝的人不是成员");
        check(!dchat::IsPending(room, "carol"), "被拒绝后从待审队列移除");

        // 审核通过的瞬间人满了：申请要**放回队列**，不能默默丢掉
        dchat::Room tiny = MakeTestGroup("alice", {});
        tiny.maxMembers = 2;
        tiny.joinMode = dchat::JoinMode::Approval;
        dchat::RequestJoin(&tiny, "bob");
        dchat::RequestJoin(&tiny, "carol");
        check(dchat::ResolveJoinRequest(&tiny, "bob", true), "第一个申请人正常入群（此时正好满员）");
        check(!dchat::ResolveJoinRequest(&tiny, "carol", true), "满员后同意 carol 失败");
        check(dchat::IsPending(tiny, "carol"), "失败的申请留在队列里，没有凭空消失");
        check(!dchat::IsRoomMember(tiny, "carol"), "carol 没能进来");
    }

    // -----------------------------------------------------------------------
    std::printf("[7] 退出群聊与群主交接\n");
    {
        // 普通成员退出
        dchat::Room room = MakeTestGroup("alice", {"bob", "carol"});
        check(dchat::LeaveRoom(&room, "bob"), "普通成员可以退出");
        check(!dchat::IsRoomMember(room, "bob"), "退出后不再是成员");
        checkText(room.owner, "alice", "普通成员退出不影响群主");

        // 群主退出：优先交给最早加入的**管理员**
        dchat::Room byAdmin = MakeTestGroup("alice", {"bob", "carol", "dave"});
        byAdmin.members[1].role = dchat::RoomRole::Member;  // bob 是普通成员
        byAdmin.members[2].role = dchat::RoomRole::Admin;   // carol 是管理员
        byAdmin.members[3].role = dchat::RoomRole::Admin;   // dave 也是管理员
        std::string newOwner;
        check(dchat::LeaveRoom(&byAdmin, "alice", &newOwner), "群主可以退出");
        checkText(newOwner, "carol", "群主交给最早加入的管理员");
        checkText(byAdmin.owner, "carol", "群主字段同步更新");
        check(dchat::RoleOf(byAdmin, "carol") == dchat::RoomRole::Owner, "新群主角色变成 Owner");
        check(dchat::RoleOf(byAdmin, "dave") == dchat::RoomRole::Admin, "另一个管理员不受影响");
        check(!dchat::IsRoomMember(byAdmin, "alice"), "退出的群主不再是成员");

        // 群主退出：没有管理员就交给最早加入的普通成员
        dchat::Room byMember = MakeTestGroup("alice", {"bob", "carol"});
        check(dchat::LeaveRoom(&byMember, "alice", &newOwner), "没有管理员时群主也能退出");
        checkText(byMember.owner, "bob", "群主交给最早加入的普通成员");

        // 群里只剩群主：退不掉（否则会留下一个没有群主的孤儿群）
        dchat::Room lonely = MakeTestGroup("alice", {});
        check(!dchat::LeaveRoom(&lonely, "alice"), "群里只剩群主时不允许退出");
        check(dchat::IsRoomMember(lonely, "alice"), "拒绝退出后群主还在群里");

        // 大厅不可退、私聊不可退
        dchat::Room hall = dchat::MakeHallRoom();
        dchat::RoomMember member;
        member.nick = "alice";
        hall.members.push_back(member);
        check(!dchat::LeaveRoom(&hall, "alice"), "大厅不能退出");
        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        check(!dchat::LeaveRoom(&dm, "alice"), "私聊没有退出这回事");

        // 退群时顺手清掉自己的待审申请
        dchat::Room applied = MakeTestGroup("alice", {"bob"});
        applied.pending.push_back("bob");
        dchat::LeaveRoom(&applied, "bob");
        check(!dchat::IsPending(applied, "bob"), "退出后待审队列里的自己也被清掉");
    }

    // -----------------------------------------------------------------------
    std::printf("[8] 踢人\n");
    {
        dchat::Room room = MakeTestGroup("alice", {"bob", "carol"});
        std::string error;

        check(dchat::KickMember(&room, "bob", &error), "可以踢普通成员");
        check(!dchat::IsRoomMember(room, "bob"), "被踢的人不在群里了");

        check(!dchat::KickMember(&room, "alice", &error), "不能踢群主");
        checkText(error, "不能踢群主", "踢群主给出明确原因");
        check(!dchat::KickMember(&room, "dave", &error), "不能踢不在群里的人");
        checkText(error, "这个人不在群里", "踢外人给出明确原因");

        dchat::Room hall = dchat::MakeHallRoom();
        check(!dchat::KickMember(&hall, "bob", &error), "大厅不能踢人");
        checkText(error, "大厅不能踢人", "大厅踢人给出明确原因");

        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        check(!dchat::KickMember(&dm, "bob", &error), "私聊不能踢人");

        // 被踢的人挂着的入群申请也要清掉
        dchat::Room room2 = MakeTestGroup("alice", {"bob"});
        room2.pending.push_back("bob");
        dchat::KickMember(&room2, "bob", &error);
        check(!dchat::IsPending(room2, "bob"), "被踢后待审队列里的申请也被清掉");
    }

    // -----------------------------------------------------------------------
    std::printf("[9] 群昵称\n");
    {
        dchat::Room room = MakeTestGroup("alice", {"bob"});
        std::string error;

        check(dchat::SetMemberDisplayName(&room, "bob", "小鲍勃", &error), "可以改自己的群昵称");
        checkText(dchat::FindMember(room, "bob")->Display(), "小鲍勃", "群昵称生效");

        check(dchat::SetMemberDisplayName(&room, "bob", "", &error), "传空串可以清掉群昵称");
        checkText(dchat::FindMember(room, "bob")->Display(), "bob", "清掉后显示账号名");

        check(!dchat::SetMemberDisplayName(&room, "carol", "x", &error), "不在群里的人改不了");
        checkText(error, "你不在这个群里", "非成员改昵称给出明确原因");

        check(!dchat::SetMemberDisplayName(&room, "bob", "这个昵称实在是太长了超过十二个字符", &error),
              "群昵称太长被拒");
        check(!dchat::SetMemberDisplayName(&room, "bob", "带 空格", &error), "群昵称含空格被拒");
        // 用和账号昵称同一套规则，避免"这个名字能起那个不能起"
        checkText(error, dchat::NickErrorText(dchat::NickError::IllegalChar),
                  "群昵称复用了账号昵称的报错文案");
    }

    // -----------------------------------------------------------------------
    std::printf("[10] 设/撤管理员\n");
    {
        dchat::Room room = MakeTestGroup("alice", {"bob", "carol"});
        std::string error;

        check(dchat::SetMemberRole(&room, "bob", dchat::RoomRole::Admin, &error), "可以设 bob 为管理员");
        check(dchat::RoleOf(room, "bob") == dchat::RoomRole::Admin, "bob 成了管理员");
        check(dchat::SetMemberRole(&room, "bob", dchat::RoomRole::Member, &error), "可以撤销 bob");
        check(dchat::RoleOf(room, "bob") == dchat::RoomRole::Member, "bob 变回普通成员");

        check(!dchat::SetMemberRole(&room, "alice", dchat::RoomRole::Member, &error),
              "不能把群主降级（否则群会没有群主）");
        checkText(error, "不能改群主的角色", "改群主角色给出明确原因");
        check(!dchat::SetMemberRole(&room, "carol", dchat::RoomRole::Owner, &error),
              "不能直接把别人设成群主");
        check(!dchat::SetMemberRole(&room, "dave", dchat::RoomRole::Admin, &error),
              "不能给不在群里的人设管理员");

        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        check(!dchat::SetMemberRole(&dm, "bob", dchat::RoomRole::Admin, &error),
              "私聊没有管理员");
    }

    // -----------------------------------------------------------------------
    std::printf("[11] 群设置权限：管理员可设，普通成员不可\n");
    {
        const std::vector<dchat::RoomSettingKey> keys = dchat::AllRoomSettingKeys();
        check(keys.size() == 7, "一共 7 个管理员可设项");

        for (dchat::RoomSettingKey key : keys) {
            check(dchat::CanChangeRoomSetting(dchat::RoomRole::Owner, key),
                  (std::string("群主能改：") + dchat::RoomSettingKeyText(key)).c_str());
            check(dchat::CanChangeRoomSetting(dchat::RoomRole::Admin, key),
                  (std::string("管理员能改：") + dchat::RoomSettingKeyText(key)).c_str());
            check(!dchat::CanChangeRoomSetting(dchat::RoomRole::Member, key),
                  (std::string("普通成员不能改：") + dchat::RoomSettingKeyText(key)).c_str());
        }

        // 设置名解析要认全，界面上写错一个字母会导致"设置没反应"
        dchat::RoomSettingKey key = dchat::RoomSettingKey::Name;
        check(dchat::ParseRoomSettingKey("maxmembers", &key) && key == dchat::RoomSettingKey::MaxMembers,
              "解析 maxmembers");
        check(dchat::ParseRoomSettingKey("joinmode", &key) && key == dchat::RoomSettingKey::JoinMode,
              "解析 joinmode");
        check(dchat::ParseRoomSettingKey("listed", &key) && key == dchat::RoomSettingKey::Listed,
              "解析 listed");
        check(dchat::ParseRoomSettingKey("cachemaxmb", &key) && key == dchat::RoomSettingKey::CacheMaxMb,
              "解析 cachemaxmb");
        check(dchat::ParseRoomSettingKey("announcement", &key) &&
                  key == dchat::RoomSettingKey::Announcement,
              "解析 announcement");
        check(dchat::ParseRoomSettingKey("importantnotice", &key) &&
                  key == dchat::RoomSettingKey::ImportantNotice,
              "解析 importantnotice");
        check(dchat::ParseRoomSettingKey("fileencryption", &key) &&
                  key == dchat::RoomSettingKey::FileEncryption,
              "解析 fileencryption");
        check(!dchat::ParseRoomSettingKey("nosuchkey", &key), "不认识的设置名被拒");
    }

    // -----------------------------------------------------------------------
    std::printf("[12] 群设置：逐项修改与拒绝路径\n");
    {
        dchat::Room room = MakeTestGroup("alice", {"bob", "carol"});

        // 群名
        dchat::RoomSettingChange change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name,
                                                                  "新群名");
        check(change.ok && change.changed, "改群名成功");
        checkText(room.name, "新群名", "群名已更新");
        checkText(change.value, "新群名", "变更值回传规范化后的群名");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name, "  带空格的群名  ");
        check(change.ok && room.name == "带空格的群名", "群名首尾空格被修掉");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name, "");
        check(!change.ok, "空群名被拒");
        // 25 个字符，正好越过 24 的上限
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name,
                                         "这个名字实在是太长了已经超过了二十四个字符的上限啦");
        check(!change.ok, "超长群名（25 字符）被拒");
        // 边界：14 个中文 + 10 个 ASCII = 正好 24 个**码点**，但占 52 字节。
        // 这一条同时守住"上限按码点算、不按字节算"——按字节算的话中文用户只能起 8 个字
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name,
                                         "群名长度上限测试一二三四五六ABCDEFGHIJ");
        check(change.ok, "正好 24 个码点的群名通过（长度按码点算，不按字节）");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name,
                                         "群名长度上限测试一二三四五六ABCDEFGHIJK");
        check(!change.ok, "25 个码点的群名被拒");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name, "同样的名字");
        const bool sameNameNoChange =
            dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Name, "同样的名字").changed;
        check(!sameNameNoChange, "群名没变时不标记为已变更（省一次广播）");
        (void)change;

        // 最大人数
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::MaxMembers, "500");
        check(change.ok && room.maxMembers == 500, "改最大人数成功");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::MaxMembers, "1");
        check(!change.ok, "最大人数低于下限被拒");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::MaxMembers, "99999");
        check(!change.ok, "最大人数超过上限被拒");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::MaxMembers, "abc");
        check(!change.ok, "最大人数填字母被拒");
        // 关键：不能把上限调到比现有成员数还小，否则"设置成功"的下一秒群里就有超编成员
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::MaxMembers, "2");
        check(!change.ok, "最大人数不能小于当前成员数（现有 3 人）");
        check(room.maxMembers == 500, "被拒时人数上限保持不变");

        // 加群方式
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::JoinMode, "direct");
        check(change.ok && room.joinMode == dchat::JoinMode::Direct, "加群方式改为直接添加");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::JoinMode, "auto");
        check(change.ok && room.joinMode == dchat::JoinMode::Auto, "加群方式改为自动审核");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::JoinMode, "approval");
        check(change.ok && room.joinMode == dchat::JoinMode::Approval, "加群方式改为管理员审核");
        checkText(change.value, "approval", "加群方式回传协议值");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::JoinMode, "随便");
        check(!change.ok, "未知加群方式被拒");
        checkText(dchat::JoinModeLabel(dchat::JoinMode::Direct), "直接添加", "直接添加的中文标签");
        checkText(dchat::JoinModeLabel(dchat::JoinMode::Approval), "管理员审核",
                  "管理员审核的中文标签");
        checkText(dchat::JoinModeLabel(dchat::JoinMode::Auto), "自动审核", "自动审核的中文标签");

        // 向公众展示
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Listed, "true");
        check(change.ok && room.listed, "勾选向公众展示");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Listed, "false");
        check(change.ok && !room.listed, "取消向公众展示");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Listed, "也许");
        check(!change.ok, "非法布尔值被拒");

        // 总缓存最大大小
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::CacheMaxMb, "512");
        check(change.ok && room.cacheMaxMb == 512, "改总缓存最大大小成功");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::CacheMaxMb, "0");
        check(!change.ok, "总缓存不能为 0");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::CacheMaxMb, "999999999");
        check(!change.ok, "总缓存超出上限被拒");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::CacheMaxMb, "-5");
        check(!change.ok, "负数总缓存被拒");

        // 群公告（可以清空）
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Announcement,
                                         "今晚八点开黑，记得上线");
        check(change.ok && room.announcement == "今晚八点开黑，记得上线", "设置群公告成功");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::Announcement, "");
        check(change.ok && room.announcement.empty(), "群公告可以清空");

        // 重要提醒开关
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::ImportantNotice, "false");
        check(change.ok && !room.importantNotice, "关掉消息重要提醒");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::ImportantNotice, "true");
        check(change.ok && room.importantNotice, "打开消息重要提醒");

        // 群文件加密开关
        check(!room.fileEncryption, "群文件加密默认是关的（升级后行为不变）");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::FileEncryption, "true");
        check(change.ok && change.changed && room.fileEncryption, "打开群文件加密");
        check(change.text.find("不会") != std::string::npos ||
                  change.text.find("已经存在") != std::string::npos,
              "开启时明确说了不回溯加密已存在的明文文件");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::FileEncryption, "true");
        check(change.ok && !change.changed, "重复开同一个值不算变化（不重复广播）");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::FileEncryption, "maybe");
        check(!change.ok, "群文件加密只接受 true / false");
        change = dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::FileEncryption, "false");
        check(change.ok && !room.fileEncryption, "关掉群文件加密");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::FileEncryption),
                  "RSET " + room.id + " fileencryption false", "RSET 广播里加密关是 false");
        dchat::ApplyRoomSetting(&room, dchat::RoomSettingKey::FileEncryption, "true");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::FileEncryption),
                  "RSET " + room.id + " fileencryption true", "RSET 广播里加密开是 true");

        // 私聊没有群设置
        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        change = dchat::ApplyRoomSetting(&dm, dchat::RoomSettingKey::Name, "x");
        check(!change.ok, "私聊没有群设置");
        checkText(change.text, "私聊没有群设置", "私聊改设置给出明确原因");
    }

    // -----------------------------------------------------------------------
    std::printf("[13] 群搜索\n");
    {
        dchat::Room room = dchat::MakeGroupRoom("dchatfans", "Dchat 爱好者", "alice");
        check(!dchat::IsSearchable(room), "默认不展示，搜不到");
        room.listed = true;
        check(dchat::IsSearchable(room), "勾选后可以被搜到");

        check(dchat::RoomMatchesKeyword(room, "dchat"), "按 ID 关键词能搜到");
        check(dchat::RoomMatchesKeyword(room, "DCHAT"), "关键词大小写不敏感");
        check(dchat::RoomMatchesKeyword(room, "爱好者"), "按中文群名能搜到");
        check(dchat::RoomMatchesKeyword(room, "  dchat  "), "关键词首尾空格被忽略");
        check(!dchat::RoomMatchesKeyword(room, "不存在的词"), "搜不到无关关键词");
        check(!dchat::RoomMatchesKeyword(room, ""), "空关键词不匹配任何群");
        check(!dchat::RoomMatchesKeyword(room, "   "), "全空格关键词不匹配任何群");

        dchat::Room hall = dchat::MakeHallRoom();
        hall.listed = true;  // 就算被手工置位也不该进搜索
        check(!dchat::IsSearchable(hall), "大厅永远不进群搜索");

        dchat::Room dm = dchat::MakeDmRoom("dm-000001", "alice", "bob");
        dm.listed = true;
        dm.name = "私聊";
        check(!dchat::IsSearchable(dm), "私聊永远不进群搜索");
    }

    // -----------------------------------------------------------------------
    std::printf("[14] 成员本地设置（免打扰 / 置顶 / 本地缓存）\n");
    {
        // 大厅默认免打扰：全服公共房间，消息最多，默认每条都提示会把人吵死
        const dchat::RoomPrefs hallPrefs = dchat::DefaultRoomPrefs(dchat::RoomKind::Hall);
        check(hallPrefs.muted, "大厅默认开启免打扰");
        check(!hallPrefs.pinned, "默认不置顶");
        check(hallPrefs.alertImportant, "默认重要提醒仍然提醒（即使免打扰）");

        const dchat::RoomPrefs groupPrefs = dchat::DefaultRoomPrefs(dchat::RoomKind::Group);
        check(!groupPrefs.muted, "普通群默认不开启免打扰");
        const dchat::RoomPrefs dmPrefs = dchat::DefaultRoomPrefs(dchat::RoomKind::Direct);
        check(!dmPrefs.muted, "私聊默认不开启免打扰");

        dchat::RoomPrefs prefs;
        prefs.muted = true;
        prefs.pinned = true;
        prefs.alertImportant = false;
        prefs.localCacheMb = 512;
        const std::string line = dchat::SerializePrefs("mygroup", prefs);
        checkText(line, "mygroup\t1\t1\t0\t512", "本地设置序列化格式");

        std::string id;
        dchat::RoomPrefs parsed;
        check(dchat::ParsePrefsLine(line, &id, &parsed), "本地设置能解析回来");
        checkText(id, "mygroup", "解析出的房间 ID 正确");
        check(parsed.muted && parsed.pinned && !parsed.alertImportant && parsed.localCacheMb == 512,
              "本地设置往返一致");

        check(!dchat::ParsePrefsLine("# 注释行", &id, &parsed), "注释行被跳过");
        check(!dchat::ParsePrefsLine("字段不够\t1\t1", &id, &parsed), "字段不足的行被跳过");
        check(!dchat::ParsePrefsLine("非法ID!\t1\t1\t1\t128", &id, &parsed), "非法房间 ID 的行被跳过");

        // 文件被手工改坏（缓存值越界）时夹到范围内，而不是整条设置失效
        check(dchat::ParsePrefsLine("mygroup\t1\t0\t1\t999999999", &id, &parsed), "越界缓存值仍可解析");
        check(parsed.localCacheMb == dchat::kMaxLocalCacheMb, "越界缓存值被夹到上限");
        check(dchat::ParsePrefsLine("mygroup\t1\t0\t1\t0", &id, &parsed), "过小缓存值仍可解析");
        check(parsed.localCacheMb == dchat::kMinLocalCacheMb, "过小缓存值被夹到下限");

        const std::string file = line + "\n" + dchat::SerializePrefs("hall", hallPrefs) + "\n" +
                                 "# 注释\n" + "\n";
        const std::vector<std::pair<std::string, dchat::RoomPrefs>> all =
            dchat::ParsePrefsFile(file);
        check(all.size() == 2, "整个设置文件解析出两条（注释和空行不算）");
        check(all[0].first == "mygroup" && all[1].first == "hall", "解析顺序与文件一致");
        check(all[1].second.muted, "大厅那条带着免打扰");
    }

    // -----------------------------------------------------------------------
    std::printf("[15] dchat-rooms.txt 的读写\n");
    {
        std::vector<dchat::Room> rooms;

        dchat::Room group = dchat::MakeGroupRoom("mygroup", "我的群聊", "alice");
        group.joinMode = dchat::JoinMode::Auto;
        group.listed = true;
        group.maxMembers = 500;
        group.cacheMaxMb = 128;
        group.importantNotice = false;
        group.announcement = "公告里 有空格\t和制表符";  // 故意塞进会破坏行格式的字符
        group.members[0].displayName = "群主大人";
        dchat::RoomMember bob;
        bob.nick = "bob";
        bob.role = dchat::RoomRole::Admin;
        group.members.push_back(bob);
        group.pending.push_back("carol");
        group.fileEncryption = true;  // 让下面的往返测试覆盖到这一项
        rooms.push_back(group);

        rooms.push_back(dchat::MakeDmRoom("dm-000001", "alice", "dave"));

        const dchat::Room hall = dchat::MakeHallRoom();
        rooms.push_back(hall);

        const std::string text = dchat::SerializeRooms(rooms);
        check(text.find("hall\t") == std::string::npos, "大厅不写进文件（由代码建）");
        check(text.find('\n') != std::string::npos, "输出是多行的");

        std::vector<dchat::Room> back = dchat::ParseRooms(text);
        check(back.size() == 2, "解析回两个房间（大厅不在文件里）");

        const dchat::Room* got = nullptr;
        for (const dchat::Room& room : back) {
            if (room.id == "mygroup") got = &room;
        }
        check(got != nullptr, "找回了 mygroup");
        if (got) {
            checkText(got->name, "我的群聊", "群名往返一致");
            checkText(got->announcement, "公告里 有空格\t和制表符",
                      "公告里的空格与制表符没有破坏行格式");
            checkText(got->owner, "alice", "群主往返一致");
            check(got->kind == dchat::RoomKind::Group, "房间种类往返一致");
            check(got->joinMode == dchat::JoinMode::Auto, "加群方式往返一致");
            check(got->listed, "向公众展示往返一致");
            check(got->maxMembers == 500, "最大人数往返一致");
            check(got->cacheMaxMb == 128, "缓存上限往返一致");
            check(!got->importantNotice, "重要提醒开关往返一致");
            check(got->fileEncryption, "群文件加密开关往返一致");
            check(got->members.size() == 2, "成员数往返一致");
            checkText(got->members[0].displayName, "群主大人", "群昵称往返一致");
            check(got->members[0].role == dchat::RoomRole::Owner, "群主角色往返一致");
            check(got->members[1].role == dchat::RoomRole::Admin, "管理员角色往返一致");
            check(got->pending.size() == 1 && got->pending[0] == "carol", "待审队列往返一致");
        }

        // 老配置文件只有 11 个字段（没有 fileencryption 那一格）。
        // 必须按"关"处理 —— 升级不能改变已有群的行为。
        {
            const std::string legacy = "R\toldgroup\tgroup\t\talice\t100\tdirect\t0\t256\t1\t\n";
            const std::vector<dchat::Room> oldRooms = dchat::ParseRooms(legacy);
            check(oldRooms.size() == 1, "11 字段的老配置能解析");
            check(!oldRooms.empty() && !oldRooms[0].fileEncryption,
                  "老配置解析后群文件加密是关的（升级不改变行为）");
        }

        const dchat::Room* dm = nullptr;
        for (const dchat::Room& room : back) {
            if (room.id == "dm-000001") dm = &room;
        }
        check(dm != nullptr, "找回了私聊房间");
        if (dm) {
            check(dm->kind == dchat::RoomKind::Direct, "私聊种类往返一致");
            check(dm->members.size() == 2, "私聊成员数往返一致");
        }

        // 文件被手工改坏：群主指向一个不在成员表里的人
        const std::string broken =
            "R\tbroken\tgroup\t" + dchat::Base64Encode(std::string("坏群")) + "\tghost\t200\tapproval\t0\t256\t1\t\n"
            "M\tbroken\talice\tmember\t\n"
            "M\tbroken\tbob\tmember\t\n";
        const std::vector<dchat::Room> fixed = dchat::ParseRooms(broken);
        check(fixed.size() == 1, "坏房间仍能解析出来");
        if (fixed.size() == 1) {
            // 群主不在成员表里是自相矛盾的状态：必须掰回一致，否则后面每一次
            // LeaveRoom / SetMemberRole 都会走进没人预料到的分支
            checkText(fixed[0].owner, "alice", "群主不在成员表时，最早加入的成员被扶正");
            check(fixed[0].members[0].role == dchat::RoomRole::Owner, "被扶正者的角色同步更新");
        }

        // 文件里出现重复的 owner
        const std::string twoOwners =
            "R\tdup\tgroup\t" + dchat::Base64Encode(std::string("重复群主")) + "\talice\t200\tapproval\t0\t256\t1\t\n"
            "M\tdup\talice\towner\t\n"
            "M\tdup\tbob\towner\t\n";
        const std::vector<dchat::Room> deduped = dchat::ParseRooms(twoOwners);
        check(deduped.size() == 1 && deduped[0].members.size() == 2, "重复群主的房间能解析");
        if (deduped.size() == 1) {
            check(deduped[0].members[0].role == dchat::RoomRole::Owner, "alice 保持群主");
            check(deduped[0].members[1].role == dchat::RoomRole::Admin,
                  "第二个 owner 被降级为管理员（一个群只能有一个群主）");
        }

        // 空文件、全是注释、垃圾行都不能炸
        check(dchat::ParseRooms("").empty(), "空文件解析出 0 个房间");
        check(dchat::ParseRooms("# 只有注释\n").empty(), "只有注释的文件解析出 0 个房间");
        check(dchat::ParseRooms("垃圾\t行\t内容\n").empty(), "无法识别的行被跳过");
        check(dchat::ParseRooms("R\t非法ID!\tgroup\t\t\t\t\t\t\t\t\n").empty(),
              "非法房间 ID 的记录被丢弃");
        check(dchat::SerializeRooms({}).find("R\t") == std::string::npos, "空列表不产出记录行");
    }

    // -----------------------------------------------------------------------
    std::printf("[16] 设置项同步给客户端的那一行\n");
    {
        dchat::Room room = dchat::MakeGroupRoom("mygroup", "我的群", "alice");
        room.maxMembers = 300;
        room.joinMode = dchat::JoinMode::Auto;

        const std::string nameLine = dchat::RoomSettingLine(room, dchat::RoomSettingKey::Name);
        // 群名可能含空格，所以走 Base64；设置名是固定集合，客户端不会猜错
        checkText(nameLine, "RSET mygroup name " + dchat::Base64Encode(std::string("我的群")),
                  "群名用 Base64 传（可能含空格）");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::MaxMembers),
                  "RSET mygroup maxmembers 300", "最大人数用明文传");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::JoinMode),
                  "RSET mygroup joinmode auto", "加群方式用协议值");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::Listed),
                  "RSET mygroup listed false", "未展示时 listed 为 false");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::CacheMaxMb),
                  "RSET mygroup cachemaxmb " + std::to_string(dchat::kDefaultGroupCacheMb),
                  "缓存上限用默认值");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::Announcement),
                  "RSET mygroup announcement " + dchat::Base64Encode(std::string()), "空公告");
        checkText(dchat::RoomSettingLine(room, dchat::RoomSettingKey::ImportantNotice),
                  "RSET mygroup importantnotice true", "重要提醒默认开");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
