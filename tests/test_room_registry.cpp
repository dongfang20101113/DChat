// 房间注册表单元测试：房间生命周期、私聊幂等、会话列表顺序、搜索、每房间独立历史、
// 预算裁剪、以及"大厅沿用原文件名"这条不丢历史的关键约束。
#include <cstdio>
#include <string>
#include <vector>

#include "history_store.h"
#include "room_registry.h"

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

void checkSize(std::size_t actual, std::size_t expected, const char* what) {
    ++g_checks;
    if (actual == expected) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s（期望 %zu，实际 %zu）\n", what, expected, actual);
    }
}

/** 房间 ID 列表，便于比对顺序。 */
std::vector<std::string> IdsOf(const std::vector<const dchat::Room*>& rooms) {
    std::vector<std::string> out;
    for (const dchat::Room* room : rooms) out.push_back(room->id);
    return out;
}

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ",";
        out += item;
    }
    return out;
}

}  // namespace

int main() {
    std::printf("== dchat room registry tests ==\n");

    // -----------------------------------------------------------------------
    std::printf("[1] 启动就有大厅\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        checkSize(registry.Size(), 1, "刚建好只有一个房间");
        check(registry.Hall() != nullptr, "大厅存在");
        checkText(registry.Hall()->id, "hall", "大厅 ID 是 hall");
        check(registry.Hall()->IsHall(), "大厅 kind 正确");
        check(registry.Find("hall") != nullptr, "Find 也能找到大厅");
        check(registry.Find("nosuch") == nullptr, "找不到的房间返回空指针");
        check(!registry.Exists("nosuch"), "Exists 对不存在的房间返回 false");

        registry.Reset();
        checkSize(registry.Size(), 1, "Reset 之后又只剩大厅");
    }

    // -----------------------------------------------------------------------
    std::printf("[2] 建群\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        dchat::Room* group = registry.CreateGroup("mygroup", "我的群", "alice");
        check(group != nullptr, "建群成功");
        checkText(group->id, "mygroup", "建群 ID 正确");
        checkText(group->owner, "alice", "建群者是群主");
        check(group->members.size() == 1, "建群后只有群主一个成员");
        checkSize(registry.Size(), 2, "现在有两个房间");

        check(registry.CreateGroup("mygroup", "重名群", "bob") == nullptr, "重复 ID 建群失败");
        check(registry.CreateGroup("", "空 ID 群", "bob") == nullptr, "空 ID 建群失败");
        check(registry.Find("mygroup") != nullptr, "重复建群没有破坏已有房间");
        checkText(registry.Find("mygroup")->name, "我的群", "已有房间的群名没被改掉");
    }

    // -----------------------------------------------------------------------
    std::printf("[3] 私聊房间：幂等、分配、两人之间只有一个会话\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);

        dchat::Room* first = registry.EnsureDmRoom("alice", "bob");
        check(first != nullptr, "建立私聊成功");
        checkText(first->id, "dm-000001", "第一个私聊号是 dm-000001");
        check(first->members.size() == 2, "私聊有两个成员");

        // 幂等：点两次头像必须还是同一个会话，否则会出现两套历史、两份未读
        dchat::Room* again = registry.EnsureDmRoom("alice", "bob");
        check(again == first, "重复打开返回同一个房间");
        dchat::Room* reversed = registry.EnsureDmRoom("bob", "alice");
        check(reversed == first, "换个方向打开也是同一个房间");
        checkSize(registry.Size(), 2, "没有多出房间（只有大厅和这一个私聊）");

        dchat::Room* second = registry.EnsureDmRoom("alice", "carol");
        checkText(second->id, "dm-000002", "第二个私聊号递增");

        check(registry.EnsureDmRoom("alice", "alice") == nullptr, "自己跟自己私聊无效");
        check(registry.EnsureDmRoom("", "bob") == nullptr, "空昵称无效");

        // 重启后恢复：私聊房间号不能跟已有的撞
        dchat::RoomRegistry restored(500, 4ull * 1024 * 1024);
        restored.LoadFromText(registry.Serialize());
        checkSize(restored.Size(), 3, "恢复后房间数正确");
        check(restored.Find("dm-000001") != nullptr, "恢复出了 dm-000001");
        checkText(restored.NextDmId(), "dm-000003", "重启后分配的私聊号接着往下走");
        dchat::Room* third = restored.EnsureDmRoom("bob", "carol");
        checkText(third->id, "dm-000003", "新私聊号不会和已有房间撞号");
        dchat::Room* existing = restored.EnsureDmRoom("alice", "bob");
        checkText(existing->id, "dm-000001", "重启后打开老会话仍然是老房间");
    }

    // -----------------------------------------------------------------------
    std::printf("[4] 会话列表：只列自己的、顺序稳定\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);

        // 大厅成员是登录时加进去的
        registry.Hall()->members.push_back(dchat::RoomMember{"alice", "", dchat::RoomRole::Member});

        registry.CreateGroup("groupa", "A 群", "alice");
        registry.CreateGroup("groupb", "B 群", "bob");  // alice 不在里面
        registry.EnsureDmRoom("alice", "carol");
        registry.EnsureDmRoom("alice", "dave");
        // 造一个号大但先建的私聊，验证私聊不是按字典序排的
        registry.EnsureDmRoom("alice", "erin");

        const std::vector<const dchat::Room*> rooms = registry.RoomsOf("alice");
        checkText(Join(IdsOf(rooms)), "hall,groupa,dm-000001,dm-000002,dm-000003",
                  "大厅在前、群聊居中、私聊按号递增");

        const std::vector<const dchat::Room*> bobRooms = registry.RoomsOf("bob");
        checkText(Join(IdsOf(bobRooms)), "groupb", "bob 只看到自己所在的群（大厅成员表里没有他）");

        check(registry.RoomsOf("nobody").empty(), "谁都不在的房间列表是空的");

        // 私聊排序：字典序会把 dm-000010 排在 dm-000002 前面，必须按号排
        dchat::RoomRegistry many(500, 4ull * 1024 * 1024);
        for (int i = 0; i < 12; ++i) {
            many.EnsureDmRoom("alice", "user" + std::to_string(i));
        }
        const std::vector<const dchat::Room*> dms = many.RoomsOf("alice");
        checkSize(dms.size(), 12, "十二个私聊都在列表里");
        checkText(dms[0]->id, "dm-000001", "第一个私聊排最前");
        checkText(dms[9]->id, "dm-000010", "第十个私聊在第十位（不是第二位）");
        checkText(dms[11]->id, "dm-000012", "最后一个私聊排最后");
    }

    // -----------------------------------------------------------------------
    std::printf("[5] 群搜索\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);

        dchat::Room* open = registry.CreateGroup("opendchat", "公开的 Dchat 群", "alice");
        open->listed = true;
        open->members.push_back(dchat::RoomMember{"bob", "", dchat::RoomRole::Member});
        open->members.push_back(dchat::RoomMember{"carol", "", dchat::RoomRole::Member});

        dchat::Room* hidden = registry.CreateGroup("hiddendchat", "不公开的 Dchat 群", "alice");
        hidden->listed = false;

        dchat::Room* small = registry.CreateGroup("smallchat", "小小的 Dchat 群", "alice");
        small->listed = true;

        const std::vector<const dchat::Room*> hits = registry.Search("dchat", 10);
        checkSize(hits.size(), 2, "只搜到勾选了向公众展示的群");
        checkText(hits[0]->id, "opendchat", "人多的群排在前面");
        checkText(hits[1]->id, "smallchat", "人少的群排在后面");

        checkSize(registry.Search("dchat", 1).size(), 1, "limit 生效");
        check(registry.Search("", 10).empty(), "空关键词不返回任何群（不做列出全部）");
        check(registry.Search("根本没有这个词", 10).empty(), "搜不到时返回空");
        checkSize(registry.Search("DCHAT", 10).size(), 2, "搜索大小写不敏感");

        // 大厅永远不进搜索
        registry.Hall()->listed = true;
        checkSize(registry.Search("hall", 10).size(), 0, "大厅搜不到");
    }

    // -----------------------------------------------------------------------
    std::printf("[6] 会话显示名\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        dchat::Room* group = registry.CreateGroup("mygroup", "我的群", "alice");

        checkText(dchat::RoomRegistry::DisplayNameFor(*group, "alice"), "我的群", "群聊显示群名");
        checkText(dchat::RoomRegistry::DisplayNameFor(*registry.Hall(), "alice"), "大厅",
                  "大厅显示大厅");

        dchat::Room* dm = registry.EnsureDmRoom("alice", "bob");
        // 私聊标题必须是**对方**：显示成自己的名字用户会以为点错了
        checkText(dchat::RoomRegistry::DisplayNameFor(*dm, "alice"), "bob", "alice 看到的私聊标题是 bob");
        checkText(dchat::RoomRegistry::DisplayNameFor(*dm, "bob"), "alice", "bob 看到的私聊标题是 alice");
        // 旁观者（不该出现，但真出现了也不能显示错）
        checkText(dchat::RoomRegistry::DisplayNameFor(*dm, "carol"), "alice",
                  "非成员看到的是第一个成员，绝不显示自己的名字");
    }

    // -----------------------------------------------------------------------
    std::printf("[7] 每个房间各自独立的历史\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        registry.CreateGroup("mygroup", "我的群", "alice");
        dchat::Room* dm = registry.EnsureDmRoom("alice", "bob");

        const unsigned long long hallSeq = registry.AppendHistory("hall", "SAY 12:00 alice 大厅消息");
        const unsigned long long groupSeq =
            registry.AppendHistory("mygroup", "RMSG mygroup 12:01 bob 群消息");
        const unsigned long long dmSeq = registry.AppendHistory(dm->id, "RMSG dm-000001 12:02 bob 悄悄话");

        check(hallSeq == 1 && groupSeq == 1 && dmSeq == 1, "每个房间的序号各自从 1 开始");

        registry.AppendHistory("hall", "SAY 12:03 alice 大厅第二条");
        check(registry.AppendHistory("mygroup", "RMSG mygroup 12:04 bob 群第二条") == 2,
              "群里的序号独立递增，不受大厅影响");

        checkSize(registry.HistorySince("hall", 0, 100).size(), 2, "大厅有两条历史");
        checkSize(registry.HistorySince("mygroup", 0, 100).size(), 2, "群里有两条历史");
        checkSize(registry.HistorySince(dm->id, 0, 100).size(), 1, "私聊有一条历史");

        checkText(registry.HistorySince("mygroup", 1, 100)[0].line,
                  "RMSG mygroup 12:04 bob 群第二条", "群历史的内容正确");
        check(registry.HistorySince("nosuch", 0, 100).empty(), "不存在的房间历史为空");
        check(registry.AppendHistory("nosuch", "x") == 0, "给不存在的房间追加历史返回 0");
        check(registry.HistoryOf("nosuch") == nullptr, "不存在的房间没有历史存储");
        check(registry.HistoryOf("mygroup") != nullptr, "存在的房间有历史存储");
    }

    // -----------------------------------------------------------------------
    std::printf("[8] 房间缓存预算：历史和文件共享\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        dchat::Room* group = registry.CreateGroup("mygroup", "我的群", "alice");
        group->cacheMaxMb = 1;  // 1 MB 预算

        // 管理员改了 cachemaxmb 之后，必须把预算**应用**下去才生效。
        // 服务端在 GSET 成功后会做这一步；这里显式调用，正是要钉住这个契约：
        // 只改 Room 上的字段、不调 SetHistoryBudget，历史就会继续按旧预算涨。
        registry.SetHistoryBudget("mygroup", dchat::RoomRegistry::BudgetBytes(*group), 0);

        // 塞进远超 1 MB 的历史
        const std::string fatLine = "RMSG mygroup 12:00 bob " + std::string(2000, 'x');
        for (int i = 0; i < 2000; ++i) registry.AppendHistory("mygroup", fatLine);
        dchat::HistoryStore* history = registry.HistoryOf("mygroup");
        check(history->Bytes() <= 1ull * 1024 * 1024, "历史不会超过房间自己的 1 MB 预算");

        // 群文件把预算吃满了：历史要跟着让位，而不是"文件归文件、历史归历史"各占一份
        registry.SetHistoryBudget("mygroup", dchat::RoomRegistry::BudgetBytes(*group),
                                  1ull * 1024 * 1024);
        checkSize(history->Size(), 0, "文件吃满预算后历史被裁空");
        check(history->LastSeq() > 0, "序号不回退（离线补发依赖它单调递增）");

        // 预算调大：新消息照常进得来
        registry.SetHistoryBudget("mygroup", 4ull * 1024 * 1024, 0);
        registry.AppendHistory("mygroup", "RMSG mygroup 13:00 bob 预算调大之后");
        checkSize(history->Size(), 1, "预算调大后又能存下消息了");

        check(dchat::RoomRegistry::BudgetBytes(*group) == 1ull * 1024 * 1024,
              "预算按 MB 换算成字节");
        registry.SetHistoryBudget("nosuch", 1024, 0);  // 不该崩
        check(true, "给不存在的房间设预算不会崩");
    }

    // -----------------------------------------------------------------------
    std::printf("[9] 历史存储的 SetBudget\n");
    {
        dchat::HistoryStore store(1000, 1024 * 1024);
        for (int i = 0; i < 100; ++i) store.Append("消息 " + std::to_string(i));
        const std::size_t before = store.Size();
        check(before > 0, "先存进去一些消息");

        store.SetBudget(1000, 0);  // 预算压到 0
        checkSize(store.Size(), 0, "预算压到 0 时全部裁掉");
        check(store.LastSeq() == 100, "裁剪不影响序号单调性");

        store.SetBudget(3, 1024 * 1024);  // 只允许 3 条
        for (int i = 0; i < 10; ++i) store.Append("新消息 " + std::to_string(i));
        checkSize(store.Size(), 3, "条数上限同样生效");
    }

    // -----------------------------------------------------------------------
    std::printf("[10] 删除房间\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        registry.CreateGroup("mygroup", "我的群", "alice");
        dchat::Room* dm = registry.EnsureDmRoom("alice", "bob");
        const std::string dmId = dm->id;
        registry.AppendHistory(dmId, "RMSG " + dmId + " 12:00 bob 悄悄话");

        check(!registry.Remove("hall", nullptr), "大厅删不掉");
        check(registry.Hall() != nullptr, "试图删除后大厅仍在");

        std::string historyPath;
        check(registry.Remove("mygroup", &historyPath), "群可以删掉");
        checkText(historyPath, "dchat-history-mygroup.txt", "回传该房间的历史文件路径");
        check(registry.Find("mygroup") == nullptr, "删掉后找不到了");
        check(registry.HistoryOf("mygroup") == nullptr, "历史存储一起没了");
        check(!registry.Remove("mygroup", nullptr), "重复删除返回 false");

        check(registry.Remove(dmId, &historyPath), "私聊房间也可以删（清空会话时用）");
        check(!registry.Remove("nosuch", nullptr), "删不存在的房间返回 false");
    }

    // -----------------------------------------------------------------------
    std::printf("[11] 存储路径：大厅必须沿用原文件名（不能丢历史）\n");
    {
        // 这条是升级安全的关键：线上大厅已经有聊天记录和一堆已上传的文件，
        // 如果升级后大厅改用一个新路径，用户看到的就是"聊天记录全没了"
        checkText(dchat::RoomRegistry::HistoryPathFor("hall"), "dchat-history.txt",
                  "大厅历史仍是 dchat-history.txt");
        checkText(dchat::RoomRegistry::HistoryPathFor("mygroup"), "dchat-history-mygroup.txt",
                  "群聊历史按 ID 分文件");
        checkText(dchat::RoomRegistry::HistoryPathFor("dm-000001"), "dchat-history-dm-000001.txt",
                  "私聊历史按 ID 分文件");

        checkText(dchat::RoomRegistry::FilesDirFor("hall"), "dchat-files",
                  "大厅文件目录仍是 dchat-files");
        checkText(dchat::RoomRegistry::FilesDirFor("mygroup"), "dchat-files/mygroup",
                  "群文件按 ID 分目录");
    }

    // -----------------------------------------------------------------------
    std::printf("[12] dchat-rooms.txt 往返\n");
    {
        dchat::RoomRegistry registry(500, 4ull * 1024 * 1024);
        registry.CreateGroup("groupone", "第一个群", "alice");
        registry.CreateGroup("grouptwo", "第二个群", "bob");
        registry.EnsureDmRoom("alice", "bob");

        dchat::Room* one = registry.Find("groupone");
        one->listed = true;
        one->joinMode = dchat::JoinMode::Auto;
        one->maxMembers = 321;
        one->announcement = "公告 带空格";
        one->members[0].displayName = "群主大人";
        registry.AppendHistory("groupone", "RMSG groupone 12:00 alice 你好");

        const std::string text = registry.Serialize();
        check(text.find("hall") == std::string::npos, "大厅不写进文件");

        dchat::RoomRegistry restored(500, 4ull * 1024 * 1024);
        checkSize(restored.LoadFromText(text), 3, "恢复出三个房间（两个群 + 一个私聊）");
        checkSize(restored.Size(), 4, "加上大厅一共四个");

        dchat::Room* back = restored.Find("groupone");
        check(back != nullptr, "群恢复出来了");
        if (back) {
            checkText(back->name, "第一个群", "群名恢复");
            checkText(back->announcement, "公告 带空格", "群公告恢复");
            checkText(back->members[0].displayName, "群主大人", "群昵称恢复");
            check(back->listed, "向公众展示恢复");
            check(back->joinMode == dchat::JoinMode::Auto, "加群方式恢复");
            check(back->maxMembers == 321, "最大人数恢复");
            check(dchat::IsRoomMember(*back, "alice"), "群主仍是成员");
        }

        // 历史不随房间元数据一起恢复 —— 它有自己的文件。这里验证"确实没有"
        // 而不是"意外带过来了"，免得以后有人以为房间文件里存了历史
        checkSize(restored.HistorySince("groupone", 0, 100).size(), 0,
                  "房间元数据文件里不含历史（历史在自己的文件里）");

        // 重复加载不能产生重复房间
        checkSize(restored.LoadFromText(text), 0, "重复加载同一份内容不会再建房间");
        checkSize(restored.Size(), 4, "房间数没有翻倍");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
