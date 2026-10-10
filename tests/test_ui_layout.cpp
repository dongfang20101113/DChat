// 主窗口外壳的布局测试：菜单栏、状态条、输入行、加号菜单、设置窗口。
//
// 这些全是几何，手点最难发现的恰恰是"偏两像素点不中"和"窗口变矮之后某块跑到屏幕外"，
// 所以这里把每个矩形的**不变量**都断言一遍：不重叠、不越界、点得中。
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "ui_layout.h"

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

void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

bool Inside(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top &&
           inner.bottom <= outer.bottom;
}

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

int Width(const RECT& rect) { return rect.right - rect.left; }
int Height(const RECT& rect) { return rect.bottom - rect.top; }

HFONT g_font = nullptr;

}  // namespace

int main() {
    std::printf("== dchat UI layout tests ==\n");

    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    constexpr int kWidth = 940;
    constexpr int kHeight = 660;

    std::printf("[1] 菜单栏\n");
    {
        const std::vector<dchat::MenuItem> items =
            dchat::LayoutMenuItems(dc, g_font, kWidth, /*connected=*/false);
        // 只有两项：**「设置」只在右上角齿轮那里**——菜单栏再摆一个「设置」，
        // 用户第一反应就是"怎么有两个设置"
        check(items.size() == 2, "两项：连接 / 帮助（设置在齿轮里）");
        check(items[0].label == L"连接", "没连上时第一项是「连接」");
        check(items[0].rect.left >= 0 && items[0].rect.top >= 0, "第一项不出界");
        check(items[0].rect.bottom <= dchat::kMenuBarHeight, "菜单项不超出菜单栏高度");
        bool ordered = true;
        for (std::size_t i = 1; i < items.size(); ++i) {
            if (items[i].rect.left < items[i - 1].rect.right) ordered = false;
        }
        check(ordered, "菜单项从左到右依次排列、互不重叠");

        // 每一项都要点得中：**点正中间必须命中自己**
        bool allHittable = true;
        for (const dchat::MenuItem& item : items) {
            const int cx = (item.rect.left + item.rect.right) / 2;
            const int cy = (item.rect.top + item.rect.bottom) / 2;
            const dchat::MenuItem* hit = dchat::HitTestMenu(items, cx, cy);
            if (!hit || hit->action != item.action) allHittable = false;
        }
        check(allHittable, "每一项的正中间都能点中自己");

        // 边界：右上角（齿轮那块）不该被当成菜单项
        check(dchat::HitTestMenu(items, items.back().rect.right + 5, 18) == nullptr,
              "菜单项右侧的空白不命中任何菜单项");

        // 连上之后第一项变成「断开」
        const std::vector<dchat::MenuItem> online =
            dchat::LayoutMenuItems(dc, g_font, kWidth, /*connected=*/true);
        check(online[0].label == L"断开", "连上后第一项变成「断开」");
        check(online.size() == items.size(), "仍然是三项，不是又多摆一个按钮");
    }

    std::printf("[2] 齿轮与状态胶囊\n");
    {
        const RECT gear = dchat::GearRect(kWidth);
        check(Width(gear) == dchat::kGearSize && Height(gear) == dchat::kGearSize,
              "齿轮是正方形热区");
        check(gear.right <= kWidth, "齿轮不越出右边界");
        check(gear.bottom <= dchat::kMenuBarHeight, "齿轮在菜单栏里");

        const RECT chip = dchat::StatusChipRect(dc, g_font, kWidth, L"● 192.168.1.100　我");
        check(chip.right <= gear.left, "状态胶囊在齿轮左边，不重叠");
        check(!Overlaps(chip, gear), "两者确实不重叠");
        check(chip.bottom <= dchat::kMenuBarHeight, "胶囊在菜单栏里");
    }

    std::printf("[3] 底部：状态条 + 输入行\n");
    {
        const int contentTop = dchat::kMenuBarHeight;
        const dchat::BottomLayout bottom =
            dchat::LayoutBottom(dc, g_font, kWidth, kHeight, contentTop);

        check(bottom.statusBar.right == kWidth && bottom.statusBar.bottom == kHeight -
                                                                              dchat::kInputRowHeight,
              "状态条横跨整个宽度、正好在输入行之上");
        check(Height(bottom.statusBar) == dchat::kStatusBarHeight, "状态条高度对得上");
        check(bottom.statusBar.top > contentTop, "状态条在记录区下方");

        check(Inside(RECT{0, 0, kWidth, kHeight}, bottom.inputPill), "输入胶囊不出界");
        check(Inside(RECT{0, 0, kWidth, kHeight}, bottom.plusButton), "＋ 不出界");
        check(Inside(RECT{0, 0, kWidth, kHeight}, bottom.sendButton), "发送不出界");
        check(!Overlaps(bottom.inputPill, bottom.plusButton), "胶囊和 ＋ 不重叠");
        check(!Overlaps(bottom.plusButton, bottom.sendButton), "＋ 和发送不重叠");
        check(bottom.inputPill.left == dchat::kInputRowMargin, "胶囊左边留了边距");
        check(kWidth - bottom.sendButton.right == dchat::kInputRowMargin, "发送右边留了边距");
        check(Height(bottom.inputPill) == dchat::kInputPillHeight, "胶囊高度对得上");
        check(Height(bottom.plusButton) == Width(bottom.plusButton), "＋ 是正方形");
        check(bottom.inputPill.bottom <= kHeight, "胶囊在窗口内");
        check(bottom.inputPill.bottom <= bottom.statusBar.bottom + dchat::kInputRowHeight,
              "胶囊在输入行里");

        // 窗口变窄也不能挤爆：极窄时胶囊仍然要有宽度
        const dchat::BottomLayout narrow =
            dchat::LayoutBottom(dc, g_font, 760, 420, dchat::kMenuBarHeight);
        check(Width(narrow.inputPill) > 200, "窗口收窄到最小尺寸时胶囊还有宽度（" +
                                                std::to_string(Width(narrow.inputPill)) + "）");
        check(Inside(RECT{0, 0, 760, 420}, narrow.sendButton), "窄窗口下发送不出界");
    }

    std::printf("[4] ＋ 菜单\n");
    {
        const dchat::BottomLayout bottom =
            dchat::LayoutBottom(dc, g_font, kWidth, kHeight, dchat::kMenuBarHeight);
        const dchat::PlusMenuLayout menu = dchat::LayoutPlusMenu(bottom);
        check(menu.valid, "菜单始终有效（它是独立弹出窗口，不受父窗口布局挤）");
        check(menu.panel.bottom <= bottom.plusButton.top, "菜单往上弹，不盖住 ＋ 按钮");
        // 这是**修过一个真 bug** 的地方：以前把面板挤在"聊天记录区"里，
        // 窗口一矮面板就被压到状态条底下，看起来就是"语音那项被遮挡"。
        // 现在面板是独立窗口，允许浮在状态条上方——但要保证它**不压住「＋」按钮本身**。
        check(menu.panel.bottom > bottom.statusBar.top,
              "面板可以浮在状态条上方（独立窗口，不再被父窗口布局剪切）");
        check(Inside(menu.panel, menu.fileItem) && Inside(menu.panel, menu.voiceItem),
              "两个菜单项都在面板里");
        check(!Overlaps(menu.fileItem, menu.voiceItem), "两个菜单项不重叠");

        const int fileX = (menu.fileItem.left + menu.fileItem.right) / 2;
        const int fileY = (menu.fileItem.top + menu.fileItem.bottom) / 2;
        check(dchat::HitTestPlusMenu(menu, fileX, fileY) == dchat::PlusAction::SendFile,
              "点第一项是「发送文件」");
        const int voiceX = (menu.voiceItem.left + menu.voiceItem.right) / 2;
        const int voiceY = (menu.voiceItem.top + menu.voiceItem.bottom) / 2;
        check(dchat::HitTestPlusMenu(menu, voiceX, voiceY) == dchat::PlusAction::RecordVoice,
              "点第二项是「录一段语音」");
        check(dchat::HitTestPlusMenu(menu, menu.panel.left - 5, fileY) == dchat::PlusAction::None,
              "点面板外的左边不算点中任何一项");
        check(dchat::InsidePlusMenu(menu, fileX, fileY), "点中间在面板里");
        check(!dchat::InsidePlusMenu(menu, menu.panel.left - 5, fileY),
              "点面板外就当作「点外面」（调用方据此收起菜单）");

        // 窗口很矮时**照样要能用**（以前会算出"无效"导致菜单打不开）：
        // 面板是独立窗口，浮在哪都行，只要求不盖住「＋」按钮
        for (int winH : {300, 420, 480}) {
            const dchat::BottomLayout shortWindow =
                dchat::LayoutBottom(dc, g_font, kWidth, winH, dchat::kMenuBarHeight);
            const dchat::PlusMenuLayout shortMenu = dchat::LayoutPlusMenu(shortWindow);
            check(shortMenu.valid && shortMenu.panel.bottom <= shortWindow.plusButton.top,
                  "窗口高 " + std::to_string(winH) + " 时菜单仍然可用且不盖住「＋」");
        }
    }

    std::printf("[5] 设置窗口\n");
    {
        const dchat::SettingsLayout layout =
            dchat::LayoutSettings(dchat::kSettingsWidth, dchat::kSettingsHeight);
        const RECT client{0, 0, dchat::kSettingsWidth, dchat::kSettingsHeight};

        check(Width(layout.themeSegment) > 300, "三段式选择器有足够宽度放下「跟随系统」四个字");
        check(layout.themeSegment.left > dchat::kSettingsPadX + 100,
              "控件起点给左列标签留了位置（不会和中文标签叠在一起）");
        check(layout.colorToggleLabel.left > layout.colorToggle.right,
              "彩色聊天的说明文字在开关右边，不重叠");
        check(!Overlaps(layout.colorToggle, layout.colorToggleLabel), "开关和说明不重叠");
        check(Inside(client, layout.hostField) && Inside(client, layout.portField),
              "地址/端口输入框都在客户区里");
        check(Inside(client, layout.voiceHint), "语音提示文字在客户区里");
        check(layout.voiceHint.bottom < layout.okButton.top,
              "提示文字在底部按钮**上方**（不会被按钮压住）");
        check(Inside(client, layout.okButton) && Inside(client, layout.cancelButton),
              "两个底部按钮都在客户区里");
        check(!Overlaps(layout.okButton, layout.cancelButton), "两个底部按钮不重叠");
        check(layout.title.bottom < layout.themeSegment.top, "标题在选择器上方");

        // 三段式选择器：每一段都要点得中，且顺序正确
        check(dchat::SegmentHitTest(layout.themeSegment, 3, layout.themeSegment.left + 2,
                                    layout.themeSegment.top + 2) == 0,
              "最左边是第 0 段");
        check(dchat::SegmentHitTest(layout.themeSegment, 3, layout.themeSegment.right - 2,
                                    layout.themeSegment.top + 2) == 2,
              "最右边是第 2 段");
        check(dchat::SegmentHitTest(layout.themeSegment, 3, layout.themeSegment.left - 1,
                                    layout.themeSegment.top + 2) == -1,
              "选择器外面不算点中");
    }

    // -----------------------------------------------------------------------
    // 左侧会话列表（多会话）
    //
    // 这一块的 bug 全是"点偏两像素没切过去"那种：手点最难发现，所以每个矩形
    // 和每一次命中都断言一遍。
    {
        std::printf("[会话列表] 几何与命中\n");
        std::vector<dchat::SessionRowInput> rows;
        for (int i = 0; i < 5; ++i) {
            dchat::SessionRowInput row;
            row.title = L"会话";
            row.subtitle = L"3 人";
            row.unread = (i == 1) ? 7 : 0;
            row.muted = (i == 2);
            row.pinned = (i == 0);
            row.important = (i == 4);
            rows.push_back(row);
        }

        const int top = dchat::kMenuBarHeight;
        const int height = 400;
        const dchat::SessionListLayout layout = dchat::LayoutSessionList(top, height, 0, rows);
        check(layout.valid, "布局有效");
        check(layout.panel.left == 0 && layout.panel.right == dchat::kSessionListWidth,
              "面板贴着左边、宽度固定");
        check(layout.panel.top == top, "面板上边贴菜单栏");
        check(layout.panel.bottom == top + height, "面板下边到状态条");
        check(layout.rows.size() == rows.size(), "五行都排得下");
        check(dchat::SessionListContentHeight(rows) ==
                  dchat::kSessionListHeaderH + 5 * dchat::kSessionRowHeight,
              "内容总高度 = 表头 + 每行高度之和");

        // 行必须**从上到下、不重叠、不跳行** —— 这三条任意一条破了，
        // 点击就会落到相邻的会话上（用户会说"点 A 打开了 B"）
        for (std::size_t i = 1; i < layout.rows.size(); ++i) {
            check(layout.rows[i].row.top == layout.rows[i - 1].row.bottom,
                  "相邻两行首尾相接、没有缝也没有重叠");
            check(layout.rows[i].index == static_cast<int>(i), "行的下标按顺序递增");
        }
        check(layout.rows[0].row.top == top + dchat::kSessionListHeaderH, "第一行在表头下面");

        // 表头两个按钮：不重叠、都落在面板内、搜索在新建左边
        check(!Overlaps(layout.searchButton, layout.newButton), "搜索和新建按钮不重叠");
        check(layout.newButton.right <= dchat::kSessionListWidth, "新建按钮不越出面板右边");
        check(layout.searchButton.left > layout.title.left, "标题不会被按钮压住");
        check(layout.searchButton.right < layout.newButton.left, "搜索在新建左边");

        // 命中：整行都是热区，不只是文字
        dchat::SessionHitResult hit =
            dchat::HitTestSessionList(layout, 4, layout.rows[2].row.top + 4);
        check(hit.what == dchat::SessionHit::Row && hit.index == 2, "点行的空白处也能命中这一行");
        hit = dchat::HitTestSessionList(layout, layout.rows[4].row.left + 3,
                                        layout.rows[4].row.bottom - 3);
        check(hit.what == dchat::SessionHit::Row && hit.index == 4, "点行的下边缘命中同一行");
        hit = dchat::HitTestSessionList(layout, layout.searchButton.left + 2,
                                        layout.searchButton.top + 2);
        check(hit.what == dchat::SessionHit::Search, "命中搜索按钮");
        hit = dchat::HitTestSessionList(layout, layout.newButton.left + 2,
                                        layout.newButton.top + 2);
        check(hit.what == dchat::SessionHit::NewGroup, "命中新建按钮");
        hit = dchat::HitTestSessionList(layout, dchat::kSessionListWidth + 5,
                                        layout.rows[0].row.top + 4);
        check(hit.what == dchat::SessionHit::None, "面板外面不算命中");
        hit = dchat::HitTestSessionList(layout, 4, top + 2);
        check(hit.what == dchat::SessionHit::None, "表头空白处不算命中任何一行");

        // 放不下时只排可见的行，而且**下标仍然指向正确的会话** ——
        // 这是滚动列表最容易出的错：可见行从第 3 条开始，但下标却从 0 数起
        const dchat::SessionListLayout scrolled = dchat::LayoutSessionList(
            top, dchat::kSessionListHeaderH + dchat::kSessionRowHeight * 2,
            dchat::kSessionRowHeight, rows);
        check(scrolled.rows.size() == 2, "只排放得下的行");
        check(scrolled.rows[0].index == 1, "滚一行之后第一行是第 1 个会话（下标不能从 0 重来）");
        check(scrolled.rows[1].index == 2, "第二行是第 2 个会话");
        check(scrolled.rows[0].row.top == top + dchat::kSessionListHeaderH, "滚动后第一行仍贴表头");

        // 极端情况不能崩：高度为 0、会话为空、滚动量为负
        const dchat::SessionListLayout empty = dchat::LayoutSessionList(top, 0, 0, rows);
        check(!empty.valid, "高度为 0 时布局无效（调用方据此跳过绘制）");
        const dchat::SessionListLayout noRows =
            dchat::LayoutSessionList(top, 300, 0, std::vector<dchat::SessionRowInput>());
        check(noRows.valid && noRows.rows.empty(), "没有会话时不产生行，但面板仍然有效");
        // 负数滚动量要夹回 0，否则第一行会跑到表头上面去、点不到
        const dchat::SessionListLayout negative = dchat::LayoutSessionList(top, 300, -50, rows);
        check(negative.rows[0].row.top == top + dchat::kSessionListHeaderH,
              "滚动量为负时夹回 0");
    }

    DeleteDC(dc);
    DeleteObject(g_font);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
