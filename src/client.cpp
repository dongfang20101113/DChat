// 聊天客户端：Win32 图形界面 + Winsock。
// 界面特点：
// - 微信式气泡记录区（自绘子窗口）：自己的消息靠右、别人的靠左、系统提示居中
// - 所有按钮/开关都是 GDI+ 抗锯齿的圆角自绘控件
// - 主题三态（深色 / 浅色 / 跟随系统，默认深色）
// - 未读提示：不在前台时计数并显示在标题栏；被 @ 或 @all 时闪任务栏
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objidl.h>    // gdiplus.h 需要 PROPID（WIN32_LEAN_AND_MEAN 下不会自动带进来）
#include <gdiplus.h>   // 缩略图缩放绘制
#include <commdlg.h>   // GetOpenFileNameW：选文件
#include <shellapi.h>  // 拖放：DragAcceptFiles / DragQueryFileW
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>    // 让窗口标题栏跟随深色模式
#include <uxtheme.h>   // 让系统绘制的滚动条跟随深色模式

#include <atomic>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bubble.h"
#include "auth.h"
#include "file_transfer.h"
#include "image_preview.h"
#include "input_history.h"
#include "protocol.h"
#include "reconnect.h"       // 断线重连的退避策略与状态机（纯逻辑，有单测）
#include "crypto.h"

#include <fstream>  // TOFU：记住服务器指纹（dchat-known-servers.txt）
#include "render.h"
#include "resource.h"
#include "rounded.h"
#include "server_command.h"  // LooksLikePasswordCommand：改密码那一行不进输入历史
#include "server_rules.h"    // RuleMbToBytes / 服务器规则
#include "chat_color.h"       // 彩色文字代码（#rrggbb / &a）
#include "color_picker.h"    // 取色盘（色相环 + 饱和度/明度方块）
#include "picker_dialog.h"   // 取色盘窗口
#include "ui_layout.h"       // 菜单栏 / 状态条 / 输入行 / 会话列表的几何与图标
#include "room.h"            // 多会话：房间 ID 规则、本地设置（免打扰/置顶/本地缓存）
#include "voice_notes.h"     // 语音消息：录音、播放、判定

namespace {

constexpr UINT WM_APP_LINE = WM_APP + 1;
constexpr UINT WM_APP_CLOSED = WM_APP + 2;
constexpr UINT WM_APP_XFER = WM_APP + 3;  // 文件发送线程 -> 界面线程
constexpr UINT WM_APP_FILECLICK = WM_APP + 4;  // 记录区点到了文件卡片 -> 主窗口处理
constexpr UINT WM_APP_PREVIEW = WM_APP + 5;    // 缩略图加载完了
constexpr UINT WM_APP_FILEOPEN = WM_APP + 6;   // 点了缩略图 -> 用默认程序打开
constexpr UINT WM_APP_VOICECLICK = WM_APP + 7;  // 点到了语音气泡 -> 播放/暂停
constexpr UINT_PTR kRecordTimerId = 1;          // 录音计时（每 200ms 刷一次界面）
constexpr UINT_PTR kStateTimerId = 2;           // 自动化测试的状态导出（见 WriteClientState）
constexpr UINT_PTR kReconnectTimerId = 4;       // 断线自动重连的等待计时
// 重连尝试的结果（lParam 是 new 出来的 std::string* 错误原因）。
// 为什么要把"连接"扔到工作线程再回消息：TCP 连不上的时候 connect() 会卡
// 十几秒，直接放在 WM_TIMER 里会把界面冻住，连窗口都拖不动。
constexpr UINT WM_APP_RECONNECT = WM_APP + 41;

// 给客户端窗口发 WM_COPYDATA 时用的标识（脚本/其他程序可以借此让它发送文件）
constexpr ULONG_PTR kFileSendMagic = 0x43484131;  // 'CHA1'

constexpr int IDC_CONNECT = 1001;
constexpr int IDC_DISCONNECT = 1002;
constexpr int IDC_SEND = 1003;
constexpr int IDC_INPUT = 1004;
constexpr int IDC_VIEW = 1005;
constexpr int IDC_STATUS = 1006;
constexpr int IDC_COLOR = 1007;
constexpr int IDC_THEME = 1008;
constexpr int IDC_FILESEND = 1009;
constexpr int IDC_VOICE = 1010;  // 「录音」按钮

constexpr int IDD_OK = 1;
constexpr int IDD_CANCEL = 2;
constexpr int IDD_HOST = 100;
constexpr int IDD_PORT = 101;
constexpr int IDD_USER = 102;
constexpr int IDD_PASSWORD = 103;
constexpr int IDD_CONFIRM = 104;
constexpr int IDD_SHOW = 105;  // 「显示密码」勾选框

constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                               WS_THICKFRAME | WS_MAXIMIZEBOX;

constexpr std::size_t kMaxItems = 400;
constexpr const wchar_t* kWindowTitle = L"dchat 客户端";
constexpr const wchar_t* kViewClass = L"DchatBubbleView";

enum class ThemeMode { Dark, Light, System };

// ---------------- 记录区数据 ----------------
enum class ItemKind { Say, Notice, Error, Announce, File };

struct Item {
    ItemKind kind = ItemKind::Notice;
    std::string raw;   // Say: 协议原文；Notice/Error: 显示文本
    std::string time;  // Notice/Error 的时间戳（Say 的时间在协议行里）
    // ---- 文件卡片（ItemKind::File）----
    std::string nick;              // 上传者
    std::string fileId;            // 服务器的文件 ID
    unsigned long long fileSize = 0;
    int fileState = 0;             // 0=可下载 1=下载中 2=已保存 3=失败
    int fileProgress = 0;          // 0..100
    std::string savedPath;         // 下载完成后保存到的路径（UTF-8）
    std::string fileNote;          // 失败原因等
    // ---- 语音（ItemKind::File 且 isVoice）----
    //
    // 语音走的还是文件卡片那一套（同一个下载流程、同一份状态机），
    // 只是**画法不同**：它是能点响的气泡，不是"点一下才下载"的卡片。
    // 复用下载流程是有意的——那套逻辑已经被限速/过期/失败重试磨过了。
    bool isVoice = false;
    // 语音时长（下载完成时读一次 WAV 头存下来；0 = 还不知道）。
    // **不在绘制里读文件**：记录区每秒都要重画（进度条在走），每帧开一次文件纯属浪费。
    int voiceSeconds = 0;
    // 图片 / 视频的缩略图（HBITMAP，析构时自动 DeleteObject）
    std::shared_ptr<void> preview;
};

std::deque<Item> g_items;
int g_scrollTop = 0;
int g_contentHeight = 0;
bool g_stickToBottom = true;
RECT g_inputPill{0, 0, 0, 0};  // 输入框的胶囊背景（父窗口绘制，输入框内嵌在里面）

// ---------------- 多会话（私聊 / 群聊）----------------
//
// ## 为什么记录区还是只有一个
//
// 直觉上"每个会话一个记录区"更干净，但那要把**气泡排版、滚动条、命中测试、
// 文件卡片状态机、语音播放状态**全部改成按会话实例化 —— 而这些逻辑绝大部分和
// 会话无关（一条消息怎么排、卡片怎么画，换个会话完全一样）。
//
// 所以这里用**换内容不换壳**：`g_items` 永远就是"当前显示的那个会话的内容"，
// 切换会话时把它和旧会话存的那份对调一下，再加上滚动位置。绘制代码一行都不用动。
// 代价是"非当前会话来了消息"要单独走一条 AppendToSession 路径 —— 那是几十行，
// 比改五处绘制逻辑便宜得多，也安全得多。
struct SessionMember {
    std::string nick;
    std::string display;  // 群昵称（空 = 用账号名）
    std::string role;     // owner / admin / member
};

struct Session {
    std::string id;
    std::string kind = "group";  // hall / group / dm
    std::string title;           // 服务端下发的显示名（群聊是群名，私聊是**对方**昵称）
    int members = 0;
    int maxMembers = 0;
    std::string owner;                 // 群主（RINFO 第 4 格；大厅和私聊为空）
    std::string myRole = "member";
    std::string joinMode = "approval";
    bool listed = false;
    std::vector<std::string> pending;  // 待审的入群申请（GREQ 收到的）

    // ---- 群管理员可设项：这里只是**本地镜像，改要走服务端** ----
    std::string name;            // 群名（RSET name，Base64 解出来的）
    std::string announcement;    // 群公告
    unsigned long long cacheMaxMb = 0;
    bool importantNotice = true;
    std::vector<SessionMember> memberList;

    // 这个会话的历史已经显示到第几条（RHIST 的序号）。
    // 服务端每次收到 ROOM 都会把历史重放一遍，**不去重就会越切越长** ——
    // 屏幕上会堆出好几份历史、好几个「以上是历史消息」。
    unsigned long long lastHistorySeq = 0;

    // ---- 本地设置：**永不上传** ----
    // 这一行是"管理员设置"和"成员设置"的分界线。服务端一行都不会读到它。
    dchat::RoomPrefs prefs;

    // ---- 记录区内容与滚动位置（切走时存回来）----
    std::deque<Item> items;
    int scrollTop = 0;
    bool stickToBottom = true;
    std::wstring draft;          // 每个会话各自的草稿：切走再切回来不丢字

    int unread = 0;
    bool unreadImportant = false;  // 未读里有没有 @我 / @全体成员
};

std::vector<Session> g_sessions;
int g_currentSession = 0;
bool g_roomsEnabled = false;    // 服务端确认过 rooms1（没有就退回"只有大厅"的老界面）
bool g_roomsDeclared = false;   // 已经向服务端声明过，避免每次登录重复发
int g_sessionScroll = 0;        // 左栏滚过去多少像素
int g_sessionHover = -1;

/** 群搜索结果（GHIT 收进来的，等 GHIT_END 再一次性显示）。 */
struct GroupHit {
    std::string id;
    std::string name;
    int members = 0;
    int maxMembers = 0;
    std::string joinMode;
};
std::vector<GroupHit> g_groupHits;
bool g_groupSearchDone = false;
std::vector<std::string> g_blockList;  // 黑名单（GBLOCKS 收进来的）
bool g_blockListDone = false;
// 这一轮 ROOMLIST 里出现过的房间 ID。收完（ROOMLIST_END）拿它跟本地的群聊对一遍，
// 本地有、名单里没有的，说明我已经不在那个群里了（退群 / 被踢），要从左栏拿掉。
std::vector<std::string> g_roomListSeen;

// 会话设置窗 / 群聊窗的句柄。
//
// **必须用进程内的全局变量记，不能用 FindWindowW 去搜**：窗口类名在两个客户端
// 进程里是同一个，FindWindowW 会跨进程搜到**别人的**窗口，再拿它的 GWLP_USERDATA
// 当本进程指针解引用就是访问越界 —— 状态导出那里实测崩过：两个客户端一起跑、
// 其中一个开着会话设置窗时，另一个每 200ms 必崩（0xc0000005）。
HWND g_roomSettingsWindow = nullptr;

// 左栏的几何与显示顺序。顺序单独存一份：几何层只知道"第几行"，
// 而"第几行对应哪个会话"要经过置顶排序 —— 两边各算一次排序迟早会错位。
dchat::SessionListLayout g_sessionLayout;
std::vector<int> g_sessionOrder;

/** 左栏显示顺序：**置顶的排前面**，其余保持服务端给的顺序（大厅、群聊、私聊）。 */
std::vector<int> SessionDisplayOrder() {
    std::vector<int> order;
    for (std::size_t i = 0; i < g_sessions.size(); ++i) {
        if (g_sessions[i].prefs.pinned) order.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < g_sessions.size(); ++i) {
        if (!g_sessions[i].prefs.pinned) order.push_back(static_cast<int>(i));
    }
    return order;
}

/** 一行下面的小字。 */
std::string SessionSubtitle(const Session& session) {
    if (session.kind == "dm") return "私聊";
    if (session.kind == "hall") return "公共大厅";
    std::string text = std::to_string(session.members) + " 人";
    if (session.maxMembers > 0) text += " / " + std::to_string(session.maxMembers);
    if (session.myRole == "owner") {
        text += "・群主";
    } else if (session.myRole == "admin") {
        text += "・管理员";
    }
    return text;
}

/** 找会话下标；-1 = 没有。 */
int FindSession(const std::string& id) {
    for (std::size_t i = 0; i < g_sessions.size(); ++i) {
        if (g_sessions[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

/** 找会话，没有就按 kind 建一个占位（后续 RINFO 会把它填全）。 */
int EnsureSession(const std::string& id, const std::string& kind) {
    const int found = FindSession(id);
    if (found >= 0) return found;
    Session session;
    session.id = id;
    session.kind = kind;
    session.title = id;  // 先拿 ID 顶着，RINFO 一到就换成真名字
    session.prefs = dchat::DefaultRoomPrefs(
        kind == "hall" ? dchat::RoomKind::Hall
                       : (kind == "dm" ? dchat::RoomKind::Direct : dchat::RoomKind::Group));
    g_sessions.push_back(std::move(session));
    return static_cast<int>(g_sessions.size()) - 1;
}

// 下面这些定义在稍后（要用到 ViewAddItem / 面板重画）。
// 都在这儿先声明一次：多会话的模型定义在文件靠前的位置，而它们要用到的
// 重画、状态刷新、发包函数散在后面 —— 靠前向声明比把模型挪来挪去稳妥。
void SwitchSession(int index);
void AppendToSession(int index, const Item& item);
void SaveRoomPrefs();
void LoadRoomPrefs();
bool IsConnected();
bool SendRawLine(const std::string& line);
void UpdateStatus();
void ClearUnread();
void RefreshSessionPanel();
void OpenRoomSettings(HWND parent, const std::string& sessionId);
void OpenGroupWindow(HWND parent);
bool WindowIsActive();
void UpdateTitle();
void FlashTaskbar();

// 会话本地设置的落盘路径。**和服务器无关**：这是这台机器上这个人自己的偏好。
constexpr const char* kRoomPrefsPath = "dchat-rooms.prefs";

struct UiState {
    HWND hwnd = nullptr;
    HWND hStatus = nullptr;
    HWND hView = nullptr;
    HWND hInput = nullptr;
    HWND hConnect = nullptr;
    HWND hDisconnect = nullptr;
    HWND hSend = nullptr;
    HWND hColorToggle = nullptr;
    HWND hThemeButton = nullptr;
    HWND hFileSend = nullptr;
    HWND hVoice = nullptr;    // 「录音」按钮
    HWND hSuggest = nullptr;  // 输入框上方的候选浮层
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontLarge = nullptr;
} ui;

// ---------------- 主窗口外壳（菜单栏 / 状态条 / 输入行）----------------
//
// 顶栏和底栏都改成**自绘**，不再是一排 BS_OWNERDRAW 按钮：
//   以前顶部一条挤了 6 个按钮（录音 / 发送文件 / 彩色聊天 / 深浅色 / 连接 / 断开），
//   "偶尔改一次"的设置和"每次都用"的发消息混在一起，还占掉整整一条把聊天挤矮。
//   现在只剩：菜单栏（连接 / 设置 / 帮助 + 齿轮）+ 底部状态条 + 输入行。
// 几何全部来自 src/ui_layout.cpp（可单测，见 tests/test_ui_layout.cpp）。
std::vector<dchat::MenuItem> g_menuItems;
RECT g_gearRect{0, 0, 0, 0};
RECT g_chipRect{0, 0, 0, 0};
dchat::BottomLayout g_bottom;
int g_menuHover = -1;   // 鼠标悬停的菜单项下标
int g_plusHover = -1;   // ＋ 菜单里悬停的项：0 发送文件 / 1 录语音 / -1 没有
bool g_plusMenuOpen = false;
bool g_gearHover = false;
bool g_chipHover = false;
int g_contentTop = 0;          // 记录区上边界（菜单栏底边）
std::string g_statusLine;      // 状态条上的只读文字
int g_voiceLimitSeconds = 60;  // 语音最长时长（设置里可改；上限还受 2 MB 约束）
bool g_chatColorEnabled = true;  // 彩色文字代码是否生效（服务器用 chatcolor 规则控制）
// 排版时的"还没被色码上色"哨兵色：真正用什么色要等绘制时看气泡（自己的/别人的）
constexpr COLORREF kAutoColor = 0xFFFFFFFF;

std::map<HWND, bool> g_hover;

dchat::TabCompleter g_tabComplete;  // Tab 指令补全的状态
std::vector<std::string> g_onlineNicks;  // 当前在线昵称（Tab 补参数用）
std::vector<std::string> g_knownNicks;   // 服务器下发的已注册昵称（可能不在线，Tab 也补）

// 输入框上方的候选浮层（类似 Minecraft 的 Tab 提示）
constexpr const wchar_t* kSuggestClass = L"DchatSuggestWnd";
constexpr int kSuggestRowH = 22;
constexpr int kSuggestTitleH = 18;
constexpr int kSuggestPadX = 10;
constexpr int kSuggestPadY = 6;
constexpr int kSuggestColumns = 3;
constexpr int kSuggestMaxRows = 8;
bool g_suggestSuppress = false;  // 程序自己改输入框时不要重算候选（否则 Tab/方向键选不中）
dchat::CompletionResult g_suggest;  // 当前候选
int g_suggestPicked = -1;          // 当前高亮的候选
int g_suggestHover = -1;           // 鼠标悬停的候选
int SuggestPanelHeight();          // 定义在下面：候选浮层的高度
void RefreshSuggestions();         // 定义在下面：重新算候选并显示/隐藏浮层
void HideSuggestions();            // 定义在下面： 收起浮层
void InvalidateSuggest();
void Layout(HWND hwnd);
void ApplySuggestion(int index);
void StartFileDownload(const std::string& id);
void RequestThumbnail(const std::string& fileId);
void UpdateStatus();        // 定义在下面：刷新状态条文字 + 控件可用状态
void InvalidateChrome(HWND hwnd);  // 定义在下面：只重画菜单栏与底部（聊天记录不动）
void LayoutWindowChrome(HWND hwnd);  // 定义在下面：算菜单栏/底部的几何
void DrawWindowChrome(HWND hwnd, HDC dc, const RECT& client);  // 定义在下面：画外壳
void OpenSettings(HWND parent);      // 定义在下面：打开设置窗口
std::string StatusChipText();        // 定义在下面：菜单栏右侧胶囊上的字
std::string BuildStatusLine();       // 定义在下面：状态条上的只读文字
bool HandleLocalCommand(const std::wstring& text);  // 定义在下面：/chatcolor 这类本机指令
void DoSendFile(HWND hwnd);          // 定义在下面：选一个文件发出去
void DoConnect(HWND hwnd);           // 定义在下面：连接
void DoDisconnect(HWND hwnd);        // 定义在下面：断开
void StartVoiceRecording();          // 定义在下面：开始录音
void StopVoiceRecordingAndSend();    // 定义在下面：结束并发送

// ---------------- 语音消息（阶段 4 桌面端） ----------------
//
// 录音/播放的实现在 voice_notes.cpp 里；这里只管界面接线：
// 按钮、计时、气泡渲染、点一下播放。
dchat::VoiceRecorder g_recorder;
dchat::VoicePlayer g_voicePlayer;
std::string g_playingVoiceId;   // 正在播的语音（空 = 没在播）
int g_playingVoiceTotal = 0;    // 正在播的这条总共多少秒
std::string g_recordingHint;    // 录音中状态栏显示的那句
void StartVoiceRecording();     // 定义在下面：开始录音
void StopVoiceRecordingAndSend();  // 定义在下面：结束并发送
void ToggleVoicePlayback(const std::string& fileId);  // 定义在下面：播/暂停
void RefreshVoiceState();       // 定义在下面：把录音/播放状态反映到界面

std::string TrimAscii(const std::string& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

// 服务器发的 NAMES/LEFT/JOINED 里去掉开头的 hh:mm，留下正文
std::string StripTimePrefix(const std::string& rest) {
    const std::size_t space = rest.find(' ');
    if (space != std::string::npos && dchat::LooksLikeTime(rest.substr(0, space))) {
        return rest.substr(space + 1);
    }
    return rest;
}

// 把 "alice, bob" 这样的名单拆成一个个昵称
void ParseNickList(const std::string& list, std::vector<std::string>* out) {
    out->clear();
    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t comma = list.find(',', begin);
        const std::string name =
            TrimAscii(list.substr(begin, comma == std::string::npos ? std::string::npos
                                                                    : comma - begin));
        if (!name.empty() && name != "(暂时没人设置昵称)") out->push_back(name);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
}

void SetOnlineNicks(const std::string& list) { ParseNickList(list, &g_onlineNicks); }

// 服务器发的 KNOWN：所有已注册账号（含不在线的），补 /ban /op /unban /ip 时用
void SetKnownNicks(const std::string& list) { ParseNickList(list, &g_knownNicks); }

void AddOnlineNick(const std::string& name) {
    if (name.empty()) return;
    for (const std::string& nick : g_onlineNicks) {
        if (nick == name) return;
    }
    g_onlineNicks.push_back(name);
}

void RemoveOnlineNick(const std::string& name) {
    for (auto it = g_onlineNicks.begin(); it != g_onlineNicks.end(); ++it) {
        if (*it == name) {
            g_onlineNicks.erase(it);
            return;
        }
    }
}

// 断线时清理未完成的传输（定义在下面「文件传输」一段里）
void ResetTransfers(const std::string& reason);
// 状态栏里显示的「发送 / 接收 xxx 45%」，由文件传输那一段维护
std::string g_transferStatus;

SOCKET g_sock = INVALID_SOCKET;
std::mutex g_sendMutex;
std::thread g_recvThread;
std::atomic<bool> g_running{false};
bool g_colorEnabled = true;
dchat::InputHistory g_inputHistory;  // 输入框历史：上键翻出自己刚发过的内容
ThemeMode g_themeMode = ThemeMode::Dark;
bool g_darkTheme = true;
int g_unread = 0;
std::string g_host = "127.0.0.1";
std::string g_nick = "user";
bool g_authPending = false;      // 已发出登录/注册请求，等待服务器回应
bool g_authed = false;           // 已经登录成功（连上但还没登录时状态栏显示"未登录"）
// 断线自动重连：状态机负责"该不该连、等多久"，这里只放它的实例。
dchat::ReconnectState g_reconnect;
// 用户自己点了「断开」：这个标志保证 WM_APP_CLOSED 不会把它又连回去。
bool g_userDisconnected = false;
bool g_heartbeatPending = false; // 刚发过心跳 PING（对应的 PONG 不显示成提示）
int g_maxFileMb = 64;            // 服务器当前的单文件上限（由服务器发的 RULES 行更新）
int g_port = dchat::kDefaultPort;

// 刚提交的账号密码。登录成功后写进 dchat-login.dat（DPAPI 加密），下次自动登录。
std::string g_pendingUser;
std::string g_pendingPassword;

// 改密码的"等待确认"的新密码。
//
// 用户在聊天框里敲了 `/cp <新密码>` 之后先记在这里，**等服务端回「密码已修改」
// 才真正更新**自动登录用的那一份。为什么不提前更新：服务器可能因为密码太短、
// 参数写错、或者这条指令在那条路上被拒而根本没改，提前改掉会让"记住的密码"
// 与实际密码不一致 —— 那正是这个功能要修的问题。
std::string g_pendingNewPassword;
ULONGLONG g_pendingNewPasswordAt = 0;
constexpr ULONGLONG kPasswordChangeTimeoutMs = 120000;  // 两分钟没回执就不认了

// ---------------- 自动化测试的状态导出 ----------------
//
// 为什么要有它：这台机器上**屏幕抓图在 DWM 下会拿到旧内容**（实测），所以
// "界面上到底有没有出现这条消息"没法靠截图判断。改成让客户端把内部状态写成
// 文本、测试脚本读文件断言 —— 这是目前唯一可靠的验收方式（见 tools/ui-rooms-auto.ps1）。
//
// 只在启动参数带 `--state-file <路径>` 时生效。正常启动 g_statePath 为空，
// WriteClientState 第一行就返回，一个字节都不会多写。
std::wstring g_statePath;
unsigned long long g_stateSeq = 0;

/** 值压成单行（正文里可能有换行）。读的一侧按**第一个** '=' 切分，所以 '=' 不必转义。 */
std::string EscapeStateValue(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            continue;
        } else {
            out += ch;
        }
    }
    return out;
}

/** 一条记录项在状态文件里的正文：Say 取解析后的发言者与正文，其它取原文。 */
void StateItemText(const Item& item, std::string* nick, std::string* text, std::string* kind) {
    if (item.kind == ItemKind::Say) {
        dchat::SayInfo say;
        if (dchat::ParseSay(item.raw, g_nick, &say)) {
            *nick = say.nick;
            *text = say.text;
            *kind = "say";
            return;
        }
    }
    *nick = item.nick;
    *text = item.raw.empty() ? item.time : item.raw;
    *kind = item.kind == ItemKind::Notice     ? "notice"
            : item.kind == ItemKind::Error    ? "error"
            : item.kind == ItemKind::Announce ? "announce"
                                              : "file";
}

/**
 * 把界面状态写成 `键=值`、一行一条的文本。
 *
 * 记录区读的是**当前会话的活副本 g_items**，非当前会话才是 session.items
 * （这个不变量见上面的长注释）—— 这里必须照同一个规则取，否则导出的内容会和
 * 界面上显示的差一份，测试就等于白测。
 *
 * 末尾一定有 `END=1`：读的一侧靠它判断文件写完整了，避免读到半截。
 */
void AppendDialogState(std::string* out);  // 定义在弹窗代码之后（要用到弹窗的结构体）

void WriteClientState() {
    if (g_statePath.empty()) return;
    constexpr std::size_t kTailMax = 6;
    const int sessionCount = static_cast<int>(g_sessions.size());
    std::string out = "DCHAT_STATE=1\n";
    out += "seq=" + std::to_string(++g_stateSeq) + "\n";
    out += "rooms.enabled=" + std::to_string(g_roomsEnabled ? 1 : 0) + "\n";
    // 连接状态：界面改版后状态栏是自绘的，测试脚本没法再从子控件里读那句
    // "已连接 … 用户：xxx"，所以这里直接给出可断言的字段。
    out += "conn.connected=" + std::to_string(IsConnected() ? 1 : 0) + "\n";
    out += "conn.authed=" + std::to_string(g_authed ? 1 : 0) + "\n";
    out += "conn.nick=" + EscapeStateValue(g_nick) + "\n";
    // 断线自动重连的状态：验证「拔网线 → 退避重试 → 恢复后自动连回」时靠这几个字段断言，
    // 不用去猜界面上的提示文字。
    out += "reconnect.wanted=" + std::to_string(g_reconnect.wanted ? 1 : 0) + "\n";
    out += "reconnect.attempt=" + std::to_string(g_reconnect.attempt) + "\n";
    out += "current.index=" + std::to_string(g_currentSession) + "\n";
    out += "current.id=" + EscapeStateValue((g_currentSession >= 0 && g_currentSession < sessionCount)
                                                ? g_sessions[g_currentSession].id
                                                : std::string()) +
           "\n";
    out += "session.count=" + std::to_string(sessionCount) + "\n";
    out += "block.count=" + std::to_string(g_blockList.size()) + "\n";
    out += "groupHits.count=" + std::to_string(g_groupHits.size()) + "\n";

    for (int i = 0; i < sessionCount; ++i) {
        const Session& session = g_sessions[i];
        const std::string p = "session." + std::to_string(i) + ".";
        out += p + "id=" + EscapeStateValue(session.id) + "\n";
        out += p + "kind=" + EscapeStateValue(session.kind) + "\n";
        out += p + "title=" + EscapeStateValue(session.title) + "\n";
        out += p + "unread=" + std::to_string(session.unread) + "\n";
        out += p + "important=" + std::to_string(session.unreadImportant ? 1 : 0) + "\n";
        // ---- 成员本地设置：这一行以下才是"永不上传"的部分 ----
        out += p + "muted=" + std::to_string(session.prefs.muted ? 1 : 0) + "\n";
        out += p + "pinned=" + std::to_string(session.prefs.pinned ? 1 : 0) + "\n";
        out += p + "alertImportant=" + std::to_string(session.prefs.alertImportant ? 1 : 0) + "\n";
        out += p + "localCacheMb=" + std::to_string(session.prefs.localCacheMb) + "\n";
        // ---- 群管理员项的本地镜像（改要走服务端）----
        out += p + "members=" + std::to_string(session.members) + "\n";
        out += p + "maxMembers=" + std::to_string(session.maxMembers) + "\n";
        out += p + "owner=" + EscapeStateValue(session.owner) + "\n";
        out += p + "myRole=" + EscapeStateValue(session.myRole) + "\n";
        out += p + "joinMode=" + EscapeStateValue(session.joinMode) + "\n";
        out += p + "listed=" + std::to_string(session.listed ? 1 : 0) + "\n";
        out += p + "name=" + EscapeStateValue(session.name) + "\n";
        out += p + "announcement=" + EscapeStateValue(session.announcement) + "\n";
        out += p + "cacheMaxMb=" + std::to_string(session.cacheMaxMb) + "\n";
        out += p + "pending.count=" + std::to_string(session.pending.size()) + "\n";
        for (std::size_t j = 0; j < session.pending.size() && j < 8; ++j) {
            out += p + "pending." + std::to_string(j) + "=" +
                   EscapeStateValue(session.pending[j]) + "\n";
        }

        const std::deque<Item>& items = (i == g_currentSession) ? g_items : session.items;
        out += p + "items=" + std::to_string(items.size()) + "\n";
        // tail 从**最新往旧**排：tail.0 就是最后一条，断言"刚发的到了没"最省事。
        std::size_t n = 0;
        for (auto it = items.rbegin(); it != items.rend() && n < kTailMax; ++it, ++n) {
            std::string nick;
            std::string text;
            std::string kind;
            StateItemText(*it, &nick, &text, &kind);
            const std::string q = p + "tail." + std::to_string(n) + ".";
            out += q + "kind=" + kind + "\n";
            out += q + "nick=" + EscapeStateValue(nick) + "\n";
            out += q + "text=" + EscapeStateValue(text) + "\n";
        }
        out += p + "tail.count=" + std::to_string(n) + "\n";
    }
    // ---- 真实控件的句柄：脚本直接对其发 WM_SETTEXT / BM_CLICK，不用猜坐标 ----
    const auto handleText = [](HWND h) {
        return std::to_string(reinterpret_cast<unsigned long long>(h));
    };
    const auto rectText = [](const RECT& r) {
        return std::to_string(r.left) + "," + std::to_string(r.top) + "," +
               std::to_string(r.right) + "," + std::to_string(r.bottom);
    };
    out += "main.window=" + handleText(ui.hwnd) + "\n";
    out += "main.input=" + handleText(ui.hInput) + "\n";
    out += "main.send=" + handleText(ui.hSend) + "\n";

    // ---- 左栏几何：**就是这一帧用来命中的那一份**（g_sessionLayout 随窗口尺寸重算），
    //      脚本照着点，等于点了界面上真正画出来的那个矩形 ----
    out += "sessionList.valid=" + std::to_string(g_sessionLayout.valid ? 1 : 0) + "\n";
    out += "sessionList.search=" + rectText(g_sessionLayout.searchButton) + "\n";
    out += "sessionList.new=" + rectText(g_sessionLayout.newButton) + "\n";
    out += "sessionList.rows=" + std::to_string(g_sessionLayout.rows.size()) + "\n";
    for (std::size_t i = 0; i < g_sessionLayout.rows.size(); ++i) {
        const dchat::SessionRowLayout& row = g_sessionLayout.rows[i];
        const std::string q = "sessionList.row." + std::to_string(i) + ".";
        out += q + "index=" + std::to_string(row.index) + "\n";
        // 点一行命中的是**显示行号**，真正切到哪个会话还要过一遍 g_sessionOrder
        // （排序规则见 SessionDisplayOrder）。这里替脚本算好，省得两边各算一次。
        std::string rowSession;
        if (row.index >= 0 && row.index < static_cast<int>(g_sessionOrder.size())) {
            const int sessionIndex = g_sessionOrder[row.index];
            if (sessionIndex >= 0 && sessionIndex < sessionCount) {
                rowSession = g_sessions[sessionIndex].id;
            }
        }
        out += q + "session=" + EscapeStateValue(rowSession) + "\n";
        out += q + "rect=" + rectText(row.row) + "\n";
    }
    // ---- 菜单栏 / 底部按钮：改版后这些是**自绘**的（第 304 行那段注释），
    //      没有控件 ID 可以发 WM_COMMAND，只能按坐标点。同样导出这一帧
    //      真正用来命中的矩形，脚本照点即可。----
    out += "menu.count=" + std::to_string(g_menuItems.size()) + "\n";
    for (std::size_t i = 0; i < g_menuItems.size(); ++i) {
        const dchat::MenuItem& item = g_menuItems[i];
        const char* action = "unknown";
        switch (item.action) {
            case dchat::MenuAction::Connect:      action = "connect"; break;
            case dchat::MenuAction::Settings:     action = "settings"; break;
            case dchat::MenuAction::Help:         action = "help"; break;
            case dchat::MenuAction::ToggleOnline: action = "online"; break;
        }
        const std::string q = "menu." + std::to_string(i) + ".";
        out += q + "action=" + action + "\n";
        out += q + "rect=" + rectText(item.rect) + "\n";
    }
    out += "bottom.plus=" + rectText(g_bottom.plusButton) + "\n";
    out += "bottom.send=" + rectText(g_bottom.sendButton) + "\n";
    RECT clientRect{};
    if (ui.hwnd != nullptr && GetClientRect(ui.hwnd, &clientRect)) {
        out += "main.gear=" + rectText(dchat::GearRect(clientRect.right)) + "\n";
    }
    AppendDialogState(&out);

    out += "END=1\n";

    // 直接走 Win32：路径是宽字符（可能有中文目录），绕开窄/宽路径那一堆坑。
    // FILE_SHARE_READ 让测试脚本在客户端还跑着的时候就能读。
    HANDLE file = CreateFileW(g_statePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(file);
}

// ---------------- 配色 ----------------
struct Palette {
    COLORREF windowBg;
    COLORREF bubbleOther;
    COLORREF bubbleOtherBorder;
    COLORREF bubbleOwn;
    COLORREF bubbleOwnBorder;
    COLORREF bubbleOwnText;
    COLORREF text;
    COLORREF system;
    COLORREF error;
    COLORREF time;
    COLORREF mention;
    COLORREF noticeBg;
    COLORREF announceBg;      // 全服公告
    COLORREF announceBorder;
    COLORREF announceText;
    COLORREF neutral;
    COLORREF neutralBorder;
    COLORREF accent;
    COLORREF accentText;
    COLORREF border;
    COLORREF panel;  // 菜单栏 / 状态条 / 设置窗口的背景（比窗口底色亮一点，才看得出分层）
    COLORREF hover;  // 菜单项悬停
    COLORREF nick[dchat::kNickPaletteSize];
};

const Palette kLightPalette{
    RGB(242, 243, 245), RGB(255, 255, 255), RGB(222, 225, 230), RGB(0, 120, 215),
    RGB(0, 100, 190),   RGB(255, 255, 255), RGB(32, 32, 32),     RGB(110, 110, 110),
    RGB(200, 0, 0),     RGB(130, 130, 130), RGB(176, 96, 0),     RGB(232, 234, 238),
    RGB(255, 246, 222), RGB(214, 150, 10),  RGB(150, 92, 0),
    RGB(233, 236, 239), RGB(205, 210, 216), RGB(0, 120, 215),    RGB(255, 255, 255),
    RGB(214, 217, 222), RGB(250, 250, 251), RGB(230, 233, 237),
    {RGB(0, 102, 204), RGB(0, 140, 60), RGB(204, 102, 0), RGB(150, 0, 150), RGB(0, 140, 140),
     RGB(200, 0, 100), RGB(110, 110, 0), RGB(90, 60, 160)}};

const Palette kDarkPalette{
    RGB(32, 32, 32),    RGB(48, 50, 54),    RGB(70, 73, 78),    RGB(0, 120, 215),
    RGB(30, 90, 160),   RGB(255, 255, 255), RGB(232, 232, 232), RGB(150, 150, 150),
    RGB(255, 110, 110), RGB(140, 140, 140), RGB(255, 190, 80),  RGB(44, 46, 50),
    RGB(58, 48, 24),    RGB(255, 190, 80),  RGB(255, 214, 140),
    RGB(52, 54, 58),    RGB(74, 77, 82),    RGB(0, 120, 215),   RGB(255, 255, 255),
    RGB(64, 66, 70),    RGB(42, 44, 48),    RGB(58, 61, 66),
    {RGB(86, 156, 214), RGB(120, 200, 130), RGB(235, 175, 95), RGB(205, 140, 235),
     RGB(95, 205, 205), RGB(245, 135, 175), RGB(205, 205, 115), RGB(155, 155, 245)}};

const Palette* g_palette = &kDarkPalette;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_pillBrush = nullptr;  // 输入框自己的底色（和输入框胶囊同色）

// ---------------- 字符编码 ----------------
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

COLORREF Adjust(COLORREF color, int delta) {
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return RGB(clamp(GetRValue(color) + delta), clamp(GetGValue(color) + delta),
               clamp(GetBValue(color) + delta));
}

// ---------------- 主题 ----------------
bool SystemPrefersDark() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value == 0;
}

// 让"由系统绘制"的部分（滚动条、窗口标题栏）也跟随深色模式。
// 不调用这个的话，深色主题下滚动条和标题栏仍然是系统的浅色。
void ApplySystemDarkMode(HWND hwnd) {
    if (!hwnd) return;
    SetWindowTheme(hwnd, g_darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    const BOOL dark = g_darkTheme ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE：Win10 20H1 起是 20，早期版本是 19，两个都设一遍
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

void ApplyThemeMode() {
    switch (g_themeMode) {
        case ThemeMode::Light:
            g_darkTheme = false;
            break;
        case ThemeMode::System:
            g_darkTheme = SystemPrefersDark();
            break;
        case ThemeMode::Dark:
        default:
            g_darkTheme = true;
            break;
    }
    g_palette = g_darkTheme ? &kDarkPalette : &kLightPalette;
    if (g_windowBrush) DeleteObject(g_windowBrush);
    g_windowBrush = CreateSolidBrush(g_palette->windowBg);
    if (g_pillBrush) DeleteObject(g_pillBrush);
    g_pillBrush = CreateSolidBrush(g_palette->bubbleOther);

    HWND controls[] = {ui.hConnect, ui.hDisconnect, ui.hSend, ui.hColorToggle, ui.hThemeButton};
    for (HWND control : controls) {
        if (control) InvalidateRect(control, nullptr, TRUE);
    }
    if (ui.hView) InvalidateRect(ui.hView, nullptr, TRUE);
    // 让滚动条、标题栏也切到对应的深浅模式
    ApplySystemDarkMode(ui.hwnd);
    ApplySystemDarkMode(ui.hView);
    ApplySystemDarkMode(ui.hInput);
    if (ui.hwnd) {
        // 标题栏颜色变化需要刷新一次非客户区
        SetWindowPos(ui.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        RedrawWindow(ui.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    }
    if (ui.hwnd) {
        RedrawWindow(ui.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }
}

// ---------------- 记录区（微信式气泡） ----------------
struct BubbleRow {
    bool isBubble = false;  // true: 聊天气泡；false: 居中的系统提示
    bool own = false;
    bool mention = false;
    bool error = false;
    bool announcement = false;  // 全服公告：更大字、居中、特殊配色
    bool isFile = false;        // 文件卡片（QQ 式：点一下才下载）
    std::string nick;
    std::string time;
    std::string text;
    RECT header{};
    RECT bubble{};
    RECT textRect{};
    // 文件卡片专用
    std::size_t itemIndex = 0;
    std::string fileId;
    std::string fileNote;
    std::string fileDetail;
    std::string fileButton;
    unsigned long long fileSize = 0;
    int fileState = 0;
    int fileProgress = 0;
    RECT buttonRect{};
    // 带颜色的正文：**排版在测量时就做好存下来**，绘制时直接用。
    // 自己排版是因为要一段一段换色画，DrawText 的 DT_WORDBREAK 帮不上忙
    // （它一次只认一个颜色）。
    dchat::ColorLayout colored;
    RECT previewRect{};  // 缩略图（没有预览时是空矩形）
    std::shared_ptr<void> preview;  // 缩略图位图（HBITMAP）
    // 语音气泡专用（isVoice 时 fileButton 里放的是时长文字）
    bool isVoice = false;
    int voiceTotalSeconds = 0;   // 这条语音总长（下载完才知道，之前是 0）
    int voicePlayedSeconds = 0;  // 正在播时播到第几秒
    bool voicePlaying = false;   // 正在播（false 时画三角形，true 时画两条竖线）
};

// 文件卡片尺寸
constexpr int kFileCardMaxW = 400;
constexpr int kFileCardH = 68;
constexpr int kFileCardPadX = 14;
constexpr int kFileCardPadY = 10;
constexpr int kFileButtonW = 104;
constexpr int kFileButtonH = 30;
constexpr int kFileCardRadius = 10;
constexpr int kFilePreviewSize = 72;   // 缩略图边长
constexpr int kFileCardHPreview = 100; // 带缩略图时卡片更高
// 语音气泡：比文件卡片矮一截，宽度也窄（就一个播放键 + 时长）
constexpr int kVoiceBubbleW = 176;
constexpr int kVoiceBubbleH = 40;
constexpr int kVoiceBubblePadX = 12;
constexpr int kVoiceBubbleRadius = 12;
constexpr int kVoiceBars = 4;         // 记号用的几根短竖条

// 鼠标悬停在哪张卡片上（用于按钮高亮）
std::string g_fileHoverId;

std::vector<BubbleRow> LayoutRows(HDC dc, int width, int* contentHeight) {
    std::vector<BubbleRow> rows;
    const int margin = dchat::kViewMargin;
    const int padX = dchat::kMessagePaddingX, padY = dchat::kMessagePaddingY;
    const int gap = dchat::kMessageGap, headerH = 16;
    const int maxBubble = (width * dchat::kMaxBubblePercent) / 100;
    int y = margin;

    std::size_t itemIndex = 0;
    for (const Item& item : g_items) {
        BubbleRow row;
        row.itemIndex = itemIndex++;
        if (item.kind == ItemKind::File) {
            // 文件卡片：头一行是"谁发的 + 时间"，下面一张卡片，卡片右侧一个按钮。
            // **语音走同一条路**，只是卡片形状和按钮内容不同（见下面的 isVoice 分支）。
            row.isBubble = false;
            row.isFile = true;
            row.nick = item.nick;
            row.time = item.time;
            // 自己发的靠右：文件卡片和语音气泡共用这一条判断，
            // 免得出现"消息靠右、自己发的语音靠左"这种自相矛盾的界面
            row.own = !g_nick.empty() && item.nick == g_nick;
            row.text = item.raw;  // 文件名
            row.fileId = item.fileId;
            row.fileSize = item.fileSize;
            row.fileState = item.fileState;
            row.fileProgress = item.fileProgress;
            row.preview = item.preview;
            row.fileNote = item.savedPath.empty() ? item.fileNote : item.savedPath;
            row.header = RECT{0, y, width, y + headerH};

            if (item.isVoice) {
                // 语音气泡：一个矮胶囊，整块可点（点一下播放/暂停）
                row.isVoice = true;
                row.voicePlaying = (g_playingVoiceId == item.fileId && g_voicePlayer.IsPlaying());
                if (row.voicePlaying) {
                    row.voicePlayedSeconds = g_voicePlayer.CurrentSeconds();
                    row.voiceTotalSeconds = g_playingVoiceTotal;
                } else if (item.fileState == 2 && !item.savedPath.empty()) {
                    // 没在播时显示下载完成时存下来的真实时长（0 = 读不出来，显示 0:00）
                    row.voiceTotalSeconds = item.voiceSeconds;
                }
                const int available = width - margin * 2;
                const int cardW = available < kVoiceBubbleW ? available : kVoiceBubbleW;
                row.bubble = RECT{margin, y + headerH, margin + cardW, y + headerH + kVoiceBubbleH};
                // 文件卡片那段代码要用的字段，这里给上合理的值（绘制走 isVoice 分支）
                row.fileButton = dchat::FormatDuration(row.voiceTotalSeconds);
                row.fileDetail = dchat::FormatVoiceSize(item.fileSize);
                y = row.bubble.bottom + gap;
                rows.push_back(row);
                continue;
            }

            const int available = width - margin * 2;
            const int cardW = available < kFileCardMaxW ? available : kFileCardMaxW;
            const bool hasPreview = (item.preview != nullptr);
            const int cardH = hasPreview ? kFileCardHPreview : kFileCardH;
            RECT card{margin, y + headerH, margin + cardW, y + headerH + cardH};
            row.bubble = card;
            const int buttonTop = card.top + (cardH - kFileButtonH) / 2;
            row.buttonRect =
                RECT{card.right - kFileCardPadX - kFileButtonW, buttonTop,
                     card.right - kFileCardPadX, buttonTop + kFileButtonH};
            if (hasPreview) {
                const int previewTop = card.top + (cardH - kFilePreviewSize) / 2;
                row.previewRect =
                    RECT{row.buttonRect.left - 10 - kFilePreviewSize, previewTop,
                         row.buttonRect.left - 10, previewTop + kFilePreviewSize};
            }
            const int textRight = hasPreview ? (row.previewRect.left - 10) : (row.buttonRect.left - 10);
            row.textRect = RECT{card.left + kFileCardPadX, card.top + kFileCardPadY,
                                textRight, card.bottom - kFileCardPadY};
            // 明细行：大小 + 上传者 + 当前状态
            row.fileDetail = dchat::FormatBytes(item.fileSize) + "　来自 " + item.nick;
            if (item.fileState == 1) {
                row.fileDetail = "下载中 " + std::to_string(item.fileProgress) + "%　" +
                                 dchat::FormatBytes(item.fileSize) + "　来自 " + item.nick;
                row.fileButton = "下载中";
            } else if (item.fileState == 2) {
                row.fileDetail = "已保存到 " + item.savedPath;
                row.fileButton = "打开文件夹";
            } else if (item.fileState == 3) {
                row.fileDetail = item.fileNote.empty() ? "下载失败，点一下重试" : item.fileNote;
                row.fileButton = "重试";
            } else {
                row.fileButton = "下载";
            }
            y = card.bottom + gap;
            rows.push_back(row);
            continue;
        }
        if (item.kind == ItemKind::Say) {
            dchat::SayInfo say;
            if (dchat::ParseSay(item.raw, g_nick, &say)) {
                row.isBubble = true;
                row.own = say.own;
                row.mention = say.mention;
                row.nick = say.nick;
                row.time = say.time;
                row.text = say.text;

                // 正文自己排版（要一段一段换色）：宽度和以前一样，高度用它算出来的
                row.colored = dchat::LayoutColoredText(dc, Utf8ToWide(say.text),
                                                       g_palette->text, g_chatColorEnabled,
                                                       maxBubble - padX * 2, ui.font);
                RECT area{0, y + headerH, width, y + headerH};
                const dchat::BubblePlacement place = dchat::PlaceBubble(
                    area, row.colored.width, row.colored.height,
                    say.own ? dchat::BubbleAlign::Right : dchat::BubbleAlign::Left, padX, padY, margin,
                    maxBubble);
                row.bubble = place.bubble;
                row.textRect = place.text;
                row.header = RECT{0, y, width, y + headerH};
                y = row.bubble.bottom + gap;
                rows.push_back(row);
                continue;
            }
        }

        // 系统提示：居中的小胶囊（字号与内边距都比正文小一号）
        row.isBubble = false;
        row.error = item.kind == ItemKind::Error;
        row.announcement = item.kind == ItemKind::Announce;
        row.time = item.time;
        const std::string line = item.time.empty() ? item.raw : ("[" + item.time + "] " + item.raw);
        const int maxWidth =
            row.announcement ? (width * dchat::kAnnounceMaxPercent) / 100
                             : (width * dchat::kNoticeMaxPercent) / 100;
        const int padX = row.announcement ? dchat::kAnnouncePaddingX : dchat::kNoticePaddingX;
        const int padY = row.announcement ? dchat::kAnnouncePaddingY : dchat::kNoticePaddingY;
        const SIZE size = dchat::MeasureWrappedText(dc, Utf8ToWide(line), maxWidth - padX * 2,
                                                   row.announcement ? ui.fontLarge : ui.fontSmall);
        RECT area{0, y, width, y};
        const dchat::BubblePlacement place = dchat::PlaceBubble(
            area, size.cx, size.cy, dchat::BubbleAlign::Center, padX, padY, margin, maxWidth);
        row.bubble = place.bubble;
        row.textRect = place.text;
        row.text = line;
        y = row.bubble.bottom + gap;
        rows.push_back(row);
    }

    if (contentHeight) *contentHeight = y + margin;
    return rows;
}

void DrawTextIn(HDC dc, const std::wstring& text, const RECT& rect, HFONT font, COLORREF color,
                UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    RECT target = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

void DrawRowHeader(HDC dc, const BubbleRow& row) {
    if (row.own) {  // 自己的消息：右侧只显示时间
        DrawTextIn(dc, Utf8ToWide(row.time), row.header, ui.fontSmall, g_palette->time,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    // 别人的消息：左侧"昵称 + 时间"，昵称用该用户的固定颜色
    const std::wstring nick = Utf8ToWide(row.nick);
    const COLORREF nickColor =
        g_colorEnabled
            ? g_palette->nick[dchat::NickColorIndex(row.nick) % dchat::kNickPaletteSize]
            : g_palette->system;
    HGDIOBJ oldFont = SelectObject(dc, ui.fontSmall);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, nickColor);
    const int x = row.header.left + 16;
    RECT nickRect{x, row.header.top, x + 300, row.header.bottom};
    DrawTextW(dc, nick.c_str(), static_cast<int>(nick.size()), &nickRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE nickSize{0, 0};
    GetTextExtentPoint32W(dc, nick.c_str(), static_cast<int>(nick.size()), &nickSize);
    SetTextColor(dc, g_palette->time);
    RECT timeRect{x + nickSize.cx + 12, row.header.top, row.header.right - 8, row.header.bottom};
    const std::wstring time = Utf8ToWide(row.time);
    DrawTextW(dc, time.c_str(), static_cast<int>(time.size()), &timeRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

void DrawRow(HDC dc, const BubbleRow& row) {
    if (row.isVoice) {
        // 语音气泡：左（或右）一个播放键，中间几根短竖条当"这是语音"的记号，
        // 右边时长。**不做假波形**：随宽度拉伸的假波形看起来像"这段声音很平"，
        // 其实什么都没表达（真波形要在录制时采样、还要随消息一起传过去）。
        DrawRowHeader(dc, row);

        COLORREF fill = row.own ? g_palette->bubbleOwn : g_palette->bubbleOther;
        COLORREF border = row.own ? g_palette->bubbleOwnBorder : g_palette->bubbleOtherBorder;
        COLORREF fg = row.own ? g_palette->bubbleOwnText : g_palette->text;
        if (!g_colorEnabled) {
            fill = row.own ? g_palette->neutral : g_palette->bubbleOther;
            border = g_palette->border;
            fg = g_palette->text;
        }
        if (row.fileState == 2) {
            // 已下好的语音可以点；鼠标悬停时给一点反馈
            const bool hover = !g_fileHoverId.empty() && g_fileHoverId == row.fileId;
            if (hover) fill = Adjust(fill, g_darkTheme ? 12 : -8);
        }
        ui::FillRoundedRect(dc, row.bubble, kVoiceBubbleRadius, fill, border, 1.0f);

        const int midY = (row.bubble.top + row.bubble.bottom) / 2;
        const int iconLeft = row.bubble.left + kVoiceBubblePadX;
        const int iconSize = 16;

        // 播放键 / 暂停键：没下好时画成灰色（**不隐藏**——隐藏会让气泡在下载完成的
        // 那一刻突然变宽，整列消息跟着跳）
        const COLORREF iconColor = (row.fileState == 2) ? fg : Adjust(fg, g_darkTheme ? -70 : 80);
        if (row.voicePlaying) {  // 两条竖线
            const int barW = 4;
            const int gap = 4;
            RECT left{iconLeft, midY - iconSize / 2, iconLeft + barW, midY + iconSize / 2};
            RECT right{iconLeft + barW + gap, midY - iconSize / 2, iconLeft + barW * 2 + gap,
                       midY + iconSize / 2};
            HBRUSH brush = CreateSolidBrush(iconColor);
            FillRect(dc, &left, brush);
            FillRect(dc, &right, brush);
            DeleteObject(brush);
        } else {  // 三角形
            POINT points[3] = {
                {iconLeft, midY - iconSize / 2},
                {iconLeft + iconSize, midY},
                {iconLeft, midY + iconSize / 2},
            };
            HBRUSH brush = CreateSolidBrush(iconColor);
            HPEN pen = CreatePen(PS_SOLID, 1, iconColor);
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            Polygon(dc, points, 3);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(brush);
            DeleteObject(pen);
        }

        // 几根短竖条：只是"这是语音"的记号，高度固定（见上面的说明）
        int barX = iconLeft + iconSize + 12;
        for (int i = 0; i < kVoiceBars; ++i) {
            const int height = (i % 2 == 0) ? 10 : 16;
            RECT bar{barX, midY - height / 2, barX + 3, midY + height / 2};
            // 正在播时中间两根亮起来，给一个"确实在走"的动静
            const bool lit = row.voicePlaying && (i == 1 || i == 2);
            HBRUSH brush = CreateSolidBrush(lit ? fg : Adjust(fg, g_darkTheme ? -60 : 70));
            FillRect(dc, &bar, brush);
            DeleteObject(brush);
            barX += 6;
        }

        // 时长 / 状态
        RECT timeRect{barX + 8, row.bubble.top, row.bubble.right - kVoiceBubblePadX,
                      row.bubble.bottom};
        std::string label = row.fileButton;  // 布局时算好的时长文字（"0:07"）
        COLORREF labelColor = fg;
        if (row.fileState == 1) {
            label = "下载中 " + std::to_string(row.fileProgress) + "%";
            labelColor = Adjust(fg, g_darkTheme ? -50 : 60);
        } else if (row.fileState == 3) {
            label = "没下来，点一下重试";
            labelColor = g_palette->error;
        } else if (row.fileState == 0) {
            label = "点一下下载";
            labelColor = Adjust(fg, g_darkTheme ? -50 : 60);
        }
        DrawTextIn(dc, Utf8ToWide(label), timeRect, ui.fontSmall, labelColor,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 正在播：底部一条细进度条。按"播放位置/总时长"算，不是估算
        // ——估出来的进度条走到一半就跳，比没有还糟。
        if (row.voicePlaying && row.voiceTotalSeconds > 0) {
            const int trackTop = row.bubble.bottom - 6;
            RECT track{row.bubble.left + kVoiceBubblePadX, trackTop,
                       row.bubble.right - kVoiceBubblePadX, trackTop + 3};
            HBRUSH trackBrush = CreateSolidBrush(Adjust(fill, g_darkTheme ? 18 : -14));
            FillRect(dc, &track, trackBrush);
            DeleteObject(trackBrush);
            RECT done = track;
            int percent = row.voicePlayedSeconds * 100 / row.voiceTotalSeconds;
            if (percent > 100) percent = 100;
            if (percent < 0) percent = 0;
            done.right = track.left + (track.right - track.left) * percent / 100;
            if (done.right > done.left) {
                HBRUSH doneBrush = CreateSolidBrush(fg);
                FillRect(dc, &done, doneBrush);
                DeleteObject(doneBrush);
            }
        }
        return;
    }
    if (row.isFile) {
        // QQ 式文件卡片：头一行显示发送者，下面是卡片本体 + 右侧按钮
        DrawRowHeader(dc, row);
        ui::FillRoundedRect(dc, row.bubble, kFileCardRadius, g_palette->bubbleOther,
                            g_palette->border, 1.0f);

        // 文件名（第一行）
        RECT nameRect = row.textRect;
        nameRect.bottom = nameRect.top + 22;
        DrawTextIn(dc, Utf8ToWide(row.text), nameRect, ui.font, g_palette->text,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        // 明细（第二行：大小 / 上传者 / 状态）
        RECT detailRect = row.textRect;
        detailRect.top = nameRect.bottom + 2;
        detailRect.bottom = detailRect.top + 18;
        const COLORREF detailColor = row.fileState == 2   ? g_palette->text
                                     : row.fileState == 3 ? g_palette->error
                                                          : g_palette->system;
        DrawTextIn(dc, Utf8ToWide(row.fileDetail), detailRect, ui.fontSmall, detailColor,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        // 下载中：卡片底部一条细进度条
        if (row.fileState == 1) {
            const int barTop = row.bubble.bottom - 10;
            RECT track{row.textRect.left, barTop, row.buttonRect.left - 10, barTop + 4};
            if (track.right > track.left) {
                ui::FillRoundedRect(dc, track, 2, g_palette->border, g_palette->border, 0.0f);
                RECT done = track;
                done.right = track.left +
                             (track.right - track.left) * (row.fileProgress < 0 ? 0 : row.fileProgress) / 100;
                if (done.right > done.left) {
                    ui::FillRoundedRect(dc, done, 2, g_palette->accent, g_palette->accent, 0.0f);
                }
            }
        }

        // 右侧按钮：下载 / 下载中 / 打开文件夹 / 重试
        if (row.preview && row.previewRect.right > row.previewRect.left) {
            // 缩略图：等比缩放铺满小方框，四角用圆角裁剪（GDI+ 画，抗锯齿）
            const HBITMAP handle = static_cast<HBITMAP>(row.preview.get());
            Gdiplus::Bitmap source(handle, nullptr);
            if (source.GetLastStatus() == Gdiplus::Ok) {
                const int boxW = row.previewRect.right - row.previewRect.left;
                const int boxH = row.previewRect.bottom - row.previewRect.top;
                const int srcW = static_cast<int>(source.GetWidth());
                const int srcH = static_cast<int>(source.GetHeight());
                if (srcW > 0 && srcH > 0) {
                    const double scale =
                        (std::min)(static_cast<double>(boxW) / srcW, static_cast<double>(boxH) / srcH);
                    const int drawW = srcW * scale > boxW ? boxW : static_cast<int>(srcW * scale);
                    const int drawH = srcH * scale > boxH ? boxH : static_cast<int>(srcH * scale);
                    Gdiplus::Graphics graphics(dc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    Gdiplus::GraphicsPath path;
                    const int r = 6;
                    const int x = row.previewRect.left, yy = row.previewRect.top;
                    const int w = boxW, h = boxH;
                    path.AddArc(x, yy, r * 2, r * 2, 180, 90);
                    path.AddArc(x + w - r * 2, yy, r * 2, r * 2, 270, 90);
                    path.AddArc(x + w - r * 2, yy + h - r * 2, r * 2, r * 2, 0, 90);
                    path.AddArc(x, yy + h - r * 2, r * 2, r * 2, 90, 90);
                    path.CloseFigure();
                    graphics.SetClip(&path);
                    graphics.DrawImage(&source, Gdiplus::Rect(x + (boxW - drawW) / 2,
                                                              yy + (boxH - drawH) / 2, drawW,
                                                              drawH));
                    graphics.ResetClip();
                }
            }
            ui::OutlineRoundedRect(dc, row.previewRect, 6, g_palette->border);
        }

        const bool actionable = row.fileState != 1;
        const bool hover = actionable && !g_fileHoverId.empty() && g_fileHoverId == row.fileId;
        COLORREF fill = actionable ? g_palette->accent : g_palette->neutral;
        COLORREF border = Adjust(fill, -20);
        COLORREF textColor = actionable ? g_palette->accentText : g_palette->system;
        if (!actionable) {
            textColor = g_palette->time;
        } else if (hover) {
            fill = Adjust(fill, g_darkTheme ? 14 : -10);
            border = Adjust(fill, -20);
        }
        ui::DrawRoundedControl(dc, row.buttonRect, kFileButtonH / 2, g_palette->bubbleOther, fill,
                               border);
        DrawTextIn(dc, Utf8ToWide(row.fileButton), row.buttonRect, ui.font, textColor,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    if (!row.isBubble) {
        COLORREF fill = g_palette->noticeBg;
        COLORREF border = g_palette->border;
        COLORREF color = row.error ? g_palette->error : g_palette->system;
        HFONT font = ui.fontSmall;
        float borderWidth = 1.0f;
        if (row.announcement) {  // 全服公告：大字、特殊颜色、加粗边框
            fill = g_palette->announceBg;
            border = g_palette->announceBorder;
            color = g_palette->announceText;
            font = ui.fontLarge;
            borderWidth = 2.0f;
        }
        ui::FillRoundedRect(dc, row.bubble, row.announcement ? 14 : 10, fill, border, borderWidth);
        DrawTextIn(dc, Utf8ToWide(row.text), row.textRect, font, color,
                   DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        return;
    }

    DrawRowHeader(dc, row);

    COLORREF fill = row.own ? g_palette->bubbleOwn : g_palette->bubbleOther;
    COLORREF border = row.own ? g_palette->bubbleOwnBorder : g_palette->bubbleOtherBorder;
    COLORREF bodyColor = row.own ? g_palette->bubbleOwnText : g_palette->text;
    float borderWidth = 1.0f;
    if (!g_colorEnabled) {  // 关闭彩色：所有气泡统一
        fill = row.own ? g_palette->neutral : g_palette->bubbleOther;
        border = g_palette->border;
        bodyColor = g_palette->text;
    } else if (row.mention) {  // 有人 @我 或 @all：边框与文字用醒目色
        border = g_palette->mention;
        bodyColor = g_palette->mention;
        borderWidth = 2.0f;
    }
    ui::FillRoundedRect(dc, row.bubble, 12, fill, border, borderWidth);
    // 正文一段一段画（每段一个颜色）。默认色用气泡自己的文字色——
    // 关掉彩色聊天或服务器禁用时，ParseChatColors 会把色码原样留在文字里，
    // 而且这时候不该再用用户的颜色（统一成气泡文字色）。
    const bool colorsOn = g_chatColorEnabled;
    const dchat::ColorLayout& layout = row.colored;
    int lineTop = row.textRect.top;
    for (const dchat::ColorLine& line : layout.lines) {
        for (const dchat::ColoredSpan& span : line.spans) {
            if (span.text.empty()) continue;
            const COLORREF color =
                (colorsOn && span.color != kAutoColor) ? span.color : bodyColor;
            RECT spanRect{row.textRect.left + span.x, lineTop, row.textRect.right,
                          lineTop + layout.lineHeight};
            DrawTextIn(dc, span.text, spanRect, ui.font, color,
                       DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        lineTop += layout.lineHeight;
    }
}

void ViewScrollToBottom(HWND view) {
    RECT client{};
    GetClientRect(view, &client);
    const int viewHeight = client.bottom - client.top;
    g_scrollTop = g_contentHeight > viewHeight ? g_contentHeight - viewHeight : 0;
}

void ViewUpdateScrollbar(HWND view) {
    RECT client{};
    GetClientRect(view, &client);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = g_contentHeight > 0 ? g_contentHeight - 1 : 0;
    info.nPage = static_cast<UINT>(client.bottom - client.top);
    info.nPos = g_scrollTop;
    SetScrollInfo(view, SB_VERT, &info, TRUE);
}

void DrawView(HWND view, HDC target) {
    RECT client{};
    GetClientRect(view, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    // 语音播完了：把"正在播"收回去，否则气泡上会一直显示暂停键。
    // 判断放在这里是因为记录区**本来就是每秒重画的**（进度条在走），
    // 不用再额外开一个定时器——多一个定时器就多一处要记得销毁的东西。
    if (!g_playingVoiceId.empty() && g_voicePlayer.ReachedEnd()) {
        g_voicePlayer.Stop();
        g_playingVoiceId.clear();
        g_playingVoiceTotal = 0;
    }

    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    FillRect(memory, &client,
             g_windowBrush ? g_windowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    // 记录区自己画一圈圆角边框（窗口区域已被裁成圆角，边框会正好贴合）
    RECT border{0, 0, width - 1, height - 1};
    ui::OutlineRoundedRect(memory, border, 8, g_palette->border);

    int contentHeight = 0;
    const std::vector<BubbleRow> rows = LayoutRows(memory, width, &contentHeight);
    g_contentHeight = contentHeight;
    if (g_stickToBottom) ViewScrollToBottom(view);
    const int maxScroll = contentHeight > height ? contentHeight - height : 0;
    if (g_scrollTop > maxScroll) g_scrollTop = maxScroll;
    if (g_scrollTop < 0) g_scrollTop = 0;
    ViewUpdateScrollbar(view);

    const int offset = -g_scrollTop;
    for (const BubbleRow& row : rows) {
        if (row.bubble.bottom + offset < 0) continue;
        if (row.bubble.top + offset > height) break;
        BubbleRow moved = row;
        OffsetRect(&moved.bubble, 0, offset);
        OffsetRect(&moved.textRect, 0, offset);
        OffsetRect(&moved.header, 0, offset);
        DrawRow(memory, moved);
    }

    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

// 在内容坐标系里找文件卡片（整张卡片都可点），返回 Item 下标；找不到返回 -1
int HitTestFileCard(int contentX, int contentY, std::string* fileId, bool* onPreview = nullptr,
                    bool* isVoice = nullptr) {
    if (!ui.hView) return -1;
    RECT client{};
    GetClientRect(ui.hView, &client);
    const int width = client.right - client.left;
    if (width <= 0) return -1;
    HDC dc = GetDC(ui.hView);
    int contentHeight = 0;
    const std::vector<BubbleRow> rows = LayoutRows(dc, width, &contentHeight);
    ReleaseDC(ui.hView, dc);
    for (const BubbleRow& row : rows) {
        if (!row.isFile) continue;
        if (contentX >= row.bubble.left && contentX < row.bubble.right &&
            contentY >= row.bubble.top && contentY < row.bubble.bottom) {
            if (fileId) *fileId = row.fileId;
            if (isVoice) *isVoice = row.isVoice;
            if (onPreview) {
                // 语音气泡整块都是"点一下播/暂停"，没有"预览区"这一说
                *onPreview = !row.isVoice && row.preview && contentX >= row.previewRect.left &&
                             contentX < row.previewRect.right &&
                             contentY >= row.previewRect.top &&
                             contentY < row.previewRect.bottom;
            }
            return static_cast<int>(row.itemIndex);
        }
    }
    return -1;
}

LRESULT CALLBACK ViewProc(HWND view, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            // 点到文件卡片（整张卡片都可点，和 QQ 一样）：交给主窗口去下载 / 打开文件夹。
            // 语音气泡也一样可点，只是点了是播放 / 暂停。
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp) + g_scrollTop;  // 屏幕坐标 -> 内容坐标
            std::string fileId;
            bool onPreview = false;
            bool isVoice = false;
            if (HitTestFileCard(x, y, &fileId, &onPreview, &isVoice) >= 0 && !fileId.empty()) {
                auto* payload = new std::string(fileId);
                UINT message = WM_APP_FILECLICK;
                if (isVoice) {
                    // 语音：整块都是播放键，没有"预览区"这一说
                    message = WM_APP_VOICECLICK;
                } else if (onPreview) {
                    // 点在缩略图上 = 用默认程序打开文件
                    message = WM_APP_FILEOPEN;
                }
                if (!PostMessageW(ui.hwnd, message, 0, reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp) + g_scrollTop;
            std::string fileId;
            const std::string next =
                HitTestFileCard(x, y, &fileId) >= 0 ? fileId : std::string();
            if (next != g_fileHoverId) {
                g_fileHoverId = next;
                InvalidateRect(view, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = view;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!g_fileHoverId.empty()) {
                g_fileHoverId.clear();
                InvalidateRect(view, nullptr, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(view, &ps);
            DrawView(view, dc);
            EndPaint(view, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(view, SB_VERT, &info);
            int position = info.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP:
                    position -= 40;
                    break;
                case SB_LINEDOWN:
                    position += 40;
                    break;
                case SB_PAGEUP:
                    position -= static_cast<int>(info.nPage);
                    break;
                case SB_PAGEDOWN:
                    position += static_cast<int>(info.nPage);
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    position = info.nTrackPos;
                    break;
                default:
                    break;
            }
            const int maxScroll =
                static_cast<int>(info.nMax) - static_cast<int>(info.nPage) + 1;
            if (position > maxScroll) position = maxScroll;
            if (position < 0) position = 0;
            g_scrollTop = position;
            g_stickToBottom = position >= maxScroll;
            InvalidateRect(view, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollTop -= (delta / WHEEL_DELTA) * 60;
            RECT client{};
            GetClientRect(view, &client);
            const int limit = g_contentHeight - (client.bottom - client.top);
            if (limit <= 0) {
                g_scrollTop = 0;
            } else {
                if (g_scrollTop > limit) g_scrollTop = limit;
                if (g_scrollTop < 0) g_scrollTop = 0;
            }
            g_stickToBottom = limit <= 0 || g_scrollTop >= limit;
            InvalidateRect(view, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(view, msg, wp, lp);
}

void ViewAddItem(ItemKind kind, const std::string& raw, const std::string& time) {
    Item item;
    item.kind = kind;
    item.raw = raw;
    item.time = time;
    g_items.push_back(item);
    while (g_items.size() > kMaxItems) g_items.pop_front();
    if (g_stickToBottom) g_scrollTop = 1 << 30;  // 绘制时会夹到底部
    if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
}

// ---------------- 多会话：切换、落消息、本地设置落盘 ----------------

/** 把某条消息追加到**指定**会话（不一定是当前显示的那个）。 */
void AppendToSession(int index, const Item& item) {
    if (index < 0 || index >= static_cast<int>(g_sessions.size())) return;

    // ⚠️ 当前会话的"活"副本是 **g_items**，不是 session.items —— 这是"换内容不换壳"
    // 那条设计的核心不变量：g_items 永远是当前会话的内容，session.items 只对
    // **非当前**会话有效。
    //
    // 往 session.items 里塞当前会话的消息会同时踩两个坑：
    //   ① 界面读的是 g_items，所以消息永远不显示；
    //   ② 下次切走时 SwitchSession 会用 g_items **覆盖** session.items，
    //      那些消息就彻底没了。
    // 真机上的症状就是"服务器日志明明收到了 MSG，两个客户端界面里一条都没有"。
    if (index == g_currentSession) {
        g_items.push_back(item);
        while (g_items.size() > kMaxItems) g_items.pop_front();
        if (g_stickToBottom) g_scrollTop = 1 << 30;  // 绘制时会夹到底部
        if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
        return;
    }

    Session& session = g_sessions[index];
    session.items.push_back(item);
    while (session.items.size() > kMaxItems) session.items.pop_front();
}

/**
 * 切换会话。
 *
 * **必须把 g_items / 滚动位置对调**，而不是清空重填：清空重填会让每次切回来
 * 都重排一遍几千条气泡（卡顿可见），而对调只是换两个 deque 的指针。
 */
void SwitchSession(int index) {
    if (index < 0 || index >= static_cast<int>(g_sessions.size())) return;
    if (index == g_currentSession && !g_items.empty()) {
        g_sessions[index].unread = 0;
        g_sessions[index].unreadImportant = false;
        return;
    }
    // 1) 把当前显示的内容与滚动位置存回它所属的会话
    if (!g_sessions.empty() && g_currentSession >= 0 &&
        g_currentSession < static_cast<int>(g_sessions.size())) {
        Session& old = g_sessions[g_currentSession];
        old.items = std::move(g_items);
        old.scrollTop = g_scrollTop;
        old.stickToBottom = g_stickToBottom;
        // 草稿也是每个会话各自一份
        if (ui.hInput) {
            const int length = GetWindowTextLengthW(ui.hInput);
            std::wstring text(static_cast<std::size_t>(length), L'\0');
            if (length > 0) GetWindowTextW(ui.hInput, &text[0], length + 1);
            old.draft = text;
        }
    }
    // 2) 换成新会话的内容
    g_currentSession = index;
    Session& next = g_sessions[index];
    g_items = std::move(next.items);
    g_scrollTop = next.stickToBottom ? (1 << 30) : next.scrollTop;
    g_stickToBottom = true;  // 切过去先看最新的一条，符合直觉
    next.unread = 0;
    next.unreadImportant = false;
    if (ui.hInput) SetWindowTextW(ui.hInput, next.draft.c_str());
    // 3) 告诉服务端"我在看这个会话了"，顺便拉一次它的最新信息与历史
    if (IsConnected() && g_authed && g_roomsEnabled) {
        SendRawLine(dchat::BuildLine("ROOM", index >= 0 ? g_sessions[index].id : std::string()));
    }
    if (ui.hView) {
        InvalidateRect(ui.hView, nullptr, TRUE);
        ViewUpdateScrollbar(ui.hView);
    }
    if (ui.hwnd) InvalidateRect(ui.hwnd, nullptr, FALSE);
    ClearUnread();
    UpdateStatus();
}

/** 本地设置落盘。写失败只影响"下次启动记住没记住"，不该打断任何操作。 */
void SaveRoomPrefs() {
    std::string text;
    text += "# dchat 会话本地设置（免打扰 / 置顶 / 本地缓存）。\n";
    text += "# **只存在这台机器上，永远不上传服务器** —— 群管理员看不到你的免打扰。\n";
    for (const Session& session : g_sessions) {
        if (session.id.empty()) continue;
        text += dchat::SerializePrefs(session.id, session.prefs) + "\n";
    }
    std::ofstream out(kRoomPrefsPath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << text;
}

void LoadRoomPrefs() {
    std::ifstream in(kRoomPrefsPath, std::ios::binary);
    if (!in) return;  // 还没有这个文件：所有人用默认值，不是错误
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto& pair : dchat::ParsePrefsFile(text)) {
        const int index = FindSession(pair.first);
        if (index >= 0) {
            g_sessions[index].prefs = pair.second;
        } else {
            // 会话还没出现（比如服务端还没把列表发过来）：先存着，等它出现时套上。
            // 直接丢掉的话，用户重启客户端后"免打扰"会莫名失效一次。
            Session placeholder;
            placeholder.id = pair.first;
            placeholder.kind = "group";
            placeholder.title = pair.first;
            placeholder.prefs = pair.second;
            g_sessions.push_back(std::move(placeholder));
        }
    }
}

int HallSessionIndex() {
    int index = FindSession(dchat::kHallId);
    if (index < 0) index = EnsureSession(dchat::kHallId, "hall");
    return index;
}

/**
 * 把一条记录投递到某个会话，并决定**要不要提示**。
 *
 * 提示规则全部来自本地设置（免打扰 / 重要提醒 / 置顶），服务端不知道也不该知道 ——
 * 这就是"成员设置永不上传"那条分界线在客户端这一侧的落点。
 *
 * @param important 是不是"重要消息"（@我 / @全体成员）
 */
void DeliverToSession(int index, const Item& item, bool important) {
    if (index < 0 || index >= static_cast<int>(g_sessions.size())) return;
    Session& session = g_sessions[index];
    const bool isCurrent = (index == g_currentSession);
    const bool active = WindowIsActive();

    AppendToSession(index, item);

    if (!isCurrent) {
        // 未读**照常计数**，哪怕是免打扰的会话。
        // 免打扰的意思是"别打扰我"（不闪任务栏、不出提示音、不计进标题的未读数），
        // 不是"有新消息也别让我知道"。完全不计数的话，用户会以为这个群死了 ——
        // 而他其实只是把它静音了。
        ++session.unread;
        if (important) session.unreadImportant = true;
        RefreshSessionPanel();
    }

    // 免打扰：不提示。但"重要提醒仍然提醒"打开时，@我 / @全体 照样提示 ——
    // 这正是那个开关存在的意义：把群静音，但别漏掉叫我的那一条。
    const bool notify = !session.prefs.muted || (important && session.prefs.alertImportant);
    if (!notify) return;
    // "我正在看这个会话、而且窗口在前台"才算真的看到了 —— 后台窗口不提示的话，
    // 用户会漏掉消息却以为客户端坏了
    const bool reallyWatching = active && isCurrent;
    if (dchat::ShouldCountUnread(reallyWatching)) {
        ++g_unread;
        UpdateTitle();
    }
    if (dchat::ShouldFlash(reallyWatching, important)) FlashTaskbar();
}

/**
 * 把一条聊天类协议行送进它该去的会话。返回 false = 这不是聊天类行，调用方自己处理。
 *
 * **实时消息和历史回放（RHIST 里裹着的原始行）共用这一个函数**：
 * 分两条路径迟早会不一致，而"历史里显示不对、实时是对的"这种 bug 最难查。
 */
bool RenderChatLine(const std::string& line) {
    const dchat::Message msg = dchat::ParseLine(line);
    std::vector<std::string> fields = msg.Words();
    std::string time;
    if (!fields.empty() && dchat::LooksLikeTime(fields[0])) {
        time = fields.front();
        fields.erase(fields.begin());
    }

    if (msg.command == "SAY") {
        // 大厅消息：老格式、没有房间号 —— 按协议约定它**永远属于大厅**。
        // 新客户端靠这一条把老格式接进多会话界面，服务端因此一个字都不用改。
        Item item;
        item.kind = ItemKind::Say;
        item.raw = line;
        DeliverToSession(HallSessionIndex(), item, dchat::MentionsMe(line, g_nick));
        return true;
    }

    if (msg.command == "RMSG") {
        if (fields.size() < 2) return true;
        const std::string roomId = fields[0];
        // 记录区里的 Say 条目一律按 **SAY** 格式存：`RMSG` 只是"这条属于哪个会话"的
        // 标注，而那个信息在**投递的时候**就已经用掉了（存进了对应的会话）。
        // 把 RMSG 原样存进去的话，绘制层的 ParseSay 不认这个命令、整条消息会画不出来。
        std::string sayLine = "SAY";
        if (!time.empty()) sayLine += " " + time;
        for (std::size_t i = 1; i < fields.size(); ++i) sayLine += " " + fields[i];
        Item item;
        item.kind = ItemKind::Say;
        item.raw = sayLine;
        DeliverToSession(EnsureSession(roomId, "group"), item, dchat::MentionsMe(line, g_nick));
        return true;
    }

    if (msg.command == "REVENT") {
        if (fields.size() < 2) return true;
        const std::string roomId = fields[0];
        std::string text = fields[1];
        for (std::size_t i = 2; i < fields.size(); ++i) text += " " + fields[i];
        Item item;
        item.kind = ItemKind::Notice;
        item.raw = text;
        item.time = time.empty() ? dchat::NowTimeString() : time;
        DeliverToSession(EnsureSession(roomId, "group"), item, false);
        return true;
    }

    if (msg.command == "ANNOUNCE") {
        const dchat::NoticeInfo notice = dchat::ParseNotice(line);
        Item item;
        item.kind = ItemKind::Announce;
        item.raw = notice.text;
        item.time = notice.time;
        // 全服公告进**大厅**：它不属于任何群，塞进当前会话会让"这条公告是哪个群发的"
        // 变成一个没人答得上来的问题。同时照常提示，免得用户待在群里就看不到。
        DeliverToSession(HallSessionIndex(), item, true);
        return true;
    }
    return false;
}

/** 让左侧会话列表重画（会话增删、未读数、免打扰变了都要调）。 */
void RefreshSessionPanel() {
    // 只 InvalidateRect 是不够的：左栏的**顺序和几何**（g_sessionOrder /
    // g_sessionLayout）是在 LayoutWindowChrome 里算的，而它平时只在 WM_SIZE 时跑。
    // 结果就是"置顶一个会话"点了之后行不会挪到最上面，要等下次改窗口大小才重排
    // （自动化联调实测抓到的）。会话数量 / 顺序 / 未读 / 免打扰变了都得走这条路。
    if (ui.hwnd == nullptr) return;
    Layout(ui.hwnd);
    InvalidateRect(ui.hwnd, nullptr, FALSE);
}

// ---------------- 未读提示 ----------------
bool WindowIsActive() { return ui.hwnd != nullptr && GetForegroundWindow() == ui.hwnd; }

void UpdateTitle() {
    std::string title = dchat::FormatUnreadTitle(WideToUtf8(kWindowTitle), g_unread);
    // 自动化冒烟用：把界面内部状态写进标题，脚本读标题就能验证命中判断，
    // 不用去猜像素（屏幕抓图在 DWM 下会拿到旧内容，实测完全不可靠）
    SetWindowTextW(ui.hwnd, Utf8ToWide(title).c_str());
}

void ClearUnread() {
    if (g_unread == 0) return;
    g_unread = 0;
    UpdateTitle();
}

void FlashTaskbar() {
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = ui.hwnd;
    info.dwFlags = FLASHW_TRAY;
    info.uCount = 3;
    FlashWindowEx(&info);
}

void MarkMessageArrived(bool mention) {
    const bool active = WindowIsActive();
    if (dchat::ShouldCountUnread(active)) {
        ++g_unread;
        UpdateTitle();
    }
    if (dchat::ShouldFlash(active, mention)) FlashTaskbar();
}

// ---------------- 自绘圆角按钮 ----------------
enum class ButtonKind { Neutral, Accent, Toggle, ToggleOn };

ButtonKind KindOf(int id) {
    if (id == IDD_OK) return ButtonKind::Accent;  // 登录对话框的主操作按钮
    switch (id) {
        case IDC_SEND:
            return ButtonKind::Accent;
        case IDC_COLOR:
            return g_colorEnabled ? ButtonKind::ToggleOn : ButtonKind::Toggle;
        default:
            return ButtonKind::Neutral;
    }
}

constexpr const wchar_t* kSettingsClass = L"DchatSettingsDlg";  // 设置窗口类名（下面判断底色要用）

// 自绘控件"圆角外面那一圈"该涂什么色：**必须和控件所在窗口的底色一致**。
// 以前这里写死 g_palette->windowBg：主窗口的底色确实是它，但设置窗口底色是
// g_palette->panel（更亮），于是「取消 / 完成」圆角外被涂成暗色方块，
// 看起来就是"边缘不平滑、有方框"。
COLORREF ParentBackgroundOf(HWND control) {
    const HWND parent = control ? GetParent(control) : nullptr;
    if (!parent) return g_palette->windowBg;
    wchar_t cls[128] = {0};
    GetClassNameW(parent, cls, 128);
    if (wcscmp(cls, kSettingsClass) == 0) return g_palette->panel;
    return g_palette->windowBg;
}

void DrawOwnerButton(const DRAWITEMSTRUCT* item) {
    if (!item) return;
    const RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hover = g_hover[item->hwndItem];

    COLORREF fill = g_palette->neutral;
    COLORREF border = g_palette->neutralBorder;
    COLORREF textColor = g_palette->text;
    const ButtonKind kind = KindOf(static_cast<int>(item->CtlID));
    if (kind == ButtonKind::Accent || kind == ButtonKind::ToggleOn) {
        fill = g_palette->accent;
        border = Adjust(fill, -20);
        textColor = g_palette->accentText;
    }
    if (disabled) {
        fill = Adjust(fill, g_darkTheme ? -8 : 10);
        textColor = Adjust(textColor, g_darkTheme ? -80 : 90);
    } else if (pressed) {
        fill = Adjust(fill, -18);
    } else if (hover) {
        fill = Adjust(fill, g_darkTheme ? 14 : -10);
    }

    HDC dc = item->hDC;
    const int radius = (rect.bottom - rect.top) / 2;
    ui::DrawRoundedControl(dc, rect, radius, g_palette->windowBg, fill, border,
                           ParentBackgroundOf(item->hwndItem));

    wchar_t label[64] = {0};
    GetWindowTextW(item->hwndItem, label, 64);
    HGDIOBJ oldFont = SelectObject(dc, ui.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    RECT textRect = rect;
    DrawTextW(dc, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

// 自绘的勾选框（登录窗口的「显示密码」用）：
// 左边一个圆角小方块（勾上时填强调色 + 白色对勾），右边是文字。
// 用系统勾选框的话在深色主题下是一块浅色，和整体风格不搭。
void DrawOwnerCheckbox(const DRAWITEMSTRUCT* item, bool checked, HFONT font) {
    if (!item) return;
    const RECT rect = item->rcItem;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hover = g_hover[item->hwndItem];
    HDC dc = item->hDC;

    // 先把整块区域铺成窗口底色：自绘控件不铺满的话，四周会残留控件自己的
    // 白色底（和圆角按钮那边同一个道理）
    HBRUSH background = CreateSolidBrush(g_palette->windowBg);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    const int box = 16;
    const RECT boxRect{rect.left, rect.top + ((rect.bottom - rect.top) - box) / 2,
                       rect.left + box, rect.top + ((rect.bottom - rect.top) - box) / 2 + box};
    COLORREF fill = checked ? g_palette->accent : g_palette->bubbleOther;
    COLORREF border = checked ? Adjust(g_palette->accent, -20) : g_palette->border;
    if (disabled) {
        fill = Adjust(fill, g_darkTheme ? -8 : 10);
    } else if (hover && !checked) {
        fill = Adjust(fill, g_darkTheme ? 14 : -10);
    }
    ui::DrawRoundedControl(dc, boxRect, 4, g_palette->windowBg, fill, border);

    if (checked) {  // 白色对勾（两段线，比画字符更清晰）
        HPEN pen = CreatePen(PS_SOLID, 2, g_palette->accentText);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, boxRect.left + 4, boxRect.top + 8, nullptr);
        LineTo(dc, boxRect.left + 7, boxRect.top + 11);
        LineTo(dc, boxRect.left + 12, boxRect.top + 5);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    wchar_t label[64] = {0};
    GetWindowTextW(item->hwndItem, label, 64);
    const RECT textRect{boxRect.right + 8, rect.top, rect.right, rect.bottom};
    DrawTextIn(dc, label, textRect, font,
               disabled ? g_palette->time : g_palette->system,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// 子类化控件时，**按窗口**记住它原来的窗口过程。
//
// 以前这里是一个全局 `WNDPROC g_oldButtonProc`，所有按钮共用：
//   - 每个按钮都往里覆盖写，谁都可能拿到别人的（甚至拿到 ButtonProc 自己，那就死递归）；
//   - 更糟的是**漏写一次就全都不画**——改版时我把保存那一步弄丢了，结果是
//     `CallWindowProcW(nullptr, ...)` 什么都不做：所有按钮（连接、登录、设置里的
//     完成/取消）全部不显示，而且因为没画完，窗口一直处于"待重画"状态空转。
// 换成按窗口记一条，SubclassControl 负责捕获旧过程，谁也不会串。
std::map<HWND, WNDPROC> g_originalProc;

void SubclassControl(HWND control, WNDPROC replacement) {
    if (!control) return;
    const WNDPROC previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(replacement)));
    g_originalProc[control] = previous;
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!g_hover[hwnd]) {
                g_hover[hwnd] = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            break;
        }
        case WM_MOUSELEAVE:
            g_hover[hwnd] = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WM_NCDESTROY: {
            const auto it = g_originalProc.find(hwnd);
            const WNDPROC previous = it == g_originalProc.end() ? nullptr : it->second;
            if (it != g_originalProc.end()) g_originalProc.erase(it);
            g_hover.erase(hwnd);
            return previous ? CallWindowProcW(previous, hwnd, msg, wp, lp)
                            : DefWindowProcW(hwnd, msg, wp, lp);
        }
        default:
            break;
    }
    const auto it = g_originalProc.find(hwnd);
    if (it == g_originalProc.end() || !it->second) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(it->second, hwnd, msg, wp, lp);
}

// ---------------- 主窗口外壳 ----------------

bool IsConnected();  // 定义在下面（外壳要用它决定显示"连接"还是"断开"）

// 画一行文字（外壳上所有文字都走这里，省得每次写 SelectObject/SetTextColor）
void TextOutUtf8(HDC dc, const std::wstring& text, const RECT& rect, HFONT font, COLORREF color,
                 UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    RECT target = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

// 菜单栏右侧那颗小胶囊上的字：一眼看出连没连上、是谁
std::string StatusChipText() {
    if (!IsConnected()) return "○ 未连接";
    if (g_authed) return "● " + g_host + "　" + g_nick;
    return "● " + g_host + "　未登录";
}

// 状态条上的只读文字（连接 · 在线人数 · 传输进度 · 录音计时）
std::string BuildStatusLine() {
    if (!IsConnected()) return "未连接——点菜单里的「连接」填服务器地址，连上后再登录或注册";
    std::string text = "已连接 " + g_host + ":" + std::to_string(g_port);
    text += g_authed ? ("　·　用户 " + g_nick) : "　·　未登录";
    if (!g_onlineNicks.empty()) text += "　·　在线 " + std::to_string(g_onlineNicks.size()) + " 人";
    if (!g_transferStatus.empty()) text += "　·　" + g_transferStatus;
    // 录音中：计时放这里，一眼能看到录了多久、还差多少
    if (g_recorder.IsRunning()) {
        const int seconds = g_recorder.ElapsedSeconds();
        text += "　·　● 录音中 " + dchat::FormatDuration(seconds) + "（最长 " +
                dchat::FormatDuration(g_voiceLimitSeconds) + "，到点自动发送）";
    }
    return text;
}

// 只重画菜单栏与底部（聊天记录不动：那里每次重画都要重新排版全部气泡）
void InvalidateChrome(HWND hwnd) {
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    RECT top{0, 0, rc.right, dchat::kMenuBarHeight + 1};
    RECT bottom{0, g_bottom.statusBar.top - 1, rc.right, rc.bottom};
    InvalidateRect(hwnd, &top, FALSE);
    InvalidateRect(hwnd, &bottom, FALSE);
}

void LayoutWindowChrome(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC dc = GetDC(hwnd);  // 量文字宽度用；三次布局共用，最后统一释放
    g_menuItems = dchat::LayoutMenuItems(dc, ui.font, rc.right, IsConnected());
    g_gearRect = dchat::GearRect(rc.right);
    g_chipRect = dchat::StatusChipRect(dc, ui.fontSmall, rc.right, Utf8ToWide(StatusChipText()));
    g_contentTop = dchat::kMenuBarHeight;
    g_bottom = dchat::LayoutBottom(dc, ui.font, rc.right, rc.bottom, g_contentTop);
    ReleaseDC(hwnd, dc);

    // ---- 左侧会话列表 ----
    // 只有服务端确认过 rooms1 才显示。对面是旧版服务端时整个左栏不出现，
    // 界面和升级前一模一样 —— 这是"老服务端 + 新客户端"也要能用。
    g_sessionOrder.clear();
    g_sessionLayout = dchat::SessionListLayout();
    if (g_roomsEnabled && !g_sessions.empty()) {
        g_sessionOrder = SessionDisplayOrder();
        std::vector<dchat::SessionRowInput> rows;
        rows.reserve(g_sessionOrder.size());
        for (int index : g_sessionOrder) {
            const Session& session = g_sessions[index];
            dchat::SessionRowInput row;
            row.title = Utf8ToWide(session.title.empty() ? session.id : session.title);
            row.subtitle = Utf8ToWide(SessionSubtitle(session));
            row.unread = session.unread;
            row.muted = session.prefs.muted;
            row.pinned = session.prefs.pinned;
            row.important = session.unreadImportant;
            rows.push_back(row);
        }
        const int listTop = dchat::kMenuBarHeight;
        const int listHeight = g_bottom.statusBar.top - listTop;
        g_sessionLayout = dchat::LayoutSessionList(listTop, listHeight, g_sessionScroll, rows);
    }
}

void DrawWindowChrome(HWND hwnd, HDC dc, const RECT& client) {
    (void)hwnd;  // 外壳完全按 client 矩形画，不需要窗口句柄（签名保持一致，方便以后用）
    const int width = client.right;

    // ---- 菜单栏 ----
    HBRUSH panelBrush = CreateSolidBrush(g_palette->panel);
    RECT menuBar{0, 0, width, dchat::kMenuBarHeight};
    FillRect(dc, &menuBar, panelBrush);
    DeleteObject(panelBrush);
    HBRUSH borderBrush = CreateSolidBrush(g_palette->border);
    RECT menuLine{0, dchat::kMenuBarHeight - 1, width, dchat::kMenuBarHeight};
    FillRect(dc, &menuLine, borderBrush);
    DeleteObject(borderBrush);

    // 菜单项（"连接/断开"跟着状态变文字，不再像以前那样两个按钮都摆着、
    // 还只有一个能用）
    for (std::size_t i = 0; i < g_menuItems.size(); ++i) {
        const dchat::MenuItem& item = g_menuItems[i];
        if (static_cast<int>(i) == g_menuHover) {
            ui::FillRoundedRect(dc, item.rect, 6, g_palette->hover, g_palette->hover, 0.0f);
        }
        TextOutUtf8(dc, item.label, item.rect, ui.font, g_palette->text,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 右侧：连接状态胶囊 + 齿轮
    if (g_chipRect.right > g_chipRect.left) {
        const COLORREF chipFill = g_chipHover ? g_palette->hover : g_palette->noticeBg;
        ui::FillRoundedRect(dc, g_chipRect, dchat::kStatusChipHeight / 2, chipFill,
                            g_palette->border, 1.0f);
        TextOutUtf8(dc, Utf8ToWide(StatusChipText()), g_chipRect, ui.fontSmall,
                    IsConnected() ? g_palette->text : g_palette->system,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (g_gearHover) {
        ui::FillRoundedRect(dc, g_gearRect, 6, g_palette->hover, g_palette->hover, 0.0f);
    }
    // 齿轮用 **Windows 自带的图标字体**（Segoe MDL2 Assets）：手画的那个缩到 16px 就糊成
    // 一团，看着很廉价。取不到字体时 DrawGlyph 返回 false，自动退回手画版本。
    if (!dchat::DrawGlyph(dc, g_gearRect, dchat::kGlyphGear, 20, g_palette->text)) {
        dchat::DrawGearIcon(dc, (g_gearRect.left + g_gearRect.right) / 2,
                            (g_gearRect.top + g_gearRect.bottom) / 2, 8, g_palette->text);
    }

    // ---- 左侧会话列表 ----
    //
    // 会话行只画**会话名 + 一行小字 + 未读气泡 + 免打扰/置顶标记**，不做头像图：
    // 没有用户头像这套东西，画一个假头像只会让人觉得"这里应该有图但没加载出来"。
    if (g_roomsEnabled && g_sessionLayout.valid) {
        const RECT& panel = g_sessionLayout.panel;
        HBRUSH panelBrush = CreateSolidBrush(g_palette->panel);
        FillRect(dc, &panel, panelBrush);
        DeleteObject(panelBrush);
        HBRUSH splitBrush = CreateSolidBrush(g_palette->border);
        RECT split{panel.right - 1, panel.top, panel.right, panel.bottom};
        FillRect(dc, &split, splitBrush);
        DeleteObject(splitBrush);

        TextOutUtf8(dc, L"会话", g_sessionLayout.title, ui.fontSmall, g_palette->system,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 表头两个按钮。**几何层早就把它们算好了、点击也接上了，但之前忘了画** ——
        // 症状就是"界面上根本看不到入口"，而代码看起来是完整的（有矩形、有命中测试、
        // 有点击处理）。这类"算得到但画不出"的问题只有真人看界面才发现得了。
        {
            const bool searchHot = (g_sessionHover == -2);   // -2 = 悬停搜索
            const bool newHot = (g_sessionHover == -3);      // -3 = 悬停新建
            const RECT& search = g_sessionLayout.searchButton;
            const RECT& create = g_sessionLayout.newButton;
            if (searchHot) {
                ui::FillRoundedRect(dc, search, 6, g_palette->hover, g_palette->hover, 0.0f);
            }
            if (newHot) {
                ui::FillRoundedRect(dc, create, 6, g_palette->hover, g_palette->hover, 0.0f);
            }
            // 图标字体取不到时退回文字，绝不能变成一个空格子
            if (!dchat::DrawGlyph(dc, search, dchat::kGlyphSearch, 16, g_palette->text)) {
                TextOutUtf8(dc, L"搜", search, ui.fontSmall, g_palette->text,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (!dchat::DrawGlyph(dc, create, dchat::kGlyphAdd, 16, g_palette->text)) {
                TextOutUtf8(dc, L"＋", create, ui.fontSmall, g_palette->text,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        for (const dchat::SessionRowLayout& row : g_sessionLayout.rows) {
            if (!row.valid) continue;
            const bool active = (row.index >= 0 &&
                                 row.index < static_cast<int>(g_sessionOrder.size()) &&
                                 g_sessionOrder[row.index] == g_currentSession);
            if (active) {
                ui::FillRoundedRect(dc, row.row, 0, g_palette->hover, g_palette->hover, 0.0f);
            } else if (row.index == g_sessionHover) {
                ui::FillRoundedRect(dc, row.row, 0, g_palette->noticeBg, g_palette->noticeBg, 0.0f);
            }
            TextOutUtf8(dc, L"", row.avatar, ui.font, g_palette->text, DT_LEFT);  // 占位，保持矩形一致
            const std::wstring title = [&] {
                if (row.index < 0 || row.index >= static_cast<int>(g_sessionOrder.size())) {
                    return std::wstring();
                }
                const Session& session = g_sessions[g_sessionOrder[row.index]];
                return Utf8ToWide(session.title.empty() ? session.id : session.title);
            }();
            const int titleColor = active ? g_palette->text : g_palette->text;
            TextOutUtf8(dc, title, row.title, ui.font, titleColor,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (row.index >= 0 && row.index < static_cast<int>(g_sessionOrder.size())) {
                const Session& session = g_sessions[g_sessionOrder[row.index]];
                TextOutUtf8(dc, Utf8ToWide(SessionSubtitle(session)), row.subtitle, ui.fontSmall,
                            g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (row.badge.right > row.badge.left) {
                const Session& session = g_sessions[g_sessionOrder[row.index]];
                if (session.prefs.muted) {
                    // 免打扰的会话只画一个小圆点，不显示数字 ——
                    // 数字会让人下意识去"清零"，而用户设免打扰正是为了不用管它。
                    // 但不能什么都不画：那样群看起来像死了。
                    // 有重要消息（@我 / @全体）时点用强调色，普通消息用灰色。
                    const int dot = 8;
                    const int centerY = (row.badge.top + row.badge.bottom) / 2;
                    RECT mark{row.badge.right - dot, centerY - dot / 2, row.badge.right,
                              centerY + dot / 2};
                    const COLORREF dotColor =
                        session.unreadImportant ? g_palette->accent : g_palette->system;
                    ui::FillRoundedRect(dc, mark, dot / 2, dotColor, dotColor, 0.0f);
                } else {
                    ui::FillRoundedRect(dc, row.badge, dchat::kSessionBadgeMin / 2,
                                        g_palette->accent, g_palette->accent, 0.0f);
                    const int unread = session.unread;
                    TextOutUtf8(dc, std::to_wstring(unread > 99 ? 99 : unread), row.badge,
                                ui.fontSmall, RGB(255, 255, 255),
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            if (row.muteMark.right > row.muteMark.left) {
                if (!dchat::DrawGlyph(dc, row.muteMark, dchat::kGlyphMute, 14, g_palette->system)) {
                    dchat::DrawThinBar(dc, row.muteMark, g_palette->system);
                }
            }
            if (row.pinMark.right > row.pinMark.left) {
                if (!dchat::DrawGlyph(dc, row.pinMark, dchat::kGlyphPin, 14, g_palette->system)) {
                    dchat::DrawThinBar(dc, row.pinMark, g_palette->system);
                }
            }
        }
        if (g_sessionLayout.rows.empty()) {
            RECT hint{panel.left + 12, panel.top + dchat::kSessionListHeaderH, panel.right - 12,
                      panel.top + dchat::kSessionListHeaderH + 40};
            TextOutUtf8(dc, L"还没有会话", hint, ui.fontSmall, g_palette->system,
                        DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }

    // ---- 状态条：只读信息，不是按钮 ----
    const RECT statusBar = g_bottom.statusBar;
    HBRUSH statusBrush = CreateSolidBrush(g_palette->panel);
    FillRect(dc, &statusBar, statusBrush);
    DeleteObject(statusBrush);
    RECT statusLine{0, statusBar.top, width, statusBar.top + 1};
    HBRUSH statusLineBrush = CreateSolidBrush(g_palette->border);
    FillRect(dc, &statusLine, statusLineBrush);
    DeleteObject(statusLineBrush);
    RECT statusText{14, statusBar.top, width - 14, statusBar.bottom};
    TextOutUtf8(dc, Utf8ToWide(g_statusLine), statusText, ui.fontSmall, g_palette->system,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // ---- 输入胶囊 ----
    if (g_inputPill.right > g_inputPill.left) {
        const int radius = (g_inputPill.bottom - g_inputPill.top) / 2;
        ui::DrawRoundedControl(dc, g_inputPill, radius, g_palette->windowBg,
                               g_palette->bubbleOther, g_palette->border);
    }

    // ---- 「＋」：文件和语音都从这里进（以前是两个常驻按钮）----
    const RECT plus = g_bottom.plusButton;
    const COLORREF plusFill = g_plusMenuOpen ? g_palette->accent : g_palette->neutral;
    const COLORREF plusBorder = g_plusMenuOpen
                                    ? dchat::AdjustColor(g_palette->accent, -20)
                                    : g_palette->neutralBorder;
    ui::DrawRoundedControl(dc, plus, (plus.bottom - plus.top) / 2, g_palette->windowBg, plusFill,
                           plusBorder);
    if (!dchat::DrawGlyph(dc, plus, dchat::kGlyphPlus, 18,
                          g_plusMenuOpen ? g_palette->accentText : g_palette->text)) {
        dchat::DrawPlusIcon(dc, (plus.left + plus.right) / 2, (plus.top + plus.bottom) / 2, 8, 2,
                            g_plusMenuOpen ? g_palette->accentText : g_palette->text);
    }

    // ---- 录音中：输入胶囊描一圈红边，说明"现在说话会被录进去" ----
    if (g_recorder.IsRunning()) {
        ui::FillRoundedRect(dc, g_inputPill, (g_inputPill.bottom - g_inputPill.top) / 2,
                            g_palette->noticeBg, g_palette->error, 2.0f);
        RECT hint{g_inputPill.left + 20, g_inputPill.top, g_inputPill.right - 12,
                  g_inputPill.bottom};
        TextOutUtf8(dc, L"● 正在录音…… 说完点「＋」里的「结束并发送语音」", hint, ui.font,
                    g_palette->error, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 「＋」菜单是**独立弹出窗口**（见 ShowPlusMenuWindow / PlusMenuProc），
    // 不在这里画：画在客户区里就得挤在"记录区"里，窗口一矮就会被状态条压住。
}

// ---------------- 连接状态 ----------------
bool IsConnected() { return g_sock != INVALID_SOCKET; }

void UpdateStatus() {
    g_statusLine = BuildStatusLine();
    if (ui.hSend) EnableWindow(ui.hSend, IsConnected());
    if (ui.hInput) EnableWindow(ui.hInput, IsConnected());
    if (ui.hwnd) InvalidateChrome(ui.hwnd);
}

// ---------------- 网络 ----------------
// 与服务端之间的加密会话。握手成功前 active() 为假，收发都是明文——
// 老服务器不认识 HELLO，握手会自然失败，也就一直走明文，不会因此连不上。
dchat::CryptoSession g_crypto;
// 服务器公钥指纹（TOFU 用：应当记住它，下次变了就警告）
std::string g_serverFingerprint;

// ---------------------------------------------------------------------------
// TOFU：记住服务器指纹
//
// 加密本身只保证"这条通道没被偷听"，**不保证对面是那台服务器**。
// 攻击者若能劫持连接，可以分别和两边各做一次握手、全程转发，双方都察觉不到。
// 记住首次见到的指纹、以后每次比对，才能发现这种情况（SSH 就是这么做的）。
// ---------------------------------------------------------------------------

const char* const kKnownServersPath = "dchat-known-servers.txt";

/** 键是 `主机:端口`——同一台机器的不同端口算不同的服务器。 */
std::string KnownServerKey() { return g_host + ":" + std::to_string(g_port); }

std::string TrimTrailing(const std::string& text) {
    std::string out = text;
    while (!out.empty() && (out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

std::string LoadTrustedFingerprint(const std::string& key) {
    std::ifstream in(kKnownServersPath);
    if (!in) return std::string();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        if (line.substr(0, space) == key) return TrimTrailing(line.substr(space + 1));
    }
    return std::string();
}

void SaveTrustedFingerprint(const std::string& key, const std::string& fingerprint) {
    // 先把其它条目读回来，再整体重写（条目很少，不值得做增量更新）
    std::vector<std::pair<std::string, std::string>> entries;
    {
        std::ifstream in(kKnownServersPath);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const std::size_t space = line.find(' ');
            if (space == std::string::npos) continue;
            const std::string existing = line.substr(0, space);
            if (existing == key) continue;
            entries.push_back({existing, TrimTrailing(line.substr(space + 1))});
        }
    }
    entries.push_back({key, fingerprint});

    std::ofstream out(kKnownServersPath);
    if (!out) return;
    out << "# dchat 记住的服务器指纹（TOFU）。\n"
        << "# 指纹变了说明服务器换了密钥，或者有人在中间；核对清楚之前不要手改这里。\n";
    for (const auto& entry : entries) out << entry.first << " " << entry.second << "\n";
}

/** 往聊天记录里塞一条系统提示（复用和服务器消息相同的显示通道）。 */
void PostNotice(const std::string& text) {
    auto* payload = new std::string("SYS " + dchat::NowTimeString() + " " + text);
    if (!PostMessageW(ui.hwnd, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

bool SendRawLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_sendMutex);
    if (g_sock == INVALID_SOCKET) return false;

    // ⚠️ 加密必须在 g_sendMutex **锁内**做：界面线程、心跳线程、接收线程
    // 都会调用这里，而 CryptoSession 的 nonce 计数器不是线程安全的——
    // 两个线程同时加密就可能撞上同一个 nonce，在 GCM 下是致命的。
    std::string data;
    if (g_crypto.active()) {
        std::string sealed;
        if (!g_crypto.Encrypt(line, &sealed)) return false;
        data = dchat::BuildLine("ENC", dchat::Base64Encode(
                                           reinterpret_cast<const unsigned char*>(sealed.data()),
                                           sealed.size())) +
               "\n";
    } else {
        data = line + "\n";
    }

    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(g_sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/** 握手期间同步读一行（最多等 timeoutMs）。用 select 控制超时，不阻塞太久。 */
bool ReadHandshakeLine(SOCKET sock, dchat::LineBuffer* buffer, std::string* out, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char chunk[512];
    for (;;) {
        if (buffer->PopLine(out)) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;

        const auto remainMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remainMs / 1000);
        tv.tv_usec = static_cast<long>((remainMs % 1000) * 1000);
        if (::select(0, &readSet, nullptr, nullptr, &tv) <= 0) return false;

        const int received = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (received <= 0) return false;
        buffer->Append(chunk, static_cast<std::size_t>(received));
        if (buffer->bad()) return false;
    }
}

/**
 * 与服务端做加密握手。成功返回 true 并激活 [g_crypto]。
 *
 * **必须在启动接收线程之前调用**：握手期间要同步读几行（WELCOME / HELLO_OK），
 * 如果接收线程已经在跑，两边会抢同一个 socket，读到的行会随机分给其中一边。
 *
 * 服务端不支持（回 ERROR）或超时都返回 false，调用方继续用明文。
 */
bool DoCryptoHandshake(SOCKET sock) {
    dchat::EcdhKeyPair keyPair;
    if (!dchat::GenerateEcdhKeyPair(&keyPair)) return false;
    std::vector<unsigned char> clientNonce;
    if (!dchat::RandomBytes(dchat::kHandshakeNonceBytes, &clientNonce)) return false;

    const std::string hello = dchat::BuildLine(
        "HELLO", std::to_string(dchat::kCryptoVersion) + " " +
                     dchat::Base64Encode(keyPair.publicKey.data(), keyPair.publicKey.size()) + " " +
                     dchat::Base64Encode(clientNonce.data(), clientNonce.size()));
    if (!SendRawLine(hello)) return false;  // 此时还没加密，发出去就是明文

    dchat::LineBuffer buffer;
    std::string line;
    for (;;) {
        if (!ReadHandshakeLine(sock, &buffer, &line, dchat::kHandshakeTimeoutMs)) return false;
        const dchat::Message msg = dchat::ParseLine(line);
        if (msg.command == "HELLO_OK") {
            const std::vector<std::string> words = msg.Words();
            if (words.size() < 2) return false;
            std::vector<unsigned char> serverPublic;
            std::vector<unsigned char> serverNonce;
            if (!dchat::Base64Decode(words[0], &serverPublic) ||
                serverPublic.size() != dchat::kP256PublicKeyBytes) {
                return false;
            }
            if (!dchat::Base64Decode(words[1], &serverNonce) ||
                serverNonce.size() != dchat::kHandshakeNonceBytes) {
                return false;
            }
            std::vector<unsigned char> shared;
            if (!dchat::ComputeSharedSecret(keyPair, serverPublic, &shared)) return false;
            const dchat::SessionKeys keys =
                dchat::DeriveSessionKeys(shared, clientNonce, serverNonce);
            if (!keys.valid()) return false;
            // 客户端发用 c2s、收用 s2c（服务端正好相反）
            if (!g_crypto.Start(keys.clientToServer, keys.serverToClient)) return false;
            g_serverFingerprint = dchat::PublicKeyFingerprint(serverPublic);

            // ---- TOFU：和上次记下的指纹比对 ----
            const std::string key = KnownServerKey();
            const std::string known = LoadTrustedFingerprint(key);
            if (known.empty()) {
                SaveTrustedFingerprint(key, g_serverFingerprint);
                PostNotice("首次连接这台服务器，已记下指纹 " + g_serverFingerprint +
                           "（以后变了会警告）");
            } else if (known != g_serverFingerprint) {
                // ⚠️ 绝不自动接受新指纹——那等于把警告变成静默接受，TOFU 就白做了。
                // 只提示，让用户自己去跟管理员核对；核对完再手工改
                // dchat-known-servers.txt 里的那一行。
                PostNotice("⚠ 服务器指纹变了！之前是 " + known + "，现在是 " +
                           g_serverFingerprint +
                           "。可能只是服务器换了密钥，也可能有人在中间——请先跟管理员核对。");
            } else {
                PostNotice("服务器身份已确认（指纹未变）");
            }
            return true;
        }
        // 服务器不认识 HELLO：老版本，保持明文
        if (msg.command == "ERROR") return false;
        // 其它行（比如 WELCOME）不能丢，照常交给界面
        auto* payload = new std::string(line);
        if (!PostMessageW(ui.hwnd, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }
}

void RecvLoop(SOCKET sock) {
    dchat::LineBuffer buffer;
    std::vector<char> chunk(2048);
    while (g_running.load()) {
        const int received = ::recv(sock, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received <= 0) break;
        buffer.Append(chunk.data(), static_cast<std::size_t>(received));
        if (buffer.bad()) break;
        std::string line;
        bool protocolError = false;
        while (buffer.PopLine(&line)) {
            std::string effective = line;
            if (g_crypto.active()) {
                // 加密启用后每一行都必须是 `ENC <base64>`。
                // ⚠️ 收到明文必须断开，不能宽容地当明文处理——否则攻击者
                // 在加密通道里发明文指令就能绕过加密（降级攻击）。
                const dchat::Message outer = dchat::ParseLine(line);
                if (outer.command != "ENC") {
                    protocolError = true;
                    break;
                }
                std::vector<unsigned char> sealed;
                if (!dchat::Base64Decode(outer.rest, &sealed)) {
                    protocolError = true;
                    break;
                }
                const std::string sealedText(sealed.begin(), sealed.end());
                if (!g_crypto.Decrypt(sealedText, &effective)) {
                    // 认证失败 = 被篡改 / 密钥不对 / 顺序错乱。绝不能继续用这条连接。
                    protocolError = true;
                    break;
                }
            }
            auto* payload = new std::string(effective);
            if (!PostMessageW(ui.hwnd, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
        if (protocolError) break;
    }
    PostMessageW(ui.hwnd, WM_APP_CLOSED, 0, 0);
}

bool StartWinsock() {
    static bool ready = false;
    if (ready) return true;
    WSADATA wsa{};
    ready = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    return ready;
}

bool ConnectToServer(const std::string& host, int port, std::string* error) {
    if (!StartWinsock()) {
        *error = "网络初始化失败";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        *error = "无法解析服务器地址";
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);
        bool connected = ::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval timeout{4, 0};
            if (select(0, nullptr, &writeSet, nullptr, &timeout) > 0) {
                int soError = 0;
                int length = sizeof(soError);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length);
                connected = soError == 0;
            }
        }
        if (connected) {
            u_long blocking = 0;
            ioctlsocket(sock, FIONBIO, &blocking);
            break;
        }
        ::closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        *error = "连接失败：服务器没启动、地址/端口不对；如果是公网，检查端口映射和防火墙";
        return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
               sizeof(noDelay));
    // TCP keepalive：走公网 / NAT 时，长时间没消息也能发现连接已经断了
    BOOL keepAlive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive),
               sizeof(keepAlive));
    tcp_keepalive keepAliveSettings{};
    keepAliveSettings.onoff = TRUE;
    keepAliveSettings.keepalivetime = 30000;
    keepAliveSettings.keepaliveinterval = 5000;
    DWORD returned = 0;
    WSAIoctl(sock, SIO_KEEPALIVE_VALS, &keepAliveSettings, sizeof(keepAliveSettings), nullptr, 0,
             &returned, nullptr, nullptr);
    g_sock = sock;
    g_crypto.Reset();
    g_serverFingerprint.clear();
    // 加密握手必须在启动接收线程**之前**同步做完：握手期间要自己读几行，
    // 如果接收线程已经在跑，两边会抢同一个 socket（原因见 DoCryptoHandshake 注释）。
    // 失败（老服务器）就保持明文，不影响连接。
    DoCryptoHandshake(sock);
    g_running = true;
    g_recvThread = std::thread(RecvLoop, sock);
    // 心跳：每 45 秒发一次 PING，让中途的 NAT / 路由别把这条空闲连接回收掉
    std::thread([] {
        while (g_running.load()) {
            for (int i = 0; i < 45 && g_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!g_running.load()) break;
            g_heartbeatPending = true;
            if (!SendRawLine(dchat::BuildLine("PING"))) g_heartbeatPending = false;
        }
    }).detach();
    return true;
}

void DisconnectFromServer(bool showNotice) {
    g_running = false;
    if (g_sock != INVALID_SOCKET) ::shutdown(g_sock, SD_BOTH);
    if (g_recvThread.joinable()) g_recvThread.join();
        if (g_sock != INVALID_SOCKET) {
            ::closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }
        g_onlineNicks.clear();  // 断开后名单作废：下次连接由服务器重新下发
        g_knownNicks.clear();
        g_authed = false;
        g_authPending = false;
        // 多会话状态也要复位：换一台服务器（或对面换了版本）时，
        // 上一次的能力协商结论不能留着 —— 留着就会出现"发出去的命令石沉大海"。
        // 会话列表本身**保留**：本地设置（免打扰/置顶）挂在会话上，丢了用户会觉得白设了。
        g_roomsDeclared = false;
        g_roomsEnabled = false;
        ResetTransfers("连接已断开，接收中断");  // 没传完的文件不留半个在硬盘上
    UpdateStatus();
    if (showNotice) ViewAddItem(ItemKind::Notice, "已断开连接", dchat::NowTimeString());
}

// ---------------- 文件传输 ----------------
// 广播式：发送方把文件切成 Base64 小块交给服务器，服务器转发给房间里的其他人，
// 接收方一边收一边写进 exe 旁边的 received\ 目录。服务器不落地文件。
//
// 发送在独立线程里做（否则大文件会把界面卡住），进度通过 WM_APP_XFER 回到界面线程；
// 接收在界面线程里做（每块 2048 字节，解码 + 写盘很快）。
struct TransferEvent {   // 发送线程 -> 界面线程
    bool finished = false;
    bool error = false;
    std::string status;  // 进度文字（UTF-8），空表示清除
    std::string notice;  // 写进记录区的一句话，空表示不写
};

struct FileSendJob {
    std::string id;           // 传输 ID（只含 ASCII，服务器转发时会加上昵称）
    std::string displayName;  // UTF-8，用于提示
    std::string base64Name;   // 协议里传的文件名
    std::wstring sourcePath;  // 本地路径（用来生成缩略图）
    std::string kind;         // 附件种类（空 = 普通文件；"voice" = 语音）
    unsigned long long total = 0;
    std::vector<char> data;
};

constexpr std::size_t kMaxThumbBytes = 128 * 1024;  // 和服务器保持一致

// 一次"点击下载"对应一个 job：服务器发 FILE_BEGIN 时建文件，FILE_DATA 往里写，
// FILE_END 收尾。整个流程在界面线程里跑（每块 2048 字节，够快）。
struct FileDownloadJob {
    std::string id;
    std::wstring path;  // 保存到的完整路径
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned long long total = 0;
    unsigned long long received = 0;
    int lastPercent = -1;
    std::string failReason;
};

std::string g_sendProgress;  // 例如「报告.pdf 62%」
bool g_sendBusy = false;
unsigned long long g_xferCounter = 0;
std::map<std::string, std::unique_ptr<FileDownloadJob>> g_downloads;  // key: 服务器文件ID

// 收到的文件统一放在 exe 同级的 received\ 目录
std::wstring ReceivedDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return std::wstring();
    std::wstring dir(exePath);
    const std::size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash + 1);
    dir += L"received\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    cached = dir;
    return cached;
}

int PercentOf(unsigned long long done, unsigned long long total) {
    if (total == 0) return 100;
    const unsigned long long percent = done * 100 / total;
    return percent > 100 ? 100 : static_cast<int>(percent);
}

void PostTransfer(bool finished, bool error, const std::string& status,
                  const std::string& notice) {
    if (!ui.hwnd) return;
    auto* event = new TransferEvent();
    event->finished = finished;
    event->error = error;
    event->status = status;
    event->notice = notice;
    if (!PostMessageW(ui.hwnd, WM_APP_XFER, 0, reinterpret_cast<LPARAM>(event))) delete event;
}

// 按文件 ID 找记录区里的那张卡片
Item* FindFileItem(const std::string& id) {
    for (Item& item : g_items) {
        if (item.kind == ItemKind::File && item.fileId == id) return &item;
    }
    return nullptr;
}

void RefreshTransferStatus();

// 卡片状态变了：刷新记录区，必要时顺带刷新状态栏
void TouchFileCard(const std::string& id, bool refreshStatus) {
    (void)id;
    if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
    if (refreshStatus) RefreshTransferStatus();
}

// 把「发送中 / 下载中」拼进状态栏（状态栏右侧是连接/断开按钮）
void RefreshTransferStatus() {
    std::string text;
    if (g_sendBusy && !g_sendProgress.empty()) {
        text = "发送 " + g_sendProgress;
    } else if (!g_downloads.empty()) {
        const FileDownloadJob* job = g_downloads.begin()->second.get();
        const Item* item = FindFileItem(job->id);
        text = "下载 " + (item ? item->raw : job->id) + " " +
               std::to_string(PercentOf(job->received, job->total)) + "%";
    }
    g_transferStatus = text;
    UpdateStatus();
}

// 结束一次下载：成功就把卡片标成"已保存"，失败就删掉半个文件并标成可重试
void FinishDownload(const std::string& id, bool ok, const std::string& reason) {
    const auto it = g_downloads.find(id);
    if (it != g_downloads.end()) {
        FileDownloadJob* job = it->second.get();
        if (job->file != INVALID_HANDLE_VALUE) {
            CloseHandle(job->file);
            job->file = INVALID_HANDLE_VALUE;
        }
        if (!ok) DeleteFileW(job->path.c_str());
        g_downloads.erase(it);
    }
    Item* item = FindFileItem(id);
    if (item) {
        if (ok) {
            item->fileState = 2;
            item->fileProgress = 100;
        } else {
            item->fileState = 3;
            item->fileNote = reason.empty() ? "下载失败，点一下重试" : reason;
        }
    }
    TouchFileCard(id, true);
}

void ResetTransfers(const std::string& reason) {
    g_sendBusy = false;
    g_sendProgress.clear();
    std::vector<std::string> ids;
    for (const auto& entry : g_downloads) ids.push_back(entry.first);
    for (const std::string& id : ids) FinishDownload(id, false, reason);
    RefreshTransferStatus();
}

// 发送线程：FILE_SEND -> N 个 FILE_CHUNK -> FILE_END
void SendFileThread(std::shared_ptr<FileSendJob> job) {
    auto abortWith = [&](const std::string& notice) {
        SendRawLine(dchat::BuildLine("FILE_CANCEL", job->id));  // 尽量让对方删掉半个文件
        PostTransfer(true, true, std::string(), notice);
    };
    // 图片/视频：先在本地做一张缩略图（视频=第一帧）跟文件一起传上去。
    // 这样接收端视频**只需要下这一小张预览图**，不用把整段视频也拉下来。
    std::string thumbnail;
    if (dchat::IsPreviewableFile(job->displayName)) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (HBITMAP bitmap = dchat::LoadThumbnail(job->sourcePath, 160)) {
            const std::vector<unsigned char> png = dchat::EncodePng(bitmap);
            thumbnail.assign(png.begin(), png.end());
            DeleteObject(bitmap);
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (thumbnail.size() > kMaxThumbBytes) thumbnail.clear();
    }

    // FILE_SEND 的字段是**按位置**解析的：第 4 格是"接着会发缩略图"的标记或者种类，
    // 二者互斥（见 BuildFileSendRest 的说明）。语音走的是"第 4 格 = kind"那条路。
    const std::string fileSendRest =
        dchat::BuildFileSendRest(job->id, job->base64Name, job->total, job->kind,
                                 !thumbnail.empty());
    if (!SendRawLine(dchat::BuildLine("FILE_SEND", fileSendRest))) {
        PostTransfer(true, true, std::string(), "发送失败：连接已断开");
        return;
    }
    for (std::size_t offset = 0; !thumbnail.empty() && offset < thumbnail.size();
         offset += dchat::kFileChunkBytes) {
        const std::size_t remain = thumbnail.size() - offset;
        const std::size_t length =
            remain < dchat::kFileChunkBytes ? remain : dchat::kFileChunkBytes;
        const std::string piece = dchat::Base64Encode(
            reinterpret_cast<const unsigned char*>(thumbnail.data() + offset), length);
        if (!SendRawLine(dchat::BuildLine("FILE_THUMB", job->id + " " + piece))) {
            abortWith("发送中断：连接已断开");
            return;
        }
    }
    const unsigned long long chunks =
        (job->total + dchat::kFileChunkBytes - 1) / dchat::kFileChunkBytes;
    int lastPercent = -1;
    for (unsigned long long index = 0; index < chunks; ++index) {
        if (!g_running.load()) {
            abortWith("发送中断：连接已断开");
            return;
        }
        const unsigned long long offset = index * dchat::kFileChunkBytes;
        const unsigned long long remain = job->total - offset;
        const std::size_t length = remain < dchat::kFileChunkBytes
                                       ? static_cast<std::size_t>(remain)
                                       : dchat::kFileChunkBytes;
        const std::string encoded = dchat::Base64Encode(
            reinterpret_cast<const unsigned char*>(job->data.data() + offset), length);
        if (!SendRawLine(dchat::BuildLine("FILE_CHUNK", job->id + " " + encoded))) {
            abortWith("发送中断：连接已断开");
            return;
        }
        const int percent = PercentOf(offset + length, job->total);
        if (percent != lastPercent) {  // 只在百分比变化时更新状态栏
            lastPercent = percent;
            PostTransfer(false, false, job->displayName + " " + std::to_string(percent) + "%",
                         std::string());
        }
    }
    if (!SendRawLine(dchat::BuildLine("FILE_END", job->id))) {
        PostTransfer(true, true, std::string(), "发送失败：连接已断开");
        return;
    }
    PostTransfer(true, false, std::string(),
                 "文件已上传：" + job->displayName + "（" + dchat::FormatBytes(job->total) +
                     "），房间里的成员可以点击下载了");
}

void StartSendFile(const std::wstring& path, const std::string& kind = std::string()) {
    if (!IsConnected()) {
        ViewAddItem(ItemKind::Error, "还没连接服务器，先连接再发文件", dchat::NowTimeString());
        return;
    }
    if (g_sendBusy) {
        ViewAddItem(ItemKind::Notice, "上一个文件还在发送中，等它发完再试",
                    dchat::NowTimeString());
        return;
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring rawName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    // 和接收端用同一套清理规则：只留文件名、去掉非法字符，免得把整条路径发出去
    std::string displayName = dchat::SanitizeFileName(WideToUtf8(rawName));
    if (displayName == "file") displayName = "未命名文件";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ViewAddItem(ItemKind::Error, "打不开这个文件（可能被占用或没有权限）",
                    dchat::NowTimeString());
        return;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        ViewAddItem(ItemKind::Error, "这是个空文件，没必要发送", dchat::NowTimeString());
        return;
    }
    // 用服务器下发的单文件上限（RULES 行），没收到就按默认值
    const unsigned long long maxBytes = dchat::RuleMbToBytes(g_maxFileMb);
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(file);
        ViewAddItem(ItemKind::Error,
                    "文件太大了：服务器当前限制单个文件最多 " + dchat::FormatBytes(maxBytes),
                    dchat::NowTimeString());
        return;
    }

    auto job = std::make_shared<FileSendJob>();
    job->total = static_cast<unsigned long long>(size.QuadPart);
    job->data.resize(static_cast<std::size_t>(job->total));
    std::size_t readTotal = 0;
    while (readTotal < job->data.size()) {
        const std::size_t remain = job->data.size() - readTotal;
        const DWORD want = static_cast<DWORD>(remain < (1u << 20) ? remain : (1u << 20));
        DWORD got = 0;
        if (!ReadFile(file, job->data.data() + readTotal, want, &got, nullptr) || got == 0) break;
        readTotal += got;
    }
    CloseHandle(file);
    if (readTotal != job->data.size()) {
        ViewAddItem(ItemKind::Error, "读文件失败，发送取消", dchat::NowTimeString());
        return;
    }

    job->displayName = displayName;
    job->base64Name = dchat::Base64Encode(displayName);
    job->sourcePath = path;
    job->kind = kind;
    job->id = "f" + std::to_string(GetTickCount64()) + "_" + std::to_string(++g_xferCounter);
    g_sendBusy = true;
    g_sendProgress = displayName + " 0%";
    ViewAddItem(ItemKind::Notice,
                "开始发送 " + displayName + "（" + dchat::FormatBytes(job->total) + "）…",
                dchat::NowTimeString());
    RefreshTransferStatus();
    // 语音：上传线程用完就把它删掉，本地不留（想听自己刚说的那条，
    // 点气泡下载服务器上那一份就行——和 QQ / 微信一样）。
    // 不删的话 voice-notes\ 会随着每次录音一直涨，用户唯一的感受就是"这程序怎么这么占地方"。
    if (job->kind == "voice") {
        std::thread([job] {
            SendFileThread(job);
            DeleteFileW(job->sourcePath.c_str());
        }).detach();
    } else {
        std::thread(SendFileThread, job).detach();
    }
}

// ------------------------------------------------------------------
// 语音消息：录音 / 播放
// ------------------------------------------------------------------

void RefreshVoiceState() {
    if (ui.hVoice) {
        const bool recording = g_recorder.IsRunning();
        // 录音中按钮文字变成方块（再点一下 = 结束并发送）
        SetWindowTextW(ui.hVoice, recording ? L"■ 发送" : L"录音");
        InvalidateRect(ui.hVoice, nullptr, TRUE);
    }
    UpdateStatus();
    if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
}

void StartVoiceRecording() {
    if (!IsConnected() || !g_authed) {
        ViewAddItem(ItemKind::Error, "先登录再发语音", dchat::NowTimeString());
        return;
    }
    if (g_recorder.IsRunning()) return;
    if (g_sendBusy) {
        ViewAddItem(ItemKind::Notice, "上一个文件还在发送中，等它发完再录音",
                    dchat::NowTimeString());
        return;
    }
    // 录音时会外放，先把正在播的语音停掉（否则录进去的是别人说的话）
    if (!g_playingVoiceId.empty()) {
        g_voicePlayer.Stop();
        g_playingVoiceId.clear();
        g_playingVoiceTotal = 0;
    }
    if (!g_recorder.Start()) {
        // 没有麦克风 / 被占用：voice_notes 那边已经给出一句人话
        ViewAddItem(ItemKind::Error, g_recorder.LastError(), dchat::NowTimeString());
        RefreshVoiceState();
        return;
    }
    g_recordingHint.clear();
    RefreshVoiceState();
    // 200ms 刷一次计时：界面是按秒走的，更密没有意义，还白烧电
    SetTimer(ui.hwnd, kRecordTimerId, 200, nullptr);
}

void StopVoiceRecordingAndSend() {
    if (!g_recorder.IsRunning()) return;
    KillTimer(ui.hwnd, kRecordTimerId);

    const int seconds = g_recorder.ElapsedSeconds();
    std::string path;
    std::string error;
    if (!g_recorder.Finished(&path, &error)) {
        // 录得太短 / 写不出文件：**一定要说清是哪种**，否则用户只会看到"按了没反应"
        ViewAddItem(ItemKind::Notice, error, dchat::NowTimeString());
        g_recordingHint.clear();
        RefreshVoiceState();
        return;
    }
    g_recordingHint.clear();
    RefreshVoiceState();

    // 大小和时长再拦一道（录音过程里设备可能给出比预期大的数据）
    const std::wstring wide = Utf8ToWide(path);
    unsigned long long bytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &info)) {
            bytes = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
    }
    const std::string name = WideToUtf8(wide.substr(wide.find_last_of(L"\\/") + 1));
    const std::string reason = dchat::WhyCannotSendVoice(name, bytes, seconds);
    if (!reason.empty()) {
        DeleteFileW(wide.c_str());
        ViewAddItem(ItemKind::Error, reason, dchat::NowTimeString());
        return;
    }
    ViewAddItem(ItemKind::Notice,
                "语音录好了（" + dchat::FormatDuration(seconds) + "，" +
                    dchat::FormatVoiceSize(bytes) + "），正在上传…",
                dchat::NowTimeString());
    StartSendFile(wide, "voice");
}

// 点一条语音气泡：播 / 暂停 / 接着播
void ToggleVoicePlayback(const std::string& fileId) {
    Item* item = FindFileItem(fileId);
    if (!item) return;

    // 就是这条：在播 → 暂停；暂停着 → 接着播
    if (g_playingVoiceId == fileId) {
        if (g_voicePlayer.IsPlaying()) {
            g_voicePlayer.Pause();
        } else {
            g_voicePlayer.Resume();
        }
        if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
        return;
    }

    if (item->fileState == 0) {  // 还没下
        StartFileDownload(fileId);
        return;
    }
    if (item->fileState == 1) return;  // 下着呢，别抢
    if (item->fileState == 3) {        // 失败过：再点一次就是重试
        StartFileDownload(fileId);
        return;
    }
    if (item->savedPath.empty()) return;

    // 换一条：先把上一条停掉（两条一起响是谁也听不清的噪音）
    g_voicePlayer.Stop();
    const int seconds = g_voicePlayer.Play(item->savedPath);
    if (seconds < 0) {
        g_playingVoiceId.clear();
        g_playingVoiceTotal = 0;
        ViewAddItem(ItemKind::Error, g_voicePlayer.LastError(), dchat::NowTimeString());
    } else {
        g_playingVoiceId = fileId;
        g_playingVoiceTotal = seconds;
    }
    if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
}

// ---------------- 「＋」弹出菜单 ----------------
//
// **做成独立的弹出窗口**，不是在主窗口客户区里画一块。
// 试过画在客户区里：面板必须挤在"聊天记录区"里（下方是状态条、上方是菜单栏），
// 窗口一矮就放不下，只能把面板压到状态条底下——看起来就是"菜单被遮挡"。
// 弹出窗口没有这个约束，和系统菜单一样能盖在任意内容上。
constexpr const wchar_t* kPlusMenuClass = L"DchatPlusMenu";
HWND g_plusMenuWindow = nullptr;
bool g_plusTrackMouse = false;

RECT PlusMenuRectForWindow(HWND parent) {
    RECT client{};
    GetClientRect(parent, &client);
    const dchat::BottomLayout bottom =
        dchat::LayoutBottom(nullptr, nullptr, client.right, client.bottom, dchat::kMenuBarHeight);
    return dchat::PlusMenuRect(bottom);
}

void DestroyPlusMenuWindow();

// 菜单面板画在哪：整块面板 + 两项（纯几何，见 ui_layout.cpp 的单测）
dchat::PlusMenuLayout PlusMenuLayoutNow() {
    dchat::PlusMenuLayout layout;
    const RECT panel = PlusMenuRectForWindow(ui.hwnd);
    layout.panel = panel;
    layout.fileItem = RECT{panel.left + 6, panel.top + 6, panel.right - 6, panel.top + 40};
    layout.voiceItem = RECT{layout.fileItem.left, layout.fileItem.bottom + 2,
                            layout.fileItem.right, layout.fileItem.bottom + 36};
    layout.valid = true;
    return layout;
}

LRESULT CALLBACK PlusMenuProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            RECT client{};
            GetClientRect(wnd, &client);
            const dchat::PlusMenuLayout layout = PlusMenuLayoutNow();

            // 面板
            ui::DrawRoundedControl(dc, client, 10, g_palette->windowBg, g_palette->neutral,
                                   g_palette->border);
            // 两项（位置相对面板；这个窗口的客户区就是面板本身）
            auto item = [&](const RECT& absolute, wchar_t glyph, const wchar_t* label, int index) {
                RECT rect{absolute.left - layout.panel.left, absolute.top - layout.panel.top,
                          absolute.right - layout.panel.left, absolute.bottom - layout.panel.top};
                if (g_plusHover == index) {
                    ui::FillRoundedRect(dc, rect, 8, g_palette->hover, g_palette->hover, 0.0f);
                }
                RECT iconRect{rect.left + 12, rect.top, rect.left + 12 + 20, rect.bottom};
                if (!dchat::DrawGlyph(dc, iconRect, glyph, 16, g_palette->text)) {
                    dchat::DrawPlusIcon(dc, iconRect.left + 10, (rect.top + rect.bottom) / 2, 6, 2,
                                        g_palette->text);
                }
                RECT textRect{iconRect.right + 8, rect.top, rect.right - 10, rect.bottom};
                RECT target = textRect;
                HGDIOBJ oldFont = SelectObject(dc, ui.font);
                const int oldMode = SetBkMode(dc, TRANSPARENT);
                const COLORREF oldColor = SetTextColor(dc, g_palette->text);
                DrawTextW(dc, label, -1, &target,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(dc, oldColor);
                SetBkMode(dc, oldMode);
                SelectObject(dc, oldFont);
            };
            item(layout.fileItem, dchat::kGlyphAttachment, L"发送文件", 0);
            item(layout.voiceItem, dchat::kGlyphMic,
                 g_recorder.IsRunning() ? L"结束并发送语音" : L"录一段语音", 1);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const dchat::PlusMenuLayout layout = PlusMenuLayoutNow();
            const int x = GET_X_LPARAM(lp) + layout.panel.left;
            const int y = GET_Y_LPARAM(lp) + layout.panel.top;
            const int hover = PtInRect(&layout.fileItem, POINT{x, y})    ? 0
                              : PtInRect(&layout.voiceItem, POINT{x, y}) ? 1
                                                                         : -1;
            if (hover != g_plusHover) {
                g_plusHover = hover;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            if (!g_plusTrackMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = wnd;
                TrackMouseEvent(&track);
                g_plusTrackMouse = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_plusTrackMouse = false;
            if (g_plusHover != -1) {
                g_plusHover = -1;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const dchat::PlusMenuLayout layout = PlusMenuLayoutNow();
            const int x = GET_X_LPARAM(lp) + layout.panel.left;
            const int y = GET_Y_LPARAM(lp) + layout.panel.top;
            const dchat::PlusAction action = dchat::HitTestPlusMenu(layout, x, y);
            DestroyPlusMenuWindow();
            if (ui.hwnd) InvalidateChrome(ui.hwnd);
            // 菜单先关掉再执行动作（发文件要弹系统对话框，语音要开始录）
            switch (action) {
                case dchat::PlusAction::SendFile:
                    DoSendFile(ui.hwnd);
                    break;
                case dchat::PlusAction::RecordVoice:
                    if (g_recorder.IsRunning()) {
                        StopVoiceRecordingAndSend();
                    } else {
                        StartVoiceRecording();
                    }
                    break;
                case dchat::PlusAction::None:
                    break;
            }
            return 0;
        }
        case WM_KILLFOCUS:  // 点到别处就收起（和系统菜单一样）
            DestroyPlusMenuWindow();
            if (ui.hwnd) InvalidateChrome(ui.hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void DestroyPlusMenuWindow() {
    if (g_plusMenuWindow) {
        DestroyWindow(g_plusMenuWindow);
        g_plusMenuWindow = nullptr;
    }
    g_plusMenuOpen = false;
}

void ShowPlusMenuWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;  // 阴影让它明显"浮"在上面
        wc.lpfnWndProc = PlusMenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPlusMenuClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    DestroyPlusMenuWindow();

    const RECT panel = PlusMenuRectForWindow(ui.hwnd);
    RECT screen{panel.left, panel.top, panel.right, panel.bottom};
    ClientToScreen(ui.hwnd, reinterpret_cast<POINT*>(&screen.left));
    ClientToScreen(ui.hwnd, reinterpret_cast<POINT*>(&screen.right));
    // WS_EX_NOACTIVATE：不要抢走输入框的焦点（抢了的话点完菜单还得再点一次输入框）；
    // WS_EX_TOOLWINDOW：不出现在任务栏和 Alt+Tab 里
    g_plusMenuWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPlusMenuClass, L"", WS_POPUP,
        screen.left, screen.top, screen.right - screen.left, screen.bottom - screen.top, ui.hwnd,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_plusMenuWindow) return;
    g_plusMenuOpen = true;
    g_plusHover = -1;
    ShowWindow(g_plusMenuWindow, SW_SHOWNOACTIVATE);
    SetWindowPos(g_plusMenuWindow, HWND_TOPMOST, screen.left, screen.top, screen.right - screen.left,
                 screen.bottom - screen.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (ui.hwnd) InvalidateChrome(ui.hwnd);
}

// 处理服务器发来的文件相关消息
// 缩略图加载结果（工作线程 -> 界面线程）
struct PreviewResult {
    std::string fileId;
    HBITMAP bitmap = nullptr;
};

// 缩略图在工作线程里做：系统缩略图提供者偶尔要几百毫秒，不能在界面线程里等
void PreviewThread(std::string fileId, std::wstring path) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HBITMAP bitmap = dchat::LoadThumbnail(path, 160);
    if (SUCCEEDED(com)) CoUninitialize();
    if (!bitmap) return;
    auto* payload = new PreviewResult();
    payload->fileId = std::move(fileId);
    payload->bitmap = bitmap;
    if (!PostMessageW(ui.hwnd, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(payload))) {
        DeleteObject(bitmap);
        delete payload;
    }
}

// 下载完了：如果是图片/视频，去取缩略图（视频取的是第一帧）
void RequestPreview(const std::string& fileId, const std::string& fileName,
                    const std::string& savedPathUtf8) {
    if (!dchat::IsPreviewableFile(fileName)) return;
    std::thread(PreviewThread, fileId, Utf8ToWide(savedPathUtf8)).detach();
}

// 只取服务器上存的那张小预览图（图片缩小图 / 视频第一帧），不下载整份文件
std::map<std::string, std::string> g_thumbBytes;  // 文件ID -> 收到的缩略图字节

void RequestThumbnail(const std::string& fileId) {
    if (!IsConnected()) return;
    SendRawLine(dchat::BuildLine("FILE_THUMB_GET", fileId));
}

void HandleFileLine(const std::string& line) {
    const dchat::Message msg = dchat::ParseLine(line);
    std::vector<std::string> fields = msg.Words();
    // 服务器发的每条消息都带 hh:mm；这里做兼容：没有时间戳也能解析
    if (!fields.empty() && dchat::LooksLikeTime(fields[0])) fields.erase(fields.begin());

    if (msg.command == "FILE_OFFER" || msg.command == "RFILE_OFFER") {
        // 有人上传好了：在记录区里加一张卡片，等人点「下载」
        //
        // RFILE_OFFER 比 FILE_OFFER 多一格**房间号**（紧跟在时间之后）。
        // 服务端为它另起了一个命令名而不是给 FILE_OFFER 加字段：老客户端按位置解析
        // FILE_OFFER（第 6 格是"有无缩略图"），往中间插一格会让它们把房间号当缩略图标志。
        std::string roomId;
        if (msg.command == "RFILE_OFFER") {
            if (fields.empty()) return;
            roomId = fields.front();
            fields.erase(fields.begin());
        }
        if (fields.size() < 4) return;
        unsigned long long size = 0;
        bool digits = !fields[3].empty() && fields[3].size() <= 20;
        for (char c : fields[3]) {
            if (c < '0' || c > '9') digits = false;
            if (digits) size = size * 10 + static_cast<unsigned long long>(c - '0');
        }
        if (!digits || size == 0 || size > dchat::kMaxFileBytes) return;
        std::vector<unsigned char> nameBytes;
        if (!dchat::Base64Decode(fields[2], &nameBytes)) return;
        Item item;
        item.kind = ItemKind::File;
        item.nick = fields[0];
        item.fileId = fields[1];
        item.raw = dchat::SanitizeFileName(std::string(nameBytes.begin(), nameBytes.end()));
        item.fileSize = size;
        item.time = dchat::NowTimeString();
        // 附件种类是**追加在末尾**的第 7 格（老服务器不发这一格）。
        // 它决定渲染成语音气泡还是普通文件卡片——**不看扩展名**：
        // 安卓端录出来是 .m4a、桌面端是 .wav，扩展名不可靠。
        const std::string kind = fields.size() >= 6 ? fields[5] : std::string();
        item.isVoice = dchat::IsVoiceKind(kind);
        const int target = roomId.empty() ? HallSessionIndex() : EnsureSession(roomId, "group");
        DeliverToSession(target, item, true);  // 有人发了文件，和「被 @」一样提醒一下
        const bool hasThumb = (fields.size() >= 5 && fields[4] == "1");
        // 语音和图片自动下载（语音的意义就是"立刻能听"）；视频不自动下整份，
        // 只取第一帧预览。判断只走 ShouldAutoDownloadVoice 这一个入口——
        // 分成两处判断迟早会不一致，而且只在特定消息上表现出来，极难排查。
        if (dchat::ShouldAutoDownloadVoice(kind, item.raw)) {
            StartFileDownload(item.fileId);
        } else if (hasThumb) {
            RequestThumbnail(item.fileId);
        }
        return;
    }

    if (fields.empty()) return;
    const std::string id = fields[0];

    if (msg.command == "FILE_THUMB_DATA") {  // 只收到一小张预览图（视频的第一帧）
        if (fields.size() != 2) return;
        std::vector<unsigned char> chunk;
        if (!dchat::Base64Decode(fields[1], &chunk)) return;
        std::string& buffer = g_thumbBytes[id];
        if (buffer.size() + chunk.size() > kMaxThumbBytes) {
            g_thumbBytes.erase(id);
            return;
        }
        buffer.append(chunk.begin(), chunk.end());
        return;
    }
    if (msg.command == "FILE_THUMB_END") {
        const auto itThumb = g_thumbBytes.find(id);
        if (itThumb == g_thumbBytes.end()) return;
        std::vector<unsigned char> bytes(itThumb->second.begin(), itThumb->second.end());
        g_thumbBytes.erase(itThumb);
        HBITMAP bitmap = dchat::DecodePng(bytes);
        if (!bitmap) return;
        if (Item* item = FindFileItem(id)) {
            item->preview = std::shared_ptr<void>(bitmap, [](void* handle) {
                DeleteObject(static_cast<HBITMAP>(handle));
            });
            item->fileNote = "预览为视频第一帧";  // 下载完成后会被保存路径覆盖
            if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
        } else {
            DeleteObject(bitmap);
        }
        return;
    }

    if (msg.command == "FILE_BEGIN") {
        // 服务器开始发数据：把文件建在 received\ 里
        // 用 < 3 而不是 != 3：协议约定「新增字段只追加在末尾，老客户端读到旧格数就停」，
        // 写成 != 3 的话将来任何一次追加字段都会让这里静默失效——下载不报错地坏掉，
        // 极难排查（这次给续传加字段时就差点踩中）。
        if (fields.size() < 3) return;
        Item* item = FindFileItem(id);
        if (!item) return;
        unsigned long long total = 0;
        bool digits = !fields[2].empty() && fields[2].size() <= 20;
        for (char c : fields[2]) {
            if (c < '0' || c > '9') digits = false;
            if (digits) total = total * 10 + static_cast<unsigned long long>(c - '0');
        }
        if (!digits || total == 0 || total > dchat::kMaxFileBytes) {
            FinishDownload(id, false, "文件大小异常");
            return;
        }
        std::vector<unsigned char> nameBytes;
        const std::string name = dchat::Base64Decode(fields[1], &nameBytes)
                                     ? dchat::SanitizeFileName(std::string(nameBytes.begin(),
                                                                         nameBytes.end()))
                                     : item->raw;
        const std::wstring path = dchat::MakeUniquePathW(ReceivedDir(), Utf8ToWide(name));
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            FinishDownload(id, false, "无法写入 received\\ 目录");
            return;
        }
        auto job = std::unique_ptr<FileDownloadJob>(new FileDownloadJob());
        job->id = id;
        job->path = path;
        job->file = file;
        job->total = total;
        g_downloads[id] = std::move(job);
        item->fileState = 1;
        item->fileProgress = 0;
        TouchFileCard(id, true);
        return;
    }

    const auto it = g_downloads.find(id);
    if (it == g_downloads.end()) return;  // 不是正在下载的文件，忽略
    FileDownloadJob* job = it->second.get();

    if (msg.command == "FILE_DATA") {
        if (fields.size() != 2) return;
        std::vector<unsigned char> chunk;
        if (!dchat::Base64Decode(fields[1], &chunk)) { FinishDownload(id, false, "数据损坏"); return; }
        if (job->received + chunk.size() > job->total) {  // 对方给的数据比声明的多
            FinishDownload(id, false, "数据超出声明的大小");
            return;
        }
        if (!chunk.empty()) {
            DWORD written = 0;
            if (!WriteFile(job->file, chunk.data(), static_cast<DWORD>(chunk.size()), &written,
                           nullptr) ||
                written != chunk.size()) {
                FinishDownload(id, false, "写文件失败");
                return;
            }
            job->received += chunk.size();
        }
        const int percent = PercentOf(job->received, job->total);
        if (percent != job->lastPercent) {  // 百分比变了才刷界面
            job->lastPercent = percent;
            if (Item* item = FindFileItem(id)) item->fileProgress = percent;
            TouchFileCard(id, true);
        }
        return;
    }

    if (msg.command == "FILE_END") {
        const bool complete = job->received == job->total;
        if (!complete) {
            FinishDownload(id, false,
                           "文件不完整（还差 " + dchat::FormatBytes(job->total - job->received) + "）");
            return;
        }
        const std::wstring saved = job->path;
        std::string savedUtf8;
        std::string fileName;
        bool voice = false;
        if (Item* item = FindFileItem(id)) {
            savedUtf8 = WideToUtf8(saved);
            item->savedPath = savedUtf8;
            fileName = item->raw;
            voice = item->isVoice;
        }
        // 语音的时长**在这里读一次并存下来**，不要在绘制里读：
        // 记录区每秒都要重画（进度条在走），每帧去开一次文件纯属浪费。
        if (voice) {
            if (Item* item = FindFileItem(id)) {
                unsigned short tag = 0;
                int rate = 0, seconds = 0;
                if (dchat::ReadWavFormat(savedUtf8, &tag, &rate, &seconds)) {
                    item->voiceSeconds = seconds;
                }
            }
        }
        FinishDownload(id, true, std::string());
        MarkMessageArrived(true);
        RequestPreview(id, fileName, savedUtf8);  // 图片/视频：取缩略图（视频是第一帧）
        return;
    }
    if (msg.command == "FILE_FAIL") {
        std::string reason = fields.size() >= 2 ? fields[1] : std::string();
        for (std::size_t i = 2; i < fields.size(); ++i) reason += " " + fields[i];
        FinishDownload(id, false, reason);
    }
}

// 点卡片：让服务器把文件发过来
void StartFileDownload(const std::string& id) {
    if (!IsConnected()) {
        ViewAddItem(ItemKind::Error, "还没连接服务器，连上之后才能下载", dchat::NowTimeString());
        return;
    }
    if (g_downloads.count(id) != 0) return;  // 已经在下载了
    Item* item = FindFileItem(id);
    if (!item) return;
    if (!SendRawLine(dchat::BuildLine("FILE_GET", id))) {
        ViewAddItem(ItemKind::Error, "下载请求发送失败，连接可能已断开", dchat::NowTimeString());
        return;
    }
    item->fileState = 1;
    item->fileProgress = 0;
    item->fileNote.clear();
    TouchFileCard(id, true);
}

// 用资源管理器打开文件所在目录，并把文件选中（QQ「打开文件夹」就是这个效果）
void OpenContainingFolder(const std::wstring& path) {
    const std::wstring args = L"/select,\"" + path + L"\"";
    const INT_PTR result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32) {  // 退化：直接打开目录
        const std::size_t slash = path.find_last_of(L"\\/");
        const std::wstring dir = slash == std::wstring::npos ? path : path.substr(0, slash);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// 点到了文件卡片：能下载就下载，下完了就打开文件夹，失败就重试
void ActivateFileCard(const std::string& id) {
    Item* item = FindFileItem(id);
    if (!item) return;
    if (item->fileState == 1) return;  // 下载中：忽略点击
    if (item->fileState == 2) {
        if (!item->savedPath.empty()) OpenContainingFolder(Utf8ToWide(item->savedPath));
        return;
    }
    StartFileDownload(id);
}

// 「发送文件」按钮：选一个文件然后发出去
void DoSendFile(HWND hwnd) {
    if (!IsConnected()) {
        ViewAddItem(ItemKind::Error, "还没连接服务器，先连接再发文件", dchat::NowTimeString());
        return;
    }
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"所有文件\0*.*\0文本文件\0*.txt;*.md;*.log\0图片\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要发送的文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    StartSendFile(path);
}

// ---------------- 连接对话框 ----------------
// 外观和主窗口保持一致：客户区自己用主题色绘制，圆角、输入框底板、按钮
// 全部走 GDI+ 自绘（ui::DrawRoundedControl），编辑框是无边框的、内嵌在底板里。
constexpr int kDlgWidth = 396;
constexpr int kDlgPad = 20;
constexpr int kDlgLabelW = 76;
constexpr int kDlgFieldGap = 12;
constexpr int kDlgFieldX = kDlgPad + kDlgLabelW + kDlgFieldGap;
constexpr int kDlgFieldW = kDlgWidth - kDlgPad - kDlgFieldX;
constexpr int kDlgFieldH = 30;
constexpr int kDlgRowH = 36;
constexpr int kDlgFirstRowY = 76;
constexpr int kDlgTextPadX = 10;
constexpr int kDlgTextPadY = 4;
constexpr int kDlgFieldRadius = 8;
constexpr int kDlgCheckH = 22;
constexpr int kDlgCheckW = 120;
constexpr int kDlgTipH = 18;
constexpr int kDlgButtonH = 32;
constexpr int kDlgButtonW = 120;      // 主按钮（连接 / 登录 / 注册并登录）
constexpr int kDlgCancelW = 78;       // 「断开」
constexpr int kDlgSwitchW = 150;      // 「没有账号？注册新账号」/「已有账号？去登录」
constexpr int kDlgButtonGap = 8;
// 注意：这里不能用 WS_CLIPCHILDREN —— 输入框的圆角底板是靠父窗口画在编辑框
// 底下的，加了它就会把编辑框那块区域裁掉，编辑框只剩自己的白底。
constexpr DWORD kDlgStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_POPUP;
constexpr DWORD kDlgExStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;

// 对话框的纵向排布：N 行输入框 →（可选的「显示密码」）→ 提示行 → 按钮行
struct DialogLayout {
    int checkY = 0;   // 「显示密码」那一行的顶端
    int tipY = 0;     // 灰色/红色提示行
    int buttonY = 0;
    int height = 0;   // 客户区高度
};

DialogLayout ComputeDialogLayout(int rows, bool withShowCheck) {
    DialogLayout out;
    int y = kDlgFirstRowY + rows * kDlgRowH - 2;
    out.checkY = y;
    if (withShowCheck) y += kDlgCheckH + 6;
    out.tipY = y;
    out.buttonY = out.tipY + kDlgTipH + 8;
    out.height = out.buttonY + kDlgButtonH + 14;
    return out;
}

// ---------------- 第一步：连接（只填服务器地址 + 端口）----------------
struct ConnectDialogState {
    std::string host;
    int port = dchat::kDefaultPort;
    bool accepted = false;
    bool done = false;
    std::wstring error;   // 校验失败的原因：显示在对话框里，不弹系统消息框
    RECT fieldRect[2]{};  // 两行输入框的圆角底板（自绘用）
    HWND edits[2]{};      // 两个编辑框（判断焦点高亮用）
    HBRUSH fieldBrush = nullptr;  // 编辑框自己的底色（和圆角底板同色），随对话框创建/销毁
};

// 「显示密码」：把密码框的遮挡字符设成 0（明文）或 ●（默认的圆点）。
// 改完之后要连父窗口一起重画：编辑框自己只重画文字，而它底下的圆角底板
// 是父窗口画的，不一起重画就会留下上一次的点号/明文。
void ApplyPasswordMask(HWND dlg, const HWND* edits, const RECT* rects, int count,
                       bool showPassword) {
    const WPARAM mask = showPassword ? 0 : static_cast<WPARAM>(0x25CF);
    for (int i = 0; i < count; ++i) {
        if (!edits[i]) continue;
        SendMessageW(edits[i], EM_SETPASSWORDCHAR, mask, 0);
        if (dlg && rects) InvalidateRect(dlg, &rects[i], TRUE);
        RedrawWindow(edits[i], nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

LRESULT CALLBACK ConnectDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<ConnectDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<ConnectDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            // 编辑框自带的底色：和圆角底板同色，这样它能自己把内容擦干净
            state->fieldBrush = CreateSolidBrush(g_palette->bubbleOther);

            const std::wstring values[2] = {Utf8ToWide(state->host), std::to_wstring(state->port)};
            const int ids[2] = {IDD_HOST, IDD_PORT};
            for (int i = 0; i < 2; ++i) {
                const int top = kDlgFirstRowY + i * kDlgRowH;
                state->fieldRect[i] =
                    RECT{kDlgFieldX, top, kDlgFieldX + kDlgFieldW, top + kDlgFieldH};
                // 无边框 + 内缩：圆角底板由父窗口画，文字因此自带内边距
                state->edits[i] = CreateWindowW(
                    L"EDIT", values[i].c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    state->fieldRect[i].left + kDlgTextPadX, top + kDlgTextPadY,
                    kDlgFieldW - kDlgTextPadX * 2, kDlgFieldH - kDlgTextPadY * 2, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), nullptr, nullptr);
                SendMessageW(state->edits[i], WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            }

            // 两个圆角自绘按钮：主操作靠右（和主窗口的「发送」一样用强调色）
            const DialogLayout layout = ComputeDialogLayout(2, false);
            HWND cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                kDlgWidth - kDlgPad - kDlgButtonW - kDlgButtonGap - kDlgCancelW, layout.buttonY,
                kDlgCancelW, kDlgButtonH, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_CANCEL)), nullptr, nullptr);
            HWND ok = CreateWindowW(
                L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                kDlgWidth - kDlgPad - kDlgButtonW, layout.buttonY, kDlgButtonW, kDlgButtonH, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_OK)), nullptr, nullptr);
            const HWND buttons[2] = {ok, cancel};
            for (HWND button : buttons) {
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
                // 复用主窗口按钮那套自绘 + 悬停/按下反馈
                SubclassControl(button, ButtonProc);
            }
            return 0;
        }
        case WM_DESTROY:
            if (state && state->fieldBrush) {
                DeleteObject(state->fieldBrush);
                state->fieldBrush = nullptr;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 背景在 WM_PAINT 里用主题色画，避免先闪一下系统底色
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(g_palette->windowBg);
            FillRect(dc, &client, bg);
            DeleteObject(bg);

            // 标题 + 副标题 + 一条分隔线，做出层次
            const RECT titleRect{kDlgPad, 14, client.right - kDlgPad, 40};
            DrawTextIn(dc, L"连接到聊天服务器", titleRect, ui.fontLarge, g_palette->text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            const RECT subRect{kDlgPad, 42, client.right - kDlgPad, 60};
            DrawTextIn(dc, L"先连上服务器，连上之后再登录或注册账号", subRect,
                       ui.fontSmall, g_palette->system,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            HBRUSH lineBrush = CreateSolidBrush(g_palette->border);
            const RECT lineRect{kDlgPad, 66, client.right - kDlgPad, 67};
            FillRect(dc, &lineRect, lineBrush);
            DeleteObject(lineBrush);

            // 两行：右对齐标签 + 圆角输入底板（有焦点的那行用强调色描边）
            const wchar_t* labels[2] = {L"服务器地址", L"端口"};
            for (int i = 0; i < 2; ++i) {
                const RECT labelRect{kDlgPad, state->fieldRect[i].top, kDlgFieldX - 10,
                                     state->fieldRect[i].bottom};
                DrawTextIn(dc, labels[i], labelRect, ui.font, g_palette->system,
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                const bool focused = GetFocus() == state->edits[i];
                ui::DrawRoundedControl(dc, state->fieldRect[i], kDlgFieldRadius,
                                       g_palette->windowBg, g_palette->bubbleOther,
                                       focused ? g_palette->accent : g_palette->border);
            }

            // 底部一行：平时是灰色提示，校验失败时换成红色原因
            const bool hasError = !state->error.empty();
            const std::wstring tip =
                hasError ? state->error
                         : L"本机测试填 127.0.0.1；连上以后再填账号";
            const DialogLayout layout = ComputeDialogLayout(2, false);
            const RECT tipRect{kDlgPad, layout.tipY, client.right - kDlgPad,
                               layout.tipY + kDlgTipH};
            DrawTextIn(dc, tip, tipRect, ui.fontSmall,
                       hasError ? g_palette->error : g_palette->time,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            DrawOwnerButton(item);  // 连接对话框里只有按钮（勾选框在账号窗口那边）
            return TRUE;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            // 给编辑框一个**实心**画刷（和圆角底板同色）：它每次重画都会先把自己
            // 这一小块擦干净。之前返回空画刷（"透明"）虽然能让父窗口的圆角透出来，
            // 但只要编辑框自己重画过（切换显示密码、删字符），旧内容就会留在原地，
            // 和新文字叠在一起——就是"点号 + 明文糊成一团"那个 bug。
            // 编辑框内缩在圆角底板里（左右 10px / 上下 4px），用同色擦掉不影响圆角。
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g_palette->text);
            SetBkColor(dc, g_palette->bubbleOther);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(state && state->fieldBrush
                                                 ? state->fieldBrush
                                                 : GetStockObject(NULL_BRUSH));
        }
        case WM_COMMAND: {
            if (!state) return 0;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                // 焦点换了一行：重画，把强调色边框挪过去
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (code == EN_CHANGE) {
                if (!state->error.empty()) {  // 用户开始改了，把上一次的红色提示清掉
                    state->error.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (id == IDD_OK) {
                // 校验失败时把原因写在对话框底部、并把焦点挪到那一行，
                // 不弹系统消息框（深色主题下它会是一块刺眼的浅色）
                auto fail = [&](const wchar_t* text, int field) {
                    state->error = text;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    if (field >= 0 && field < 2 && state->edits[field]) {
                        SetFocus(state->edits[field]);
                    }
                };
                wchar_t host[128] = {0};
                wchar_t port[32] = {0};
                GetDlgItemTextW(hwnd, IDD_HOST, host, 128);
                GetDlgItemTextW(hwnd, IDD_PORT, port, 32);
                const int parsedPort = _wtoi(port);
                if (WideToUtf8(host).empty()) {
                    fail(L"请填写服务器地址（本机测试用 127.0.0.1）", 0);
                    return 0;
                }
                if (parsedPort <= 0 || parsedPort > 65535) {
                    fail(L"端口请填 1-65535（服务器默认 5555）", 1);
                    return 0;
                }
                state->host = WideToUtf8(host);
                state->port = parsedPort;
                state->accepted = true;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDD_CANCEL) {
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if (state) state->done = true;
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool PromptConnect(HWND parent, ConnectDialogState& state) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ConnectDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // 客户区背景由 WM_PAINT 用主题色绘制
        wc.lpszClassName = L"DchatConnectDlg";
        RegisterClassExW(&wc);
        registered = true;
    }
    const DialogLayout layout = ComputeDialogLayout(2, false);
    RECT rc{0, 0, kDlgWidth, layout.height};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    // 居中显示在主窗口上（比 CW_USEDEFAULT 的左上角好看得多），并保证不跑出屏幕
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
        if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
        if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    }

    HWND dlg = CreateWindowExW(kDlgExStyle, L"DchatConnectDlg", L"连接到聊天服务器", kDlgStyle, x, y,
                               width, height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return false;
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_DCHAT));
    if (icon) {  // 标题栏左上角也放上应用图标，和任务栏/主窗口一致
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
    ApplySystemDarkMode(dlg);  // 标题栏跟随主题（客户区颜色由自绘负责）
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    // 地址是记住的：进来就全选，直接敲就能换一台服务器
    SetFocus(GetDlgItem(dlg, IDD_HOST));
    SendDlgItemMessageW(dlg, IDD_HOST, EM_SETSEL, 0, -1);
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 回车 = 提交，Esc = 取消。这个窗口不是真正的对话框，
        // 系统不会自动找默认按钮，所以这里自己处理。
        const bool mine = msg.hwnd == dlg || IsChild(dlg, msg.hwnd);
        if (mine && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDD_OK, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDD_CANCEL, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return state.accepted;
}

// ---------------- 第二步：登录 / 注册（两个独立的界面）----------------
// 连上服务器之后才弹出来。登录和注册是**两个不同的窗口**（标题栏、标题、输入框、
// 按钮都不一样）：登录窗口只有用户名 + 密码，注册窗口多一个确认密码。
// 左下角的按钮在两者之间切换，已经填好的内容会带到另一个界面，不用重打。
constexpr int IDD_SWITCH = 106;  // 「没有账号？注册新账号」/「已有账号？去登录」

struct AuthDialogState {
    HWND parent = nullptr;
    std::string host;             // 显示用："已连接到 host:port"
    int port = dchat::kDefaultPort;
    bool isRegister = false;      // 当前显示的是哪个界面
    bool pending = false;         // 请求已经发出去，正等服务器回答
    bool accepted = false;        // 登录 / 注册成功
    bool done = false;            // 该关窗口了（成功、取消、或出错退出）
    bool showPassword = false;    // 「显示密码」勾选框
    std::wstring error;           // 红色提示（服务器拒绝的原因 / 本地校验失败）
    std::wstring info;            // 灰色提示（"正在登录…"）
    std::string user;             // 提交出去的用户名（成功后就是昵称）
    // 切换界面时把已经填的内容带过去
    std::wstring prefillUser, prefillPassword, prefillConfirm;
    HWND dlg = nullptr;           // 当前显示的窗口（登录窗口或注册窗口）
    HWND edits[3]{};              // 用户名 / 密码 / 确认密码（注册界面才用第三个）
    RECT fieldRect[3]{};          // 三行输入框的圆角底板
    HBRUSH fieldBrush = nullptr;
    DialogLayout layout;
};

// 打开的账号窗口：服务器回了 ERROR / LOGGEDIN 时，把结果写进这个窗口（没开就是 nullptr）
AuthDialogState* g_authDlg = nullptr;

int AuthRowCount(const AuthDialogState& state) { return state.isRegister ? 3 : 2; }

void SwitchAuthWindow(AuthDialogState* state, bool toRegister);  // 定义在下面

void LayoutAuthWindow(AuthDialogState* state) {
    if (!state) return;
    const int rows = AuthRowCount(*state);
    state->layout = ComputeDialogLayout(rows, true);
    for (int i = 0; i < 3; ++i) {
        const int top = kDlgFirstRowY + i * kDlgRowH;
        state->fieldRect[i] = RECT{kDlgFieldX, top, kDlgFieldX + kDlgFieldW, top + kDlgFieldH};
    }
    if (!state->dlg) return;
    RECT rc{0, 0, kDlgWidth, state->layout.height};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    RECT wr{};
    GetWindowRect(state->dlg, &wr);
    SetWindowPos(state->dlg, nullptr, wr.left, wr.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < 3; ++i) {
        const bool used = i < rows;
        ShowWindow(state->edits[i], used ? SW_SHOW : SW_HIDE);
        if (!used) continue;
        SetWindowPos(state->edits[i], nullptr, state->fieldRect[i].left + kDlgTextPadX,
                     state->fieldRect[i].top + kDlgTextPadY, kDlgFieldW - kDlgTextPadX * 2,
                     kDlgFieldH - kDlgTextPadY * 2, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(GetDlgItem(state->dlg, IDD_SHOW), nullptr, kDlgFieldX, state->layout.checkY,
                 kDlgCheckW, kDlgCheckH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(state->dlg, IDD_SWITCH), nullptr, kDlgPad, state->layout.buttonY,
                 kDlgSwitchW, kDlgButtonH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(state->dlg, IDD_CANCEL), nullptr,
                 kDlgWidth - kDlgPad - kDlgButtonW - kDlgButtonGap - kDlgCancelW,
                 state->layout.buttonY, kDlgCancelW, kDlgButtonH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(state->dlg, IDD_OK), nullptr, kDlgWidth - kDlgPad - kDlgButtonW,
                 state->layout.buttonY, kDlgButtonW, kDlgButtonH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(state->dlg, nullptr, TRUE);
}

// 服务器对 LOGIN / REGISTER 的回答：写进还开着的账号窗口。
// ok = true 表示认证通过（窗口自动关掉，回到主窗口）；
// ok = false 时把原因显示成红色提示，连接不动，用户可以改完再提交、或者切到另一个界面。
void AuthDialogOnResult(bool ok, const std::wstring& text) {
    AuthDialogState* state = g_authDlg;
    if (!state) return;
    state->pending = false;
    if (ok) {
        state->accepted = true;
        state->done = true;
        if (state->dlg) DestroyWindow(state->dlg);
        return;
    }
    state->error = text;
    state->info.clear();
    if (state->dlg) {
        // 失败后不需要改尺寸，重画就行（提示行会把红色原因显示出来）
        InvalidateRect(state->dlg, nullptr, TRUE);
    }
}

LRESULT CALLBACK AuthDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AuthDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<AuthDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->dlg = hwnd;
            state->fieldBrush = CreateSolidBrush(g_palette->bubbleOther);

            const std::wstring values[3] = {state->prefillUser, state->prefillPassword,
                                            state->prefillConfirm};
            const int ids[3] = {IDD_USER, IDD_PASSWORD, IDD_CONFIRM};
            for (int i = 0; i < 3; ++i) {
                const bool isPassword = i >= 1;
                state->edits[i] = CreateWindowW(
                    L"EDIT", values[i].c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                        (isPassword ? ES_PASSWORD : 0),
                    0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])),
                    nullptr, nullptr);
                SendMessageW(state->edits[i], WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            }
            HWND ok = CreateWindowW(L"BUTTON", state->isRegister ? L"注册并登录" : L"登录",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_OK)),
                                    nullptr, nullptr);
            HWND cancel = CreateWindowW(L"BUTTON", L"断开", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                              BS_OWNERDRAW,
                                        0, 0, 10, 10, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_CANCEL)),
                                        nullptr, nullptr);
            HWND sw = CreateWindowW(
                L"BUTTON", state->isRegister ? L"已有账号？去登录" : L"没有账号？注册新账号",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_SWITCH)), nullptr, nullptr);
            HWND show = CreateWindowW(L"BUTTON", L"显示密码", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                              BS_OWNERDRAW,
                                      0, 0, 10, 10, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_SHOW)),
                                      nullptr, nullptr);
            const HWND buttons[4] = {ok, cancel, sw, show};
            for (HWND button : buttons) {
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
                SubclassControl(button, ButtonProc);
            }
            LayoutAuthWindow(state);
            ApplyPasswordMask(hwnd, state->edits + 1, state->fieldRect + 1, 2,
                              state->showPassword);
            return 0;
        }
        case WM_DESTROY:
            if (state && state->fieldBrush) {
                DeleteObject(state->fieldBrush);
                state->fieldBrush = nullptr;
            }
            if (state && state->dlg == hwnd) state->dlg = nullptr;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(g_palette->windowBg);
            FillRect(dc, &client, bg);
            DeleteObject(bg);

            const bool reg = state->isRegister;
            const RECT titleRect{kDlgPad, 14, client.right - kDlgPad, 40};
            DrawTextIn(dc, reg ? L"注册新账号" : L"登录", titleRect, ui.fontLarge, g_palette->text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            const RECT subRect{kDlgPad, 42, client.right - kDlgPad, 60};
            const std::wstring sub = L"已连接到 " + Utf8ToWide(state->host) + L":" +
                                     std::to_wstring(state->port) +
                                     (reg ? L"　注册成功后会自动登录" : L"　用已有账号登录");
            DrawTextIn(dc, sub, subRect, ui.fontSmall, g_palette->system,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            HBRUSH lineBrush = CreateSolidBrush(g_palette->border);
            const RECT lineRect{kDlgPad, 66, client.right - kDlgPad, 67};
            FillRect(dc, &lineRect, lineBrush);
            DeleteObject(lineBrush);

            const wchar_t* labels[3] = {L"用户名", L"密码", L"确认密码"};
            const int rows = AuthRowCount(*state);
            for (int i = 0; i < rows; ++i) {
                const RECT labelRect{kDlgPad, state->fieldRect[i].top, kDlgFieldX - 10,
                                     state->fieldRect[i].bottom};
                DrawTextIn(dc, labels[i], labelRect, ui.font, g_palette->system,
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                const bool focused = GetFocus() == state->edits[i];
                ui::DrawRoundedControl(dc, state->fieldRect[i], kDlgFieldRadius,
                                       g_palette->windowBg, g_palette->bubbleOther,
                                       focused ? g_palette->accent : g_palette->border);
            }

            const bool hasError = !state->error.empty();
            const std::wstring tip =
                hasError ? state->error
                         : (state->pending
                                ? state->info
                                : (reg ? L"确认密码要和上面一致；注册成功就自动进房间"
                                       : L"还没有账号？点左下角「没有账号？注册新账号」"));
            const RECT tipRect{kDlgPad, state->layout.tipY, client.right - kDlgPad,
                               state->layout.tipY + kDlgTipH};
            DrawTextIn(dc, tip, tipRect, ui.fontSmall,
                       hasError ? g_palette->error : g_palette->time,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            // 「显示密码」是勾选框，其余按钮走通用自绘
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (item && item->CtlID == IDD_SHOW) {
                DrawOwnerCheckbox(item, state ? state->showPassword : false, ui.font);
            } else {
                DrawOwnerButton(item);
            }
            return TRUE;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g_palette->text);
            SetBkColor(dc, g_palette->bubbleOther);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(state && state->fieldBrush ? state->fieldBrush
                                                                        : GetStockObject(NULL_BRUSH));
        }
        case WM_COMMAND: {
            if (!state) return 0;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE);  // 焦点边框跟着挪
                return 0;
            }
            if (code == EN_CHANGE) {
                if (!state->error.empty()) {  // 开始改了就把上一次的红色提示清掉
                    state->error.clear();
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }
            if (id == IDD_SHOW) {
                state->showPassword = !state->showPassword;
                ApplyPasswordMask(hwnd, state->edits + 1, state->fieldRect + 1, 2,
                                  state->showPassword);
                InvalidateRect(GetDlgItem(hwnd, IDD_SHOW), nullptr, TRUE);
                return 0;
            }
            if (id == IDD_SWITCH) {
                if (state->pending) return 0;  // 正在等服务器回答，先别切界面
                SwitchAuthWindow(state, !state->isRegister);
                return 0;
            }
            if (id == IDD_OK) {
                if (state->pending) return 0;  // 别重复提交
                auto fail = [&](const wchar_t* text, int field) {
                    state->error = text;
                    state->info.clear();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    if (field >= 0 && field < 3 && state->edits[field]) {
                        SetFocus(state->edits[field]);
                    }
                };
                wchar_t user[64] = {0};
                wchar_t password[128] = {0};
                wchar_t confirm[128] = {0};
                GetDlgItemTextW(hwnd, IDD_USER, user, 64);
                GetDlgItemTextW(hwnd, IDD_PASSWORD, password, 128);
                if (state->isRegister) GetDlgItemTextW(hwnd, IDD_CONFIRM, confirm, 128);

                dchat::NickError nickError = dchat::NickError::None;
                const std::string normalizedUser = dchat::NormalizeNick(WideToUtf8(user), &nickError);
                const std::string passwordText = WideToUtf8(password);
                dchat::PasswordError passwordError = dchat::PasswordError::None;
                if (normalizedUser.empty()) {
                    fail(Utf8ToWide(dchat::NickErrorText(nickError)).c_str(), 0);
                    return 0;
                }
                if (!dchat::ValidatePassword(passwordText, &passwordError)) {
                    fail(Utf8ToWide(dchat::PasswordErrorText(passwordError)).c_str(), 1);
                    return 0;
                }
                if (state->isRegister) {
                    const std::string confirmText = WideToUtf8(confirm);
                    if (confirmText.empty()) {
                        fail(L"请再输一遍密码（确认密码）", 2);
                        return 0;
                    }
                    if (confirmText != passwordText) {
                        fail(L"两次输入的密码不一致", 2);
                        return 0;
                    }
                }
                state->user = normalizedUser;
                g_pendingUser = normalizedUser;  // 登录成功后据此写"记住账号"
                g_pendingPassword = passwordText;
                state->prefillUser = user;
                state->prefillPassword = password;
                state->prefillConfirm = confirm;
                state->pending = true;
                state->error.clear();
                state->info = state->isRegister ? L"正在注册…" : L"正在登录…";
                g_authPending = true;
                const std::string line = dchat::BuildLine(
                    state->isRegister ? "REGISTER" : "LOGIN", normalizedUser + " " + passwordText);
                if (!SendRawLine(line)) {
                    state->pending = false;
                    g_authPending = false;
                    fail(L"发送登录请求失败，连接可能已断开", -1);
                    return 0;
                }
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (id == IDD_CANCEL) {  // 「断开」：整个登录流程结束（调用方负责断开连接）
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if (state) state->done = true;
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 打开当前模式对应的窗口（登录窗口 / 注册窗口）
HWND OpenAuthWindow(AuthDialogState* state, int x, int y) {
    const wchar_t* cls = state->isRegister ? L"DchatRegisterDlg" : L"DchatLoginDlg";
    const wchar_t* title = state->isRegister ? L"注册新账号" : L"登录";
    const DialogLayout layout = ComputeDialogLayout(AuthRowCount(*state), true);
    RECT rc{0, 0, kDlgWidth, layout.height};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    HWND dlg = CreateWindowExW(kDlgExStyle, cls, title, kDlgStyle, x, y, rc.right - rc.left,
                               rc.bottom - rc.top, state->parent, nullptr, GetModuleHandleW(nullptr),
                               state);
    if (!dlg) return nullptr;
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_DCHAT));
    if (icon) {
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
    ApplySystemDarkMode(dlg);
    return dlg;
}

// 在「登录」和「注册」两个界面之间切换：记下已经填的内容，换成另一个窗口显示
void SwitchAuthWindow(AuthDialogState* state, bool toRegister) {
    if (!state || !state->dlg) return;
    wchar_t user[64] = {0};
    wchar_t password[128] = {0};
    wchar_t confirm[128] = {0};
    GetDlgItemTextW(state->dlg, IDD_USER, user, 64);
    GetDlgItemTextW(state->dlg, IDD_PASSWORD, password, 128);
    if (state->isRegister) GetDlgItemTextW(state->dlg, IDD_CONFIRM, confirm, 128);
    state->prefillUser = user;
    state->prefillPassword = password;
    state->prefillConfirm = confirm;

    RECT wr{};
    GetWindowRect(state->dlg, &wr);
    HWND old = state->dlg;
    state->dlg = nullptr;  // 先清掉：WM_DESTROY 里就不会再动它
    DestroyWindow(old);

    state->isRegister = toRegister;
    state->error.clear();
    state->info.clear();
    HWND dlg = OpenAuthWindow(state, wr.left, wr.top);
    if (!dlg) {
        state->done = true;  // 窗口建不出来就别卡着了
        return;
    }
    ShowWindow(dlg, SW_SHOW);
    SetFocus(GetDlgItem(dlg, IDD_USER));
    SendDlgItemMessageW(dlg, IDD_USER, EM_SETSEL, 0, -1);
}

#include <dpapi.h>  // CryptProtectData / DATA_BLOB：自动登录加密保存密码

// ---------------------------------------------------------------------------
// 记住账号（自动登录）
//
// **密码绝不明文落盘**。用 Windows 的 DPAPI（CryptProtectData）：
//   - 密文只有**同一个 Windows 账户**能解开，换个用户或拷到别的机器都无效；
//   - 密钥由系统托管，我们不用管它存在哪、也不用自己发明加密方式。
// 这比自己搞个异或/Base64 强得多，也比"明文存一个 txt"安全得多。
//
// 文件在当前目录的 dchat-login.dat，格式：
//   第一行：host \t port \t user
//   之后：DPAPI 密文的原始字节
// ---------------------------------------------------------------------------
constexpr const char* kLoginReminderFile = "dchat-login.dat";

std::string DpapiProtect(const std::string& plain) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"dchat login", nullptr, nullptr, nullptr, 0, &out)) {
        return std::string();
    }
    std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

std::string DpapiUnprotect(const std::string& blob) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()));
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return std::string();
    }
    std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

struct SavedLogin {
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
};

bool SaveLoginReminder(const std::string& host, int port, const std::string& user,
                       const std::string& password) {
    if (user.empty()) return false;
    const std::string secret = DpapiProtect(password);
    if (secret.empty()) return false;
    std::ofstream out(kLoginReminderFile, std::ios::binary);
    if (!out) return false;
    out << host << '\t' << port << '\t' << user << '\n';
    out.write(secret.data(), static_cast<std::streamsize>(secret.size()));
    return out.good();
}

bool LoadLoginReminder(SavedLogin* saved) {
    if (saved == nullptr) return false;
    std::ifstream in(kLoginReminderFile, std::ios::binary);
    if (!in) return false;
    std::string head;
    if (!std::getline(in, head)) return false;
    if (!head.empty() && head.back() == '\r') head.pop_back();
    std::string secret((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (secret.empty()) return false;
    // 第一行拆成 host / port / user
    std::vector<std::string> fields;
    std::size_t from = 0;
    while (true) {
        const std::size_t tab = head.find('\t', from);
        fields.push_back(head.substr(from, tab == std::string::npos ? std::string::npos
                                                                    : tab - from));
        if (tab == std::string::npos) break;
        from = tab + 1;
    }
    if (fields.size() < 3 || fields[2].empty()) return false;
    saved->host = fields[0];
    saved->port = std::atoi(fields[1].c_str());
    saved->user = fields[2];
    saved->password = DpapiUnprotect(secret);
    return !saved->password.empty();
}

/**
 * 服务端回「密码已修改」之后，把本机记住的密码换成刚才提交的新密码。
 *
 * 为什么必须做：改完密码，自动登录/断线重连还在用旧密码 —— 用户看到的只是
 * 「密码错误」，根本想不到是自己刚改过。**只在服务端确认之后才换**：服务器可能
 * 因为密码太短、参数写错而拒绝，提前换会让记住的密码和真实密码不一致。
 *
 * 抽成独立函数是为了能自检（--selftest-password-sync 直接调它），
 * 界面那条路（合成按键送不进编辑框）在这台机器上没法自动化。
 *
 * 返回 true 表示真的换了。
 */
bool AdoptNewPasswordIfConfirmed(const std::string& systemText) {
    if (g_pendingNewPassword.empty()) return false;
    if (systemText.find("密码已修改") == std::string::npos) return false;
    if (GetTickCount64() - g_pendingNewPasswordAt >= kPasswordChangeTimeoutMs) {
        g_pendingNewPassword.clear();  // 太久以前的不认
        return false;
    }
    g_pendingPassword = g_pendingNewPassword;
    g_pendingNewPassword.clear();
    if (g_pendingUser.empty()) return true;  // 没记住账号，内存里的换了就行
    SaveLoginReminder(g_host, g_port, g_pendingUser, g_pendingPassword);
    return true;
}

void ClearLoginReminder() {
    std::remove(kLoginReminderFile);
}

bool PromptAuth(HWND parent, AuthDialogState& state, bool forceAutoLogin = false) {
    static bool registered = false;
    if (!registered) {
        const wchar_t* classes[2] = {L"DchatLoginDlg", L"DchatRegisterDlg"};
        for (const wchar_t* name : classes) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = AuthDialogProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;  // 客户区背景由 WM_PAINT 用主题色绘制
            wc.lpszClassName = name;
            RegisterClassExW(&wc);
        }
        registered = true;
    }
    state.parent = parent;

    // ---- 自动登录 ----
    //
    // 只在**本次进程第一次**弹账号窗口时尝试，而且要求存下来的服务器地址和当前
    // 要连的一致。否则用户想换个账号/换台服务器的时候会被自动登录顶回去。
    static bool autoLoginTried = false;
    bool autoSubmit = false;
    // forceAutoLogin：断线重连时用。这时候进程早就不是第一次弹这个窗口了，
    // 但**必须**再自动登一次，否则用户得在每次掉线后手打一遍密码。
    if (!autoLoginTried || forceAutoLogin) {
        autoLoginTried = true;
        SavedLogin saved;
        if (LoadLoginReminder(&saved) && saved.host == g_host && saved.port == g_port) {
            state.prefillUser = Utf8ToWide(saved.user);
            state.prefillPassword = Utf8ToWide(saved.password);
            autoSubmit = true;
        }
    }

    const DialogLayout layout = ComputeDialogLayout(AuthRowCount(state), true);
    RECT rc{0, 0, kDlgWidth, layout.height};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
        if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
        if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    }
    HWND dlg = OpenAuthWindow(&state, x, y);
    if (!dlg) return false;
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(GetDlgItem(dlg, IDD_USER));
    g_authDlg = &state;

    // 自动登录：走**和点「登录」完全一样的那条路径**（同一个 WM_COMMAND 处理），
    // 所以账号规范化、校验、失败提示全都一致，不会多出一套"自动登录专用"逻辑。
    if (autoSubmit && state.dlg) {
        PostMessageW(state.dlg, WM_COMMAND, MAKEWPARAM(IDD_OK, BN_CLICKED), 0);
    }

    // 模态循环：界面可能在登录 / 注册之间切换，所以每轮都重新取"当前窗口"。
    // 回车 = 主按钮，Esc = 断开（这个窗口不是真正的对话框，系统不会自动找默认按钮）。
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND current = state.dlg;
        const bool mine = current && (msg.hwnd == current || IsChild(current, msg.hwnd));
        if (mine && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(current, WM_COMMAND, MAKEWPARAM(IDD_OK, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(current, WM_COMMAND, MAKEWPARAM(IDD_CANCEL, BN_CLICKED), 0);
                continue;
            }
        }
        if (!current || !IsDialogMessageW(current, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    g_authDlg = nullptr;
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return state.accepted;
}

// ---------------- 设置窗口 ----------------
//
// 「偶尔才改一次」的东西全在这里：主题、彩色聊天、服务器地址端口、语音时长。
// 以前这些是顶栏的常驻按钮（彩色聊天 / 浅色 / 连接 / 断开 / 录音 / 发送文件），
// 天天占着一条，还让人分不清哪个是"状态"、哪个是"操作"。
//
// 样式和连接/登录窗口保持一致：自绘圆角、系统标题栏下不刺眼。
constexpr int IDS_HOST = 200;
constexpr int IDS_PORT = 201;
constexpr int IDS_OK = 202;
constexpr int IDS_CANCEL = 203;

// 语音时长的可选项（秒）。上限还受 voice_notes 的 2 MB 字节数约束，这里不给出超过它的值。
constexpr int kVoiceLimitOptions[3] = {30, 60, 120};
constexpr const wchar_t* kVoiceLimitLabels[3] = {L"30 秒", L"1 分钟", L"2 分钟"};
constexpr const wchar_t* kThemeChoiceLabels[3] = {L"深色", L"浅色", L"跟随系统"};

struct SettingsState {
    HWND dlg = nullptr;
    HWND hostEdit = nullptr;
    HWND portEdit = nullptr;
    HBRUSH fieldBrush = nullptr;
    dchat::SettingsLayout layout;
    // 打开时的快照；点「完成」才写回全局，点「取消」/ Esc 原样丢弃
    ThemeMode themeMode = ThemeMode::Dark;
    bool colorEnabled = true;
    int voiceLimit = 60;
    int themeChoice = 0;
    int voiceChoice = 1;
    int themeHover = -1;
    int voiceHover = -1;
    bool accepted = false;
};

// 三段式选择器：画出来（点中的那一项由调用方解释）
void DrawSegment(HDC dc, const RECT& rect, const wchar_t* const* labels, int count, int selected,
                 HFONT font, int hovered) {
    ui::DrawRoundedControl(dc, rect, 8, g_palette->panel, g_palette->neutral,
                           g_palette->neutralBorder, g_palette->panel);
    const int width = (rect.right - rect.left) / (count > 0 ? count : 1);
    for (int i = 0; i < count; ++i) {
        RECT part{rect.left + i * width + 2, rect.top + 2, rect.left + (i + 1) * width - 2,
                  rect.bottom - 2};
        const bool on = (i == selected);
        if (on) {
            ui::FillRoundedRect(dc, part, 6, g_palette->accent, g_palette->accent, 0.0f);
        } else if (i == hovered) {
            ui::FillRoundedRect(dc, part, 6, g_palette->hover, g_palette->hover, 0.0f);
        }
        DrawTextIn(dc, labels[i], part, font, on ? g_palette->accentText : g_palette->system,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 开关（彩色聊天）：轨道 + 圆点
void DrawToggle(HDC dc, const RECT& track, bool on) {
    ui::DrawRoundedControl(dc, track, (track.bottom - track.top) / 2, g_palette->panel,
                           on ? g_palette->accent : g_palette->neutral,
                           on ? g_palette->accent : g_palette->neutralBorder);
    const int knobSize = (track.bottom - track.top) - 4;
    RECT knob{on ? track.right - knobSize - 2 : track.left + 2, track.top + 2,
              on ? track.right - 2 : track.left + 2 + knobSize, track.bottom - 2};
    const COLORREF knobColor = on ? g_palette->accentText : g_palette->text;
    ui::FillRoundedRect(dc, knob, knobSize / 2, knobColor, knobColor, 0.0f);
}

void LayoutSettingsWindow(SettingsState* state) {
    RECT client{};
    GetClientRect(state->dlg, &client);
    state->layout = dchat::LayoutSettings(client.right, client.bottom);
    const dchat::SettingsLayout& layout = state->layout;
    MoveWindow(state->hostEdit, layout.hostField.left + 12, layout.hostField.top + 5,
               (layout.hostField.right - layout.hostField.left) - 24,
               (layout.hostField.bottom - layout.hostField.top) - 10, TRUE);
    MoveWindow(state->portEdit, layout.portField.left + 12, layout.portField.top + 5,
               (layout.portField.right - layout.portField.left) - 24,
               (layout.portField.bottom - layout.portField.top) - 10, TRUE);
    MoveWindow(GetDlgItem(state->dlg, IDS_OK), layout.okButton.left, layout.okButton.top,
               layout.okButton.right - layout.okButton.left,
               layout.okButton.bottom - layout.okButton.top, TRUE);
    MoveWindow(GetDlgItem(state->dlg, IDS_CANCEL), layout.cancelButton.left,
               layout.cancelButton.top, layout.cancelButton.right - layout.cancelButton.left,
               layout.cancelButton.bottom - layout.cancelButton.top, TRUE);
}

void ApplySettingsAndClose(SettingsState* state) {
    // 校验失败时**不弹 MessageBox**：那是系统对话框，在深色界面里会闪一个白框，
    // 和整套自定义配色格格不入（用户反馈过"输入框白边"，这是同一类问题的另一处）。
    // 改成就地纠正：把合法值写回输入框，用户直接看见被改对了的内容。
    wchar_t buffer[32] = {0};
    GetWindowTextW(state->portEdit, buffer, 32);
    int port = _wtoi(buffer);
    if (port < 1 || port > 65535) {
        // 空/0/乱输 → 回到默认 5555；太大 → 夹到 65535。写回去让他看见。
        port = (port < 1) ? 5555 : 65535;
        const std::wstring fixed = std::to_wstring(port);
        SetWindowTextW(state->portEdit, fixed.c_str());
        SendMessageW(state->portEdit, EM_SETSEL, 0, -1);
        SetFocus(state->portEdit);
        return;
    }
    wchar_t host[256] = {0};
    GetWindowTextW(state->hostEdit, host, 256);
    if (host[0] == L'\0') {
        // 地址为空就退回上一次能用的那个（g_host 不会为空：启动时给过默认值）
        const std::wstring fallback = Utf8ToWide(g_host);
        SetWindowTextW(state->hostEdit, fallback.c_str());
        SendMessageW(state->hostEdit, EM_SETSEL, 0, -1);
        SetFocus(state->hostEdit);
        return;
    }

    g_host = WideToUtf8(host);
    g_port = port;
    g_colorEnabled = state->colorEnabled;
    g_voiceLimitSeconds = state->voiceLimit;
    const ThemeMode previous = g_themeMode;
    g_themeMode = state->themeMode;
    if (previous != g_themeMode) {
        ApplyThemeMode();
    } else {
        if (ui.hView) InvalidateRect(ui.hView, nullptr, TRUE);  // 彩色开关变了要重画气泡
        if (ui.hwnd) InvalidateRect(ui.hwnd, nullptr, FALSE);
    }
    state->accepted = true;
    UpdateStatus();
    DestroyWindow(state->dlg);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<SettingsState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->dlg = hwnd;

            state->themeChoice = state->themeMode == ThemeMode::Dark
                                     ? 0
                                     : (state->themeMode == ThemeMode::Light ? 1 : 2);
            for (int i = 0; i < 3; ++i) {
                if (kVoiceLimitOptions[i] == state->voiceLimit) state->voiceChoice = i;
            }

            state->hostEdit = CreateWindowExW(
                0, L"EDIT", Utf8ToWide(g_host).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_HOST)), nullptr, nullptr);
            state->portEdit = CreateWindowExW(
                0, L"EDIT", std::to_wstring(g_port).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_PORT)), nullptr, nullptr);
            const HWND ok = CreateWindowW(L"BUTTON", L"完成",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                          10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_OK)),
                                          nullptr, nullptr);
            const HWND cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_CANCEL)), nullptr, nullptr);
            // 两个编辑框：透明背景 + 实心画刷（和登录窗口同一套做法，避免重画残影）
            state->fieldBrush = CreateSolidBrush(g_palette->bubbleOther);
            SendMessageW(state->hostEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SendMessageW(state->portEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SubclassControl(ok, ButtonProc);
            SubclassControl(cancel, ButtonProc);
            SendMessageW(state->hostEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(4, 4));
            SendMessageW(state->portEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(4, 4));
            LayoutSettingsWindow(state);
            return 0;
        }
        case WM_SIZE:
            if (state) LayoutSettingsWindow(state);
            return 0;
        case WM_DESTROY:
            if (state) {
                if (state->fieldBrush) {
                    DeleteObject(state->fieldBrush);
                    state->fieldBrush = nullptr;
                }
                state->dlg = nullptr;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g_palette->text);
            SetBkColor(dc, g_palette->bubbleOther);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(state && state->fieldBrush
                                                 ? state->fieldBrush
                                                 : GetStockObject(NULL_BRUSH));
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);

            // **双缓冲**：整窗内容先画到内存 DC，最后一次性贴上去。
            // 直接画屏幕的话，悬停高亮这种"整窗重画"会露出没画完的一帧
            // ——用户看到的就是"鼠标移上去黑一闪"。
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP canvas = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(buffer, canvas);
            const int saved = SaveDC(buffer);

            HBRUSH bg = CreateSolidBrush(g_palette->panel);
            FillRect(buffer, &client, bg);
            DeleteObject(bg);
            if (!state) {
                RestoreDC(buffer, saved);
                SelectObject(buffer, oldBitmap);
                DeleteObject(canvas);
                DeleteDC(buffer);
                EndPaint(hwnd, &ps);
                return 0;
            }
            const dchat::SettingsLayout& layout = state->layout;

            DrawTextIn(buffer, L"设置", layout.title, ui.fontLarge, g_palette->text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            // 关闭只有标题栏那一个：在标题正下方再画一个 ✕ 会让人以为有两个关闭键
            HBRUSH line = CreateSolidBrush(g_palette->border);
            FillRect(buffer, &layout.separator, line);
            DeleteObject(line);

            auto section = [&](const wchar_t* text, int top) {
                RECT rect{layout.title.left, top, layout.title.right, top + 22};
                DrawTextIn(buffer, text, rect, ui.fontSmall, g_palette->system,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            };
            auto label = [&](const wchar_t* text, const RECT& control) {
                RECT rect{layout.title.left, control.top, layout.themeSegment.left - 16,
                          control.bottom};
                DrawTextIn(buffer, text, rect, ui.font, g_palette->text,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            };

            section(L"外观", layout.themeSegment.top - 34);
            label(L"主题", layout.themeSegment);
            DrawSegment(buffer, layout.themeSegment, kThemeChoiceLabels, 3, state->themeChoice, ui.font,
                        state->themeHover);
            label(L"彩色聊天", layout.colorToggle);
            DrawToggle(buffer, layout.colorToggle, state->colorEnabled);
            DrawTextIn(buffer, L"给昵称和气泡上色", layout.colorToggleLabel, ui.fontSmall,
                       g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            section(L"服务器", layout.hostField.top - 34);
            label(L"地址", layout.hostField);
            ui::DrawRoundedControl(buffer, layout.hostField, 8, g_palette->panel,
                                   g_palette->bubbleOther,
                                   GetFocus() == state->hostEdit ? g_palette->accent
                                                                 : g_palette->border,
                                   g_palette->panel);
            label(L"端口", layout.portField);
            ui::DrawRoundedControl(buffer, layout.portField, 8, g_palette->panel,
                                   g_palette->bubbleOther,
                                   GetFocus() == state->portEdit ? g_palette->accent
                                                                 : g_palette->border,
                                   g_palette->panel);

            section(L"语音", layout.voiceSegment.top - 34);
            label(L"最长时长", layout.voiceSegment);
            DrawSegment(buffer, layout.voiceSegment, kVoiceLimitLabels, 3, state->voiceChoice, ui.font,
                        state->voiceHover);
            DrawTextIn(buffer, L"当前是未压缩 PCM，2 MB 上限大约 1 分钟", layout.voiceHint,
                       ui.fontSmall, g_palette->system,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            RestoreDC(buffer, saved);
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, oldBitmap);
            DeleteObject(canvas);
            DeleteDC(buffer);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!state) return 0;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const int themeHover = dchat::SegmentHitTest(state->layout.themeSegment, 3, x, y);
            const int voiceHover = dchat::SegmentHitTest(state->layout.voiceSegment, 3, x, y);
            if (themeHover != state->themeHover || voiceHover != state->voiceHover) {
                state->themeHover = themeHover;
                state->voiceHover = voiceHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!state) return 0;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (PtInRect(&state->layout.closeButton, POINT{x, y})) {
                DestroyWindow(hwnd);
                return 0;
            }
            const int theme = dchat::SegmentHitTest(state->layout.themeSegment, 3, x, y);
            if (theme >= 0) {
                state->themeChoice = theme;
                state->themeMode = theme == 0 ? ThemeMode::Dark
                                              : (theme == 1 ? ThemeMode::Light : ThemeMode::System);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int voice = dchat::SegmentHitTest(state->layout.voiceSegment, 3, x, y);
            if (voice >= 0) {
                state->voiceChoice = voice;
                state->voiceLimit = kVoiceLimitOptions[voice];
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->layout.colorToggle, POINT{x, y})) {
                state->colorEnabled = !state->colorEnabled;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_COMMAND:
            if (!state) break;
            if (LOWORD(wp) == IDS_OK) {
                ApplySettingsAndClose(state);
                return 0;
            }
            if (LOWORD(wp) == IDS_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 打开设置窗口（模态）。取消 / Esc / ✕ 都等于丢弃改动。
// ---------------------------------------------------------------------------
// 会话设置窗口（群聊 / 私聊 / 大厅）
// ---------------------------------------------------------------------------
//
// ## 为什么是一个窗口而不是三个
//
// "群设置"和"私聊设置"的差别只有**行不一样**：群聊有群名、最大人数、加群方式、
// 成员表……私聊只有免打扰、置顶、黑名单。做成三个窗口就是三份各自维护的绘制与
// 命中测试代码，而它们迟早会对不上（改了一个忘了另两个）。
//
// 所以这里**行是按会话类型现算出来的**（BuildRoomRows），窗口本身完全不懂业务。
// 加一个设置项 = 在 BuildRoomRows 里加一行，三个界面同时生效。
//
// ## 两类设置的边界画在哪
//
// 行上带 `key` 的 = 群管理员设置项，改动会发 `GSET` 出去，**全群可见**；
// 行上带 `local` 的 = 成员本地设置，只改本机的 dchat-rooms.prefs，**永不上传**。
// 这个区分在界面上也看得见：管理员项没有权限时是灰的，本地项永远可点。

enum class RoomRowKind {
    Section,   // 分组标题（"群设置" / "我的设置"）
    ReadOnly,  // 只读：群聊 ID（不可更改）
    Text,      // 文本，点一下就地编辑
    Number,    // 数字，点一下就地编辑
    Choice,    // 多选一，点一下循环
    Toggle,    // 开关
    Action,    // 一个按钮（退出群聊 / 解散群聊 / 删除本地聊天记录）
    Member,    // 成员行：左边昵称，右边两个按钮
    Request,   // 入群申请：左边昵称，右边同意 / 拒绝
    Hint,      // 一行灰字说明
};

struct RoomRow {
    RoomRowKind kind = RoomRowKind::Hint;
    std::string label;   // 左边的名字
    std::string value;   // 右边显示的值
    std::string key;     // GSET 的设置名；非空 = 管理员设置项
    std::string local;   // 本地设置名（mute/pin/alert/cache）；非空 = 只改本机
    std::string arg;     // Action/Member/Request 的参数（昵称或动作名）
    std::string role;    // Member 行：owner / admin / member
    bool enabled = true; // 有没有权限改（没有就画灰、点了给提示）
    bool on = false;     // Toggle 的当前状态
    RECT rect{};
    RECT box{};          // 右侧控件的矩形
    RECT extra{};        // 第二个按钮（踢人 / 拒绝）
    bool hasExtra = false;
};

struct RoomSettingsState {
    HWND dlg = nullptr;
    HWND edit = nullptr;   // 就地编辑用的 EDIT（同一时刻只有一个）
    int editRow = -1;
    int hover = -1;
    int hoverExtra = -1;
    std::string sessionId;
    std::vector<RoomRow> rows;
    int scrollTop = 0;
    int contentHeight = 0;
    // 同 GroupWindowState::closed：给 OpenRoomSettings 的模态嵌套循环一个退出信号，
    // 而不是用线程级的 PostQuitMessage 把整个客户端一起送走。
    bool* closed = nullptr;
};

constexpr const wchar_t* kRoomSettingsClass = L"DchatRoomSettingsDlg";
constexpr int kRoomWndWidth = 500;
constexpr int kRoomWndHeight = 620;
constexpr int kRoomRowHeight = 34;
constexpr int kRoomSectionHeight = 30;
constexpr int kRoomHintHeight = 24;
constexpr int kRoomPadX = 18;
constexpr int kRoomLabelWidth = 150;

/** 发一条多会话命令。返回 false 表示连接已断。 */
bool SendRoomCommand(const std::string& command, const std::string& rest) {
    if (!IsConnected()) return false;
    return SendRawLine(dchat::BuildLine(command, rest));
}

const char* JoinModeLabelOf(const std::string& mode) {
    if (mode == "direct") return "直接添加";
    if (mode == "auto") return "自动审核";
    return "管理员审核";
}

const char* RoleLabelOf(const std::string& role) {
    if (role == "owner") return "群主";
    if (role == "admin") return "管理员";
    return "成员";
}

/** 数字 -> 文本。服务端回来的都是字符串，这里只做显示。 */
std::string U64Text(unsigned long long value) { return std::to_string(value); }

/**
 * 按会话类型算出要显示哪些行。
 *
 * 权限判断也在这里：没有管理权限的设置项标记成 `enabled = false`，
 * 窗口把它们画灰、点了给一句明确的提示 —— 比"点下去没反应"好得多。
 */
void BuildRoomRows(const Session& session, std::vector<RoomRow>* rows) {
    rows->clear();
    const bool isGroup = (session.kind == "group");
    const bool isHall = (session.kind == "hall");
    const bool isDm = (session.kind == "dm");
    const bool canAdmin =
        dchat::RoomRoleCanAdminister(session.myRole == "owner" ? dchat::RoomRole::Owner
                                          : session.myRole == "admin" ? dchat::RoomRole::Admin
                                                                      : dchat::RoomRole::Member);

    auto section = [&](const char* text) {
        RoomRow row;
        row.kind = RoomRowKind::Section;
        row.label = text;
        rows->push_back(row);
    };
    auto hint = [&](const std::string& text) {
        RoomRow row;
        row.kind = RoomRowKind::Hint;
        row.label = text;
        rows->push_back(row);
    };
    auto readOnly = [&](const std::string& label, const std::string& value) {
        RoomRow row;
        row.kind = RoomRowKind::ReadOnly;
        row.label = label;
        row.value = value;
        rows->push_back(row);
    };

    // ---- 会话身份（所有人可见）----
    section(isDm ? "私聊" : (isHall ? "大厅" : "群聊"));
    readOnly(isDm ? "对方" : "名称", session.title.empty() ? session.id : session.title);
    readOnly("会话 ID", session.id);
    if (isGroup) {
        // 群聊 ID 不可更改 —— 这是需求里明确要求的，界面上也要说清楚，
        // 否则用户会一直找"哪里能改名"
        hint("会话 ID 建好后不可更改");
    }
    readOnly("我的身份", RoleLabelOf(session.myRole));
    if (!isDm) {
        readOnly("成员", std::to_string(session.members) +
                             (session.maxMembers > 0 ? " / " + std::to_string(session.maxMembers)
                                                     : ""));
    }

    // ---- 群管理员可设项（走服务端）----
    if (isGroup || isHall) {
        section("群设置（管理员修改，全群可见）");
        auto setting = [&](RoomRowKind kind, const char* label, const std::string& value,
                           const char* key, bool on) {
            RoomRow row;
            row.kind = kind;
            row.label = label;
            row.value = value;
            row.key = key;
            row.on = on;
            row.enabled = canAdmin;
            rows->push_back(row);
        };
        setting(RoomRowKind::Text, "群聊名称", session.name.empty() ? session.title : session.name,
                "name", false);
        setting(RoomRowKind::Number, "最大人数", std::to_string(session.maxMembers), "maxmembers",
                false);
        setting(RoomRowKind::Choice, "加群方式", JoinModeLabelOf(session.joinMode), "joinmode",
                false);
        setting(RoomRowKind::Toggle, "向公众展示",
                session.listed ? "其他人可以在群搜索里找到" : "不在群搜索里出现", "listed",
                session.listed);
        setting(RoomRowKind::Number, "总缓存最大大小",
                session.cacheMaxMb ? U64Text(session.cacheMaxMb) + " MB" : std::string(), "cachemaxmb",
                false);
        setting(RoomRowKind::Toggle, "消息重要提醒",
                session.importantNotice ? "管理员可以发 @全体成员" : "谁都发不了 @全体成员",
                "importantnotice", session.importantNotice);
        {
            RoomRow row;
            row.kind = RoomRowKind::Text;
            row.label = "群公告";
            row.value = session.announcement.empty() ? "（没有公告）" : session.announcement;
            row.key = "announcement";
            row.enabled = canAdmin;
            rows->push_back(row);
        }
        if (!canAdmin) hint("你不是群主或管理员，上面这些只能看不能改");
    }

    // ---- 我的本地设置（永不上传）----
    section("我的设置（只在这台电脑上，不上传）");
    {
        RoomRow row;
        row.kind = RoomRowKind::Toggle;
        row.label = "免打扰";
        row.value = session.prefs.muted ? "不提示新消息" : "新消息会提示";
        row.local = "mute";
        row.on = session.prefs.muted;
        rows->push_back(row);
    }
    {
        RoomRow row;
        row.kind = RoomRowKind::Toggle;
        row.label = "置顶";
        row.value = session.prefs.pinned ? "排在最上面" : "按默认顺序排列";
        row.local = "pin";
        row.on = session.prefs.pinned;
        rows->push_back(row);
    }
    {
        RoomRow row;
        row.kind = RoomRowKind::Toggle;
        row.label = "重要提醒仍然提醒";
        row.value = session.prefs.alertImportant ? "@我 / @全体成员 照常提示" : "免打扰时连 @我 也不提示";
        row.local = "alert";
        row.on = session.prefs.alertImportant;
        rows->push_back(row);
    }
    {
        RoomRow row;
        row.kind = RoomRowKind::Number;
        row.label = "本地缓存上限";
        row.value = U64Text(session.prefs.localCacheMb) + " MB";
        row.local = "cache";
        rows->push_back(row);
    }
    if (isHall) {
        hint("大厅是所有人的公共房间：默认免打扰，不能退出、不能删除");
    }

    // ---- 成员与入群申请（群主 / 管理员）----
    if ((isGroup || isHall) && canAdmin) {
        section("成员");
        for (const SessionMember& member : session.memberList) {
            RoomRow row;
            row.kind = RoomRowKind::Member;
            row.label = member.nick;
            row.value = member.display.empty() ? std::string() : member.display;
            row.role = member.role;
            row.arg = member.nick;
            // 群主只能有一个、管理员之间不能互相踢 —— 和服务器端同一套规则。
            // 界面先把不能做的按钮藏掉/变灰，比让用户点了再被服务器拒绝好。
            row.enabled = (member.nick != g_nick) && (session.myRole == "owner" ||
                                                      member.role == "member");
            row.on = (member.role != "member");
            rows->push_back(row);
        }
        if (session.memberList.empty()) hint("成员表还没加载出来");

        section("入群申请");
        for (const std::string& nick : session.pending) {
            RoomRow row;
            row.kind = RoomRowKind::Request;
            row.label = nick;
            row.arg = nick;
            row.hasExtra = true;
            rows->push_back(row);
        }
        if (session.pending.empty()) hint("现在没有待审核的申请");
    }

    // ---- 危险操作 ----
    section("其它");
    if (isGroup) {
        RoomRow leave;
        leave.kind = RoomRowKind::Action;
        leave.label = "退出群聊";
        leave.arg = "leave";
        rows->push_back(leave);
    }
    if (isGroup && session.myRole == "owner") {
        RoomRow destroy;
        destroy.kind = RoomRowKind::Action;
        destroy.label = "解散群聊";
        destroy.arg = "delete";
        rows->push_back(destroy);
    }
    {
        RoomRow clear;
        clear.kind = RoomRowKind::Action;
        clear.label = "删除本地聊天记录";
        clear.arg = "clearlocal";
        rows->push_back(clear);
    }
    if (isDm) {
        RoomRow block;
        block.kind = RoomRowKind::Action;
        block.label = "拉黑对方";
        block.arg = "block";
        rows->push_back(block);
    }
}

/**
 * 把一条设置改动**先应用到本地镜像**（乐观更新）。
 *
 * 为什么必须这么做：只发 `GSET` 出去、等服务器回 `RSET` 才改界面的话，
 * 用户点完开关**看不到任何反应**，会以为按钮坏了（真机实测就是这样反馈的）。
 * 服务器的 `RSET` 随后回来会再确认一次，值不会有偏差；万一被服务端拒绝，
 * 那条 `ERROR` 会显示出来，而随后的 `RSET` 会把界面纠正回去。
 *
 * 集中成一个函数是为了让"发出去"和"本地先改"用**同一套 key 语义** ——
 * 分散在两处的话，迟早会有一个 key 只有一边认识。
 */
void ApplyLocalRoomSetting(Session& session, const std::string& key, const std::string& value) {
    if (key == "name") {
        session.name = value;
        if (!value.empty()) session.title = value;
    } else if (key == "maxmembers") {
        session.maxMembers = std::atoi(value.c_str());
    } else if (key == "joinmode") {
        session.joinMode = value;
    } else if (key == "listed") {
        session.listed = (value == "true");
    } else if (key == "cachemaxmb") {
        session.cacheMaxMb = std::strtoull(value.c_str(), nullptr, 10);
    } else if (key == "announcement") {
        session.announcement = value;
    } else if (key == "importantnotice") {
        session.importantNotice = (value == "true");
    }
}

void LayoutRoomSettings(RoomSettingsState* state) {
    RECT client{};
    GetClientRect(state->dlg, &client);
    int y = 56 - state->scrollTop;
    for (RoomRow& row : state->rows) {
        int height = kRoomRowHeight;
        if (row.kind == RoomRowKind::Section) height = kRoomSectionHeight;
        if (row.kind == RoomRowKind::Hint) height = kRoomHintHeight;
        row.rect = RECT{kRoomPadX, y, client.right - kRoomPadX, y + height};
        const int boxRight = client.right - kRoomPadX;
        const int boxLeft = kRoomPadX + kRoomLabelWidth;
        switch (row.kind) {
            case RoomRowKind::Toggle:
                row.box = RECT{boxLeft, y + 7, boxLeft + 44, y + 27};
                break;
            case RoomRowKind::Member:
                row.extra = RECT{boxRight - 60, y + 5, boxRight, y + 27};
                row.box = RECT{boxRight - 152, y + 5, boxRight - 68, y + 27};
                break;
            case RoomRowKind::Request:
                row.extra = RECT{boxRight - 60, y + 5, boxRight, y + 27};
                row.box = RECT{boxRight - 130, y + 5, boxRight - 68, y + 27};
                break;
            case RoomRowKind::Action:
                row.box = RECT{boxLeft, y + 5, boxLeft + 150, y + 29};
                break;
            case RoomRowKind::ReadOnly:
            case RoomRowKind::Hint:
            case RoomRowKind::Section:
                break;
            default:
                row.box = RECT{boxLeft, y + 5, boxRight, y + 29};
                break;
        }
        y += height;
    }
    state->contentHeight = y + state->scrollTop - 56;
    if (state->edit && state->editRow >= 0 &&
        state->editRow < static_cast<int>(state->rows.size())) {
        const RECT box = state->rows[state->editRow].box;
        MoveWindow(state->edit, box.left + 6, box.top + 2, (box.right - box.left) - 12,
                   (box.bottom - box.top) - 4, TRUE);
    }
}

void RefreshRoomSettings(RoomSettingsState* state);

/** 就地编辑结束：把新值发出去。 */
void CommitRoomRowEdit(RoomSettingsState* state, bool keep) {    if (!state->edit || state->editRow < 0) return;
    const int index = state->editRow;
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(state->edit)), L'\0');
    if (!text.empty()) GetWindowTextW(state->edit, &text[0], static_cast<int>(text.size()) + 1);
    DestroyWindow(state->edit);
    state->edit = nullptr;
    state->editRow = -1;
    if (!keep || index >= static_cast<int>(state->rows.size())) return;
    const RoomRow row = state->rows[index];  // 拷一份：发命令会触发重画、rows 可能被重建
    const std::string value = WideToUtf8(text);
    if (row.key == "name" || row.key == "announcement") {
        // 群名和公告可能含空格，走 Base64（和服务端的约定一致）
        SendRoomCommand("GSET", state->sessionId + " " + row.key + " " +
                                    dchat::Base64Encode(value));
    } else if (!row.key.empty()) {
        SendRoomCommand("GSET", state->sessionId + " " + row.key + " " + value);
    } else if (row.local == "cache") {
        // 本地缓存上限：纯本机的事，服务端根本不知道有这一项
        const int sessionIndex = FindSession(state->sessionId);
        if (sessionIndex >= 0) {
            unsigned long long mb = std::strtoull(value.c_str(), nullptr, 10);
            if (mb < dchat::kMinLocalCacheMb) mb = dchat::kMinLocalCacheMb;
            if (mb > dchat::kMaxLocalCacheMb) mb = dchat::kMaxLocalCacheMb;
            g_sessions[sessionIndex].prefs.localCacheMb = mb;
            SaveRoomPrefs();
        }
        RefreshRoomSettings(state);
        return;
    }
    // 管理员设置项：本地先改再重画，和开关走同一条路径 ——
    // 等服务器回 RSET 才变的话，用户编辑完什么都看不到
    const int sessionIndex = FindSession(state->sessionId);
    if (sessionIndex >= 0 && !row.key.empty()) {
        ApplyLocalRoomSetting(g_sessions[sessionIndex], row.key, value);
    }
    RefreshRoomSettings(state);
}

void RefreshRoomSettings(RoomSettingsState* state) {
    const int index = FindSession(state->sessionId);
    if (index < 0) return;
    BuildRoomRows(g_sessions[index], &state->rows);
    LayoutRoomSettings(state);
    if (state->dlg) InvalidateRect(state->dlg, nullptr, FALSE);
}

void PaintRoomSettings(RoomSettingsState* state, HDC dc) {
    RECT client{};
    GetClientRect(state->dlg, &client);
    HBRUSH back = CreateSolidBrush(g_palette->panel);
    FillRect(dc, &client, back);
    DeleteObject(back);

    const int sessionIndex = FindSession(state->sessionId);
    const bool valid = sessionIndex >= 0;

    // 标题栏
    RECT title{0, 0, client.right, 52};
    HBRUSH titleBrush = CreateSolidBrush(g_palette->windowBg);
    FillRect(dc, &title, titleBrush);
    DeleteObject(titleBrush);
    TextOutUtf8(dc, L"会话设置", RECT{kRoomPadX, 0, client.right - kRoomPadX, 52}, ui.fontLarge,
                g_palette->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT close{kRoomPadX, 10, kRoomPadX + 60, 42};
    (void)close;
    HBRUSH line = CreateSolidBrush(g_palette->border);
    RECT sep{0, 51, client.right, 52};
    FillRect(dc, &sep, line);
    DeleteObject(line);

    if (!valid) {
        TextOutUtf8(dc, L"这个会话已经不存在了", RECT{kRoomPadX, 70, client.right, 100}, ui.font,
                    g_palette->system, DT_LEFT | DT_TOP | DT_SINGLELINE);
        return;
    }

    for (std::size_t i = 0; i < state->rows.size(); ++i) {
        const RoomRow& row = state->rows[i];
        if (row.rect.bottom < 52 || row.rect.top > client.bottom) continue;
        const bool hovered = (static_cast<int>(i) == state->hover);
        switch (row.kind) {
            case RoomRowKind::Section:
                TextOutUtf8(dc, Utf8ToWide(row.label), row.rect, ui.fontSmall, g_palette->accent,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                break;
            case RoomRowKind::Hint:
                TextOutUtf8(dc, Utf8ToWide(row.label), row.rect, ui.fontSmall, g_palette->system,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            case RoomRowKind::ReadOnly:
                TextOutUtf8(dc, Utf8ToWide(row.label),
                            RECT{row.rect.left, row.rect.top, row.rect.left + kRoomLabelWidth,
                                 row.rect.bottom},
                            ui.font, g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                TextOutUtf8(dc, Utf8ToWide(row.value),
                            RECT{row.rect.left + kRoomLabelWidth, row.rect.top, row.rect.right,
                                 row.rect.bottom},
                            ui.font, g_palette->text,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            case RoomRowKind::Toggle: {
                TextOutUtf8(dc, Utf8ToWide(row.label),
                            RECT{row.rect.left, row.rect.top, row.rect.left + kRoomLabelWidth,
                                 row.rect.bottom},
                            ui.font, g_palette->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                DrawToggle(dc, row.box, row.on);
                TextOutUtf8(dc, Utf8ToWide(row.value),
                            RECT{row.box.right + 12, row.rect.top, row.rect.right, row.rect.bottom},
                            ui.fontSmall, g_palette->system,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            case RoomRowKind::Action: {
                const bool hot = hovered;
                ui::DrawRoundedControl(dc, row.box, 6, g_palette->panel,
                                       hot ? g_palette->hover : g_palette->neutral,
                                       g_palette->neutralBorder);
                TextOutUtf8(dc, Utf8ToWide(row.label), row.box, ui.font,
                            row.arg == "delete" ? RGB(0xE8, 0x6B, 0x6B) : g_palette->text,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                break;
            }
            case RoomRowKind::Member:
            case RoomRowKind::Request: {
                TextOutUtf8(dc, Utf8ToWide(row.label),
                            RECT{row.rect.left, row.rect.top, row.box.left - 8, row.rect.bottom},
                            ui.font, g_palette->text,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                const std::string info =
                    row.kind == RoomRowKind::Request
                        ? std::string("申请入群")
                        : (std::string(RoleLabelOf(row.role)) +
                           (row.value.empty() ? std::string() : "・" + row.value));
                TextOutUtf8(dc, Utf8ToWide(info),
                            RECT{row.box.left + 4, row.rect.top, row.box.right + 60, row.rect.bottom},
                            ui.fontSmall, g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                const wchar_t* primary =
                    row.kind == RoomRowKind::Request ? L"同意"
                                                     : (row.role == "member" ? L"设为管理员"
                                                                             : L"撤销管理员");
                ui::DrawRoundedControl(dc, row.box, 6, g_palette->panel,
                                       hovered ? g_palette->hover : g_palette->neutral,
                                       g_palette->neutralBorder);
                TextOutUtf8(dc, primary, row.box, ui.fontSmall,
                            row.enabled ? g_palette->text : g_palette->system,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (row.hasExtra) {
                    ui::DrawRoundedControl(dc, row.extra, 6, g_palette->panel,
                                           state->hoverExtra == static_cast<int>(i)
                                               ? g_palette->hover
                                               : g_palette->neutral,
                                           g_palette->neutralBorder);
                    TextOutUtf8(dc, row.kind == RoomRowKind::Request ? L"拒绝" : L"踢出",
                                row.extra, ui.fontSmall,
                                row.enabled ? RGB(0xE8, 0x6B, 0x6B) : g_palette->system,
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                break;
            }
            default: {
                TextOutUtf8(dc, Utf8ToWide(row.label),
                            RECT{row.rect.left, row.rect.top, row.rect.left + kRoomLabelWidth,
                                 row.rect.bottom},
                            ui.font, g_palette->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                const bool editing = (static_cast<int>(i) == state->editRow);
                if (!editing) {
                    ui::DrawRoundedControl(dc, row.box, 6, g_palette->panel,
                                           hovered && row.enabled ? g_palette->hover
                                                                  : g_palette->neutral,
                                           g_palette->neutralBorder);
                    TextOutUtf8(dc, Utf8ToWide(row.value), 
                                RECT{row.box.left + 8, row.box.top, row.box.right - 8,
                                     row.box.bottom},
                                ui.font, row.enabled ? g_palette->text : g_palette->system,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                break;
            }
        }
    }
}

LRESULT CALLBACK RoomSettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<RoomSettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* created = reinterpret_cast<RoomSettingsState*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            created->dlg = hwnd;
            return TRUE;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (state) PaintRoomSettings(state, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // 背景由 WM_PAINT 画，避免闪
        case WM_MOUSEMOVE: {
            if (!state) break;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            int hover = -1;
            int hoverExtra = -1;
            for (std::size_t i = 0; i < state->rows.size(); ++i) {
                const RoomRow& row = state->rows[i];
                if (PtInRect(&row.rect, POINT{x, y})) hover = static_cast<int>(i);
                if (row.hasExtra && PtInRect(&row.extra, POINT{x, y})) {
                    hoverExtra = static_cast<int>(i);
                }
            }
            if (hover != state->hover || hoverExtra != state->hoverExtra) {
                state->hover = hover;
                state->hoverExtra = hoverExtra;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!state) break;
            RECT client{};
            GetClientRect(hwnd, &client);
            const int maxScroll = state->contentHeight > (client.bottom - 52)
                                      ? state->contentHeight - (client.bottom - 52)
                                      : 0;
            state->scrollTop -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 48;
            if (state->scrollTop < 0) state->scrollTop = 0;
            if (state->scrollTop > maxScroll) state->scrollTop = maxScroll;
            LayoutRoomSettings(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!state) break;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const int index = state->hover;
            if (index < 0 || index >= static_cast<int>(state->rows.size())) break;
            const RoomRow row = state->rows[index];  // 拷一份：发命令会重建 rows
            const int sessionIndex = FindSession(state->sessionId);
            if (sessionIndex < 0) break;
            Session& session = g_sessions[sessionIndex];

            if (state->edit) CommitRoomRowEdit(state, true);

            // ---- 本地设置：只改本机，永不上传 ----
            if (!row.local.empty()) {
                if (row.local == "mute") {
                    session.prefs.muted = !session.prefs.muted;
                } else if (row.local == "pin") {
                    session.prefs.pinned = !session.prefs.pinned;
                } else if (row.local == "alert") {
                    session.prefs.alertImportant = !session.prefs.alertImportant;
                } else if (row.local == "cache") {
                    // 「本地缓存上限」是**数字**不是开关，要就地开一个编辑框让用户填。
                    // 以前这里没有这个分支、却照样在下面 return 0 —— 于是这一行
                    // **怎么点都没反应**：提交路径（CommitRoomRowEdit 里的 row.local ==
                    // "cache" 分支）早就写好了，缺的只是"点开编辑框"这一步，
                    // 结果就是界面上根本改不了这一项。自动化联调抓到的，
                    // 和 /dm、群昵称属于同一类"功能写好了却没有入口"。
                    state->edit = CreateWindowExW(
                        0, L"EDIT", Utf8ToWide(U64Text(session.prefs.localCacheMb)).c_str(),
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10, hwnd,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
                    if (state->edit) {
                        state->editRow = index;
                        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font),
                                     TRUE);
                        LayoutRoomSettings(state);
                        SetFocus(state->edit);
                        SendMessageW(state->edit, EM_SETSEL, 0, -1);
                    }
                    return 0;
                }
                SaveRoomPrefs();
                RefreshSessionPanel();
                RefreshRoomSettings(state);
                return 0;
            }

            // ---- 只读 / 分组：点一下不说话 ----
            if (row.kind == RoomRowKind::Section || row.kind == RoomRowKind::ReadOnly ||
                row.kind == RoomRowKind::Hint) {
                return 0;
            }

            // ---- 管理员设置项 ----
            if (!row.key.empty()) {
                if (!row.enabled) {
                    ViewAddItem(ItemKind::Notice, "只有群主和管理员能改这一项",
                                dchat::NowTimeString());
                    return 0;
                }
                if (row.kind == RoomRowKind::Toggle) {
                    const bool next = !row.on;
                    const std::string value = next ? "true" : "false";
                    SendRoomCommand("GSET", state->sessionId + " " + row.key + " " + value);
                    // 本地先改再重画：只发不等的话用户点完看不到任何反应
                    ApplyLocalRoomSetting(session, row.key, value);
                    RefreshRoomSettings(state);
                    return 0;
                }
                if (row.kind == RoomRowKind::Choice) {
                    // 三个值循环。顺序固定，用户点几下就能到自己要的那个
                    const std::string next = session.joinMode == "direct"    ? "approval"
                                             : session.joinMode == "approval" ? "auto"
                                                                              : "direct";
                    SendRoomCommand("GSET", state->sessionId + " " + row.key + " " + next);
                    ApplyLocalRoomSetting(session, row.key, next);
                    RefreshRoomSettings(state);
                    return 0;
                }
                // 文本 / 数字：就地开一个编辑框
                std::string initial = row.value;
                if (row.key == "maxmembers") initial = std::to_string(session.maxMembers);
                if (row.key == "cachemaxmb") initial = U64Text(session.cacheMaxMb);
                if (row.key == "announcement" && initial == "（没有公告）") initial.clear();
                state->edit = CreateWindowExW(0, L"EDIT", Utf8ToWide(initial).c_str(),
                                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                              hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (state->edit) {
                    state->editRow = index;
                    SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
                    LayoutRoomSettings(state);
                    SetFocus(state->edit);
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                }
                return 0;
            }

            // ---- 成员行的两个按钮 ----
            if (row.kind == RoomRowKind::Member) {
                if (PtInRect(&row.extra, POINT{x, y})) {
                    SendRoomCommand("GKICK", state->sessionId + " " + row.arg);
                } else if (row.role == "member") {
                    SendRoomCommand("GMEMBER", state->sessionId + " " + row.arg + " admin");
                } else {
                    SendRoomCommand("GMEMBER", state->sessionId + " " + row.arg + " member");
                }
                return 0;
            }
            if (row.kind == RoomRowKind::Request) {
                const bool accept = !PtInRect(&row.extra, POINT{x, y});
                SendRoomCommand("GREQ", state->sessionId + " " + row.arg + (accept ? " ok" : " no"));
                const int target = FindSession(state->sessionId);
                if (target >= 0) {
                    g_sessions[target].pending.erase(
                        std::remove(g_sessions[target].pending.begin(),
                                    g_sessions[target].pending.end(), row.arg),
                        g_sessions[target].pending.end());
                }
                RefreshRoomSettings(state);
                return 0;
            }

            // ---- 危险操作 ----
            if (row.kind == RoomRowKind::Action) {
                if (row.arg == "leave") {
                    SendRoomCommand("GLEAVE", state->sessionId);
                    DestroyWindow(hwnd);
                } else if (row.arg == "delete") {
                    SendRoomCommand("GDELETE", state->sessionId);
                    DestroyWindow(hwnd);
                } else if (row.arg == "block") {
                    SendRoomCommand("BLOCK", session.title);
                    ViewAddItem(ItemKind::Notice, "已把 " + session.title + " 加入黑名单",
                                dchat::NowTimeString());
                } else if (row.arg == "clearlocal") {
                    // 删除本地聊天记录：**只删这台机器上的**。
                    // 服务端那份不动 —— 那是"群的历史"，不是"我的历史"。
                    session.items.clear();
                    if (sessionIndex == g_currentSession) {
                        g_items.clear();
                        g_scrollTop = 0;
                        if (ui.hView) {
                            InvalidateRect(ui.hView, nullptr, TRUE);
                            ViewUpdateScrollbar(ui.hView);
                        }
                    }
                    ViewAddItem(ItemKind::Notice, "已删除本机上这个会话的聊天记录",
                                dchat::NowTimeString());
                }
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (state && state->edit && wp == VK_RETURN) {
                CommitRoomRowEdit(state, true);
                RefreshRoomSettings(state);
                return 0;
            }
            if (state && state->edit && wp == VK_ESCAPE) {
                CommitRoomRowEdit(state, false);
                RefreshRoomSettings(state);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (state && state->edit) CommitRoomRowEdit(state, true);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // 通知 OpenRoomSettings 的模态嵌套循环退出。**不要用 PostQuitMessage**：
            // 它是线程级的，主循环也会收到 WM_QUIT —— 后果是"关掉设置窗 = 整个客户端退出"。
            if (state && state->closed) *state->closed = true;
            g_roomSettingsWindow = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/** 打开某个会话的设置窗口（群聊 / 私聊 / 大厅共用这一个）。 */
void OpenRoomSettings(HWND parent, const std::string& sessionId) {
    if (FindSession(sessionId) < 0) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = RoomSettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kRoomSettingsClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    RoomSettingsState state;
    // 模态循环的退出信号（窗口销毁时由 WM_DESTROY 置位）。
    bool closed = false;
    state.closed = &closed;
    state.sessionId = sessionId;
    RefreshRoomSettings(&state);

    RECT rc{0, 0, kRoomWndWidth, kRoomWndHeight};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND dlg = CreateWindowExW(kDlgExStyle, kRoomSettingsClass, L"会话设置", kDlgStyle, x, y, width,
                               height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return;
    g_roomSettingsWindow = dlg;  // 记进进程内全局：状态导出要用它，不能靠 FindWindowW
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(dlg);
    RefreshRoomSettings(&state);  // 有了真实客户区尺寸之后再排一次版

    // 模态嵌套循环：**靠 closed 退出**，不靠 PostQuitMessage（线程级，会把主循环
    // 一起干掉 = 关掉设置窗就退出整个客户端）。
    MSG msg;
    while (!closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// ---------------------------------------------------------------------------
// 群聊窗口：新建群聊 + 搜索群聊
// ---------------------------------------------------------------------------
//
// 两件事放一个窗口，因为它们本来就是一件事的两头：想加群的人先搜，搜不到就自己建。
// 分成两个窗口的话，"搜不到"之后用户还得回去找另一个入口。
//
// 搜索结果（GHIT）是**异步**回来的，所以这里不用轮询也不用手动刷新：
// 收齐一轮（GHIT_END）之后由 WM_APP_LINE 往这个窗口发一条消息，
// 窗口收到就重画面板。
constexpr const wchar_t* kGroupWindowClass = L"DchatGroupWindow";
constexpr int kGroupWndWidth = 470;
constexpr int kGroupWndHeight = 560;
constexpr int kGroupResultRowHeight = 46;

struct GroupWindowState {
    HWND dlg = nullptr;
    HWND idEdit = nullptr;
    HWND nameEdit = nullptr;
    HWND keywordEdit = nullptr;
    // 输入框的背景刷子。系统默认的 EDIT 是**白底黑字**，压在这个深色界面上
    // 非常突兀（真机反馈原话："这个输入很突兀"）。WM_CTLCOLOREDIT 里换成主题色，
    // 和程序里其它输入框保持一致。
    HBRUSH fieldBrush = nullptr;
    RECT createButton{};
    RECT searchButton{};
    RECT resultsTop{};
    int hoverJoin = -1;
    std::string hint;
    // 窗口销毁时置位，用来通知 OpenGroupWindow 里那个模态嵌套循环退出。
    // **不能用 PostQuitMessage**：它是线程级的，主循环也会收到 WM_QUIT，
    // 于是"关掉群聊窗"就变成了"退出整个客户端"（真机 + 自动化都实测过）。
    bool* closed = nullptr;
};

HWND g_groupWindow = nullptr;
constexpr UINT kMsgGroupHits = WM_APP + 40;

void LayoutGroupWindow(GroupWindowState* state) {
    RECT client{};
    GetClientRect(state->dlg, &client);
    const int left = 18;
    const int right = client.right - 18;
    const int editHeight = 26;
    int y = 66;
    MoveWindow(state->idEdit, left + 90, y, right - left - 90 - 110, editHeight, TRUE);
    state->createButton = RECT{right - 104, y - 2, right, y + editHeight + 2};
    y += editHeight + 10;
    MoveWindow(state->nameEdit, left + 90, y, right - left - 90, editHeight, TRUE);
    y += editHeight + 26;

    MoveWindow(state->keywordEdit, left + 90, y, right - left - 90 - 110, editHeight, TRUE);
    state->searchButton = RECT{right - 104, y - 2, right, y + editHeight + 2};
    y += editHeight + 18;
    state->resultsTop = RECT{left, y, right, client.bottom - 14};
}

// 「＋ 新建群聊」和「搜索」两个表头按钮共用一个窗口
void OpenGroupWindow(HWND parent);

/**
 * 群搜索结果里第 index 行的「加入」按钮矩形。
 *
 * 命中测试和自动化测试的状态导出**共用这一个函数**：这个项目已经吃过"布局两边各算
 * 一次、迟早错位"的亏（见 ui_layout.h 开头那段），坐标只留一份就不会再错。
 */
RECT GroupJoinRect(const GroupWindowState* state, int clientRight, std::size_t index) {
    int rowY = state->resultsTop.top + (state->hint.empty() ? 0 : 28);
    rowY += static_cast<int>(index) * kGroupResultRowHeight;
    return RECT{clientRight - 18 - 78, rowY + 6, clientRight - 18, rowY + 30};
}

/**
 * 导出两个**自绘**弹窗的可点区域（会话设置窗、群聊窗）。
 *
 * 为什么不在测试脚本里把布局再算一遍：这两个窗口的按钮没有 HWND（第 304 行那段
 * 注释解释了为什么改成自绘），只能按坐标点；而"布局两边各算一次、迟早错位"这个坑
 * 项目里已经吃过。所以把**这一帧真正用来命中的那份矩形**导出来给脚本。
 *
 * 坐标一律是**弹窗客户区坐标**；脚本自己换算到屏幕再发消息。
 */
void AppendDialogState(std::string* out) {
    const auto rectText = [](const RECT& r) {
        return std::to_string(r.left) + "," + std::to_string(r.top) + "," +
               std::to_string(r.right) + "," + std::to_string(r.bottom);
    };
    const auto handleText = [](HWND h) {
        return std::to_string(reinterpret_cast<unsigned long long>(h));
    };

    // ---- 会话设置窗 ----
    HWND roomDlg = g_roomSettingsWindow;  // 进程内全局，见它的声明处（不能用 FindWindowW）
    *out += "dialog.roomSettings.open=" + std::to_string(roomDlg != nullptr ? 1 : 0) + "\n";
    if (roomDlg != nullptr) {
        auto* state = reinterpret_cast<RoomSettingsState*>(
            GetWindowLongPtrW(roomDlg, GWLP_USERDATA));
        *out += "dialog.roomSettings.hwnd=" + handleText(roomDlg) + "\n";
        if (state != nullptr) {
            *out += "dialog.roomSettings.id=" + EscapeStateValue(state->sessionId) + "\n";
            *out += "dialog.roomSettings.edit=" + handleText(state->edit) + "\n";
            *out += "dialog.roomSettings.scrollTop=" + std::to_string(state->scrollTop) + "\n";
            *out += "dialog.roomSettings.rows=" + std::to_string(state->rows.size()) + "\n";
            for (std::size_t i = 0; i < state->rows.size(); ++i) {
                const RoomRow& row = state->rows[i];
                const std::string q = "dialog.roomSettings.row." + std::to_string(i) + ".";
                *out += q + "label=" + EscapeStateValue(row.label) + "\n";
                *out += q + "value=" + EscapeStateValue(row.value) + "\n";
                *out += q + "key=" + EscapeStateValue(row.key) + "\n";
                *out += q + "local=" + EscapeStateValue(row.local) + "\n";
                *out += q + "arg=" + EscapeStateValue(row.arg) + "\n";
                *out += q + "role=" + EscapeStateValue(row.role) + "\n";
                *out += q + "enabled=" + std::to_string(row.enabled ? 1 : 0) + "\n";
                *out += q + "on=" + std::to_string(row.on ? 1 : 0) + "\n";
                *out += q + "hasExtra=" + std::to_string(row.hasExtra ? 1 : 0) + "\n";
                *out += q + "row=" + rectText(row.rect) + "\n";
                *out += q + "box=" + rectText(row.box) + "\n";
                *out += q + "extra=" + rectText(row.extra) + "\n";
            }
        }
    }

    // ---- 群聊窗（新建 / 搜索）----
    *out += "dialog.group.open=" + std::to_string(g_groupWindow != nullptr ? 1 : 0) + "\n";
    if (g_groupWindow != nullptr) {
        auto* state = reinterpret_cast<GroupWindowState*>(
            GetWindowLongPtrW(g_groupWindow, GWLP_USERDATA));
        *out += "dialog.group.hwnd=" + handleText(g_groupWindow) + "\n";
        // 窗口**存不存在**和**看不看得见**是两件事：FindWindow 找得到隐藏窗口，
        // 而隐藏窗口点了没反应。少了这个字段，"点了没反应"就只能靠猜。
        *out += "dialog.group.visible=" +
                std::to_string(IsWindowVisible(g_groupWindow) ? 1 : 0) + "\n";
        if (state != nullptr) {
            *out += "dialog.group.create=" + rectText(state->createButton) + "\n";
            *out += "dialog.group.search=" + rectText(state->searchButton) + "\n";
            *out += "dialog.group.edit.id=" + handleText(state->idEdit) + "\n";
            *out += "dialog.group.edit.name=" + handleText(state->nameEdit) + "\n";
            *out += "dialog.group.edit.keyword=" + handleText(state->keywordEdit) + "\n";
            // 关键词原文也导出来：搜索搜不到时，第一件要分清的事就是
            // "客户端到底把什么词发出去了" —— 少了它就只能靠猜。
            {
                std::wstring keyword(
                    static_cast<std::size_t>(GetWindowTextLengthW(state->keywordEdit)), L'\0');
                if (!keyword.empty()) {
                    GetWindowTextW(state->keywordEdit, &keyword[0],
                                   static_cast<int>(keyword.size()) + 1);
                }
                *out += "dialog.group.keyword=" + EscapeStateValue(WideToUtf8(keyword)) + "\n";
            }
            *out += "dialog.group.hint=" + EscapeStateValue(state->hint) + "\n";
            RECT client{};
            GetClientRect(state->dlg, &client);
            for (std::size_t i = 0; i < g_groupHits.size() && i < 10; ++i) {
                *out += "dialog.group.join." + std::to_string(i) + "=" +
                        rectText(GroupJoinRect(state, client.right, i)) + "\n";
            }
        }
    }

    // ---- 群搜索结果的内容（点了搜索之后才有）----
    for (std::size_t i = 0; i < g_groupHits.size() && i < 10; ++i) {
        const GroupHit& hit = g_groupHits[i];
        const std::string q = "groupHits." + std::to_string(i) + ".";
        *out += q + "id=" + EscapeStateValue(hit.id) + "\n";
        *out += q + "name=" + EscapeStateValue(hit.name) + "\n";
        *out += q + "members=" + std::to_string(hit.members) + "\n";
        *out += q + "maxMembers=" + std::to_string(hit.maxMembers) + "\n";
        *out += q + "joinMode=" + EscapeStateValue(hit.joinMode) + "\n";
    }
}

LRESULT CALLBACK GroupWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<GroupWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* created = reinterpret_cast<GroupWindowState*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            created->dlg = hwnd;
            return TRUE;
        }
        case kMsgGroupHits:
            if (state) {
                state->hint = g_groupHits.empty()
                                  ? "没有找到公开的群（只有管理员勾了「向公众展示」的群才会出现在这里）"
                                  : ("搜到 " + std::to_string(g_groupHits.size()) + " 个群");
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH back = CreateSolidBrush(g_palette->panel);
            FillRect(dc, &client, back);
            DeleteObject(back);
            RECT title{0, 0, client.right, 52};
            HBRUSH titleBrush = CreateSolidBrush(g_palette->windowBg);
            FillRect(dc, &title, titleBrush);
            DeleteObject(titleBrush);
            TextOutUtf8(dc, L"群聊", RECT{18, 0, client.right - 18, 52}, ui.fontLarge,
                        g_palette->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            HBRUSH line = CreateSolidBrush(g_palette->border);
            RECT sep{0, 51, client.right, 52};
            FillRect(dc, &sep, line);
            DeleteObject(line);

            if (state) {
                TextOutUtf8(dc, L"群聊 ID", RECT{18, 66, 108, 92}, ui.font, g_palette->text,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                TextOutUtf8(dc, L"群聊名称", RECT{18, 102, 108, 128}, ui.font, g_palette->text,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                // 三个输入框的边框由这里画 —— EDIT 上已经没有 WS_EX_CLIENTEDGE 了，
                // 那个是系统用浅色 COLOR_3DHIGHLIGHT 画的 3D 边，深色主题下就是白框。
                //
                // 画在 EDIT **外面**一圈：画在它自己的矩形里会被 EDIT 重绘时盖掉。
                // 位置直接向 EDIT 要（GetWindowRect 再转成客户坐标），不在这里重算 ——
                // 布局的唯一真相是 LayoutGroupWindow，两边各算一次迟早错位。
                HBRUSH editFrame = CreateSolidBrush(g_palette->border);
                for (HWND editBox : {state->idEdit, state->nameEdit, state->keywordEdit}) {
                    RECT box{};
                    if (editBox == nullptr || !GetWindowRect(editBox, &box)) continue;
                    MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&box), 2);
                    InflateRect(&box, 1, 1);
                    FrameRect(dc, &box, editFrame);
                }
                DeleteObject(editFrame);
                ui::DrawRoundedControl(dc, state->createButton, 6, g_palette->panel,
                                       g_palette->accent, g_palette->accent);
                TextOutUtf8(dc, L"创建群聊", state->createButton, ui.font, g_palette->accentText,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                TextOutUtf8(dc, L"ID 只能用 3-16 位小写字母 / 数字 / 下划线 / 减号，建好后不可更改",
                            RECT{18, 138, client.right - 18, 160}, ui.fontSmall, g_palette->system,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                const int searchTop = static_cast<int>(state->resultsTop.top) - 44;
                TextOutUtf8(dc, L"关键词", RECT{18, searchTop, 108, searchTop + 26}, ui.font,
                            g_palette->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                ui::DrawRoundedControl(dc, state->searchButton, 6, g_palette->panel,
                                       g_palette->neutral, g_palette->neutralBorder);
                TextOutUtf8(dc, L"搜索群聊", state->searchButton, ui.font, g_palette->text,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // ---- 结果列表 ----
                if (!state->hint.empty()) {
                    TextOutUtf8(dc, Utf8ToWide(state->hint),
                                RECT{18, state->resultsTop.top, client.right - 18,
                                     state->resultsTop.top + 24},
                                ui.fontSmall, g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                int y = state->resultsTop.top + (state->hint.empty() ? 0 : 28);
                for (std::size_t i = 0; i < g_groupHits.size(); ++i) {
                    const GroupHit& hit = g_groupHits[i];
                    RECT row{18, y, client.right - 18, y + kGroupResultRowHeight - 6};
                    if (row.bottom > state->resultsTop.bottom) break;
                    TextOutUtf8(dc, Utf8ToWide(hit.name), RECT{row.left, row.top, row.right - 90, row.top + 22},
                                ui.font, g_palette->text,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    const std::string sub = hit.id + "　" + std::to_string(hit.members) + " / " +
                                            std::to_string(hit.maxMembers) + " 人　" +
                                            JoinModeLabelOf(hit.joinMode);
                    TextOutUtf8(dc, Utf8ToWide(sub),
                                RECT{row.left, row.top + 20, row.right - 90, row.bottom}, ui.fontSmall,
                                g_palette->system, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    RECT join{row.right - 78, row.top + 6, row.right, row.top + 30};
                    ui::DrawRoundedControl(dc, join, 6, g_palette->panel,
                                           static_cast<int>(i) == state->hoverJoin ? g_palette->hover
                                                                                   : g_palette->neutral,
                                           g_palette->neutralBorder);
                    TextOutUtf8(dc, L"加入", join, ui.fontSmall, g_palette->text,
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    y += kGroupResultRowHeight;
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            // 输入框跟随主题：不改的话是系统默认的白底黑字，在深色界面上很扎眼
            if (!state) break;
            HDC editDc = reinterpret_cast<HDC>(wp);
            SetTextColor(editDc, g_palette->text);
            SetBkColor(editDc, g_palette->windowBg);
            if (!state->fieldBrush) state->fieldBrush = CreateSolidBrush(g_palette->windowBg);
            return reinterpret_cast<LRESULT>(state->fieldBrush);
        }
        case WM_MOUSEMOVE: {
            if (!state) break;
            RECT client{};
            GetClientRect(state->dlg, &client);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            int hoverJoin = -1;
            for (std::size_t i = 0; i < g_groupHits.size(); ++i) {
                const RECT join = GroupJoinRect(state, client.right, i);
                if (PtInRect(&join, POINT{x, y})) hoverJoin = static_cast<int>(i);
            }
            if (hoverJoin != state->hoverJoin) {
                state->hoverJoin = hoverJoin;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!state) break;
            RECT client{};
            GetClientRect(state->dlg, &client);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (PtInRect(&state->createButton, POINT{x, y})) {
                std::wstring id(static_cast<std::size_t>(GetWindowTextLengthW(state->idEdit)), L'\0');
                if (!id.empty()) GetWindowTextW(state->idEdit, &id[0], static_cast<int>(id.size()) + 1);
                std::wstring name(static_cast<std::size_t>(GetWindowTextLengthW(state->nameEdit)), L'\0');
                if (!name.empty()) GetWindowTextW(state->nameEdit, &name[0], static_cast<int>(name.size()) + 1);
                const std::string idText = WideToUtf8(id);
                const std::string nameText = WideToUtf8(name);
                if (idText.empty() || nameText.empty()) {
                    state->hint = "群聊 ID 和群聊名称都要填";
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                // 先在本地校验一遍 ID：不合规的 ID 让服务器拒也行，但本地判能立刻
                // 告诉用户"哪个字符不行"，而不用等一个来回
                dchat::RoomIdError idError = dchat::RoomIdError::None;
                const std::string normalized = dchat::NormalizeGroupId(idText, &idError);
                if (normalized.empty()) {
                    state->hint = dchat::RoomIdErrorText(idError);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                SendRoomCommand("GCREATE",
                                normalized + " " + dchat::Base64Encode(nameText));
                state->hint = "已创建「" + nameText + "」（ID：" + normalized + "）";
                SetWindowTextW(state->idEdit, L"");
                SetWindowTextW(state->nameEdit, L"");
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            if (PtInRect(&state->searchButton, POINT{x, y})) {
                std::wstring keyword(static_cast<std::size_t>(GetWindowTextLengthW(state->keywordEdit)), L'\0');
                if (!keyword.empty()) {
                    GetWindowTextW(state->keywordEdit, &keyword[0], static_cast<int>(keyword.size()) + 1);
                }
                const std::string text = WideToUtf8(keyword);
                if (text.empty()) {
                    state->hint = "先填一个关键词再搜";
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                g_groupHits.clear();
                g_groupSearchDone = false;
                state->hint = "正在搜索…";
                SendRoomCommand("GSEARCH", dchat::Base64Encode(text));
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            if (state->hoverJoin >= 0 &&
                state->hoverJoin < static_cast<int>(g_groupHits.size())) {
                SendRoomCommand("GJOIN", g_groupHits[state->hoverJoin].id);
                state->hint = "已申请加入「" + g_groupHits[state->hoverJoin].name + "」";
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (state && state->fieldBrush) {
                DeleteObject(state->fieldBrush);
                state->fieldBrush = nullptr;
            }
            // 通知 OpenGroupWindow 里那个模态嵌套循环：窗口没了，该退出了。
            if (state && state->closed) *state->closed = true;
            g_groupWindow = nullptr;
            // **不要在这里 PostQuitMessage**：它是线程级的，主循环也会收到 WM_QUIT，
            // 于是"关掉群聊窗"就变成了"退出整个客户端"。
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenGroupWindow(HWND parent) {
    if (g_groupWindow) {
        SetForegroundWindow(g_groupWindow);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = GroupWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kGroupWindowClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    GroupWindowState state;
    // 模态循环的退出信号：窗口销毁时由 WM_DESTROY 置位（见 state.closed 的说明）。
    bool closed = false;
    state.closed = &closed;
    RECT rc{0, 0, kGroupWndWidth, kGroupWndHeight};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND dlg = CreateWindowExW(kDlgExStyle, kGroupWindowClass, L"群聊", kDlgStyle, x, y, width,
                               height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return;
    g_groupWindow = dlg;

    // 三个输入框：ID / 名称 / 搜索关键词。用系统 EDIT，中文输入法才能正常工作
    // （自绘的输入控件要自己处理 IME 组合串，那是另一个量级的活）。
    //
    // ⚠️ **不要加 WS_EX_CLIENTEDGE**：那是系统画的 3D 边框，用的是
    // COLOR_3DHIGHLIGHT（浅色），而 WM_CTLCOLOREDIT 只改背景、改不了边框 ——
    // 深色主题下就成了三个刺眼的白框（用户截图反馈的"白边"就是这个）。
    // 边框改由父窗口在 WM_PAINT 里按 g_palette->border 自己描一圈。
    auto makeEdit = [&](const wchar_t* initial) {
        HWND edit = CreateWindowExW(0, L"EDIT", initial,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                                    10, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
        // 没有边框了，文字会贴着边缘；左右各留 8px，看起来才像一个输入框
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        return edit;
    };
    state.idEdit = makeEdit(L"");
    state.nameEdit = makeEdit(L"");
    state.keywordEdit = makeEdit(L"");
    LayoutGroupWindow(&state);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(state.idEdit);
    LayoutGroupWindow(&state);

    // 模态嵌套循环：**靠 closed 退出**，不靠 PostQuitMessage（那个是线程级的，
    // 会把主循环一起干掉 = 关掉群聊窗就退出整个客户端）。
    MSG msg;
    while (!closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

void OpenSettings(HWND parent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // 背景由 WM_PAINT 用主题色画
        wc.lpszClassName = kSettingsClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    SettingsState state;
    state.themeMode = g_themeMode;
    state.colorEnabled = g_colorEnabled;
    state.voiceLimit = g_voiceLimitSeconds;

    RECT rc{0, 0, dchat::kSettingsWidth, dchat::kSettingsHeight};
    AdjustWindowRectEx(&rc, kDlgStyle, FALSE, kDlgExStyle);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
        if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
        if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    }

    HWND dlg = CreateWindowExW(kDlgExStyle, kSettingsClass, L"设置", kDlgStyle, x, y, width, height,
                               parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return;
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(state.hostEdit);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
            (msg.hwnd == dlg || IsChild(dlg, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(dlg);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                ApplySettingsAndClose(&state);
                continue;
            }
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    SetFocus(ui.hInput);
}

// ---------------- 界面行为 ----------------
void DoConnect(HWND hwnd) {
    if (IsConnected()) return;
    ConnectDialogState state;
    // 只记住上次连的服务器（地址 + 端口）；账号在连上之后的第二步才填
    state.host = g_host;
    state.port = g_port;
    if (!PromptConnect(hwnd, state)) return;

    ViewAddItem(ItemKind::Notice,
                "正在连接 " + state.host + ":" + std::to_string(state.port) + " ...",
                dchat::NowTimeString());
    std::string error;
    if (!ConnectToServer(state.host, state.port, &error)) {
        ViewAddItem(ItemKind::Error, error, dchat::NowTimeString());
        return;
    }
    g_host = state.host;
    g_port = state.port;
    g_authed = false;   // 已经连上，但还没登录
    UpdateStatus();

    // 第二步：连上以后再登录 / 注册（两个独立界面，可以来回切换）
    AuthDialogState auth;
    auth.host = state.host;
    auth.port = state.port;
    if (PromptAuth(hwnd, auth)) {
        if (g_nick.empty()) g_nick = auth.user;
        ViewAddItem(ItemKind::Notice, "已登录：" + g_nick, dchat::NowTimeString());
        UpdateStatus();
        return;
    }
    // 用户在账号窗口里点了「断开」/ 关掉了窗口：这次连接就不要了
    if (IsConnected()) SendRawLine(dchat::BuildLine("QUIT"));
    DisconnectFromServer(false);
    ViewAddItem(ItemKind::Notice, "已取消登录，连接已断开", dchat::NowTimeString());
}

void DoDisconnect(HWND hwnd) {
    if (!IsConnected()) return;
    g_userDisconnected = true;          // 用户自己要断的，别自动连回去
    g_reconnect.Cancel();
    KillTimer(hwnd, kReconnectTimerId);
    SendRawLine(dchat::BuildLine("QUIT"));
    DisconnectFromServer(true);
    (void)hwnd;
}

// ---------------- 断线自动重连 ----------------
//
// 分工：判断（该不该连、等多久、何时放弃）全在 dchat::ReconnectState 里，
// 有单测；这里只做三件事——SetTimer、在到点时发起一次连接、把结果喂回状态机。

/** 按状态机给的延迟挂一次重连计时；负数 = 不连了。 */
void ScheduleReconnect(HWND hwnd, int delayMs) {
    KillTimer(hwnd, kReconnectTimerId);
    if (delayMs < 0) {
        if (g_authed || g_reconnect.wanted) {
            ViewAddItem(ItemKind::Error,
                        "自动重连已放弃（试了 " + std::to_string(g_reconnect.policy.maxAttempts) +
                            " 次）。网络好了可以手动点「连接」。",
                        dchat::NowTimeString());
        }
        return;
    }
    ViewAddItem(ItemKind::Notice,
                "连接断了，" + std::to_string(delayMs / 1000) + " 秒后自动重连（第 " +
                    std::to_string(g_reconnect.attempt + 1) + "/" +
                    std::to_string(g_reconnect.policy.maxAttempts) + " 次）",
                dchat::NowTimeString());
    SetTimer(hwnd, kReconnectTimerId, static_cast<UINT>(delayMs), nullptr);
}

/** 到点了：在工作线程里连一次，结果回到界面线程处理。 */
void BeginReconnectAttempt(HWND hwnd) {
    if (!g_reconnect.wanted || IsConnected()) return;
    const std::string host = g_host;
    const int port = g_port;
    ViewAddItem(ItemKind::Notice, "正在重连 " + host + ":" + std::to_string(port) + " ...",
                dchat::NowTimeString());
    std::thread([hwnd, host, port]() {
        std::string error;
        const bool ok = ConnectToServer(host, port, &error);
        auto* payload = new std::string(error);
        if (!PostMessageW(hwnd, WM_APP_RECONNECT, ok ? 1 : 0,
                          reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

/** 重连连上了：用记住的账号自动登录（forceAutoLogin）。 */
void FinishReconnectAttempt(HWND hwnd, bool connected, const std::string& error) {
    if (!g_reconnect.wanted) return;  // 期间用户自己断开/退出了
    if (!connected) {
        ViewAddItem(ItemKind::Error, "重连失败：" + error, dchat::NowTimeString());
        ScheduleReconnect(hwnd, g_reconnect.OnAttemptFailed());
        return;
    }
    g_authed = false;
    g_userDisconnected = false;
    UpdateStatus();
    AuthDialogState auth;
    auth.host = g_host;
    auth.port = g_port;
    if (PromptAuth(hwnd, auth, true)) {
        if (g_nick.empty()) g_nick = auth.user;
        g_reconnect.OnSucceeded();
        ViewAddItem(ItemKind::Notice, "已重新连上：" + g_nick, dchat::NowTimeString());
        UpdateStatus();
        return;
    }
    // 自动登录没成功（没有记住的账号、或者用户把窗口关了）
    if (IsConnected()) SendRawLine(dchat::BuildLine("QUIT"));
    DisconnectFromServer(false);
    ScheduleReconnect(hwnd, g_reconnect.OnAttemptFailed());
}

void SendCurrentInput() {
    if (!IsConnected() || !ui.hInput) return;
    const int length = GetWindowTextLengthW(ui.hInput);
    if (length <= 0) return;
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(ui.hInput, &text[0], length + 1);
    // 本机指令（/chatcolor 之类）就地处理，不发到服务器——
    // 它们只是改输入框内容或往记录区写帮助，不需要联网
    if (HandleLocalCommand(text)) return;
    SetWindowTextW(ui.hInput, L"");
    const std::string utf8 = WideToUtf8(text);
    if (utf8.empty()) return;
    HideSuggestions();  // 发出去了，候选浮层收起来
    // 转义换行和反斜杠：协议是行式的，不转义的话多行文本会被 BuildLine 截断成一行，
    // 而且服务端也没法按"行数"做限制。
    const std::string escaped = dchat::EscapeText(utf8);
    if (!SendRawLine(dchat::BuildLine("MSG", escaped))) {
        ViewAddItem(ItemKind::Error, "发送失败，连接可能已断开", dchat::NowTimeString());
        DisconnectFromServer(false);
        return;
    }
    // 改密码：把新密码记在"待确认"里，等服务端的成功回执再更新自动登录凭据。
    // 只认**改自己**的那种写法（一个参数）；`/cp <昵称> <新密码>` 是控制台专用，
    // 在这里发出去只会被服务器拒掉。
    if (dchat::IsCommandAttempt(utf8)) {
        const dchat::ServerCommand command = dchat::ParseServerCommand(utf8);
        if (command.knownCommand && command.kind == dchat::ServerCommand::Kind::ChangePassword &&
            command.name.empty() && !command.password.empty()) {
            g_pendingNewPassword = command.password;
            g_pendingNewPasswordAt = GetTickCount64();
        }
    }
    // 记进历史（按 ↑ 能翻回来）：聊天和指令都记，只有 /changepassword 会把密码去掉，
    // 那样指令能翻回来、密码又不会重新出现在输入框里
    g_inputHistory.Add(dchat::HistoryTextFor(utf8));
}

std::wstring CurrentInputText() {
    if (!ui.hInput) return std::wstring();
    const int length = GetWindowTextLengthW(ui.hInput);
    if (length <= 0) return std::wstring();
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(ui.hInput, &text[0], length + 1);
    return text;
}

void SetInputText(const std::wstring& text) {
    if (!ui.hInput) return;
    SetWindowTextW(ui.hInput, text.c_str());
    const int length = GetWindowTextLengthW(ui.hInput);
    SendMessageW(ui.hInput, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
}

// ---------------- /chatcolor：彩色文字代码 ----------------
//
// 三个入口（`/charcolor` 是常见的手误写法，一并认）：
//   /chatcolor            和 help 一样
//   /chatcolor help       常用色码对照表（同时把表放进聊天记录，方便对照）
//   /chatcolor choose     弹色板；选一个颜色就把 `#rrggbb` 放进输入框
//
// 色板那条"未确定就再选一次"的约定：**没按确定键之前，上一次挑的色码会被这次覆盖**，
// 不会越堆越长（用户明确要求过）。
// ---------------- 取色盘（Windows 画图那种色相环 + 饱和度/明度方块）----------------
//
// 以前这里是一个 32 格的固定色板，颜色太少。现在是真色盘：
//   - 外圈色相环：点哪儿取哪个色相
//   - 中间方块：横轴饱和度、纵轴明度，色相沿用环上选的
//   - 底部：当前色 / 原色对比、十六进制输入框、一排常用色
// 几何与 HSV 换算全在 src/color_picker.cpp（纯计算 + 47 项单测）。
// 取色盘在 src/picker_dialog.cpp（绘制也在一起：几何和绘制必须共用一套）

// 上一次放进输入框、还没确定的那段色码：**没按确定之前再选一次要覆盖它**，
// 不然色码会越堆越长（用户明确要求过）
std::wstring g_lastPickedCode;

// 把色码放进输入框：先删掉上一次"还没确定"的那段，再插入这次的
void InsertColorCode(const std::wstring& code) {
    std::wstring text = CurrentInputText();
    if (!g_lastPickedCode.empty()) {
        const std::size_t at = text.find(g_lastPickedCode);
        if (at != std::wstring::npos) text.erase(at, g_lastPickedCode.size());
    }
    text = code + text;  // 色码放最前面：后面的文字都用这个颜色
    SetInputText(text);
    g_lastPickedCode = code;
}

void ChatColorCommand(HWND hwnd, const std::wstring& argument) {
    std::wstring action = argument;
    for (wchar_t& ch : action) ch = static_cast<wchar_t>(towlower(ch));
    while (!action.empty() && action.front() == L' ') action.erase(action.begin());
    while (!action.empty() && action.back() == L' ') action.pop_back();

    if (action.empty() || action == L"help" || action == L"?" || action == L"列表") {
        ViewAddItem(ItemKind::Notice, WideToUtf8(dchat::ChatColorHelpText()),
                    dchat::NowTimeString());
        return;
    }
    if (action == L"choose" || action == L"choose " || action == L"色板" || action == L"pick") {
        // 打开时以输入框里已有的色码为起点（没有就用白色）
        COLORREF initial = RGB(255, 255, 255);
        const std::wstring existing = CurrentInputText();
        const std::size_t at = existing.find(L'#');
        if (at != std::wstring::npos) {
            dchat::ParseHexColor(existing.substr(at, 7), &initial);
        }
        COLORREF color = 0;
        dchat::SetPickerFonts(ui.font, ui.fontSmall);
        if (!dchat::ShowColorPicker(hwnd, initial, &color)) return;  // 取消 = 这次不选
        InsertColorCode(dchat::ColorToHex(color));
        return;
    }
    ViewAddItem(ItemKind::Error, "用法：/chatcolor help 看色码表，/chatcolor choose 弹色盘",
                dchat::NowTimeString());
}

// 发送前拦一道：`/chatcolor ...` / `/charcolor ...` 是本机指令，不发到服务器
bool HandleLocalCommand(const std::wstring& text) {
    if (text.size() < 2 || text[0] != L'/') return false;
    std::wstring name = text.substr(1);
    const std::size_t space = name.find(L' ');
    std::wstring argument;
    if (space != std::wstring::npos) {
        argument = name.substr(space + 1);
        name = name.substr(0, space);
    }
    for (wchar_t& ch : name) ch = static_cast<wchar_t>(towlower(ch));
    // 两种拼写都认（用户可能按英式/美式习惯打）
    if (name == L"chatcolor" || name == L"charcolor" || name == L"chatcolour" ||
        name == L"color" || name == L"colour") {
        ChatColorCommand(ui.hwnd, argument);
        g_inputHistory.Add(WideToUtf8(text));
        SetWindowTextW(ui.hInput, L"");
        g_lastPickedCode.clear();  // 指令走完了，不再"未确定"
        return true;
    }
    return false;
}

WNDPROC g_oldInputProc = nullptr;

LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            SendCurrentInput();
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) {
            if (!g_suggest.matches.empty()) {
                // 候选浮层开着：↑↓ 在候选之间移动（MC 里 Tab 是循环，方向键同样能选）
                const int count = static_cast<int>(g_suggest.matches.size());
                int index = g_suggestPicked;
                if (index < 0 || index >= count) {
                    index = (wp == VK_UP) ? count - 1 : 0;
                } else {
                    index = (wp == VK_UP) ? (index - 1 + count) % count : (index + 1) % count;
                }
                ApplySuggestion(index);
                return 0;
            }
            // 类似 Minecraft：上键翻出自己刚发过的内容，下键往回翻，翻到底回到原来的草稿
            g_inputHistory.SetDraft(WideToUtf8(CurrentInputText()));
            std::string recalled;
            const bool changed =
                wp == VK_UP ? g_inputHistory.Up(&recalled) : g_inputHistory.Down(&recalled);
            if (changed) SetInputText(Utf8ToWide(recalled));
            return 0;  // 吞掉按键，避免默认行为把光标跳到行首/行尾
        }
        if (wp == VK_ESCAPE) {
            g_inputHistory.ResetBrowse();  // 退出翻历史状态
            HideSuggestions();             // 顺手把候选浮层收起来
            return 0;
        }
        if (wp == VK_TAB) {
            // 类似 Minecraft：Tab 在候选里循环（候选显示在输入框上方的浮层里）
            const std::string current = WideToUtf8(CurrentInputText());
            const dchat::CompletionResult done =
                g_tabComplete.Next(current, g_onlineNicks, g_knownNicks);
            if (done.text != current) SetInputText(Utf8ToWide(done.text));
            g_suggest = done;
            g_suggestPicked = done.picked;
            g_suggestHover = -1;
            const bool show = !done.matches.empty();
            if (ui.hSuggest) {
                if (show) {
                    if (!IsWindowVisible(ui.hSuggest)) {  // 刚出现：摆好位置再显示
                        ShowWindow(ui.hSuggest, SW_SHOWNOACTIVATE);
                        Layout(ui.hwnd);
                    }
                    InvalidateSuggest();
                } else {
                    ShowWindow(ui.hSuggest, SW_HIDE);
                }
            }
            return 0;
        }
    }
    return CallWindowProcW(g_oldInputProc, hwnd, msg, wp, lp);
}

int SuggestPanelHeight();  // 定义在下面（输入框上方的候选浮层）
void RefreshSuggestions();
void HideSuggestions();

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    LayoutWindowChrome(hwnd);  // 菜单栏 / 状态条 / 输入行的几何（ui_layout）

    // 记录区：左右各留一点边距，上下紧贴菜单栏与状态条。
    // 多会话时给左侧会话列表让出位置（左栏只在服务端确认支持多会话后才出现）。
    const int listWidth = g_roomsEnabled ? dchat::kSessionListWidth : 0;
    const int viewTop = g_contentTop;
    const int viewBottom = g_bottom.statusBar.top;
    const int viewHeight = viewBottom - viewTop;
    MoveWindow(ui.hView, listWidth + 12, viewTop, width - listWidth - 24,
               viewHeight > 60 ? viewHeight : 60, TRUE);

    // 输入框：父窗口画胶囊背景，编辑框内嵌在里面
    // （这样文字天然有内边距，圆角也不会被方形底色盖住）
    g_inputPill = g_bottom.inputPill;
    const int innerPadX = dchat::kMessagePaddingX;
    // **编辑框的高度要贴住文字本身**，再整体竖直居中。
    // 以前是"胶囊高度减去上下各 5 像素"，那个高度比文字行高大，单行编辑框默认把文字
    // 顶在上边，看起来就是"文字偏上"。
    int textHeight = 0;
    {
        HDC dc = GetDC(hwnd);
        HGDIOBJ oldFont = SelectObject(dc, ui.font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        textHeight = metrics.tmHeight;
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd, dc);
    }
    const int pillHeight = g_inputPill.bottom - g_inputPill.top;
    const int editHeight = textHeight > 0 && textHeight < pillHeight ? textHeight : pillHeight - 10;
    const int editTop = g_inputPill.top + (pillHeight - editHeight) / 2;
    MoveWindow(ui.hInput, g_inputPill.left + innerPadX, editTop,
               (g_inputPill.right - g_inputPill.left) - innerPadX * 2, editHeight, TRUE);
    MoveWindow(ui.hSend, g_bottom.sendButton.left, g_bottom.sendButton.top,
               g_bottom.sendButton.right - g_bottom.sendButton.left,
               g_bottom.sendButton.bottom - g_bottom.sendButton.top, TRUE);

    // 候选浮层：贴在输入框正上方（高度随候选行数变化）
    const int suggestH = SuggestPanelHeight();
    if (ui.hSuggest && suggestH > 0) {
        MoveWindow(ui.hSuggest, g_inputPill.left, g_inputPill.top - 6 - suggestH,
                   g_inputPill.right - g_inputPill.left, suggestH, TRUE);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------- 输入框上方的候选浮层（类似 Minecraft）----------------
// 面板里的一行：要么是分组标题，要么是最多 kSuggestColumns 个候选
struct SuggestLine {
    bool isTitle = false;
    std::string title;
    std::vector<std::size_t> items;
    int top = 0;
    int bottom = 0;
};

// 把候选按分组排成若干行（分组名相同的候选连续排列）
std::vector<SuggestLine> LayoutSuggestPanel() {
    std::vector<SuggestLine> lines;
    std::string current;
    int y = kSuggestPadY;
    for (std::size_t i = 0; i < g_suggest.matches.size(); ++i) {
        const std::string group =
            i < g_suggest.groups.size() ? g_suggest.groups[i] : std::string();
        if (group != current) {
            SuggestLine title;
            title.isTitle = true;
            title.title = group;
            title.top = y;
            title.bottom = y + kSuggestTitleH;
            lines.push_back(title);
            y += kSuggestTitleH;
            current = group;
        }
        if (lines.empty() || lines.back().isTitle ||
            static_cast<int>(lines.back().items.size()) >= kSuggestColumns) {
            SuggestLine line;
            line.title = group;
            line.top = y;
            line.bottom = y + kSuggestRowH;
            lines.push_back(line);
            y += kSuggestRowH;
        }
        lines.back().items.push_back(i);
    }
    return lines;
}

RECT SuggestCellRect(const SuggestLine& line, int width, std::size_t positionInLine) {
    const int usable = width - kSuggestPadX * 2;
    const int cellW = usable > 0 ? usable / kSuggestColumns : 1;
    const int left = kSuggestPadX + static_cast<int>(positionInLine) * cellW;
    return RECT{left, line.top, left + cellW, line.bottom};
}

int SuggestPanelHeight() {
    if (g_suggest.matches.empty()) return 0;
    const std::vector<SuggestLine> lines = LayoutSuggestPanel();
    int height = kSuggestPadY * 2;
    for (const SuggestLine& line : lines) height += (line.bottom - line.top);
    return height;
}

int SuggestIndexAt(int x, int y) {
    RECT client{};
    if (ui.hSuggest) GetClientRect(ui.hSuggest, &client);
    const std::vector<SuggestLine> lines = LayoutSuggestPanel();
    for (const SuggestLine& line : lines) {
        if (line.isTitle || y < line.top || y >= line.bottom) continue;
        for (std::size_t i = 0; i < line.items.size(); ++i) {
            const RECT cell = SuggestCellRect(line, client.right, i);
            if (x >= cell.left && x < cell.right) return static_cast<int>(line.items[i]);
        }
    }
    return -1;
}

void InvalidateSuggest() {
    if (ui.hSuggest) InvalidateRect(ui.hSuggest, nullptr, TRUE);
}

// 输入内容变了 / 按了 Tab：重新算候选，显示或隐藏浮层
void RefreshSuggestions() {
    if (!ui.hInput || !ui.hSuggest) return;
    if (g_suggestSuppress) return;  // 程序自己改的输入框内容，保持当前候选和选中项
    const std::string text = WideToUtf8(CurrentInputText());
    const int keep = g_suggestPicked;
    g_suggest = dchat::Suggest(text, g_onlineNicks, g_knownNicks);
    g_suggestPicked = (keep >= 0 && keep < static_cast<int>(g_suggest.matches.size())) ? keep : -1;
    g_suggestHover = -1;
    const bool show = !g_suggest.matches.empty();
    ShowWindow(ui.hSuggest, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) {
        Layout(ui.hwnd);  // 面板高度可能变了，重新摆一下
        InvalidateSuggest();
    }
}

void HideSuggestions() {
    g_suggest = dchat::CompletionResult();
    g_suggestPicked = -1;
    g_suggestHover = -1;
    if (ui.hSuggest) ShowWindow(ui.hSuggest, SW_HIDE);
}

// 选中某个候选：把输入框里正在补的那个词换成它
void ApplySuggestion(int index) {
    if (index < 0 || index >= static_cast<int>(g_suggest.matches.size())) return;
    const std::string picked = g_suggest.matches[static_cast<std::size_t>(index)];
    g_suggestSuppress = true;  // 让下面这次 SetWindowText 不要重算候选
    SetInputText(Utf8ToWide(g_suggest.isArgument ? (g_suggest.head + picked) : picked));
    g_suggestSuppress = false;
    g_suggestPicked = index;  // 候选列表保持不变，只移动高亮（Tab / ↑↓ 都能继续选）
    // 只是换个高亮：面板的位置和大小都没变，重画它就行。
    // 千万别在这里调 Layout()——那会把所有子窗口重新摆放一遍，
    // 每按一次 ↑↓ 整块界面重建，看起来就是"黑一下"。
    if (ui.hSuggest && !IsWindowVisible(ui.hSuggest)) {
        ShowWindow(ui.hSuggest, SW_SHOWNOACTIVATE);
    }
    InvalidateSuggest();
}

LRESULT CALLBACK SuggestProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            RECT client{};
            GetClientRect(wnd, &client);
            // 双缓冲：先在内存里画好整块面板，再一次性贴上去。
            // 直接在窗口 DC 上边画边刷，快速连按 ↑↓ 时会看到花屏/残影。
            const int width = client.right - client.left, height = client.bottom - client.top;
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
            HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
            // 先把整块矩形铺成窗口底色再画圆角：浮层是独立窗口，只画圆角的话
            // 四角会露出窗口自己的黑色底，每次重排/重画就是"黑一下"
            HBRUSH background = CreateSolidBrush(g_palette->windowBg);
            FillRect(memory, &client, background);
            DeleteObject(background);
            ui::FillRoundedRect(memory, client, 8, g_palette->noticeBg, g_palette->border, 1.0f);
            const HDC paint = memory;   // 下面统一画到内存 DC 上
            const std::vector<SuggestLine> lines = LayoutSuggestPanel();
            for (const SuggestLine& line : lines) {
                if (line.isTitle) {  // 分组标题（灰色小字）
                    RECT titleRect{kSuggestPadX, line.top, client.right - kSuggestPadX,
                                   line.bottom};
                    DrawTextIn(paint, Utf8ToWide(line.title), titleRect, ui.fontSmall,
                               g_palette->time,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    continue;
                }
                for (std::size_t k = 0; k < line.items.size(); ++k) {
                    const std::size_t index = line.items[k];
                    RECT cell = SuggestCellRect(line, client.right, k);
                    const bool picked = (static_cast<int>(index) == g_suggestPicked);
                    const bool hover = (static_cast<int>(index) == g_suggestHover);
                    if (picked) {
                        ui::FillRoundedRect(paint, cell, 4, g_palette->accent, g_palette->accent,
                                            0.0f);
                    } else if (hover) {
                        ui::FillRoundedRect(paint, cell, 4, g_palette->neutral, g_palette->border,
                                            0.0f);
                    }
                    // 候选名 + 后面灰色的用法说明（按名字实际宽度接着画）
                    RECT nameRect = cell;
                    nameRect.left += 6;
                    nameRect.right -= 4;
                    const std::wstring name = Utf8ToWide(g_suggest.matches[index]);
                    const int nameWidth = [&] {
                        HGDIOBJ oldFont = SelectObject(paint, ui.fontSmall);
                        SIZE size{};
                        GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()),
                                              &size);
                        SelectObject(dc, oldFont);
                        return static_cast<int>(size.cx);
                    }();
                    DrawTextIn(paint, name, nameRect, ui.fontSmall,
                               picked ? g_palette->accentText : g_palette->text,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (index < g_suggest.hints.size() && !g_suggest.hints[index].empty() &&
                        nameWidth + 8 < nameRect.right - nameRect.left) {
                        RECT hintRect = nameRect;
                        hintRect.left += nameWidth + 8;
                        DrawTextIn(paint, Utf8ToWide(g_suggest.hints[index]), hintRect,
                                   ui.fontSmall, picked ? g_palette->accentText : g_palette->time,
                                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                       DT_END_ELLIPSIS);
                    }
                }
            }
            BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
            SelectObject(memory, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const int index = SuggestIndexAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index != g_suggestHover) {
                g_suggestHover = index;
                InvalidateSuggest();
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = wnd;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_suggestHover != -1) {
                g_suggestHover = -1;
                InvalidateSuggest();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const int index = SuggestIndexAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index >= 0) ApplySuggestion(index);
            SetFocus(ui.hInput);  // 点完候选继续打字
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            ui.hwnd = hwnd;
            ui.font = CreateFontW(dchat::kMessageFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Microsoft YaHei UI");
            // 系统提示用小一号字体
            ui.fontSmall = CreateFontW(dchat::kNoticeFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            // 全服公告用更大的字号 + 半粗体
            ui.fontLarge = CreateFontW(dchat::kAnnounceFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            WNDCLASSEXW viewClass{};
            viewClass.cbSize = sizeof(viewClass);
            viewClass.style = CS_HREDRAW | CS_VREDRAW;
            viewClass.lpfnWndProc = ViewProc;
            viewClass.hInstance = GetModuleHandleW(nullptr);
            viewClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            viewClass.hbrBackground = nullptr;
            viewClass.lpszClassName = kViewClass;
            RegisterClassExW(&viewClass);

            ui.hView = CreateWindowExW(0, kViewClass, L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS, 0, 0,
                                       10, 10, hwnd, reinterpret_cast<HMENU>(IDC_VIEW), nullptr,
                                       nullptr);
            // 候选浮层：在记录区之后创建，所以画在它上面；平时隐藏
            WNDCLASSEXW suggestClass{};
            suggestClass.cbSize = sizeof(suggestClass);
            suggestClass.style = CS_HREDRAW | CS_VREDRAW;
            suggestClass.lpfnWndProc = SuggestProc;
            suggestClass.hInstance = GetModuleHandleW(nullptr);
            suggestClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            suggestClass.hbrBackground = nullptr;
            suggestClass.lpszClassName = kSuggestClass;
            RegisterClassExW(&suggestClass);
            ui.hSuggest = CreateWindowExW(0, kSuggestClass, L"", WS_CHILD, 0, 0, 10, 10, hwnd,
                                          nullptr, nullptr, nullptr);
            ui.hInput = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,
                                        0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_INPUT), nullptr,
                                        nullptr);
            // 让文字与胶囊边缘留出内边距（否则文字会贴着圆角开始，看着不贴合）
            SendMessageW(ui.hInput, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(dchat::kMessagePaddingX, dchat::kMessagePaddingX));
            // 整个界面只剩这一个真正的按钮：「发送」。
            // 「连接/断开」「设置」「彩色聊天」「深浅色」「录音」「发送文件」全部收进了
            // 菜单栏和「＋」——以前顶部一条挤 6 个按钮，偶尔改一次的设置和每次都用
            // 的发消息混在一起，占掉一整条还把聊天挤矮。
            ui.hSend = CreateWindowW(L"BUTTON", L"发送", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                                     10, 10, hwnd, reinterpret_cast<HMENU>(IDC_SEND), nullptr,
                                     nullptr);

            SendMessageW(ui.hInput, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SendMessageW(ui.hSend, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            g_oldInputProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(ui.hInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            SubclassControl(ui.hSend, ButtonProc);
            DragAcceptFiles(hwnd, TRUE);  // 允许把文件直接拖进窗口发送

            const std::string now = dchat::NowTimeString();
            ViewAddItem(ItemKind::Notice, "欢迎使用 dchat 客户端", now);
            ViewAddItem(ItemKind::Notice, "先启动服务器（dchat_server.exe），再点菜单里的「连接」",
                        now);
            ViewAddItem(ItemKind::Notice, "本机测试：127.0.0.1，端口 5555；@all 通知所有人", now);
            ViewAddItem(ItemKind::Notice,
                        "发文件和语音点输入框右边的「＋」；主题、彩色聊天、服务器地址点右上角齿轮",
                        now);

            ApplyThemeMode();
            Layout(hwnd);
            UpdateStatus();
            UpdateTitle();
            SetFocus(ui.hInput);
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item && item->CtlType == ODT_BUTTON) DrawOwnerButton(item);
            return TRUE;
        }
        case WM_COMMAND: {
            if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_INPUT) {
                // 编辑框是透明的（背景由父窗口画），内容变化时让父窗口重画胶囊背景，
                // 否则文字移动会留下残影
                InvalidateRect(hwnd, &g_inputPill, TRUE);
                InvalidateRect(ui.hInput, nullptr, TRUE);
                RefreshSuggestions();  // 候选浮层随输入实时更新
                return 0;
            }
            switch (LOWORD(wp)) {
                case IDC_SEND:
                    SendCurrentInput();
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_SETTINGCHANGE:
            if (g_themeMode == ThemeMode::System) ApplyThemeMode();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) ClearUnread();
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息只会发给焦点窗口，这里转发给鼠标底下的记录区
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (WindowFromPoint(pt) == ui.hView) {
                SendMessageW(ui.hView, WM_MOUSEWHEEL, wp, lp);
                return 0;
            }
            break;
        }
        case WM_APP_LINE: {
            auto* payload = reinterpret_cast<std::string*>(lp);
            if (payload) {
                const dchat::Message raw = dchat::ParseLine(*payload);
                // 文件传输的几条消息不进记录区，由文件模块自己处理。
                // RFILE_ 是"群聊/私聊里的附件"（比 FILE_ 多一格房间号），
                // 也走同一个模块：文件 ID 是全局唯一的，下载流程完全一样。
                if (raw.command.compare(0, 5, "FILE_") == 0 ||
                    raw.command.compare(0, 6, "RFILE_") == 0) {
                    HandleFileLine(*payload);
                    delete payload;
                    return 0;
                }

                // ---- 多会话（私聊 / 群聊）的控制行：全部不进记录区 ----
                if (raw.command == "FEATURES") {
                    // 服务端确认能力。**必须以它为准**：没有 rooms1 就说明对面是旧版
                    // 服务端，这时要老老实实退回"只有大厅"的老界面，而不是发一堆
                    // 它看不懂的命令然后什么都不发生。
                    g_roomsEnabled = raw.rest.find("rooms1") != std::string::npos;
                    if (!g_roomsEnabled) {
                        ViewAddItem(ItemKind::Notice,
                                    "服务器版本较旧，本次连接只能用大厅（没有群聊和私聊）",
                                    dchat::NowTimeString());
                    }
                    Layout(ui.hwnd);
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "ROOMLIST") {
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 6) {
                        const std::string id = fields[0];
                        const std::string kind = fields[1];
                        std::string title;
                        std::vector<unsigned char> nameBytes;
                        if (dchat::Base64Decode(fields[2], &nameBytes)) {
                            title.assign(nameBytes.begin(), nameBytes.end());
                        }
                        const int index = EnsureSession(id, kind);
                        Session& session = g_sessions[index];
                        if (!title.empty()) session.title = title;
                        session.kind = kind;
                        session.members = std::atoi(fields[3].c_str());
                        session.maxMembers = std::atoi(fields[4].c_str());
                        session.myRole = fields[5];
                        g_roomListSeen.push_back(id);  // 这一轮名单里有它（ROOMLIST_END 要用）
                    }
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "ROOMLIST_END") {
                    // 名单是**全量**的：这一轮没出现的群聊，说明我已经不在里面了
                    // （退群 / 被踢），要从左栏拿掉。以前只增不减 —— 退群之后那个群
                    // 会永远留在列表里（自动化联调实测）。
                    // 只处理群聊：大厅不是"退出"语义，私聊也不在这份名单里。
                    for (int i = static_cast<int>(g_sessions.size()) - 1; i >= 0; --i) {
                        if (g_sessions[i].kind != "group") continue;
                        bool seen = false;
                        for (const std::string& seenId : g_roomListSeen) {
                            if (g_sessions[i].id == seenId) { seen = true; break; }
                        }
                        if (seen) continue;
                        const bool removingCurrent = (i == g_currentSession);
                        if (removingCurrent) {
                            // 守住"g_items 是当前会话的活副本"这条不变量：
                            // 删的就是当前会话，先把活副本存回去，再把大厅的换上来。
                            g_sessions[i].items = g_items;
                            g_sessions[i].scrollTop = g_scrollTop;
                        }
                        g_sessions.erase(g_sessions.begin() + i);
                        if (removingCurrent) {
                            g_currentSession = -1;
                            if (!g_sessions.empty()) {
                                const int hall = FindSession(dchat::kHallId);
                                g_currentSession = hall >= 0 ? hall : 0;
                                Session& landed = g_sessions[g_currentSession];
                                g_items = landed.items;
                                g_scrollTop = landed.scrollTop;
                                g_stickToBottom = landed.stickToBottom;
                                SendRawLine(dchat::BuildLine("ROOM", landed.id));
                            } else {
                                g_items.clear();
                                g_scrollTop = 0;
                            }
                        } else if (i < g_currentSession) {
                            --g_currentSession;
                        }
                    }
                    g_roomListSeen.clear();
                    // 列表发完了：如果当前会话还是空的（第一次登录），落到大厅上
                    if (g_sessions.empty()) EnsureSession(dchat::kHallId, "hall");
                    if (g_currentSession < 0 ||
                        g_currentSession >= static_cast<int>(g_sessions.size())) {
                        SwitchSession(HallSessionIndex());
                    }
                    if (ui.hwnd) Layout(ui.hwnd);
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "RINFO") {
                    // RINFO <房间ID> <kind> <显示名B64> <群主> <成员数> <上限> <我的角色> <加群方式> <listed>
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 9) {
                        const int index = EnsureSession(fields[0], fields[1]);
                        Session& session = g_sessions[index];
                        session.kind = fields[1];
                        std::vector<unsigned char> nameBytes;
                        if (dchat::Base64Decode(fields[2], &nameBytes)) {
                            session.title.assign(nameBytes.begin(), nameBytes.end());
                        }
                        session.owner = fields[3];
                        session.members = std::atoi(fields[4].c_str());
                        session.maxMembers = std::atoi(fields[5].c_str());
                        session.myRole = fields[6];
                        session.joinMode = fields[7];
                        session.listed = (fields[8] == "true");
                        session.memberList.clear();  // 紧接着会收到整张成员表
                    }
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "RSET") {
                    // RSET <房间ID> <设置名> <值>；群名和公告的值是 Base64
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 2) {
                        const int index = FindSession(fields[0]);
                        if (index >= 0) {
                            Session& session = g_sessions[index];
                            const std::string& key = fields[1];
                            const std::string value = fields.size() >= 3 ? fields[2] : std::string();
                            std::string text;
                            std::vector<unsigned char> bytes;
                            if (key == "name" || key == "announcement") {
                                if (dchat::Base64Decode(value, &bytes)) {
                                    text.assign(bytes.begin(), bytes.end());
                                }
                            }
                            if (key == "name") {
                                session.name = text;
                                if (!text.empty()) session.title = text;
                            } else if (key == "announcement") {
                                session.announcement = text;
                            } else if (key == "maxmembers") {
                                session.maxMembers = std::atoi(value.c_str());
                            } else if (key == "joinmode") {
                                session.joinMode = value;
                            } else if (key == "listed") {
                                session.listed = (value == "true");
                            } else if (key == "cachemaxmb") {
                                session.cacheMaxMb = std::strtoull(value.c_str(), nullptr, 10);
                            } else if (key == "importantnotice") {
                                session.importantNotice = (value == "true");
                            }
                        }
                    }
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "RMEMBERS") {
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 3) {
                        const int index = FindSession(fields[0]);
                        if (index >= 0) {
                            SessionMember member;
                            member.nick = fields[1];
                            member.role = fields[2];
                            std::vector<unsigned char> bytes;
                            if (fields.size() >= 4 && dchat::Base64Decode(fields[3], &bytes)) {
                                member.display.assign(bytes.begin(), bytes.end());
                            }
                            Session& session = g_sessions[index];
                            bool replaced = false;
                            for (SessionMember& existing : session.memberList) {
                                if (existing.nick == member.nick) {
                                    existing = member;
                                    replaced = true;
                                    break;
                                }
                            }
                            if (!replaced) session.memberList.push_back(member);
                        }
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "RMEMBERS_END") {
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "RHIST") {
                    // RHIST <房间ID> <序号> <原始协议行>：序号之后的原文本身就是一条
                    // 完整的协议行，所以**原样再走一遍实时那条路径** —— 两条路径共用
                    // 同一个函数，历史里能显示对的东西实时也一定显示得对。
                    const dchat::Message outer = dchat::ParseLine(raw.rest);
                    const std::vector<std::string> fields = outer.Words();
                    if (fields.size() >= 3) {
                        // **按序号去重**（见 Session::lastHistorySeq）。
                        // 服务端每次收到 ROOM 都会重放历史，不去重的话切一次多一份。
                        unsigned long long seq = 0;
                        bool numeric = !fields[1].empty();
                        for (char ch : fields[1]) {
                            if (ch < '0' || ch > '9') numeric = false;
                        }
                        if (numeric) seq = std::strtoull(fields[1].c_str(), nullptr, 10);
                        const int index = FindSession(fields[0]);
                        bool seen = false;
                        if (index >= 0) {
                            Session& target = g_sessions[static_cast<std::size_t>(index)];
                            seen = (seq != 0 && seq <= target.lastHistorySeq);
                            if (seq > target.lastHistorySeq) target.lastHistorySeq = seq;
                        }
                        if (!seen) {
                            std::size_t offset = fields[0].size() + 1 + fields[1].size() + 1;
                            if (offset <= raw.rest.size()) RenderChatLine(raw.rest.substr(offset));
                        }
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "RHIST_END") {
                    if (ui.hView) {
                        g_stickToBottom = true;
                        g_scrollTop = 1 << 30;
                        InvalidateRect(ui.hView, nullptr, TRUE);
                        ViewUpdateScrollbar(ui.hView);
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "GDMLINK") {
                    // GDMLINK <房间ID> <对方昵称>：对方主动找我，或者我自己点了某人
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 2) {
                        const int index = EnsureSession(fields[0], "dm");
                        if (g_sessions[index].title.empty() || g_sessions[index].title == fields[0]) {
                            g_sessions[index].title = fields[1];
                        }
                        RefreshSessionPanel();
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "GHIT") {
                    // GHIT <房间ID> <群名B64> <成员数> <上限> <加群方式>
                    std::vector<std::string> fields = raw.Words();
                    if (fields.size() >= 5) {
                        std::vector<unsigned char> bytes;
                        std::string name = fields[0];
                        if (dchat::Base64Decode(fields[1], &bytes)) {
                            name.assign(bytes.begin(), bytes.end());
                        }
                        g_groupHits.push_back({fields[0], name, std::atoi(fields[2].c_str()),
                                               std::atoi(fields[3].c_str()), fields[4]});
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "GHIT_END") {
                    g_groupSearchDone = true;
                    // 搜索结果窗口开着就通知它重画。不用轮询：收齐一轮才发一条消息，
                    // 空闲时一个定时器都不用跑。
                    if (g_groupWindow) PostMessageW(g_groupWindow, kMsgGroupHits, 0, 0);
                    delete payload;
                    return 0;
                }
                if (raw.command == "GREQ") {
                    // GREQ <时间> <房间ID> <昵称>：有人申请入群（或 GREQS 的逐行回应）
                    std::vector<std::string> fields = raw.Words();
                    if (!fields.empty() && dchat::LooksLikeTime(fields[0])) {
                        fields.erase(fields.begin());
                    }
                    if (fields.size() >= 2) {
                        const int index = FindSession(fields[0]);
                        const std::string title =
                            index >= 0 ? g_sessions[index].title : fields[0];
                        ViewAddItem(ItemKind::Notice,
                                    fields[1] + " 申请加入「" + title + "」" +
                                        (index >= 0 && g_sessions[index].myRole != "member"
                                             ? "（用群设置里的「入群申请」同意或拒绝）"
                                             : ""),
                                    dchat::NowTimeString());
                        if (index >= 0) {
                            g_sessions[index].pending.push_back(fields[1]);
                        }
                    }
                    delete payload;
                    return 0;
                }
                if (raw.command == "GREQ_END") {
                    RefreshSessionPanel();
                    delete payload;
                    return 0;
                }
                if (raw.command == "GBLOCKS") {
                    // 一轮黑名单 = 连续若干行 + GBLOCKS_END。协议里没有单独的"开始"
                    // 标记，所以用"上一轮已经收完"来判断这是新一轮的开头 ——
                    // 不清空的话，重新请求（拉黑之后服务端会主动推一次）会越积越多。
                    if (g_blockListDone) {
                        g_blockList.clear();
                        g_blockListDone = false;
                    }
                    if (!raw.rest.empty()) g_blockList.push_back(raw.rest);
                    delete payload;
                    return 0;
                }
                if (raw.command == "GBLOCKS_END") {
                    g_blockListDone = true;
                    delete payload;
                    return 0;
                }

                // ---- 聊天类：按房间投递 ----
                if (RenderChatLine(*payload)) {
                    delete payload;
                    return 0;
                }
                // 维护在线名单（Tab 补昵称参数要用）
                if (raw.command == "RULES") {
                    // 服务器把当前规则发过来了：客户端据此调整本地的"单文件上限"
                    std::vector<std::string> fields = raw.Words();
                    if (!fields.empty() && dchat::LooksLikeTime(fields[0])) fields.erase(fields.begin());
                    if (!fields.empty()) {
                        const int mb = std::atoi(fields[0].c_str());
                        if (mb >= 1 && mb <= 4096) g_maxFileMb = mb;
                    }
                    // chatcolor=1|0 是**具名**字段（追加在末尾），按名字找，
                    // 这样以后服务器再加字段也不会读错位置
                    for (const std::string& field : fields) {
                        if (field.compare(0, 10, "chatcolor=") == 0) {
                            const bool on = field.substr(10) != "0";
                            if (on != g_chatColorEnabled) {
                                g_chatColorEnabled = on;
                                if (ui.hView) InvalidateRect(ui.hView, nullptr, TRUE);
                                ViewAddItem(ItemKind::Notice,
                                            on ? "服务器已开启彩色文字代码"
                                               : "服务器已关闭彩色文字代码（色码会原样显示）",
                                            dchat::NowTimeString());
                            }
                        }
                    }
                    delete payload;
                    return 0;  // 这是给客户端用的控制行，不显示在聊天记录里
                }
                if (raw.command == "PONG" && g_heartbeatPending) {
                    g_heartbeatPending = false;  // 心跳的回应，安安静静地吞掉
                    delete payload;
                    return 0;
                }
                if (raw.command == "NAMES") {
                    SetOnlineNicks(StripTimePrefix(raw.rest));
                } else if (raw.command == "KNOWN") {
                    // 已注册账号名单（给 Tab 补 /ban /op /unban /ip 用），不进聊天记录
                    SetKnownNicks(StripTimePrefix(raw.rest));
                    delete payload;
                    return 0;
                } else if (raw.command == "JOINED" || raw.command == "LEFT") {
                    std::vector<std::string> who = raw.Words();
                    if (!who.empty() && dchat::LooksLikeTime(who[0])) who.erase(who.begin());
                    if (!who.empty()) {
                        if (raw.command == "JOINED") {
                            AddOnlineNick(who[0]);
                        } else {
                            RemoveOnlineNick(who[0]);
                        }
                    }
                }
                const dchat::NoticeInfo notice = dchat::ParseNotice(*payload);
                // 改密码成功了：把"记住的账号"里的密码换成新的。
                // 不换的话，下次自动登录/断线重连还会拿旧密码去登 —— 用户看到的
                // 只是"密码错误"，完全想不到是自己刚才改过。
                if (raw.command == "SYS" && AdoptNewPasswordIfConfirmed(notice.text)) {
                    ViewAddItem(ItemKind::Notice, "本机记住的密码已更新，下次自动登录用新密码",
                                dchat::NowTimeString());
                }
                const bool mention = dchat::MentionsMe(*payload, g_nick);
                if (g_authPending && raw.command == "ERROR") {
                    g_authPending = false;
                    if (g_authDlg) {
                        // 账号窗口还开着：把原因写在窗口里，连接不动，用户可以改完再试
                        AuthDialogOnResult(false, Utf8ToWide(notice.text));
                    } else {
                        // 窗口已经关了（例如用户中途取消）：按老办法处理
                        ViewAddItem(ItemKind::Error, notice.text, notice.time);
                        DisconnectFromServer(false);
                        ViewAddItem(ItemKind::Notice, "登录 / 注册未成功，请检查用户名与密码后重试",
                                    dchat::NowTimeString());
                    }
                } else if (g_authPending && raw.command == "LOGGEDIN") {
                    // 服务器明确确认认证通过（不是靠"收到任何提示"来判断，避免被 WELCOME 干扰）
                    g_authPending = false;
                    if (!notice.text.empty()) g_nick = notice.text;
                    g_authed = true;
                    // 登录成功 = 这轮重连结束，退避计数清零
                    g_reconnect.OnSucceeded();
                    g_userDisconnected = false;
                    KillTimer(hwnd, kReconnectTimerId);
                    // 认证确认成功之后才"记住账号" —— 失败的登录不能覆盖掉
                    // 上一次能用的凭据（比如临时输错一次密码）。
                    if (!g_pendingUser.empty()) {
                        SaveLoginReminder(g_host, g_port, g_pendingUser, g_pendingPassword);
                    }
                    if (g_authDlg) {
                        AuthDialogOnResult(true, std::wstring());
                    } else {
                        ViewAddItem(ItemKind::Notice, "已登录：" + g_nick, notice.time);
                    }
                    // 声明多会话能力。服务端会回一行 FEATURES 确认；没有确认、
                    // 或者确认里没有 rooms1，就说明对面还是旧版服务端 ——
                    // 那时我们会一直停在"只有大厅"的老界面，而不是发一堆
                    // 对方看不懂的命令然后什么都不发生。
                    if (!g_roomsDeclared) {
                        SendRawLine(dchat::BuildLine("FEATURES", "rooms1"));
                        g_roomsDeclared = true;
                        // 顺带把黑名单拉回来。以前**从来没有**人请求过它，
                        // 于是 g_blockList 永远是空的（自动化联调实测：拉黑明明
                        // 成功了，界面上却什么都看不到）。
                        SendRawLine(dchat::BuildLine("BLOCKS"));
                    }
                    UpdateStatus();
                } else {
                    dchat::SayInfo say;
                    if (dchat::ParseSay(*payload, g_nick, &say)) {
                        ViewAddItem(ItemKind::Say, *payload, std::string());
                    } else {
                        const ItemKind kind = notice.isAnnouncement
                                                  ? ItemKind::Announce
                                                  : (notice.isError ? ItemKind::Error
                                                                    : ItemKind::Notice);
                        ViewAddItem(kind, notice.text, notice.time);
                    }
                }
                MarkMessageArrived(mention);
                delete payload;
            }
            return 0;
        }
        case WM_APP_FILECLICK: {
            // 记录区里点到了文件卡片
            auto* fileId = reinterpret_cast<std::string*>(lp);
            if (fileId) {
                ActivateFileCard(*fileId);
                delete fileId;
            }
            return 0;
        }
        case WM_APP_PREVIEW: {
            // 缩略图加载完了：挂到卡片上（item 一销毁就自动 DeleteObject）
            auto* result = reinterpret_cast<PreviewResult*>(lp);
            if (result) {
                if (Item* item = FindFileItem(result->fileId)) {
                    item->preview = std::shared_ptr<void>(result->bitmap,
                                                          [](void* handle) {
                                                              DeleteObject(static_cast<HBITMAP>(
                                                                  handle));
                                                          });
                    if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
                } else if (result->bitmap) {
                    DeleteObject(result->bitmap);
                }
                delete result;
            }
            return 0;
        }
        case WM_APP_FILEOPEN: {
            // 点了缩略图：用默认程序打开这个文件
            auto* fileId = reinterpret_cast<std::string*>(lp);
            if (fileId) {
                if (Item* item = FindFileItem(*fileId)) {
                    if (item->fileState == 2 && !item->savedPath.empty()) {
                        ShellExecuteW(nullptr, L"open", Utf8ToWide(item->savedPath).c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);
                    } else {
                        ActivateFileCard(*fileId);  // 还没下载就当成点卡片
                    }
                }
                delete fileId;
            }
            return 0;
        }
        case WM_APP_VOICECLICK: {
            // 记录区里点到了语音气泡：播放 / 暂停 / 接着播
            auto* fileId = reinterpret_cast<std::string*>(lp);
            if (fileId) {
                ToggleVoicePlayback(*fileId);
                delete fileId;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // 点在哪一块：菜单项 / 齿轮 / 状态胶囊 / 「＋」。
            // 这些都在父窗口里自绘，命中判断也在这里（几何来自 ui_layout）。
            // 注意：「＋」菜单是**独立窗口**，它的点击由 PlusMenuProc 处理；
            // 点到菜单以外时它自己会收到 WM_KILLFOCUS 并收起。
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);

            if (PtInRect(&g_bottom.plusButton, POINT{x, y})) {
                if (g_plusMenuOpen) {
                    DestroyPlusMenuWindow();
                    InvalidateChrome(hwnd);
                } else {
                    ShowPlusMenuWindow();
                }
                return 0;
            }
            if (PtInRect(&g_gearRect, POINT{x, y})) {
                OpenSettings(hwnd);
                return 0;
            }
            if (g_chipRect.right > g_chipRect.left && PtInRect(&g_chipRect, POINT{x, y})) {
                // 点状态胶囊：列出在线成员（以前这是顶栏的一个按钮）
                if (IsConnected()) {
                    std::string list;
                    for (std::size_t i = 0; i < g_onlineNicks.size(); ++i) {
                        if (i) list += "、";
                        list += g_onlineNicks[i];
                    }
                    ViewAddItem(ItemKind::Notice,
                                list.empty() ? "现在房间里没有别人"
                                             : ("在线 " + std::to_string(g_onlineNicks.size()) +
                                                " 人：" + list),
                                dchat::NowTimeString());
                } else {
                    ViewAddItem(ItemKind::Notice, "还没连接。点菜单里的「连接」填服务器地址",
                                dchat::NowTimeString());
                }
                return 0;
            }
            if (const dchat::MenuItem* item = dchat::HitTestMenu(g_menuItems, x, y)) {
                switch (item->action) {
                    case dchat::MenuAction::Connect:
                        if (IsConnected()) {
                            DoDisconnect(hwnd);
                        } else {
                            DoConnect(hwnd);
                        }
                        break;
                    case dchat::MenuAction::Settings:
                        OpenSettings(hwnd);
                        break;
                    case dchat::MenuAction::Help:
                        ViewAddItem(ItemKind::Notice,
                                    "用法：输入框里回车发送；发文件和语音点右边的「＋」；"
                                    "主题 / 彩色聊天 / 服务器地址 / 语音时长在右上角齿轮里。"
                                    "指令输入 /help，按 Tab 会补全。",
                                    dchat::NowTimeString());
                        break;
                    case dchat::MenuAction::ToggleOnline:
                        break;
                }
                Layout(hwnd);  // 「连接/断开」的文字要跟着状态变
                UpdateStatus();
                return 0;
            }
            if (y < dchat::kMenuBarHeight) return 0;  // 点菜单栏空白处：不做任何事

            // ---- 左侧会话列表：点一行就切过去 ----
            if (g_roomsEnabled && g_sessionLayout.valid) {
                const dchat::SessionHitResult hit = dchat::HitTestSessionList(g_sessionLayout, x, y);
                if (hit.what == dchat::SessionHit::Row && hit.index >= 0 &&
                    hit.index < static_cast<int>(g_sessionOrder.size())) {
                    SwitchSession(g_sessionOrder[hit.index]);
                    return 0;
                }
                if (hit.what == dchat::SessionHit::NewGroup || hit.what == dchat::SessionHit::Search) {
                    // 表头两个按钮：都打开群聊窗口（新建和搜索本就在一个窗口里，
                    // 搜不到就自己建 —— 分成两个入口的话用户还得回头找）
                    OpenGroupWindow(hwnd);
                    return 0;
                }
            }
            break;                                     // 其它地方交给默认处理
        }
        case WM_RBUTTONUP: {
            // 右键会话列表里的一行 = 打开那个会话的设置。
            // 设置项本身（免打扰、置顶、群设置、成员……）全在窗口里，不在右键菜单里
            // 再列一遍 —— 两处入口迟早会出现"菜单里有、窗口里没有"这种不一致，
            // 而用户会以为功能坏了。
            if (g_roomsEnabled && g_sessionLayout.valid) {
                const int x = GET_X_LPARAM(lp);
                const int y = GET_Y_LPARAM(lp);
                const dchat::SessionHitResult hit =
                    dchat::HitTestSessionList(g_sessionLayout, x, y);
                if (hit.what == dchat::SessionHit::Row && hit.index >= 0 &&
                    hit.index < static_cast<int>(g_sessionOrder.size())) {
                    const int sessionIndex = g_sessionOrder[hit.index];
                    SwitchSession(sessionIndex);  // 右键一个会话，多半就是要看它
                    OpenRoomSettings(hwnd, g_sessions[sessionIndex].id);
                    return 0;
                }
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            // 会话列表的悬停高亮（和菜单栏各自独立，互不干扰）
            if (g_roomsEnabled && g_sessionLayout.valid) {
                const dchat::SessionHitResult hit =
                    dchat::HitTestSessionList(g_sessionLayout, x, y);
                // -1 无、>=0 第几行、-2 搜索按钮、-3 新建按钮（见绘制处的约定）
                int hover = -1;
                if (hit.what == dchat::SessionHit::Row) {
                    hover = hit.index;
                } else if (hit.what == dchat::SessionHit::Search) {
                    hover = -2;
                } else if (hit.what == dchat::SessionHit::NewGroup) {
                    hover = -3;
                }
                if (hover != g_sessionHover) {
                    g_sessionHover = hover;
                    InvalidateRect(hwnd, &g_sessionLayout.header, FALSE);
                }
            }
            int hover = -1;
            for (std::size_t i = 0; i < g_menuItems.size(); ++i) {
                if (PtInRect(&g_menuItems[i].rect, POINT{x, y})) {
                    hover = static_cast<int>(i);
                    break;
                }
            }
            const bool gearHover = PtInRect(&g_gearRect, POINT{x, y}) != 0;
            const bool chipHover =
                g_chipRect.right > g_chipRect.left && PtInRect(&g_chipRect, POINT{x, y}) != 0;
            if (hover != g_menuHover || gearHover != g_gearHover || chipHover != g_chipHover) {
                g_menuHover = hover;
                g_gearHover = gearHover;
                g_chipHover = chipHover;
                InvalidateChrome(hwnd);
            }
            if (y < dchat::kMenuBarHeight) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (g_menuHover != -1 || g_gearHover || g_chipHover) {
                g_menuHover = -1;
                g_gearHover = false;
                g_chipHover = false;
                InvalidateChrome(hwnd);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && g_plusMenuOpen) {  // Esc 收起「＋」菜单
                g_plusMenuOpen = false;
                g_plusHover = -1;
                InvalidateChrome(hwnd);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == kStateTimerId) {
                WriteClientState();  // 自动化测试：把界面状态导出给脚本断言
                return 0;
            }
            if (wp == kRecordTimerId) {
                if (!g_recorder.IsRunning()) {
                    KillTimer(hwnd, kRecordTimerId);  // 已经收尾了，别再空转
                    return 0;
                }
                const int seconds = g_recorder.ElapsedSeconds();
                UpdateStatus();
                // 到点自动收尾并发送：录满上限还不停，用户会一直录到超限、
                // 最后被拦下来——白录一场。上限取"设置里选的"和"字节数允许的"较小值。
                const int limit = (std::min)(g_voiceLimitSeconds, dchat::kMaxVoiceSeconds);
                if (seconds >= limit) {
                    ViewAddItem(ItemKind::Notice,
                                "语音最长 " + dchat::FormatDuration(limit) + "，已自动发送",
                                dchat::NowTimeString());
                    StopVoiceRecordingAndSend();
                }
                return 0;
            }
            if (wp == kReconnectTimerId) {
                KillTimer(hwnd, kReconnectTimerId);
                BeginReconnectAttempt(hwnd);
                return 0;
            }
            break;
        case WM_APP_RECONNECT: {
            // 重连尝试的结果（工作线程连的，这里回到界面线程收尾）
            std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(lp));
            FinishReconnectAttempt(hwnd, wp == 1, error ? *error : std::string());
            return 0;
        }
        case WM_APP_XFER: {
            // 发送线程的进度/结果回到界面线程
            auto* event = reinterpret_cast<TransferEvent*>(lp);
            if (event) {
                if (event->finished) g_sendBusy = false;
                if (!event->notice.empty()) {
                    ViewAddItem(event->error ? ItemKind::Error : ItemKind::Notice, event->notice,
                                dchat::NowTimeString());
                }
                g_sendProgress = event->status;
                RefreshTransferStatus();
                delete event;
            }
            return 0;
        }
        case WM_DROPFILES: {
            // 把文件拖进窗口就是发送它
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            wchar_t path[MAX_PATH] = {0};
            if (count > 0 && DragQueryFileW(drop, 0, path, MAX_PATH)) {
                if (count > 1) {
                    ViewAddItem(ItemKind::Notice, "一次发送一个文件（这次发第一个）",
                                dchat::NowTimeString());
                }
                StartSendFile(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_COPYDATA: {
            // 其他程序（脚本等）可以把一个 UTF-8 文件路径交给已经打开的客户端来发送
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!data || data->dwData != kFileSendMagic || !data->lpData || data->cbData == 0) {
                break;
            }
            const std::string text(reinterpret_cast<const char*>(data->lpData), data->cbData);
            const std::string trimmed(text.c_str());  // 去掉结尾的 '\0'
            if (!trimmed.empty()) StartSendFile(Utf8ToWide(trimmed));
            return TRUE;
        }
        case WM_APP_CLOSED: {
            // **先记下掉线前是否登录过**：下面 DisconnectFromServer 会把它清掉，
            // 而"登录过之后被动掉线"正是自动重连的唯一触发条件。
            const bool wasLoggedIn = g_authed;
            if (g_authDlg) {
                // 登录 / 注册还没完成就掉线了：让账号窗口把原因显示出来
                AuthDialogOnResult(false, L"与服务器的连接已断开");
            }
            if (g_sock != INVALID_SOCKET) {
                DisconnectFromServer(false);
                ViewAddItem(ItemKind::Notice, "与服务器的连接已断开", dchat::NowTimeString());
            }
            // 自动重连（用户自己点断开时 g_userDisconnected 为真，状态机会拒绝）
            if (wasLoggedIn) {
                ScheduleReconnect(hwnd, g_reconnect.OnDisconnected(true, g_userDisconnected));
            }
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc,
                     g_windowBrush ? g_windowBrush : reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            // 整个外壳（菜单栏 / 状态条 / 输入胶囊 / ＋ / 齿轮 / ＋菜单）都在这里画；
            // 聊天记录由记录区子窗口自己画，这里不碰。
            DrawWindowChrome(hwnd, dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            if (reinterpret_cast<HWND>(lp) == ui.hInput) {
                // 禁用状态的输入框收到的是 WM_CTLCOLORSTATIC：给它和胶囊同色的实心画刷，
                // 这样它重画时会先把自己那一小块擦干净（空画刷会留下旧文字的残影）
                SetTextColor(dc, g_palette->system);
                SetBkColor(dc, g_palette->bubbleOther);
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(g_pillBrush ? g_pillBrush
                                                             : GetStockObject(NULL_BRUSH));
            }
            SetTextColor(dc, g_palette->text);
            SetBkColor(dc, g_palette->windowBg);
            return reinterpret_cast<LRESULT>(
                g_windowBrush ? g_windowBrush : reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g_palette->text);
            SetBkColor(dc, g_palette->bubbleOther);
            SetBkMode(dc, TRANSPARENT);
            // 给实心画刷（和胶囊同色）：输入框每次重画先擦掉自己那块，
            // 这样按 ↑↓ 快速换内容时不会残留旧文字。输入框内缩在胶囊里，
            // 擦掉的是内部矩形，圆角不受影响。
            return reinterpret_cast<LRESULT>(g_pillBrush ? g_pillBrush
                                                         : GetStockObject(NULL_BRUSH));
        }
        case WM_CLOSE:
            DoDisconnect(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // 会话的本地设置（免打扰 / 置顶 / 本地缓存）在退出时落盘。
            // 也在这里存是因为它们是**纯客户端**的东西：丢了服务端不会有任何记录，
            // 用户只会觉得"我上次明明设过免打扰"。
            SaveRoomPrefs();
            if (ui.font) DeleteObject(ui.font);
            if (ui.fontSmall) DeleteObject(ui.fontSmall);
            if (ui.fontLarge) DeleteObject(ui.fontLarge);
            if (g_windowBrush) {
                DeleteObject(g_windowBrush);
                g_windowBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int showCmd) {
    SetProcessDPIAware();
    // 自动化测试用：`--state-file <路径>` 让客户端周期性把界面状态写进这个文件。
    // 正常启动没有这个参数，g_statePath 为空 —— 导出函数第一行就返回。
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv != nullptr) {
            for (int i = 1; i < argc; ++i) {
                if (std::wstring(argv[i]) == L"--state-file" && i + 1 < argc) {
                    g_statePath = argv[++i];
                    continue;
                }
                if (std::wstring(argv[i]) == L"--selftest-password-sync") {
                    // 自检：改密码之后"记住的账号"会不会跟着更新。
                    // 界面那条路在这台机器上没法自动化（合成按键送不进编辑框），
                    // 所以直接测决策函数 + DPAPI 落盘这两段。
                    // 退出码 0 = 通过。
                    SaveLoginReminder("127.0.0.1", 5555, "selftest_user", "oldpass123");
                    g_pendingUser = "selftest_user";
                    g_pendingPassword = "oldpass123";
                    g_host = "127.0.0.1";
                    g_port = 5555;
                    // ① 无关的系统提示不动它
                    g_pendingNewPassword = "newpass456";
                    g_pendingNewPasswordAt = GetTickCount64();
                    const bool ignored = !AdoptNewPasswordIfConfirmed("服务器已开启彩色文字代码");
                    // ② 过期的不认
                    g_pendingNewPasswordAt = GetTickCount64() - (kPasswordChangeTimeoutMs + 1000);
                    const bool stale = !AdoptNewPasswordIfConfirmed("密码已修改，下次登录请用新密码");
                    // ③ 正常路径：认了，并且真的写进文件
                    g_pendingNewPassword = "newpass456";
                    g_pendingNewPasswordAt = GetTickCount64();
                    const bool adopted = AdoptNewPasswordIfConfirmed("密码已修改，下次登录请用新密码");
                    SavedLogin probe;
                    const bool loaded = LoadLoginReminder(&probe);
                    const bool fileOk =
                        loaded && probe.user == "selftest_user" && probe.password == "newpass456";
                    // ④ 没有待确认的新密码时，重复的回执不能再改（幂等）
                    const bool idempotent = !AdoptNewPasswordIfConfirmed("密码已修改，下次登录请用新密码");
                    ClearLoginReminder();
                    std::printf("无关提示不动=%d 过期不认=%d 认了新密码=%d 落盘=%d 幂等=%d\n",
                                ignored ? 1 : 0, stale ? 1 : 0, adopted ? 1 : 0, fileOk ? 1 : 0,
                                idempotent ? 1 : 0);
                    return (ignored && stale && adopted && fileOk && idempotent) ? 0 : 1;
                }
                if (std::wstring(argv[i]) == L"--selftest-login") {
                    // 自检：把"记住账号"的存取完整走一遍，**不启动界面**。
                    // 验证的是最容易错的部分：DPAPI 加解密 + 文件格式 + 中文与空格。
                    // 退出码 0 = 通过，1 = 失败。自动化测试用。
                    SavedLogin probe;
                    const bool saved = SaveLoginReminder("127.0.0.1", 5555, "selftest_user",
                                                         "p@ss w0rd 中文");
                    const bool loaded = saved && LoadLoginReminder(&probe);
                    const bool same = loaded && probe.host == "127.0.0.1" && probe.port == 5555 &&
                                      probe.user == "selftest_user" &&
                                      probe.password == "p@ss w0rd 中文";
                    // 密文里绝不该出现明文密码
                    bool plaintextLeak = false;
                    {
                        std::ifstream raw(kLoginReminderFile, std::ios::binary);
                        std::string blob((std::istreambuf_iterator<char>(raw)),
                                         std::istreambuf_iterator<char>());
                        plaintextLeak = blob.find("p@ss w0rd") != std::string::npos;
                    }
                    ClearLoginReminder();
                    std::printf("保存=%d 读取=%d 一致=%d 明文泄漏=%d\n", saved ? 1 : 0,
                                loaded ? 1 : 0, same ? 1 : 0, plaintextLeak ? 1 : 0);
                    return (same && !plaintextLeak) ? 0 : 1;
                }
            }
            LocalFree(argv);
        }
    }
    ui::Startup();  // 圆角与气泡都用 GDI+ 画
    // 会话的本地设置（免打扰 / 置顶 / 本地缓存）先读回来。
    // 这些是**纯本地**的东西，服务端永远不知道它们存在 —— 读在连接之前，
    // 这样"免打扰"从第一秒就生效，而不是等用户切一次会话才想起来。
    LoadRoomPrefs();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    // 应用图标：聊天气泡（见 resources/dchat.ico）
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_DCHAT), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_DCHAT), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DchatClientWnd";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"DchatClientWnd", kWindowTitle, kWindowStyle, CW_USEDEFAULT,
                                CW_USEDEFAULT, 940, 660, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    // 只有自动化测试才需要周期性导出（200ms，和录音计时同一个节奏）
    if (!g_statePath.empty()) SetTimer(hwnd, kStateTimerId, 200, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ui::Shutdown();
    return static_cast<int>(msg.wParam);
}
