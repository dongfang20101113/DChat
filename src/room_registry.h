// 服务端的房间注册表：内存里所有房间 + 每个房间各自的历史记录。
//
// 为什么单独一层：server.cpp 已经 2500 行了，把"房间表 + 每房间历史 + 私聊房间分配
// + 搜索 + 会话列表"全塞进去，它会变成一个没人敢改的文件。这一层只做状态管理，
// **不碰网络、不碰文件系统**（文件读写照旧由 server.cpp 按 dchat 一贯的做法做），
// 因此可以直接单测。
//
// ⚠️ 线程安全：本类**不自带锁**。调用方（server.cpp）用一把 g_roomsMutex 罩住，
//    和 g_files / g_history / g_clients 的做法一致。理由是这样才能在同一把锁里
//    完成"查房间 -> 改成员 -> 取广播目标"这一串操作；如果每层各锁各的，
//    迟早会出现"广播到一半房间没了"这种只在并发下复现的问题。
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "history_store.h"
#include "room.h"

namespace dchat {

/** 一个房间 + 它自己的历史记录。 */
struct RoomEntry {
    RoomEntry(const Room& value, std::size_t historyLines, unsigned long long historyBytes)
        : room(value), history(historyLines, historyBytes) {}

    Room room;
    HistoryStore history;  // 每个房间独立：大厅的历史和某个群的完全不相干
};

class RoomRegistry {
public:
    /**
     * @param historyLines 单个房间在内存里最多保留多少条历史
     * @param historyBytes 单个房间的字节上限（会被房间自己的 cachemaxmb 覆盖，见 SetHistoryBudget）
     */
    RoomRegistry(std::size_t historyLines, unsigned long long historyBytes);

    // ---- 生命周期 ----

    /** 清空全部房间并重新建立大厅。启动时调用。 */
    void Reset();

    /**
     * 从 dchat-rooms.txt 的内容恢复群聊与私聊。
     * 大厅不在这里（Reset 已经建好了）。**同名房间以先出现的为准**，重复的跳过。
     * @return 恢复出来的房间数
     */
    std::size_t LoadFromText(const std::string& text);

    /** 导出 dchat-rooms.txt 的内容（不含大厅）。 */
    std::string Serialize() const;

    // ---- 查询 ----

    Room* Find(const std::string& id);
    const Room* Find(const std::string& id) const;
    bool Exists(const std::string& id) const;

    /** 大厅。永远存在，Reset 之后就有。 */
    Room* Hall();
    const Room* Hall() const;

    std::size_t Size() const { return entries_.size(); }

    /** 全部房间的只读快照（按 ID 排序）。 */
    std::vector<Room> Snapshot() const;

    /**
     * 某个账号所在的全部房间，用于会话列表。
     * 顺序固定：大厅在前，然后群聊、私聊，各组内按 ID 排序 ——
     * 顺序固定客户端才能稳定地做"置顶 + 未读"排序，否则每次登录列表都在跳。
     */
    std::vector<const Room*> RoomsOf(const std::string& nick) const;

    /** 搜索向公众展示的群。空关键词返回空（不做"列出全部"）。 */
    std::vector<const Room*> Search(const std::string& keyword, std::size_t limit) const;

    /**
     * 房间在**某人眼里**的显示名：群聊用群名，私聊用对方的昵称。
     * 私聊房间本身没有名字，让每个客户端各算一遍迟早会有一端算错
     * （比如忘了排除自己，于是私聊标题显示成自己的名字）。
     */
    static std::string DisplayNameFor(const Room& room, const std::string& viewer);

    // ---- 创建与销毁 ----

    /**
     * 建群。id 必须已经过 NormalizeGroupId；已存在或非法返回 nullptr。
     * 调用方负责把建群者加为群主（MakeGroupRoom 已经做了）。
     */
    Room* CreateGroup(const std::string& id, const std::string& name, const std::string& owner);

    /**
     * 打开两人之间的私聊，不存在就建一个。**幂等**：反复调用返回同一个房间，
     * 所以"点两次头像"不会产生两个私聊会话、两套历史。
     * 自己跟自己会返回 nullptr（没有意义，而且会让成员表只有一个成员）。
     */
    Room* EnsureDmRoom(const std::string& a, const std::string& b);

    /**
     * 删掉一个房间（群主解散群聊时用）。大厅删不掉。
     * @param historyPath 若非空，写入该房间历史文件的路径，供调用方删除它
     * @return 是否真的删掉了
     */
    bool Remove(const std::string& id, std::string* historyPath = nullptr);

    /** 下一个私聊房间号（`dm-000001` 这种）。已有的最大号 +1。 */
    std::string NextDmId() const;

    // ---- 历史 ----

    unsigned long long AppendHistory(const std::string& roomId, const std::string& line);
    HistoryStore* HistoryOf(const std::string& roomId);
    std::vector<HistoryEntry> HistorySince(const std::string& roomId, unsigned long long afterSeq,
                                           std::size_t limit) const;

    /** 某个房间最近 limit 条历史（旧 -> 新）。客户端切进会话时用它回放。 */
    std::vector<HistoryEntry> RecentHistory(const std::string& roomId, std::size_t limit) const;

    /**
     * 房间的总缓存预算变了（管理员改 cachemaxmb）时重新给历史分配字节数。
     *
     * 预算由**历史消息和文件共享**：filesUsedBytes 是当前该房间的文件占用，
     * 历史只能用剩下的部分。文件大、价值低，所以是先保住历史、文件先被淘汰，
     * 这个顺序在 server.cpp 的淘汰逻辑里体现。
     */
    void SetHistoryBudget(const std::string& roomId, unsigned long long totalBytes,
                          unsigned long long filesUsedBytes);

    /** 房间的字节预算上限（MB -> 字节）。 */
    static unsigned long long BudgetBytes(const Room& room);

    /**
     * 每个房间历史文件的路径。
     *
     * 大厅沿用原来的 `dchat-history.txt` —— 这一点是刻意的：升级到多会话**不能**
     * 让线上已有的聊天记录凭空消失，而大厅那些记录正是最老、最舍不得丢的一批。
     */
    static std::string HistoryPathFor(const std::string& roomId);

    /** 每个房间文件暂存目录的路径。大厅同样沿用原路径 `dchat-files`。 */
    static std::string FilesDirFor(const std::string& roomId);

private:
    RoomEntry* Entry(const std::string& id);
    const RoomEntry* Entry(const std::string& id) const;

    std::size_t historyLines_;
    unsigned long long historyBytes_;
    std::map<std::string, std::unique_ptr<RoomEntry>> entries_;
};

}  // namespace dchat
