// dchat × DeepSeek Harness 机器人插件的单元测试
//
// 测的是"错了也不会报错、只会表现得很怪"的那几块：
//   * 配置解析     —— 静默忽略一个键 = "我明明配了却没生效"的悬案
//   * 分条         —— 切错就是刷屏或者把字切坏（UTF-8 半截）
//   * @ 判定和白黑名单 —— 判错就是"在该回的时候装死"或者"不该回的时候插嘴"
//   * NDJSON 解析  —— 解错就是群里出现一片 "\u4f60\u597d"
//   * 命令模板     —— {session} 展开错就是每次调用都失败，而日志只说"出错了"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bot_chunk.h"
#include "bot_config.h"
#include "bot_event.h"
#include "bot_file.h"
#include "bot_http.h"
#include "bot_json.h"
#include "bot_policy.h"
#include "bot_process.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [失败] %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace dchat::dshbot;
    namespace fs = std::filesystem;

    // =====================================================================
    std::printf("[1] 配置解析\n");
    {
        std::string error;
        Config config;
        const std::string text =
            "server.host = chat.example.com\n"
            "server.port = 6000\n"
            "bot.nick = 小d\n"
            "bot.password = abc12345\n"
            "rooms = hall, test ,dev\n"
            "reply.mode = blacklist\n"
            "reply.list = 坏人,spammer\n"
            "typing.maxchars = 40\n"
            "brain.command = dsh headless --json {session} -\n"
            "brain.prompttemplate = 群里 {nick} 说：{text}\\n请回答\n";
        Check(ParseConfig(text, &config, &error), "基本配置能解析");
        Check(config.host == "chat.example.com", "主机");
        Check(config.port == 6000, "端口");
        Check(config.nick == "小d", "昵称");
        Check(config.rooms.size() == 3 && config.rooms[1] == "test", "会话列表按逗号切且去空白");
        Check(config.reply.mode == ReplyMode::Blacklist, "黑名单模式");
        Check(config.reply.nicks.size() == 2, "名单两项");
        Check(config.typing.maxChars == 40, "单条上限");
        Check(config.brain.command.size() == 5 && config.brain.command[0] == "dsh",
              "命令模板按空格切");
        Check(config.brain.command[3] == "{session}", "{session} 原样保留到调用时再展开");
        Check(config.brain.promptTemplate.find('\n') != std::string::npos,
              "\\n 转义还原成真换行");
        Check(config.brain.permissionMode == "read-only", "权限模式默认只读");
    }
    {
        // 示例配置必须能被自己解析 —— 否则 --example 生成的文件一跑就报错，
        // 这是最容易在"改默认值"时踩到的地方。
        std::string error;
        Config config;
        Check(ParseConfig(ExampleConfig(), &config, &error), "--example 的输出能被自己解析");
        Check(!config.rooms.empty(), "示例里有会话");
        Check(config.nick == "小d", "示例昵称");
    }
    {
        std::string error;
        Config config;
        Check(!ParseConfig("bot.nick = a\nbot.password = b\nrooms = hall\nnonsense = 1\n", &config,
                           &error),
              "未知键直接报错（不静默忽略）");
        Check(error.find("第 4 行") != std::string::npos, "错误信息带行号");
        Check(!ParseConfig("bot.nick = a\nbot.password = b\nrooms = hall\nserver.port = abc\n",
                           &config, &error),
              "端口不是数字要报错");
        Check(!ParseConfig("bot.nick = a\nbot.password = b\n", &config, &error),
              "没有 rooms 要报错");
        Check(!ParseConfig("bot.nick = a\nrooms = hall\n", &config, &error),
              "没有密码要报错");
        Check(!ParseConfig("bot.nick = a\nbot.password = b\nrooms = hall\nreply.mode = whitelist\n",
                           &config, &error),
              "白名单模式但名单为空要报错");
        Check(!ParseConfig("bot.nick = a\nbot.password = b\nrooms = hall\nbrain.command =\n",
                           &config, &error),
              "空命令要报错");
        Check(!ParseConfig("bot.nick = a\nbot.password = b\nrooms = hall\nreply.mode = sometimes\n",
                           &config, &error),
              "非法 reply.mode 要报错");
    }
    {
        // 带引号的命令：路径里有空格时必须能正确切开
        std::string error;
        Config config;
        Check(ParseConfig(
                  "bot.nick = a\nbot.password = b\nrooms = hall\n"
                  "brain.command = \"C:/Program Files/dsh/dsh.cmd\" headless --json -\n",
                  &config, &error),
              "带引号的命令能解析");
        Check(config.brain.command.size() == 4, "引号里的空格不切分");
        Check(config.brain.command[0] == "C:/Program Files/dsh/dsh.cmd", "引号内容原样保留");
    }

    // =====================================================================
    std::printf("[2] 分条（回答切成一条条聊天消息）\n");
    {
        Config config;
        config.typing.maxChars = 40;
        config.typing.minChars = 1;
        config.typing.maxChunks = 24;
        config.typing.stripMarkdown = false;
        const std::vector<std::string> chunks =
            SplitForChat("第一句。第二句！第三句？", config.typing);
        Check(chunks.size() == 3, "按句末标点切成三条");
        Check(chunks[0] == "第一句。", "第一条保留标点");
        Check(chunks[2] == "第三句？", "最后一条");
    }
    {
        Config config;
        config.typing.maxChars = 20;
        config.typing.minChars = 1;
        config.typing.stripMarkdown = false;
        // 一句话 60 个字没有标点：必须在长度上限附近硬切，且每段都不超
        const std::string longSentence =
            "这是一句非常非常长的话完全没有标点符号所以只能靠长度来切开它不然会变成一堵墙";
        const std::vector<std::string> chunks = SplitForChat(longSentence, config.typing);
        bool allWithinLimit = true;
        for (const std::string& chunk : chunks) {
            if (CodePointCount(chunk) > 20) allWithinLimit = false;
        }
        Check(chunks.size() > 1, "长句被切开");
        Check(allWithinLimit, "每一条都在 20 字以内");
        std::string rejoined;
        for (const std::string& chunk : chunks) rejoined += chunk;
        Check(rejoined == longSentence, "拼回去和原文一模一样（没有丢字/多字）");
    }
    {
        Config config;
        config.typing.maxChars = 40;
        config.typing.minChars = 10;
        config.typing.stripMarkdown = false;
        const std::vector<std::string> chunks = SplitForChat("嗯。好的我知道了你说的这个情况。", config.typing);
        Check(chunks.size() == 1, "太短的一句并到下一句，不单独刷屏");
    }
    {
        Config config;
        config.typing.stripMarkdown = true;
        const std::vector<std::string> chunks =
            SplitForChat("## 结论\n**今天**天气不错。", config.typing);
        std::string joined;
        for (const std::string& chunk : chunks) joined += chunk;
        Check(joined.find("**") == std::string::npos, "去掉 ** 记号");
        Check(joined.find("##") == std::string::npos, "去掉标题记号");
        Check(joined.find("今天天气不错") != std::string::npos, "正文没被吃掉");
    }
    {
        Config config;
        config.typing.maxChars = 20;
        config.typing.minChars = 1;
        config.typing.maxChunks = 3;
        config.typing.stripMarkdown = false;
        std::string text;
        for (int i = 0; i < 12; ++i) text += "第" + std::to_string(i) + "句。";
        const std::vector<std::string> chunks = SplitForChat(text, config.typing);
        Check(chunks.size() == 3, "条数被 maxChunks 卡住");
        Check(chunks.back().find("…") != std::string::npos, "被截断的最后一条有省略号");
    }
    {
        // 边界：空输入、全空白、只有换行
        Config config;
        Check(SplitForChat("", config.typing).empty(), "空输入没有分条");
        Check(SplitForChat("   \n\n  ", config.typing).empty(), "全空白没有分条");
    }
    {
        // UTF-8：截断不能切出半个汉字
        const std::string chinese = "中文测试内容";
        Check(CodePointCount(chinese) == 6, "中文按码点数长度");
        const std::string cut = TruncateCodePoints(chinese, 3);
        Check(cut == "中文测", "按码点截断");
        Check(CodePointCount(TruncateCodePoints("😀😀😀", 2)) == 2, "四字节 emoji 也能正确计数/截断");
    }
    {
        Config config;
        config.typing.minGapMs = 300;
        config.typing.perCharMs = 50;
        config.typing.maxGapMs = 1200;
        Check(ChunkDelayMs("五个字的话", config.typing) == 550, "间隔 = 基础 + 每字");
        Check(ChunkDelayMs("", config.typing) == 300, "空串按基础间隔");
        std::string longText;
        for (int i = 0; i < 200; ++i) longText += "字";
        Check(ChunkDelayMs(longText, config.typing) == 1200, "超长按上限截住");
    }

    // =====================================================================
    std::printf("[3] @ 判定\n");
    {
        TriggerConfig trigger;
        trigger.mentionOnly = true;
        TriggerMatch match = MatchTrigger("@小d 你好呀", "小d", trigger);
        Check(match.matched, "@主名命中");
        Check(match.text == "你好呀", "去掉 @ 之后剩正文");
        Check(MatchTrigger("@小D 你好", "小d", trigger).matched, "大小写不敏感");
        Check(!MatchTrigger("@小dd 你好", "小d", trigger).matched, "名字后面还连着字符不算");
        Check(!MatchTrigger("abc@小d 你好", "小d", trigger).matched, "前面紧挨字母不算");
        Check(!MatchTrigger("今天天气不错", "小d", trigger).matched, "没 @ 不命中");
        Check(MatchTrigger("@小d 你好你好", "小d", trigger).text == "你好你好", "空格只吃掉一个");
        TriggerMatch bare = MatchTrigger("@小d", "小d", trigger);
        Check(bare.matched && !bare.text.empty(), "只 @ 一声也给一句能接的话");
        trigger.aliases.push_back("ds");
        Check(MatchTrigger("@ds 在吗", "小d", trigger).matched, "别名命中");
        Check(MatchTrigger("@ds 在吗", "小d", trigger).text == "在吗", "别名也被去掉");
        TriggerConfig chatty;
        chatty.mentionOnly = false;
        const TriggerMatch every = MatchTrigger("随便说一句", "小d", chatty);
        Check(every.matched && every.text == "随便说一句", "mentionOnly=false 时每句都算");
    }

    // =====================================================================
    std::printf("[4] 白名单 / 黑名单\n");
    {
        ReplyPolicy policy;
        Check(ReplyAllowed(policy, "任何人"), "all 模式谁都回");
        policy.mode = ReplyMode::Whitelist;
        policy.nicks = {"Alice", "bob"};
        Check(ReplyAllowed(policy, "alice"), "白名单命中（大小写不敏感）");
        Check(!ReplyAllowed(policy, "carol"), "白名单外的不回");
        policy.mode = ReplyMode::Blacklist;
        Check(!ReplyAllowed(policy, "BOB"), "黑名单命中的不回");
        Check(ReplyAllowed(policy, "carol"), "黑名单外的回");
    }

    // =====================================================================
    std::printf("[5] NDJSON 事件解析\n");
    {
        DshEvent event;
        Check(ParseDshEvent("{\"type\":\"session\",\"sessionId\":\"session-42\",\"cwd\":\"D:\\\\codes\"}",
                            &event),
              "session 事件");
        Check(event.sessionId == "session-42", "会话 id");
        Check(ParseDshEvent("{\"type\":\"text\",\"text\":\"你好\\n世界\"}", &event), "text 事件");
        Check(event.text == "你好\n世界", "\\n 转义还原");
        Check(ParseDshEvent("{\"type\":\"text\",\"text\":\"\\u4f60\\u597d\"}", &event),
              "\\u 转义");
        Check(event.text == "你好", "\\uXXXX 解成中文");
        Check(ParseDshEvent(
                  "{\"type\":\"status\",\"phase\":\"step_end\",\"usage\":{\"inputTokens\":10,"
                  "\"outputTokens\":2}}",
                  &event),
              "带嵌套对象的事件");
        Check(event.phase == "step_end", "阶段名");
        Check(ParseDshEvent("{\"type\":\"thinking\",\"text\":\"\"}", &event), "thinking 事件");
        Check(!ParseDshEvent("not json at all", &event), "非 JSON 行被拒绝");
        Check(!ParseDshEvent("{\"no_type\":1}", &event), "没有 type 的行被拒绝");
        Check(ParseDshEvent("{\"type\":\"text\",\"text\":\"a\\\"b\\\\c\"}", &event), "转义引号");
        Check(event.text == "a\"b\\c", "引号和反斜杠正确还原");
    }

    // =====================================================================
    std::printf("[6] 命令模板展开\n");
    {
        const std::vector<std::string> command = {"dsh", "headless", "--json", "{session}", "-"};
        const std::vector<std::string> withSession = BuildArgv(command, "提问", "session-1");
        Check(withSession.size() == 6, "有会话时插入两个参数");
        Check(withSession[3] == "--session-id" && withSession[4] == "session-1", "--session-id 参数");
        const std::vector<std::string> without = BuildArgv(command, "提问", "");
        Check(without.size() == 4, "没会话时 {session} 整个消失");
        for (const std::string& arg : without) {
            Check(arg != "--session-id", "不会留下空的 --session-id");
        }
        const std::vector<std::string> inlinePrompt =
            BuildArgv({"tool", "--task", "{prompt}"}, "你好", "");
        Check(inlinePrompt.size() == 3 && inlinePrompt[2] == "你好", "{prompt} 展开成一个参数");
    }

    // =====================================================================
    std::printf("[7] 配置写回（管理界面保存时用）\n");
    {
        const std::string original =
            "# 这是注释，保存时不能丢\n"
            "bot.nick = 小d\n"
            "bot.password = abc12345\n"
            "server.host = 127.0.0.1\n"
            "rooms = grp1\n"
            "reply.mode = all\n";
        const std::string updated = UpdateConfigText(
            original, {{"rooms", "grp1,grp2"}, {"reply.mode", "blacklist"}, {"reply.list", "坏人"}});
        Check(updated.find("# 这是注释，保存时不能丢") != std::string::npos, "注释被保留");
        Check(updated.find("rooms = grp1,grp2") != std::string::npos, "已有的键被就地替换");
        Check(updated.find("reply.mode = blacklist") != std::string::npos, "模式写进去了");
        Check(updated.find("reply.list = 坏人") != std::string::npos, "原来没有的键追加到末尾");
        Check(updated.find("server.host = 127.0.0.1") != std::string::npos, "没改的键不动");

        // 写出去的东西必须还能被解析回来（转义、逗号分隔这些细节最容易在这里出错）
        Config config;
        std::string error;
        Check(ParseConfig(updated, &config, &error), "写回的配置能重新解析");
        Check(config.rooms.size() == 2 && config.rooms[1] == "grp2", "rooms 写回后是两项");
        Check(config.reply.mode == ReplyMode::Blacklist, "模式写回后是 blacklist");
        Check(config.reply.nicks.size() == 1 && config.reply.nicks[0] == "坏人", "名单写回");

        // 以后版本新增、当前版本还不认识的键：写回时**一个字都不能动**
        // （不能因为"我不认识"就把它删掉——那是在替用户改配置）
        const std::string future = UpdateConfigText("future.key = 保留\nrooms = a\n",
                                                    {{"rooms", "b"}});
        Check(future.find("future.key = 保留") != std::string::npos, "不认识的键原样保留");
    }
    {
        // 带换行 / 反斜杠的值：换行必须由调用方先转义（否则配置结构直接坏掉），
        // 而路径**不转义**——否则读回来时 `D:\new` 里的 `\n` 会变成换行。
        const std::string updated = UpdateConfigText(
            "bot.nick = a\n",
            {{"reply.acktext", EscapeConfigValue("第一行\n第二行")}, {"brain.cwd", "D:\\dsh"}});
        Check(updated.find("reply.acktext = 第一行\\n第二行") != std::string::npos,
              "换行写成 \\n 转义");
        Check(updated.find("brain.cwd = D:\\dsh") != std::string::npos, "路径原样写");
        Config config;
        std::string error;
        Check(ParseConfig(updated + "bot.password = b\nrooms = hall\n", &config, &error),
              "写回的配置能解析");
        Check(config.ackText == "第一行\n第二行", "转义往返一致");
        Check(config.brain.cwd == "D:\\dsh", "路径往返一致");
    }

    // =====================================================================
    std::printf("[8] JSON 输入输出\n");
    {
        Check(JsonQuote("普通") == "\"普通\"", "中文原样");
        Check(JsonQuote("a\"b") == "\"a\\\"b\"", "引号转义");
        Check(JsonQuote("a\nb") == "\"a\\nb\"", "换行转义");
        const std::vector<std::string> items = {"a", "b"};
        Check(JsonQuoteArray(items) == "[\"a\",\"b\"]", "数组拼接");

        FlatJson parsed;
        std::string error;
        Check(ParseFlatJson(
                  "{\"rooms\":[\"a\",\"b\"],\"mentionOnly\":false,\"maxChars\":30,"
                  "\"ackText\":\"hi\\n\"}",
                  &parsed, &error),
              "扁平对象能解析");
        std::vector<std::string> rooms;
        Check(parsed.GetArray("rooms", &rooms) && rooms.size() == 2, "数组字段");
        Check(parsed.GetBool("mentionOnly", true) == false, "布尔字段");
        Check(parsed.GetInt("maxChars", 0) == 30, "数字字段");
        Check(parsed.GetString("ackText", "") == "hi\n", "字符串转义还原");
        Check(parsed.GetString("missing", "默认") == "默认", "缺字段用默认值");
        Check(!ParseFlatJson("[]", &parsed, &error), "非对象被拒绝");
        Check(!ParseFlatJson("{\"a\":}", &parsed, &error), "缺值被拒绝");
        Check(ParseFlatJson("{\"a\":[]}", &parsed, &error), "空数组合法");
    }

    // =====================================================================
    std::printf("[9] HTTP 请求解析（管理界面的入口）\n");
    {
        const std::string raw =
            "POST /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Dchat-Bot: 1\r\n"
            "Content-Length: 10\r\n\r\n{\"a\":true}";
        HttpRequest request;
        std::size_t total = 0;
        Check(ParseHttpRequest(raw, &request, &total) == 1, "完整请求解析成功");
        Check(request.method == "POST", "方法");
        Check(request.path == "/api/config", "路径");
        Check(request.headers["x-dchat-bot"] == "1", "头部名转小写、值去掉空格");
        Check(request.body == "{\"a\":true}", "请求体");
        Check(total == raw.size(), "总长度");

        Check(ParseHttpRequest("GET / HTTP/1.1\r\n", &request, &total) == 0, "头部不全要求继续读");
        Check(ParseHttpRequest("BOGUS / HTTP/1.1\r\n\r\n", &request, &total) == -1, "非法方法被拒绝");

        const std::string withQuery =
            "GET /api/state?x=1&q=%E4%BD%A0%E5%A5%BD HTTP/1.1\r\n\r\n";
        Check(ParseHttpRequest(withQuery, &request, &total) == 1, "带查询串");
        Check(request.path == "/api/state", "路径不含查询串");
        Check(QueryParam(request.query, "q") == "你好", "查询串百分号解码");
        Check(QueryParam(request.query, "none").empty(), "不存在的参数返回空");
    }

    // =====================================================================
    std::printf("[10] 发文件指令与路径约束\n");
    {
        const FileDirective one = ExtractFileDirectives("给你看看\n[[file: a.txt]]");
        Check(one.files.size() == 1 && one.files[0] == "a.txt", "抠出走单独一行的发文件指令");
        Check(one.text == "给你看看", "指令被删掉，正文留下");

        const FileDirective inlineOne = ExtractFileDirectives("文件在这 [[file: b/c.txt]] 看吧");
        Check(inlineOne.files.size() == 1 && inlineOne.files[0] == "b/c.txt", "写在一行中间也能认");
        Check(inlineOne.text.find("文件在这") != std::string::npos &&
                  inlineOne.text.find("看吧") != std::string::npos,
              "指令两侧的文字都保留");

        const FileDirective many = ExtractFileDirectives("[[file: a]][[file: b]]");
        Check(many.files.size() == 2, "一条回答里可以有多个");

        const FileDirective broken = ExtractFileDirectives("没闭合 [[file: a.txt");
        Check(broken.files.empty(), "没闭合的不算指令");
        Check(broken.text.find("[[file:") != std::string::npos, "没闭合的原样留在正文里");

        const FileDirective none = ExtractFileDirectives("普通的一句话");
        Check(none.files.empty() && none.text == "普通的一句话", "没有指令时正文不动");

        const FileDirective onlyFile = ExtractFileDirectives("[[file: a.txt]]");
        Check(onlyFile.files.size() == 1 && onlyFile.text.empty(), "只发文件时正文为空");
    }
    {
        // 路径约束：只允许发工作目录里的文件
        std::error_code code;
        const fs::path root = fs::temp_directory_path(code) / "dchat-bot-test";
        fs::create_directories(root / "sub", code);
        std::ofstream(root / "a.txt") << "hello";
        std::ofstream(root / "sub" / "b.txt") << "world";
        fs::create_directories(root.parent_path() / "dchat-bot-test-outside", code);
        std::ofstream(root.parent_path() / "dchat-bot-test-outside" / "x.txt") << "secret";

        const std::string rootText = root.string();
        Check(!ResolveInsideWorkspace(rootText, "a.txt").empty(), "相对路径能解析");
        Check(!ResolveInsideWorkspace(rootText, "sub/b.txt").empty(), "子目录能解析");
        Check(ResolveInsideWorkspace(rootText, "../dchat-bot-test-outside/x.txt").empty(),
              "../ 越界被拒");
        Check(ResolveInsideWorkspace(rootText, "a.txt").find(rootText) == 0, "解析结果在工作目录里");
        Check(ResolveInsideWorkspace(rootText, "missing.txt").empty(), "不存在的文件被拒");
        Check(ResolveInsideWorkspace(rootText, "sub").empty(), "目录被拒");
        Check(IsInsideDirectory(rootText, rootText) == false, "目录本身不算在里面");
        Check(IsInsideDirectory(rootText, (root / "sub" / "b.txt").string()), "子目录里的文件算");
        Check(!IsInsideDirectory(rootText, rootText + "2/a.txt"), "work2 不算 work 里面");

        // 额外只读目录：发文件时多查几个根，但**改文件**仍然只有工作目录
        const fs::path outside = root.parent_path() / "dchat-bot-test-outside";
        Check(ResolveInsideWorkspace(rootText, (outside / "x.txt").string()).empty(),
              "不在工作目录里的文件，改文件那条路拒绝");
        Check(!ResolveSendablePath({rootText, outside.string()}, (outside / "x.txt").string()).empty(),
              "但配了只读目录之后可以发出去");
        Check(ResolveSendablePath({rootText}, (outside / "x.txt").string()).empty(),
              "没配只读目录时发不出去");

        fs::remove_all(root, code);
        fs::remove_all(root.parent_path() / "dchat-bot-test-outside", code);
    }

    // =====================================================================
    std::printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
