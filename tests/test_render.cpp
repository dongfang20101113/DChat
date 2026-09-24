// 显示规则单元测试：协议行解析、@提及、昵称配色、未读提示。不需要图形界面。
#include <cstdio>
#include <string>

#include "render.h"

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
    std::printf("== dchat render tests ==\n");

    {
        std::printf("[1] SAY 解析（气泡靠左还是靠右就靠这里）\n");
        dchat::SayInfo say;
        check(dchat::ParseSay("SAY alice 大家好", "bob", &say), "普通 SAY 能解析");
        check(say.nick == "alice" && say.text == "大家好", "昵称与正文分离");
        check(!say.own, "别人的消息不是自己的（气泡靠左）");
        check(say.time.empty(), "没有时间字段时 time 为空");

        check(dchat::ParseSay("SAY 21:05 alice 你好", "bob", &say), "带时间戳的 SAY 能解析");
        check(say.time == "21:05" && say.nick == "alice" && say.text == "你好",
              "时间/昵称/正文都正确");

        check(dchat::ParseSay("SAY 21:05 alice 我自己发的", "alice", &say), "自己的 SAY 能解析");
        check(say.own, "自己发的消息标记为 own（气泡靠右）");

        check(!dchat::ParseSay("SYS 21:05 系统消息", "alice", &say), "非 SAY 行不会被当成消息");
        check(!dchat::ParseSay("SAY 21:05", "alice", &say), "缺少昵称的 SAY 解析失败");
    }

    {
        std::printf("[2] @提及判定\n");
        check(dchat::MentionsNick("在吗 @alice", "alice"), "普通提及");
        check(dchat::MentionsNick("@alice 早", "alice"), "开头提及");
        check(dchat::MentionsNick("@alice，看这个", "alice"), "后接中文标点也算（中文不用空格分词）");
        check(dchat::MentionsNick("@alice你好", "alice"), "后接中文也算");
        check(dchat::MentionsNick("hello @ALICE hi", "alice"), "不区分大小写");
        check(dchat::MentionsNick("@小雷 在吗", "小雷"), "中文昵称也能被提及");
        check(!dchat::MentionsNick("@alicex 在吗", "alice"), "@alicex 不算提及 alice");
        check(!dchat::MentionsNick("a@alice.com", "alice"), "邮箱式写法不算");
        check(!dchat::MentionsNick("在吗", "alice"), "没有 @ 就不算");
        check(!dchat::MentionsNick("@alice", ""), "自己昵称为空时不算");
        check(dchat::MentionsAll("@all 开会了") && dchat::MentionsAll("@ALL 开会了"),
              "@all 命中且不区分大小写");
        check(!dchat::MentionsAll("@allx 开会了") && !dchat::MentionsAll("a@all.com"),
              "@allx 与邮箱写法不算 @all");
    }

    {
        std::printf("[3] 解析出的提及标记\n");
        dchat::SayInfo say;
        dchat::ParseSay("SAY 21:05 bob 在吗 @alice", "alice", &say);
        check(say.mention, "被 @ 的消息标记 mention（气泡高亮）");
        dchat::ParseSay("SAY 21:05 bob @all 开会", "alice", &say);
        check(say.mention, "@all 也算提及");
        dchat::ParseSay("SAY 21:05 bob 普通消息", "alice", &say);
        check(!say.mention, "普通消息不标记 mention");
        dchat::ParseSay("SAY 21:05 bob @alice 在吗", "carol", &say);
        check(!say.mention, "没有提到我时不标记（同一条消息旁观者视角）");
        dchat::ParseSay("SAY 21:05 alice 我自己 @alice", "alice", &say);
        check(say.own && !say.mention, "自己 @ 自己算自己的消息，不提醒自己");
        check(dchat::MentionsMe("SAY 21:05 bob @alice 在吗", "alice"), "MentionsMe 认出给我的消息");
        check(!dchat::MentionsMe("SYS 21:05 系统消息", "alice"), "系统消息不算给我的");
    }

    {
        std::printf("[4] 系统提示解析\n");
        dchat::NoticeInfo notice = dchat::ParseNotice("WELCOME 21:05 dchat Server");
        check(notice.text == "已连接到服务器 dchat Server" && notice.time == "21:05", "欢迎语");
        notice = dchat::ParseNotice("JOINED 21:06 alice");
        check(notice.text == "alice 加入了聊天室", "加入通知");
        notice = dchat::ParseNotice("LEFT 21:07 alice");
        check(notice.text == "alice 离开了聊天室", "离开通知");
        notice = dchat::ParseNotice("NAMES 21:08 alice, bob");
        check(notice.text == "在线成员：alice, bob", "在线名单");
        notice = dchat::ParseNotice("SYS 21:09 服务器维护");
        check(notice.text == "服务器维护" && !notice.isError, "系统消息");
        notice = dchat::ParseNotice("ANNOUNCE 21:09 服务器将在 10 分钟后维护");
        check(notice.isAnnouncement && notice.time == "21:09" &&
                  notice.text == "服务器将在 10 分钟后维护",
              "公告被识别为公告并保留时间与内容");
        notice = dchat::ParseNotice("ERROR 21:10 昵称已被占用");
        check(notice.text == "昵称已被占用" && notice.isError, "错误消息被标记为 error");
        notice = dchat::ParseNotice("LOGGEDIN 21:10 alice");
        check(notice.isNotice && !notice.isError && notice.time == "21:10" &&
                  notice.text == "alice",
              "登录成功提示被识别为通知并带出用户名");
        notice = dchat::ParseNotice("PONG 21:11");
        check(notice.text == "服务器回应正常（PONG）", "PONG");
        notice = dchat::ParseNotice("WHAT 21:12 未知");
        check(!notice.isNotice && notice.text == "WHAT 21:12 未知", "未知命令按原文显示");
        notice = dchat::ParseNotice("   ");
        check(notice.text.empty(), "空行不产生提示");
        notice = dchat::ParseNotice("SYS 服务器维护");
        check(notice.time.empty() && notice.text == "服务器维护", "没有时间字段也能解析");
    }

    {
        std::printf("[5] 昵称配色稳定性\n");
        check(dchat::NickColorIndex("alice") == dchat::NickColorIndex("alice"),
              "同一个昵称始终是同一个颜色");
        bool inRange = true;
        for (const char* nick : {"alice", "bob", "小雷", "user123", ""}) {
            const int index = dchat::NickColorIndex(nick);
            if (index < 0 || index >= dchat::kNickPaletteSize) inRange = false;
        }
        check(inRange, "颜色下标始终落在调色板范围内");
        int distinct = 0;
        for (const char* nick : {"bob", "carol", "dave", "erin", "frank"})
            if (dchat::NickColorIndex(nick) != dchat::NickColorIndex("alice")) ++distinct;
        check(distinct >= 3, "不同昵称多数会分到不同颜色");
    }

    {
        std::printf("[6] 未读提示规则\n");
        check(dchat::ShouldCountUnread(false), "窗口不在前台时计入未读");
        check(!dchat::ShouldCountUnread(true), "窗口在前台时不计未读");
        check(dchat::ShouldFlash(false, true), "不在前台 + 有人叫我 → 闪任务栏");
        check(!dchat::ShouldFlash(false, false), "不在前台但只是普通消息 → 不闪");
        check(!dchat::ShouldFlash(true, true), "已经在看就不必闪");
        check(dchat::FormatUnreadTitle("dchat 客户端", 0) == "dchat 客户端", "无未读时标题不变");
        check(dchat::FormatUnreadTitle("dchat 客户端", 3) == "【3】dchat 客户端",
              "有未读时标题前置计数");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
