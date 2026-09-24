// 输入框历史记录单元测试：上键翻自己发过的内容、下键往回翻、没有历史时无效。
#include <cstdio>
#include <string>

#include "input_history.h"

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
}  // namespace

int main() {
    std::printf("== dchat input history tests ==\n");

    {
        std::printf("[1] 没有历史时按上键无效\n");
        dchat::InputHistory history;
        std::string text = "正在写的内容";
        check(!history.Up(&text), "空历史按上键返回 false");
        check(text == "正在写的内容", "空历史按上键不改动输入框");
        check(!history.Down(&text), "没在翻历史时按下键也不动");
        check(history.Count() == 0, "历史条数为 0");
    }

    {
        std::printf("[2] 发一条之后能按上键取回\n");
        dchat::InputHistory history;
        history.Add("第一条消息");
        std::string text;
        check(history.Up(&text), "按上键取到内容");
        check(text == "第一条消息", "取回的正是刚发的那条");
        check(!history.Up(&text), "只有一条时再按上键无效（第一条则无效）");
        check(text == "第一条消息", "无效时输入框内容保持不变");
        check(history.Down(&text) && text.empty(), "按上键后按下键回到空草稿");
    }

    {
        std::printf("[3] 连续按上键依次往前翻\n");
        dchat::InputHistory history;
        history.Add("第一条");
        history.Add("第二条");
        history.Add("第三条");
        std::string text;
        check(history.Up(&text) && text == "第三条", "第一次按上键是最新一条");
        check(history.Up(&text) && text == "第二条", "再按是上一条");
        check(history.Up(&text) && text == "第一条", "再按是最旧一条");
        check(!history.Up(&text) && text == "第一条", "最旧一条时再按无效");
        check(history.Down(&text) && text == "第二条", "下键往回翻");
        check(history.Down(&text) && text == "第三条", "继续下键到最新一条");
    }

    {
        std::printf("[4] 草稿会被保留\n");
        dchat::InputHistory history;
        history.Add("历史消息");
        std::string text;
        history.SetDraft("我正在写的草稿");
        check(history.Up(&text) && text == "历史消息", "上键取到历史消息");
        check(history.Down(&text) && text == "我正在写的草稿", "翻回底部恢复草稿");
        // 翻回底部之后浏览状态结束：此时输入框里的内容就是新的草稿
        history.SetDraft("翻回底部后的内容");
        check(history.Up(&text) && text == "历史消息", "再次上键仍是历史消息");
        check(history.Down(&text) && text == "翻回底部后的内容", "新草稿被正确记住");
    }

    {
        std::printf("[5] 发送后从最新一条重新开始\n");
        dchat::InputHistory history;
        history.Add("旧消息");
        std::string text;
        check(history.Up(&text) && text == "旧消息", "先翻到旧消息");
        history.Add("新消息");
        check(history.Up(&text) && text == "新消息", "发送后按上键是最新发的");
        check(history.Down(&text) && text.empty(), "下键回到草稿（发送后草稿是空的）");
    }

    {
        std::printf("[6] 边界情况\n");
        dchat::InputHistory history;
        history.Add("");
        check(history.Count() == 0, "空字符串不会进历史");
        for (int i = 0; i < 250; ++i) history.Add("消息 " + std::to_string(i));
        check(history.Count() == dchat::InputHistory::kMaxEntries, "历史条数有上限（不会无限增长）");
        std::string text;
        check(history.Up(&text) && text == "消息 249", "上键取到的是最新一条");
        for (int i = 0; i < 500; ++i) history.Up(&text);
        check(text == "消息 50", "一直按上键停在最旧保留的那条（不会越界）");

        dchat::InputHistory fresh;
        fresh.Add("a");
        fresh.ResetBrowse();
        check(fresh.Up(&text) && text == "a", "重置浏览状态后仍能正常取历史");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
