// 客户端的**会话核心**：连接、握手、登录注册、收发消息、文件与语音、指令派发。
//
// 这一层刻意不依赖任何界面库，目的是：
//   1. 能被交叉编译验证（Linux 与 macOS 都能编，本机就能查错）；
//   2. 逻辑集中在一处，多出来的界面层只剩"把字符串画出来"；
//   3. 界面换了逻辑不用重写 —— 终端版和 Cocoa 版共用同一个 ChatCore。
//
// 界面通过 ChatCoreDelegate 收到通知。**所有回调都在接收线程上触发**，
// 界面必须自己切回主线程（Cocoa 用 dispatch_async，终端版直接打印）。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "files.h"
#include "net.h"
#include "server_command.h"
#include "trust.h"
#include "voice.h"

namespace dchat {

/** 一条要显示的消息。界面自己决定怎么渲染（终端版刷屏、Cocoa 版加进 NSTextView）。 */
struct ChatMessage {
    enum class Kind {
        Say,      // 别人的发言
        Own,      // 自己的发言
        System,   // 服务器通知
        Error,    // 出错
        Notice,   // 加入/离开之类
        FileOffer,// 附件卡片
    };
    Kind kind = Kind::System;
    std::string nick;             // 发言者（Say/Own 才有）
    std::string text;             // 正文
    std::string time;             // hh:mm
    bool mentionMe = false;       // 有人 @ 我
    bool hasColor = false;        // 正文里有没有色码（界面据此决定要不要解析）
    std::string fileId;           // 附件 ID（Kind::FileOffer 才有）
    bool isVoice = false;         // 附件是不是语音
    bool valid = false;           // 解析成功（无效的不要显示）
};

/** 一个多会话房间（大厅 / 群聊 / 私聊）。字段来自 ROOMLIST / RINFO / RSET。 */
struct RoomInfo {
    std::string id;
    std::string kind;      // hall / group / dm
    std::string name;      // 显示名（RSET name 更新；空 = 还没拿到）
    std::string owner;     // 群主昵称
    int members = 0;
    int maxMembers = 0;
    std::string myRole;    // owner / admin / member
    std::string joinMode;  // open / approval / auto
    bool listed = false;   // 是否向公众展示（可被搜到）
};

/** 一个群成员（RMEMBERS）。 */
struct MemberInfo {
    std::string nick;
    std::string role;     // owner / admin / member
    std::string display;  // 群昵称，空 = 用账号名
};

/** `/find` 搜到的一个群（GHIT）。 */
struct GroupHit {
    std::string id;
    std::string name;
    int members = 0;
    int maxMembers = 0;
    std::string joinMode;
};

/** 界面要实现的回调。全都在接收线程上触发。 */
class ChatCoreDelegate {
public:
    virtual ~ChatCoreDelegate() = default;
    virtual void OnChatMessage(const ChatMessage& message) = 0;
    virtual void OnConnected(bool encrypted, const std::string& fingerprint,
                             const TrustDecision& trust) = 0;
    virtual void OnLoggedIn(const std::string& nick) = 0;
    virtual void OnDisconnected(const std::string& reason) = 0;
    virtual void OnOnlineNicks(const std::vector<std::string>& nicks) = 0;
    virtual void OnTransferProgress(const std::string& text) = 0;
    virtual void OnColorSettingChanged(bool enabled, const std::string& rawRest) = 0;
    /**
     * 用户敲了 /voice。
     *
     * 录音本身留给界面做（要起子进程、要管"再敲一次停"的状态），核心只把这件事
     * 转交出去 —— 这样终端版和 Cocoa 版共用同一套指令解析，界面各自实现录音。
     * @param arg /voice 后面的参数（秒数，可为空）
     */
    virtual void OnVoiceCommand(const std::string& arg) = 0;
};

/** 连接参数。 */
struct ConnectOptions {
    std::string host;
    int port = 5555;
    std::string user;
    std::string password;
    bool wantRegister = false;
};

class ChatCore {
public:
    explicit ChatCore(ChatCoreDelegate* delegate);
    ~ChatCore();

    ChatCore(const ChatCore&) = delete;
    ChatCore& operator=(const ChatCore&) = delete;

    /** 连上并登录（阻塞到握手完成；之后的收发在接收线程里）。 */
    bool Connect(const ConnectOptions& options, std::string* error);

    /**
     * 只连接、先不登录（交互模式用：可能连上了才问用户名密码）。
     *
     * 和 Connect() 的区别只有一个：不发登录请求。TOFU 检查、指纹文件更新、
     * 接收线程的挂载都一样 —— 这段顺序不能改（**必须先比对指纹再发密码**）。
     */
    bool ConnectOnly(const ConnectOptions& options, std::string* error);

    /** 记下账号，稍后由 SubmitCredentials() 发出去（交互模式：连接与登录分两步）。 */
    void SetCredentials(const std::string& user, const std::string& password, bool wantRegister);

    /** 把 SetCredentials 记下的账号发出去。返回 false 表示连接已断。 */
    bool SubmitCredentials();

    void Disconnect();
    bool Connected() const { return connection_.Running(); }
    bool Welcomed() const { return welcomed_; }
    /** 登录完成没有。界面据此在发文件/语音前先拦一下，给更直白的提示。 */
    bool LoggedIn() const { return !selfNick_.empty(); }

    /** 发一条聊天消息（会自动走多行转义）。 */
    bool SendMessage(const std::string& text);

    /** 输入框里回车：本地指令就地处理，其余发给服务器。 */
    void SubmitInput(const std::string& text);

    /** Tab 补全（复用协议层的补全器，和另外两端同一套候选逻辑）。 */
    CompletionResult Complete(const std::string& text);

    // ---- 多会话（群聊 / 私聊）----
    //
    // 和桌面端、安卓端走**同一套协议**（docs/rooms-protocol.md）。区别只在形态：
    // 那两端是窗口和列表，这里是 `/rooms` 打一张表 + `/open` 切换。
    /** 我的会话列表（大厅 + 群 + 私聊），顺序同服务端 ROOMLIST 给的顺序。 */
    const std::vector<RoomInfo>& Rooms() const { return rooms_; }
    /** 当前会话 id；空 = 还没拿到房间列表（或在跟老服务端说话）。 */
    const std::string& CurrentRoom() const { return currentRoom_; }
    /** 当前会话的成员表，切房间时由 RMEMBERS 重新填。 */
    const std::vector<MemberInfo>& CurrentMembers() const { return members_; }
    /** 最后一次 `/find` 的结果。 */
    const std::vector<GroupHit>& LastGroupHits() const { return groupHits_; }
    /** 服务端有没有确认 rooms1 能力。 */
    bool RoomsCapable() const { return roomsCapable_; }
    /** 待审的入群申请（当前会话的）。`/audit` 会重新拉一次。 */
    const std::vector<std::string>& PendingRequests() const { return pending_; }
    /** 我的黑名单。 */
    const std::vector<std::string>& BlockList() const { return blocks_; }
    /**
     * 切到某个会话：发 `ROOM <id>`，**之后的发言都进这个会话**。
     *
     * 协议就是这么设计的：服务端记住"这个人当前在哪个房间"，`MSG` 按那个路由。
     * 所以这里必须真的发出去，不能只在本地改个变量 —— 否则话还是说进大厅。
     */
    bool SwitchRoom(const std::string& id);

    /**
     * 喂一行"收到的服务端消息"（就是接收线程拿到的那一行）。
     *
     * 公开出来是为了**可测**：多会话那套解析（ROOMLIST / RINFO / RSET / RMEMBERS）
     * 全是纯逻辑，不该非得连上一台真服务器才能验。批处理模式也走这里。
     * 注意：发请求的那几个方法（SwitchRoom / /create …）在没连接时会失败，
     * 那是预期行为，测试只验"解析出来的状态对不对"。
     */
    void FeedLine(const std::string& line) { HandleLine(line); }

    const std::string& SelfNick() const { return selfNick_; }
    const std::vector<std::string>& OnlineNicks() const { return onlineNicks_; }
    bool ColorEnabled() const { return colorEnabled_; }
    void SetColorEnabled(bool enabled);
    FileTransfers& Files() { return *files_; }
    const std::string& Host() const { return host_; }
    int Port() const { return port_; }

    /** 换个目录存 TOFU 指纹备忘（终端版用当前目录，Cocoa 版用 ~/Library/...）。 */
    void SetKnownServersPath(const std::string& path) { knownServersPath_ = path; }

private:
    void HandleLine(const std::string& line);
    void HandleSay(const std::string& line);
    /** 多会话那几条命令（ROOMLIST / RINFO / RSET / RMSG / GHIT …）。认领了返回 true。 */
    bool HandleRoomLine(const std::string& command, const std::vector<std::string>& words,
                        const std::string& time);
    RoomInfo* FindRoom(const std::string& id);
    const RoomInfo* FindRoom(const std::string& id) const;
    /** 用服务端给的信息更新一条房间；没有就新增。 */
    void UpsertRoom(const RoomInfo& incoming);
    /** `/rooms` 的实际输出。 */
    void ShowRooms();
    /** 发一条多会话协议命令（GCREATE / GSET …）。 */
    bool SendRoom(const std::string& command, const std::string& rest);
    void Push(ChatMessage message);
    void Notice(const std::string& text, ChatMessage::Kind kind = ChatMessage::Kind::Notice);

    ChatCoreDelegate* delegate_ = nullptr;
    ClientConnection connection_;
    std::unique_ptr<FileTransfers> files_;
    TabCompleter completer_;
    std::string selfNick_;
    std::vector<std::string> onlineNicks_;
    std::vector<std::string> knownNicks_;
    std::string host_;
    int port_ = 5555;
    bool colorEnabled_ = true;
    bool welcomed_ = false;
    std::string knownServersPath_ = "dchat-known-servers.txt";
    // 交互模式：连接与登录分两步，账号先记在这里，SubmitCredentials() 再发
    std::string pendingUser_;
    std::string pendingPassword_;
    bool pendingRegister_ = false;
    // ---- 多会话状态 ----
    std::vector<RoomInfo> rooms_;
    std::vector<MemberInfo> members_;
    std::vector<GroupHit> groupHits_;
    std::string currentRoom_;
    bool roomsCapable_ = false;
    std::vector<std::string> pending_;  // 待审的入群申请
    std::vector<std::string> blocks_;   // 黑名单
};

}  // namespace dchat
