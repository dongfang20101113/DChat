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
#include "render.h"
#include "resource.h"
#include "rounded.h"
#include "server_command.h"  // LooksLikePasswordCommand：改密码那一行不进输入历史

namespace {

constexpr UINT WM_APP_LINE = WM_APP + 1;
constexpr UINT WM_APP_CLOSED = WM_APP + 2;
constexpr UINT WM_APP_XFER = WM_APP + 3;  // 文件发送线程 -> 界面线程
constexpr UINT WM_APP_FILECLICK = WM_APP + 4;  // 记录区点到了文件卡片 -> 主窗口处理
constexpr UINT WM_APP_PREVIEW = WM_APP + 5;    // 缩略图加载完了
constexpr UINT WM_APP_FILEOPEN = WM_APP + 6;   // 点了缩略图 -> 用默认程序打开

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
    // 图片 / 视频的缩略图（HBITMAP，析构时自动 DeleteObject）
    std::shared_ptr<void> preview;
};

std::deque<Item> g_items;
int g_scrollTop = 0;
int g_contentHeight = 0;
bool g_stickToBottom = true;
RECT g_inputPill{0, 0, 0, 0};  // 输入框的胶囊背景（父窗口绘制，输入框内嵌在里面）

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
    HWND hSuggest = nullptr;  // 输入框上方的候选浮层
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontLarge = nullptr;
} ui;

std::map<HWND, bool> g_hover;

dchat::TabCompleter g_tabComplete;  // Tab 指令补全的状态
std::vector<std::string> g_onlineNicks;  // 当前在线昵称（Tab 补参数用）

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

void SetOnlineNicks(const std::string& list) {  // "alice, bob"
    g_onlineNicks.clear();
    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t comma = list.find(',', begin);
        const std::string name =
            TrimAscii(list.substr(begin, comma == std::string::npos ? std::string::npos
                                                                    : comma - begin));
        if (!name.empty() && name != "(暂时没人设置昵称)") g_onlineNicks.push_back(name);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
}

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
bool g_heartbeatPending = false; // 刚发过心跳 PING（对应的 PONG 不显示成提示）
int g_port = dchat::kDefaultPort;

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
    COLORREF nick[dchat::kNickPaletteSize];
};

const Palette kLightPalette{
    RGB(242, 243, 245), RGB(255, 255, 255), RGB(222, 225, 230), RGB(0, 120, 215),
    RGB(0, 100, 190),   RGB(255, 255, 255), RGB(32, 32, 32),     RGB(110, 110, 110),
    RGB(200, 0, 0),     RGB(130, 130, 130), RGB(176, 96, 0),     RGB(232, 234, 238),
    RGB(255, 246, 222), RGB(214, 150, 10),  RGB(150, 92, 0),
    RGB(233, 236, 239), RGB(205, 210, 216), RGB(0, 120, 215),    RGB(255, 255, 255),
    RGB(214, 217, 222),
    {RGB(0, 102, 204), RGB(0, 140, 60), RGB(204, 102, 0), RGB(150, 0, 150), RGB(0, 140, 140),
     RGB(200, 0, 100), RGB(110, 110, 0), RGB(90, 60, 160)}};

const Palette kDarkPalette{
    RGB(32, 32, 32),    RGB(48, 50, 54),    RGB(70, 73, 78),    RGB(0, 120, 215),
    RGB(30, 90, 160),   RGB(255, 255, 255), RGB(232, 232, 232), RGB(150, 150, 150),
    RGB(255, 110, 110), RGB(140, 140, 140), RGB(255, 190, 80),  RGB(44, 46, 50),
    RGB(58, 48, 24),    RGB(255, 190, 80),  RGB(255, 214, 140),
    RGB(52, 54, 58),    RGB(74, 77, 82),    RGB(0, 120, 215),   RGB(255, 255, 255),
    RGB(64, 66, 70),
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

const wchar_t* ThemeLabel(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return L"主题：浅色";
        case ThemeMode::System:
            return L"主题：跟随系统";
        case ThemeMode::Dark:
        default:
            return L"主题：深色";
    }
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

void ApplyThemeMode();

void CycleThemeMode() {
    switch (g_themeMode) {
        case ThemeMode::Dark:
            g_themeMode = ThemeMode::Light;
            break;
        case ThemeMode::Light:
            g_themeMode = ThemeMode::System;
            break;
        case ThemeMode::System:
        default:
            g_themeMode = ThemeMode::Dark;
            break;
    }
    ApplyThemeMode();
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

    if (ui.hThemeButton) SetWindowTextW(ui.hThemeButton, ThemeLabel(g_themeMode));
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
    RECT previewRect{};  // 缩略图（没有预览时是空矩形）
    std::shared_ptr<void> preview;  // 缩略图位图（HBITMAP）
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
            // 文件卡片：头一行是"谁发的 + 时间"，下面一张卡片，卡片右侧一个按钮
            row.isBubble = false;
            row.isFile = true;
            row.nick = item.nick;
            row.time = item.time;
            row.text = item.raw;  // 文件名
            row.fileId = item.fileId;
            row.fileSize = item.fileSize;
            row.fileState = item.fileState;
            row.fileProgress = item.fileProgress;
            row.preview = item.preview;
            row.fileNote = item.savedPath.empty() ? item.fileNote : item.savedPath;
            row.header = RECT{0, y, width, y + headerH};
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

                const SIZE size = dchat::MeasureWrappedText(dc, Utf8ToWide(say.text),
                                                           maxBubble - padX * 2, ui.font);
                RECT area{0, y + headerH, width, y + headerH};
                const dchat::BubblePlacement place = dchat::PlaceBubble(
                    area, size.cx, size.cy,
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
    DrawTextIn(dc, Utf8ToWide(row.text), row.textRect, ui.font, bodyColor,
               DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
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
int HitTestFileCard(int contentX, int contentY, std::string* fileId, bool* onPreview = nullptr) {
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
            if (onPreview) {
                *onPreview = row.preview && contentX >= row.previewRect.left &&
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
            // 点到文件卡片（整张卡片都可点，和 QQ 一样）：交给主窗口去下载 / 打开文件夹
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp) + g_scrollTop;  // 屏幕坐标 -> 内容坐标
            std::string fileId;
            bool onPreview = false;
            if (HitTestFileCard(x, y, &fileId, &onPreview) >= 0 && !fileId.empty()) {
                // 点在缩略图上 = 用默认程序打开文件；点在卡片其它地方 = 下载 / 打开文件夹
                auto* payload = new std::string(fileId);
                const UINT message = onPreview ? WM_APP_FILEOPEN : WM_APP_FILECLICK;
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

// ---------------- 未读提示 ----------------
bool WindowIsActive() { return ui.hwnd != nullptr && GetForegroundWindow() == ui.hwnd; }

void UpdateTitle() {
    const std::string title = dchat::FormatUnreadTitle(WideToUtf8(kWindowTitle), g_unread);
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
    ui::DrawRoundedControl(dc, rect, radius, g_palette->windowBg, fill, border);

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

WNDPROC g_oldButtonProc = nullptr;

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
        default:
            break;
    }
    return CallWindowProcW(g_oldButtonProc, hwnd, msg, wp, lp);
}

// ---------------- 连接状态 ----------------
bool IsConnected() { return g_sock != INVALID_SOCKET; }

void UpdateStatus() {
    if (!ui.hStatus) return;
    std::wstring text;
    if (IsConnected()) {
        text = L"已连接 " + Utf8ToWide(g_host) + L":" + std::to_wstring(g_port) + L"　用户：" +
               Utf8ToWide(g_nick);
        if (!g_transferStatus.empty()) text += L"　｜ " + Utf8ToWide(g_transferStatus);
    } else {
        text = L"未连接　点右边「连接」填写服务器地址、用户名和密码";
    }
    SetWindowTextW(ui.hStatus, text.c_str());
    EnableWindow(ui.hConnect, !IsConnected());
    EnableWindow(ui.hDisconnect, IsConnected());
    EnableWindow(ui.hSend, IsConnected());
    EnableWindow(ui.hInput, IsConnected());
    EnableWindow(ui.hFileSend, IsConnected());
    InvalidateRect(ui.hConnect, nullptr, TRUE);
    InvalidateRect(ui.hDisconnect, nullptr, TRUE);
    InvalidateRect(ui.hSend, nullptr, TRUE);
    InvalidateRect(ui.hFileSend, nullptr, TRUE);
}

// ---------------- 网络 ----------------
bool SendRawLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_sendMutex);
    if (g_sock == INVALID_SOCKET) return false;
    const std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(g_sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
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
        while (buffer.PopLine(&line)) {
            auto* payload = new std::string(line);
            if (!PostMessageW(ui.hwnd, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
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

    if (!SendRawLine(dchat::BuildLine("FILE_SEND", job->id + " " + job->base64Name + " " +
                                                      std::to_string(job->total) +
                                                      (thumbnail.empty() ? "" : " 1")))) {
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

void StartSendFile(const std::wstring& path) {
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
    if (static_cast<unsigned long long>(size.QuadPart) > dchat::kMaxFileBytes) {
        CloseHandle(file);
        ViewAddItem(ItemKind::Error,
                    "文件太大了，单个文件最多 " + dchat::FormatBytes(dchat::kMaxFileBytes),
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
    job->id = "f" + std::to_string(GetTickCount64()) + "_" + std::to_string(++g_xferCounter);
    g_sendBusy = true;
    g_sendProgress = displayName + " 0%";
    ViewAddItem(ItemKind::Notice,
                "开始发送 " + displayName + "（" + dchat::FormatBytes(job->total) + "）…",
                dchat::NowTimeString());
    RefreshTransferStatus();
    std::thread(SendFileThread, job).detach();
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

    if (msg.command == "FILE_OFFER") {
        // 有人上传好了：在记录区里加一张卡片，等人点「下载」
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
        g_items.push_back(item);
        while (g_items.size() > kMaxItems) g_items.pop_front();
        if (g_stickToBottom) g_scrollTop = 1 << 30;
        if (ui.hView) InvalidateRect(ui.hView, nullptr, FALSE);
        MarkMessageArrived(true);  // 有人发了文件，和「被 @」一样提醒一下
        const bool hasThumb = (fields.size() >= 5 && fields[4] == "1");
        // 图片自动下载（下完直接出缩略图）；视频不自动下整份，只取第一帧预览
        if (dchat::ShouldAutoDownload(item.raw)) {
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
        if (fields.size() != 3) return;
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
        if (Item* item = FindFileItem(id)) {
            savedUtf8 = WideToUtf8(saved);
            item->savedPath = savedUtf8;
            fileName = item->raw;
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
constexpr int kDlgCheckY = kDlgFirstRowY + 5 * kDlgRowH - 2;  // 「显示密码」那一行
constexpr int kDlgCheckH = 22;
constexpr int kDlgCheckW = 120;
constexpr int kDlgTipY = kDlgCheckY + kDlgCheckH + 6;
constexpr int kDlgButtonH = 32;
constexpr int kDlgButtonW = 132;
constexpr int kDlgCancelW = 88;
constexpr int kDlgButtonGap = 8;
constexpr int kDlgButtonY = kDlgTipY + 26;
constexpr int kDlgHeight = kDlgButtonY + kDlgButtonH + 14;
// 注意：这里不能用 WS_CLIPCHILDREN —— 输入框的圆角底板是靠父窗口画在编辑框
// 底下的，加了它就会把编辑框那块区域裁掉，编辑框只剩自己的白底。
constexpr DWORD kDlgStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_POPUP;
constexpr DWORD kDlgExStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;

struct ConnectDialogState {
    std::string host;
    std::string user;
    std::string password;
    int port = dchat::kDefaultPort;
    bool isRegister = false;  // 填了确认密码 = 注册
    bool accepted = false;
    bool done = false;
    bool showPassword = false;  // 「显示密码」：勾上后两个密码框显示明文
    std::wstring error;         // 校验失败的原因：显示在对话框里，不弹系统消息框
    RECT fieldRect[5]{};        // 五行输入框的圆角底板（自绘用）
    HWND edits[5]{};            // 五个编辑框（判断焦点高亮用）
    HBRUSH fieldBrush = nullptr;  // 编辑框自己的底色（和圆角底板同色），随对话框创建/销毁
};

// 「显示密码」：把两个密码框的遮挡字符设成 0（明文）或 ●（默认的圆点）。
// 改完之后要连父窗口一起重画：编辑框自己只重画文字，而它底下的圆角底板
// 是父窗口画的，不一起重画就会留下上一次的点号/明文。
void ApplyPasswordMask(HWND dlg, ConnectDialogState* state) {
    if (!state) return;
    const WPARAM mask = state->showPassword ? 0 : static_cast<WPARAM>(0x25CF);
    for (int i = 3; i <= 4; ++i) {  // 密码 / 确认密码
        if (!state->edits[i]) continue;
        SendMessageW(state->edits[i], EM_SETPASSWORDCHAR, mask, 0);
        if (dlg) InvalidateRect(dlg, &state->fieldRect[i], TRUE);
        RedrawWindow(state->edits[i], nullptr, nullptr,
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

            const std::wstring values[5] = {Utf8ToWide(state->host), std::to_wstring(state->port),
                                            Utf8ToWide(state->user), L"", L""};
            const int ids[5] = {IDD_HOST, IDD_PORT, IDD_USER, IDD_PASSWORD, IDD_CONFIRM};
            for (int i = 0; i < 5; ++i) {
                const int top = kDlgFirstRowY + i * kDlgRowH;
                state->fieldRect[i] =
                    RECT{kDlgFieldX, top, kDlgFieldX + kDlgFieldW, top + kDlgFieldH};
                const bool isPassword = i == 3 || i == 4;
                // 无边框 + 内缩：圆角底板由父窗口画，文字因此自带内边距
                state->edits[i] = CreateWindowW(
                    L"EDIT", values[i].c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                        (isPassword ? ES_PASSWORD : 0),
                    state->fieldRect[i].left + kDlgTextPadX, top + kDlgTextPadY,
                    kDlgFieldW - kDlgTextPadX * 2, kDlgFieldH - kDlgTextPadY * 2, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), nullptr, nullptr);
                SendMessageW(state->edits[i], WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            }

            // 两个圆角自绘按钮：主操作靠右（和主窗口的「发送」一样用强调色）
            HWND cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                kDlgWidth - kDlgPad - kDlgButtonW - kDlgButtonGap - kDlgCancelW, kDlgButtonY,
                kDlgCancelW, kDlgButtonH, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_CANCEL)), nullptr, nullptr);
            HWND ok = CreateWindowW(
                L"BUTTON", L"登录 / 注册", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                kDlgWidth - kDlgPad - kDlgButtonW, kDlgButtonY, kDlgButtonW, kDlgButtonH, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_OK)), nullptr, nullptr);
            const HWND buttons[2] = {ok, cancel};
            for (HWND button : buttons) {
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
                // 复用主窗口按钮那套自绘 + 悬停/按下反馈
                SetWindowLongPtrW(button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc));
            }

            // 「显示密码」勾选框（自绘，样式和整体一致）
            HWND show = CreateWindowW(
                L"BUTTON", L"显示密码", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                kDlgFieldX, kDlgCheckY, kDlgCheckW, kDlgCheckH, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDD_SHOW)), nullptr, nullptr);
            SendMessageW(show, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            SetWindowLongPtrW(show, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc));
            ApplyPasswordMask(hwnd, state);  // 默认遮挡（勾选框没勾）
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
            DrawTextIn(dc, L"填好服务器地址和账号，点「登录 / 注册」进入房间", subRect,
                       ui.fontSmall, g_palette->system,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            HBRUSH lineBrush = CreateSolidBrush(g_palette->border);
            const RECT lineRect{kDlgPad, 66, client.right - kDlgPad, 67};
            FillRect(dc, &lineRect, lineBrush);
            DeleteObject(lineBrush);

            // 五行：右对齐标签 + 圆角输入底板（有焦点的那行用强调色描边）
            const wchar_t* labels[5] = {L"服务器地址", L"端口", L"用户名", L"密码", L"确认密码"};
            for (int i = 0; i < 5; ++i) {
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
                         : L"提示：填「确认密码」= 注册新账号；留空 = 登录已有账号";
            const RECT tipRect{kDlgPad, kDlgTipY, client.right - kDlgPad, kDlgTipY + 18};
            DrawTextIn(dc, tip, tipRect, ui.fontSmall,
                       hasError ? g_palette->error : g_palette->time,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
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
            if (id == IDD_SHOW) {
                state->showPassword = !state->showPassword;
                ApplyPasswordMask(hwnd, state);
                InvalidateRect(GetDlgItem(hwnd, IDD_SHOW), nullptr, TRUE);
                return 0;
            }
            if (id == IDD_OK) {
                // 校验失败时把原因写在对话框底部、并把焦点挪到那一行，
                // 不弹系统消息框（深色主题下它会是一块刺眼的浅色）
                auto fail = [&](const wchar_t* text, int field) {
                    state->error = text;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    if (field >= 0 && field < 5 && state->edits[field]) {
                        SetFocus(state->edits[field]);
                    }
                };
                wchar_t host[128] = {0};
                wchar_t port[32] = {0};
                wchar_t user[64] = {0};
                wchar_t password[128] = {0};
                wchar_t confirm[128] = {0};
                GetDlgItemTextW(hwnd, IDD_HOST, host, 128);
                GetDlgItemTextW(hwnd, IDD_PORT, port, 32);
                GetDlgItemTextW(hwnd, IDD_USER, user, 64);
                GetDlgItemTextW(hwnd, IDD_PASSWORD, password, 128);
                GetDlgItemTextW(hwnd, IDD_CONFIRM, confirm, 128);
                const int parsedPort = _wtoi(port);
                dchat::NickError nickError = dchat::NickError::None;
                const std::string normalizedUser = dchat::NormalizeNick(WideToUtf8(user), &nickError);
                dchat::PasswordError passwordError = dchat::PasswordError::None;
                if (WideToUtf8(host).empty()) {
                    fail(L"请填写服务器地址（本机测试用 127.0.0.1）", 0);
                    return 0;
                }
                if (parsedPort <= 0 || parsedPort > 65535) {
                    fail(L"端口请填 1-65535（服务器默认 5555）", 1);
                    return 0;
                }
                if (normalizedUser.empty()) {
                    fail(Utf8ToWide(dchat::NickErrorText(nickError)).c_str(), 2);
                    return 0;
                }
                if (!dchat::ValidatePassword(WideToUtf8(password), &passwordError)) {
                    fail(Utf8ToWide(dchat::PasswordErrorText(passwordError)).c_str(), 3);
                    return 0;
                }
                const std::string confirmText = WideToUtf8(confirm);
                if (!confirmText.empty() && confirmText != WideToUtf8(password)) {
                    fail(L"两次输入的密码不一致", 4);
                    return 0;
                }
                state->host = WideToUtf8(host);
                state->port = parsedPort;
                state->user = normalizedUser;
                state->password = WideToUtf8(password);
                state->isRegister = !confirmText.empty();  // 填了确认密码 = 注册
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
    RECT rc{0, 0, kDlgWidth, kDlgHeight};
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

    HWND dlg = CreateWindowExW(kDlgExStyle, L"DchatConnectDlg", L"登录 / 注册", kDlgStyle, x, y,
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
    // 服务器地址是记住的，所以焦点直接落在用户名上（打开就能敲账号）
    SetFocus(GetDlgItem(dlg, IDD_USER));
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

// ---------------- 界面行为 ----------------
void DoConnect(HWND hwnd) {
    if (IsConnected()) return;
    ConnectDialogState state;
    // 只记住上次连的服务器（地址 + 端口）；用户名和密码每次都留空，
    // 免得打开窗口就看到 "user" 这种无意义的占位内容
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
    g_nick = state.user;  // 用户名即昵称，用于识别自己发的消息
    g_authPending = true;
    const std::string auth = dchat::BuildLine(state.isRegister ? "REGISTER" : "LOGIN",
                                             state.user + " " + state.password);
    if (!SendRawLine(auth)) {
        ViewAddItem(ItemKind::Error, "发送登录请求失败", dchat::NowTimeString());
        g_authPending = false;
        DisconnectFromServer(false);
        return;
    }
    ViewAddItem(ItemKind::Notice, state.isRegister ? "正在注册并登录…" : "正在登录…",
                dchat::NowTimeString());
    UpdateStatus();
}

void DoDisconnect(HWND hwnd) {
    if (!IsConnected()) return;
    SendRawLine(dchat::BuildLine("QUIT"));
    DisconnectFromServer(true);
    (void)hwnd;
}

void SendCurrentInput() {
    if (!IsConnected() || !ui.hInput) return;
    const int length = GetWindowTextLengthW(ui.hInput);
    if (length <= 0) return;
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(ui.hInput, &text[0], length + 1);
    SetWindowTextW(ui.hInput, L"");
    const std::string utf8 = WideToUtf8(text);
    if (utf8.empty()) return;
    HideSuggestions();  // 发出去了，候选浮层收起来
    if (!SendRawLine(dchat::BuildLine("MSG", utf8))) {
        ViewAddItem(ItemKind::Error, "发送失败，连接可能已断开", dchat::NowTimeString());
        DisconnectFromServer(false);
        return;
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
            const dchat::CompletionResult done = g_tabComplete.Next(current, g_onlineNicks);
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
    const int margin = 12, stripH = 32, gap = 8;
    const int buttonW = 78, colorW = 96, themeW = 128, fileW = 96;
    const int contentW = rc.right - margin * 2;
    const int rightStack = buttonW * 2 + colorW + themeW + fileW + gap * 4;

    const int statusW = contentW - rightStack - gap;
    MoveWindow(ui.hStatus, margin, margin + 6, statusW > 120 ? statusW : 120, stripH - 6, TRUE);
    int x = rc.right - margin - rightStack;
    MoveWindow(ui.hFileSend, x, margin, fileW, stripH, TRUE);
    x += fileW + gap;
    MoveWindow(ui.hColorToggle, x, margin, colorW, stripH, TRUE);
    x += colorW + gap;
    MoveWindow(ui.hThemeButton, x, margin, themeW, stripH, TRUE);
    x += themeW + gap;
    MoveWindow(ui.hConnect, x, margin, buttonW, stripH, TRUE);
    x += buttonW + gap;
    MoveWindow(ui.hDisconnect, x, margin, buttonW, stripH, TRUE);

    const int viewTop = margin + stripH + gap;
    const int viewHeight = rc.bottom - viewTop - stripH - gap * 2 - margin;
    MoveWindow(ui.hView, margin, viewTop, contentW, viewHeight > 60 ? viewHeight : 60, TRUE);
    // 输入框：父窗口画胶囊背景，编辑框内嵌在里面（这样文字天然有内边距，圆角也不会被方形底色盖住）
    const int pillLeft = margin;
    const int pillTop = rc.bottom - margin - stripH;
    const int pillWidth = contentW - buttonW - gap;
    g_inputPill = RECT{pillLeft, pillTop, pillLeft + pillWidth, pillTop + stripH};
    const int innerPadX = dchat::kMessagePaddingX;
    const int innerPadY = 5;
    MoveWindow(ui.hInput, pillLeft + innerPadX, pillTop + innerPadY,
               pillWidth - innerPadX * 2, stripH - innerPadY * 2, TRUE);
    MoveWindow(ui.hSend, rc.right - margin - buttonW, rc.bottom - margin - stripH, buttonW, stripH,
               TRUE);
    // 候选浮层：贴在输入框正上方（高度随候选行数变化）
    const int suggestH = SuggestPanelHeight();
    if (ui.hSuggest && suggestH > 0) {
        MoveWindow(ui.hSuggest, pillLeft, pillTop - 6 - suggestH, pillWidth, suggestH, TRUE);
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
    g_suggest = dchat::Suggest(text, g_onlineNicks);
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

            ui.hStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                       hwnd, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);

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
            ui.hColorToggle = CreateWindowW(L"BUTTON", L"彩色聊天",
                                            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                                            reinterpret_cast<HMENU>(IDC_COLOR), nullptr, nullptr);
            ui.hFileSend = CreateWindowW(L"BUTTON", L"发送文件",
                                         WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                                         reinterpret_cast<HMENU>(IDC_FILESEND), nullptr, nullptr);
            ui.hThemeButton = CreateWindowW(L"BUTTON", ThemeLabel(g_themeMode),
                                            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                                            reinterpret_cast<HMENU>(IDC_THEME), nullptr, nullptr);
            ui.hConnect = CreateWindowW(L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,
                                        0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_CONNECT),
                                        nullptr, nullptr);
            ui.hDisconnect = CreateWindowW(L"BUTTON", L"断开",
                                           WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                                           reinterpret_cast<HMENU>(IDC_DISCONNECT), nullptr, nullptr);
            ui.hSend = CreateWindowW(L"BUTTON", L"发送", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                                     10, 10, hwnd, reinterpret_cast<HMENU>(IDC_SEND), nullptr,
                                     nullptr);

            HWND controls[] = {ui.hStatus, ui.hInput,      ui.hColorToggle, ui.hThemeButton,
                               ui.hConnect, ui.hDisconnect, ui.hSend,        ui.hFileSend};
            for (HWND control : controls) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
            }
            g_oldInputProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(ui.hInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            g_oldButtonProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                ui.hSend, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc)));
            HWND ownerDrawn[] = {ui.hColorToggle, ui.hThemeButton, ui.hConnect, ui.hDisconnect,
                                 ui.hFileSend};
            for (HWND control : ownerDrawn) {
                SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc));
            }
            DragAcceptFiles(hwnd, TRUE);  // 允许把文件直接拖进窗口发送

            const std::string now = dchat::NowTimeString();
            ViewAddItem(ItemKind::Notice, "欢迎使用dchat 客户端", now);
            ViewAddItem(ItemKind::Notice, "先启动服务器（dchat_server.exe），再点「连接」", now);
            ViewAddItem(ItemKind::Notice, "本机测试：127.0.0.1，端口 5555；@all 通知所有人", now);

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
                case IDC_CONNECT:
                    DoConnect(hwnd);
                    return 0;
                case IDC_DISCONNECT:
                    DoDisconnect(hwnd);
                    return 0;
                case IDC_SEND:
                    SendCurrentInput();
                    return 0;
                case IDC_FILESEND:
                    DoSendFile(hwnd);
                    return 0;
                case IDC_COLOR:
                    g_colorEnabled = !g_colorEnabled;
                    InvalidateRect(ui.hColorToggle, nullptr, TRUE);
                    InvalidateRect(ui.hView, nullptr, TRUE);
                    return 0;
                case IDC_THEME:
                    CycleThemeMode();
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
                // 文件传输的几条消息不进记录区，由文件模块自己处理
                if (raw.command.compare(0, 5, "FILE_") == 0) {
                    HandleFileLine(*payload);
                    delete payload;
                    return 0;
                }
                // 维护在线名单（Tab 补昵称参数要用）
                if (raw.command == "PONG" && g_heartbeatPending) {
                    g_heartbeatPending = false;  // 心跳的回应，安安静静地吞掉
                    delete payload;
                    return 0;
                }
                if (raw.command == "NAMES") {
                    SetOnlineNicks(StripTimePrefix(raw.rest));
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
                const bool mention = dchat::MentionsMe(*payload, g_nick);
                if (g_authPending && raw.command == "ERROR") {
                    // 登录 / 注册被服务器拒绝：先把原因显示出来，再断开连接让用户改完重连
                    ViewAddItem(ItemKind::Error, notice.text, notice.time);
                    g_authPending = false;
                    DisconnectFromServer(false);
                    ViewAddItem(ItemKind::Notice, "登录 / 注册未成功，请检查用户名与密码后重试",
                                dchat::NowTimeString());
                } else if (g_authPending && raw.command == "LOGGEDIN") {
                    // 服务器明确确认认证通过（不是靠"收到任何提示"来判断，避免被 WELCOME 干扰）
                    g_authPending = false;
                    if (!notice.text.empty()) g_nick = notice.text;
                    ViewAddItem(ItemKind::Notice, "已登录：" + g_nick, notice.time);
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
            if (g_sock != INVALID_SOCKET) {
                DisconnectFromServer(false);
                ViewAddItem(ItemKind::Notice, "与服务器的连接已断开", dchat::NowTimeString());
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
            // 记录区的边框由它自己绘制；输入框的胶囊背景在这里画（编辑框是透明的，只负责文字）
            if (g_inputPill.right > g_inputPill.left) {
                const int radius = (g_inputPill.bottom - g_inputPill.top) / 2;
                ui::DrawRoundedControl(dc, g_inputPill, radius, g_palette->windowBg,
                                       g_palette->bubbleOther, g_palette->border);
            }
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
    ui::Startup();  // 圆角与气泡都用 GDI+ 画

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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ui::Shutdown();
    return static_cast<int>(msg.wParam);
}
