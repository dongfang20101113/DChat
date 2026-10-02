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

// 「＋」弹出来的小菜单（往上弹，贴在输入行上方）
struct PlusMenuLayout {
    RECT panel{};
    RECT fileItem{};   // 发送文件
    RECT voiceItem{};  // 录一段语音
    bool valid = false;  // 窗口太矮放不下时为 false（那时不弹菜单）
};

PlusMenuLayout LayoutPlusMenu(const BottomLayout& bottom, int contentTop);

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
// 图标与装饰（不依赖字体，缺字形也不会变方块）
// ---------------------------------------------------------------------------

// 齿轮（设置）
void DrawGearIcon(HDC dc, int centerX, int centerY, int radius, COLORREF color);

// 加号（附件入口）
void DrawPlusIcon(HDC dc, int centerX, int centerY, int armLength, int thickness,
                  COLORREF color);

// 汉堡（三横线，方案 B 用；留着以免以后想换回去）
void DrawHamburgerIcon(HDC dc, const RECT& box, COLORREF color);

// 播放三角 / 暂停双竖线（语音气泡）
void DrawPlayIcon(HDC dc, const RECT& box, COLORREF color);
void DrawPauseIcon(HDC dc, const RECT& box, COLORREF color);

// 一条圆角"进度/分隔"细线
void DrawThinBar(HDC dc, const RECT& rect, COLORREF color);

// 把颜色调亮/调暗（画悬停、按下、边框时用）
COLORREF AdjustColor(COLORREF color, int delta);

}  // namespace dchat
