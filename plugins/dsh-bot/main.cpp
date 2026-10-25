// dchat-dsh-bot 入口
//
//   dchat-dsh-bot --config dchat-dsh-bot.conf      跑起来
//   dchat-dsh-bot --check  --config …              只校验配置并打印摘要，不连服务器
//   dchat-dsh-bot --example                       打印带注释的示例配置
//
// 为什么单独有个 --check：配置写错的表现是"机器人上了线但不说话"，而配置文件
// 在服务器上、日志在另一个文件里，来回查很烦。先本地校验一遍能省掉大半。
#include <cstdio>
#include <cstring>
#include <string>

#include "bot.h"
#include "bot_config.h"

namespace {

void PrintUsage() {
    std::printf(
        "用法：dchat-dsh-bot [--config <文件>] [--check] [--example]\n"
        "\n"
        "  -c, --config <文件>   配置文件（默认 dchat-dsh-bot.conf）\n"
        "      --check           只校验配置并打印摘要，不连服务器\n"
        "      --example         打印带注释的示例配置\n"
        "  -h, --help            这份帮助\n");
}

const char* ReplyModeText(dchat::dshbot::ReplyMode mode) {
    switch (mode) {
        case dchat::dshbot::ReplyMode::All: return "all（谁都回）";
        case dchat::dshbot::ReplyMode::Whitelist: return "whitelist（只回名单里）";
        case dchat::dshbot::ReplyMode::Blacklist: return "blacklist（名单里不回）";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "dchat-dsh-bot.conf";
    bool checkOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--check") {
            checkOnly = true;
        } else if (arg == "--example") {
            std::printf("%s", dchat::dshbot::ExampleConfig().c_str());
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else if (arg == "--version") {
            std::printf("dchat-dsh-bot 1.0（DeepSeek Harness ↔ dchat 插件）\n");
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    dchat::dshbot::Config config;
    std::string error;
    if (!dchat::dshbot::LoadConfigFile(configPath, &config, &error)) {
        std::fprintf(stderr, "配置错误：%s\n", error.c_str());
        return 2;
    }

    if (checkOnly) {
        std::printf("配置文件：%s\n", configPath.c_str());
        std::printf("服务器：%s:%d（加密 %s）\n", config.host.c_str(), config.port,
                    config.encrypt.c_str());
        std::printf("机器人账号：%s\n", config.nick.c_str());
        std::printf("接入会话：");
        for (std::size_t i = 0; i < config.rooms.size(); ++i) {
            std::printf("%s%s", i == 0 ? "" : ", ", config.rooms[i].c_str());
        }
        std::printf("\n触发方式：%s\n", config.trigger.mentionOnly ? "@ 才回" : "每句都回");
        std::printf("回复范围：%s", ReplyModeText(config.reply.mode));
        if (!config.reply.nicks.empty()) {
            std::printf("（");
            for (std::size_t i = 0; i < config.reply.nicks.size(); ++i) {
                std::printf("%s%s", i == 0 ? "" : ", ", config.reply.nicks[i].c_str());
            }
            std::printf("）");
        }
        std::printf("\n分条：每条 ≤ %d 字，最多 %d 条，间隔 %d-%d ms\n", config.typing.maxChars,
                    config.typing.maxChunks, config.typing.minGapMs, config.typing.maxGapMs);
        std::printf("大脑：");
        for (const std::string& word : config.brain.command) std::printf("%s ", word.c_str());
        std::printf("\n权限模式：%s\n", config.brain.permissionMode.c_str());
        if (!config.brain.patch.empty()) std::printf("补丁：%s\n", config.brain.patch.c_str());
        std::printf("配置没问题。\n");
        return 0;
    }

    return dchat::dshbot::RunBot(config, configPath);
}
