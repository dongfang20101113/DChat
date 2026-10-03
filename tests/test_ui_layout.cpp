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

    DeleteDC(dc);
    DeleteObject(g_font);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
