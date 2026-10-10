// 服务端房间注册表的实现。设计理由见 room_registry.h 顶部注释。
#include "room_registry.h"

#include <algorithm>

namespace dchat {
namespace {

/** 一兆字节。房间的缓存预算按 MB 配置，内部按字节算。 */
constexpr unsigned long long kBytesPerMb = 1024ull * 1024ull;

/** 私聊房间的排序用：号小的在前。 */
bool DmIdLess(const std::string& left, const std::string& right) {
    unsigned long long a = 0;
    unsigned long long b = 0;
    const bool okA = ParseDmSerial(left, &a);
    const bool okB = ParseDmSerial(right, &b);
    if (okA && okB) return a < b;
    if (okA != okB) return okA;  // 能解析的排前面，纯防御
    return left < right;
}

}  // namespace

RoomRegistry::RoomRegistry(std::size_t historyLines, unsigned long long historyBytes)
    : historyLines_(historyLines == 0 ? 1 : historyLines), historyBytes_(historyBytes) {
    Reset();
}

void RoomRegistry::Reset() {
    entries_.clear();
    Room hall = MakeHallRoom();
    entries_.emplace(hall.id, std::make_unique<RoomEntry>(hall, historyLines_,
                                                          std::min(historyBytes_, BudgetBytes(hall))));
}

RoomEntry* RoomRegistry::Entry(const std::string& id) {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.get();
}

const RoomEntry* RoomRegistry::Entry(const std::string& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.get();
}

Room* RoomRegistry::Find(const std::string& id) {
    RoomEntry* entry = Entry(id);
    return entry ? &entry->room : nullptr;
}

const Room* RoomRegistry::Find(const std::string& id) const {
    const RoomEntry* entry = Entry(id);
    return entry ? &entry->room : nullptr;
}

bool RoomRegistry::Exists(const std::string& id) const { return Entry(id) != nullptr; }

Room* RoomRegistry::Hall() {
    RoomEntry* entry = Entry(kHallId);
    return entry ? &entry->room : nullptr;
}

const Room* RoomRegistry::Hall() const {
    const RoomEntry* entry = Entry(kHallId);
    return entry ? &entry->room : nullptr;
}

std::vector<Room> RoomRegistry::Snapshot() const {
    std::vector<Room> out;
    out.reserve(entries_.size());
    for (const auto& pair : entries_) out.push_back(pair.second->room);
    return out;
}

std::size_t RoomRegistry::LoadFromText(const std::string& text) {
    std::size_t loaded = 0;
    for (Room& room : ParseRooms(text)) {
        if (room.id.empty() || Exists(room.id)) continue;  // 重复的以先出现的为准
        const unsigned long long budget =
            std::min(historyBytes_, room.cacheMaxMb * kBytesPerMb);
        entries_.emplace(room.id, std::make_unique<RoomEntry>(room, historyLines_, budget));
        ++loaded;
    }
    return loaded;
}

std::string RoomRegistry::Serialize() const {
    return SerializeRooms(Snapshot());  // SerializeRooms 自己会跳过大厅
}

std::vector<const Room*> RoomRegistry::RoomsOf(const std::string& nick) const {
    std::vector<const Room*> halls;
    std::vector<const Room*> groups;
    std::vector<const Room*> dms;
    for (const auto& pair : entries_) {
        const Room& room = pair.second->room;
        if (!IsRoomMember(room, nick)) continue;
        switch (room.kind) {
            case RoomKind::Hall:
                halls.push_back(&room);
                break;
            case RoomKind::Group:
                groups.push_back(&room);
                break;
            case RoomKind::Direct:
                dms.push_back(&room);
                break;
        }
    }
    // 私聊按房间号排，不能按字典序：dm-000010 在字典序里排在 dm-000002 前面
    std::sort(dms.begin(), dms.end(), [](const Room* a, const Room* b) {
        return DmIdLess(a->id, b->id);
    });

    std::vector<const Room*> out;
    out.reserve(halls.size() + groups.size() + dms.size());
    out.insert(out.end(), halls.begin(), halls.end());
    out.insert(out.end(), groups.begin(), groups.end());
    out.insert(out.end(), dms.begin(), dms.end());
    return out;
}

std::vector<const Room*> RoomRegistry::Search(const std::string& keyword, std::size_t limit) const {
    std::vector<const Room*> hits;
    if (limit == 0) return hits;
    for (const auto& pair : entries_) {
        const Room& room = pair.second->room;
        if (!IsSearchable(room)) continue;
        if (!RoomMatchesKeyword(room, keyword)) continue;
        hits.push_back(&room);
    }
    // 人多的群排前面：搜索结果有限时，先看到活跃的群比先看到刚建的空群有用
    std::sort(hits.begin(), hits.end(), [](const Room* a, const Room* b) {
        if (a->members.size() != b->members.size()) return a->members.size() > b->members.size();
        return a->id < b->id;  // 人数相同按 ID，保证顺序稳定
    });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

std::string RoomRegistry::DisplayNameFor(const Room& room, const std::string& viewer) {
    if (room.kind != RoomKind::Direct) return room.name;
    for (const RoomMember& member : room.members) {
        if (member.nick != viewer) return member.nick;  // 私聊标题永远是**对方**
    }
    // 只剩自己（或根本不是成员）：退回群名，绝不显示 viewer 自己的名字 ——
    // 私聊列表里出现自己的昵称，用户会以为点错了
    return room.name;
}

Room* RoomRegistry::CreateGroup(const std::string& id, const std::string& name,
                                const std::string& owner) {
    if (id.empty() || Exists(id)) return nullptr;
    Room room = MakeGroupRoom(id, name, owner);
    const unsigned long long budget = std::min(historyBytes_, BudgetBytes(room));
    auto entry = std::make_unique<RoomEntry>(room, historyLines_, budget);
    Room* result = &entry->room;
    entries_.emplace(id, std::move(entry));
    return result;
}

Room* RoomRegistry::EnsureDmRoom(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a == b) return nullptr;

    // 幂等查找：两个人之间永远只有一个私聊房间，所以"点了两次头像"不会产生
    // 两个会话、两套历史、两份未读
    for (const auto& pair : entries_) {
        const Room& room = pair.second->room;
        if (room.kind != RoomKind::Direct || room.members.size() != 2) continue;
        const bool match = (room.members[0].nick == a && room.members[1].nick == b) ||
                           (room.members[0].nick == b && room.members[1].nick == a);
        if (match) return &pair.second->room;
    }

    const std::string id = NextDmId();
    Room room = MakeDmRoom(id, a, b);
    // 私聊没有群主、没有管理员可设项，但缓存预算照样生效（占的是服务器磁盘）
    auto entry = std::make_unique<RoomEntry>(room, historyLines_, BudgetBytes(room));
    Room* result = &entry->room;
    entries_.emplace(id, std::move(entry));
    return result;
}

bool RoomRegistry::Remove(const std::string& id, std::string* historyPath) {
    if (id == kHallId) return false;  // 大厅删不掉
    const auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (historyPath) *historyPath = HistoryPathFor(id);
    entries_.erase(it);
    return true;
}

std::string RoomRegistry::NextDmId() const {
    unsigned long long maxSerial = 0;
    for (const auto& pair : entries_) {
        unsigned long long serial = 0;
        if (ParseDmSerial(pair.first, &serial) && serial > maxSerial) maxSerial = serial;
    }
    // 从 1 开始，不从 0：dm-000000 看着像是"没有房间"的哨兵值
    return FormatDmId(maxSerial + 1);
}

unsigned long long RoomRegistry::AppendHistory(const std::string& roomId, const std::string& line) {
    RoomEntry* entry = Entry(roomId);
    if (!entry) return 0;
    return entry->history.Append(line);
}

HistoryStore* RoomRegistry::HistoryOf(const std::string& roomId) {
    RoomEntry* entry = Entry(roomId);
    return entry ? &entry->history : nullptr;
}

std::vector<HistoryEntry> RoomRegistry::HistorySince(const std::string& roomId,
                                                     unsigned long long afterSeq,
                                                     std::size_t limit) const {
    const RoomEntry* entry = Entry(roomId);
    if (!entry) return {};
    return entry->history.Since(afterSeq, limit);
}

std::vector<HistoryEntry> RoomRegistry::RecentHistory(const std::string& roomId,
                                                      std::size_t limit) const {
    const RoomEntry* entry = Entry(roomId);
    if (!entry) return {};
    return entry->history.Recent(limit);
}

void RoomRegistry::SetHistoryBudget(const std::string& roomId, unsigned long long totalBytes,
                                    unsigned long long filesUsedBytes) {
    RoomEntry* entry = Entry(roomId);
    if (!entry) return;
    // 历史和文件共享同一个预算：文件先占，剩下的才是历史的。
    // 剩 0 就是"文件已经把预算吃满了"，此时历史被裁空是**正确**行为 ——
    // 宁可丢历史也不能让磁盘无限涨，那正是管理员设这个上限的目的。
    const unsigned long long remaining = totalBytes > filesUsedBytes ? totalBytes - filesUsedBytes : 0;
    entry->history.SetBudget(historyLines_, remaining);
}

unsigned long long RoomRegistry::BudgetBytes(const Room& room) {
    return room.cacheMaxMb * kBytesPerMb;
}

std::string RoomRegistry::HistoryPathFor(const std::string& roomId) {
    // 大厅沿用原文件名：升级到多会话不能让线上已有的聊天记录凭空消失
    if (roomId == kHallId) return "dchat-history.txt";
    return "dchat-history-" + roomId + ".txt";
}

std::string RoomRegistry::FilesDirFor(const std::string& roomId) {
    // 同上：大厅沿用原来的 dchat-files 目录，已经传上去的文件还在
    if (roomId == kHallId) return "dchat-files";
    return "dchat-files/" + roomId;
}

}  // namespace dchat
