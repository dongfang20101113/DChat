// 主窗口"外壳"的布局与图标：菜单栏、状态条、输入行、齿轮、加号菜单。
//
// ## 为什么单独一个模块
//
// 这些是**几何**，不是业务：菜单项多宽、点击落在哪一项、状态条在哪、齿轮在哪个矩形里。
// 放在 client.cpp 里的话只能靠手点验证，而"点偏了两像素没反应"这种问题手点最难发现。
// 抽出来之后每个矩形都能在单测里断言（见 tests/test_ui_layout.cpp）。
//
// 图标也在这里画：齿轮和加号不依赖任何字体（**不能依赖 emoji**——Segoe UI Emoji
// 在有些系统上缺字形会画成方块），而且和 tools/ui_mockup.cpp 的效果图共用同一份代码，
// 效果图里看到的就是真做出来的样子。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dchat {

// ---------------------------------------------------------------------------
// 尺寸：界面和测试共用同一套数字，改一处就够
// ---------------------------------------------------------------------------
inline constexpr int kMenuBarHeight = 34;    // 菜单栏（连接 / 设置 / 帮助）
inline constexpr int kStatusBarHeight = 24;  // 底部状态条（只读信息）
inline constexpr int kInputRowHeight = 56;   // 输入行（胶囊 + ＋ + 发送）
inline constexpr int kInputPillHeight = 40;
inline constexpr int kInputRowMargin = 12;
inline constexpr int kPlusSize = 40;         // 「＋」按钮是正方形
inline constexpr int kSendWidth = 78;
inline constexpr int kGearSize = 32;         // 齿轮的点击区域（图标本身更小）
inline constexpr int kStatusChipHeight = 22;  // 菜单栏右侧的连接状态胶囊
inline constexpr int kMenuItemPadX = 12;      // 菜单项左右内边距
inline constexpr int kMenuItemsTop = 2;       // 菜单项相对菜单栏顶部的偏移

// ---------------------------------------------------------------------------
// 菜单栏上的项目
// ---------------------------------------------------------------------------
enum class MenuAction {
    Connect,       // 连接 / 断开（按当前状态切换）
    Settings,      // 打开设置窗口
    Help,          // 帮助（进记录区的提示）
    ToggleOnline,  // 点右侧的状态胶囊：展开在线成员
};

struct MenuItem {
    MenuAction action = MenuAction::Connect;
    std::wstring label;
    RECT rect{};   // 点击热区
    bool active = false;  // 鼠标悬停 / 菜单打开时高亮
};

// 算出菜单项的矩形（需要 HDC 量文字宽度）
std::vector<MenuItem> LayoutMenuItems(HDC dc, HFONT font, int windowWidth, bool connected);

// 点在哪一项上？返回 nullptr 表示没命中（点空白处应当关闭菜单而不是做事）
const MenuItem* HitTestMenu(const std::vector<MenuItem>& items, int x, int y);

// 齿轮的矩形（菜单栏最右侧）
RECT GearRect(int windowWidth);

// 菜单栏右侧的连接状态胶囊
RECT StatusChipRect(HDC dc, HFONT font, int windowWidth, const std::wstring& text);

// ---------------------------------------------------------------------------
// 底部：状态条 + 输入行
// ---------------------------------------------------------------------------
struct BottomLayout {
    RECT statusBar{};   // 只读状态条
    RECT inputPill{};   // 输入胶囊
    RECT plusButton{};  // 「＋」
    RECT sendButton{};  // 「发送」
};

// @param contentTop 菜单栏底边（聊天记录区的上边界）
BottomLayout LayoutBottom(HDC dc, HFONT font, int windowWidth, int windowHeight,
                          int contentTop);

// 「＋」弹出来的小菜单（**独立弹出窗口**，往上弹、压在内容上）
inline constexpr int kPlusMenuWidth = 180;
inline constexpr int kPlusMenuItemHeight = 34;

struct PlusMenuLayout {
    RECT panel{};
    RECT fileItem{};   // 发送文件
    RECT voiceItem{};  // 录一段语音（或"结束并发送"）
    bool valid = false;
};

// 面板矩形（客户区坐标）：贴在「＋」上方，**下边界不压到按钮**
RECT PlusMenuRect(const BottomLayout& bottom);

PlusMenuLayout LayoutPlusMenu(const BottomLayout& bottom);

enum class PlusAction { None, SendFile, RecordVoice };

// 点在「＋」菜单的哪一项上？点在菜单外返回 None（调用方据此收起菜单）
PlusAction HitTestPlusMenu(const PlusMenuLayout& menu, int x, int y);

// 点是否落在「＋」菜单面板里（用来判断"点外面就收起"）
bool InsidePlusMenu(const PlusMenuLayout& menu, int x, int y);

// ---------------------------------------------------------------------------
// 设置窗口的布局（客户区坐标，不含系统标题栏）
// ---------------------------------------------------------------------------
struct SettingsLayout {
    RECT title{};
    RECT closeButton{};
    RECT separator{};
    RECT themeSegment{};     // 深色 / 浅色 / 跟随系统
    RECT colorToggle{};      // 彩色聊天的开关
    RECT colorToggleLabel{};
    RECT hostField{};
    RECT portField{};
    RECT voiceSegment{};     // 30 秒 / 1 分钟 / 2 分钟
    RECT voiceHint{};
    RECT okButton{};
    RECT cancelButton{};
};

inline constexpr int kSettingsWidth = 620;
inline constexpr int kSettingsHeight = 470;
inline constexpr int kSettingsPadX = 24;
inline constexpr int kSettingsLabelWidth = 150;  // 左列标签宽度（中文标签要留够）
inline constexpr int kSettingsRowHeight = 34;
inline constexpr int kSettingsSectionGap = 12;

SettingsLayout LayoutSettings(int clientWidth, int clientHeight);

// 三段式选择器里，点中的是第几段？返回 -1 表示没命中
int SegmentHitTest(const RECT& segment, int segments, int x, int y);

// ---------------------------------------------------------------------------
// 左侧会话列表（多会话改造新增）
// ---------------------------------------------------------------------------
//
// 和别处一样的理由：一行多高、点在哪一行、未读气泡画在哪个矩形里，全是几何。
// 写在 client.cpp 里就只能靠手点验证，而"点偏两像素没切过去"这种问题手点最难发现。
inline constexpr int kSessionListWidth = 208;   // 左栏宽度
inline constexpr int kSessionListHeaderH = 34;  // 表头（标题 + 两个按钮）
inline constexpr int kSessionRowHeight = 58;
inline constexpr int kSessionAvatarSize = 36;
inline constexpr int kSessionRowPadX = 10;
inline constexpr int kSessionBadgeMin = 18;     // 未读气泡最小直径

/** 一行要显示的东西。**纯数据**，不带任何 client.cpp 的类型，这样单测直接塞假数据。 */
struct SessionRowInput {
    std::wstring title;
    std::wstring subtitle;   // 群名/成员数，或私聊的最后一句话
    int unread = 0;
    bool muted = false;
    bool pinned = false;
    bool important = false;  // 有未读的重要消息（@我 / @全体）
};

struct SessionRowLayout {
    RECT row{};
    RECT avatar{};
    RECT title{};
    RECT subtitle{};
    RECT badge{};
    RECT pinMark{};
    RECT muteMark{};
    int index = -1;          // 对应哪个会话；-1 表示不是一个会话行
    bool valid = false;
};

struct SessionListLayout {
    RECT panel{};
    RECT header{};
    RECT title{};            // 表头上的「会话」
    RECT searchButton{};     // 搜索群聊
    RECT newButton{};        // 新建群聊
    std::vector<SessionRowLayout> rows;
    bool valid = false;
};

/**
 * 算左栏的几何。
 *
 * 只排**放得下**的行：列表比栏高时后面的行不生成（滚动由调用方按 scrollTop 偏移
 * 之后重新调用本函数实现 —— 让"可见范围"由几何层决定，点击命中就不会算错）。
 *
 * @param top     左栏顶边（菜单栏底边）
 * @param height  左栏高度
 * @param scrollTop 已经滚过去多少像素
 */
SessionListLayout LayoutSessionList(int top, int height, int scrollTop,
                                    const std::vector<SessionRowInput>& sessions);

/** 内容总高度（用于滚动条）。 */
int SessionListContentHeight(const std::vector<SessionRowInput>& sessions);

/** 点在哪个按钮/哪一行上？ */
enum class SessionHit { None, Row, Search, NewGroup };

struct SessionHitResult {
    SessionHit what = SessionHit::None;
    int index = -1;  // what == Row 时有效
};

SessionHitResult HitTestSessionList(const SessionListLayout& layout, int x, int y);

/** 会话行的图标字形（系统图标字体）。 */
inline constexpr wchar_t kGlyphSearch = L'\uE721';   // 放大镜
inline constexpr wchar_t kGlyphAdd = L'\uE710';      // 加号
inline constexpr wchar_t kGlyphMute = L'\uE74F';     // 铃铛关闭（免打扰）
inline constexpr wchar_t kGlyphPin = L'\uE718';      // 图钉（置顶）
inline constexpr wchar_t kGlyphHall = L'\uE7F4';     // 大厅 / 公共房间
inline constexpr wchar_t kGlyphGroup = L'\uE902';    // 群聊
inline constexpr wchar_t kGlyphPerson = L'\uE77B';   // 私聊

// ---------------------------------------------------------------------------
// 图标与装饰
//
// **用 Windows 自带的图标字体**（Segoe MDL2 Assets，Win10/11 都在），不自己画：
//   - 手画的齿轮缩小到 16px 就糊成一团（试过，看着很廉价）；
//   - 也不用 emoji 字体（Segoe UI Emoji 在某些系统上缺字形，会画成方块）。
// 取不到图标字体时**自动退回手画的版本**——没有字体也不能变成空格子。
// ---------------------------------------------------------------------------

// 系统图标字体里的字形（都是单色，绘制时用主题色，深/浅主题都能上色）
inline constexpr wchar_t kGlyphGear = L'\uE713';      // 设置
inline constexpr wchar_t kGlyphMic = L'\uE720';       // 麦克风
inline constexpr wchar_t kGlyphPlus = L'\uE710';      // 加号（细体）
inline constexpr wchar_t kGlyphAttachment = L'\uE723';  // 回形针
inline constexpr wchar_t kGlyphClose = L'\uE711';     // 关闭

// 图标字体名；取不到时 DrawGlyph 返回 false，调用方自己画退路
inline constexpr const wchar_t* kIconFontName = L"Segoe MDL2 Assets";

// 在 box 里居中画一个图标字形。fontHeight 是**像素**（负值按 GDI 习惯取字符高度）。
// 返回 false 表示这台机器没有图标字体（调用方应当画自己的退路图标）。
bool DrawGlyph(HDC dc, const RECT& box, wchar_t glyph, int fontHeight, COLORREF color);

// 图标字体到底能不能用（缓存一次探测结果）
bool HasIconFont();

// 手画的齿轮（图标字体不可用时的退路）
void DrawGearIcon(HDC dc, int centerX, int centerY, int radius, COLORREF color);

// 加号（也是退路；正常路径用 DrawGlyph）
void DrawPlusIcon(HDC dc, int centerX, int centerY, int armLength, int thickness,
                  COLORREF color);

// 汉堡（三横线）
void DrawHamburgerIcon(HDC dc, const RECT& box, COLORREF color);

// 播放三角 / 暂停双竖线（语音气泡）
void DrawPlayIcon(HDC dc, const RECT& box, COLORREF color);
void DrawPauseIcon(HDC dc, const RECT& box, COLORREF color);

// 一条圆角"进度/分隔"细线
void DrawThinBar(HDC dc, const RECT& rect, COLORREF color);

// 把颜色调亮/调暗（画悬停、按下、边框时用）
COLORREF AdjustColor(COLORREF color, int delta);

}  // namespace dchat
