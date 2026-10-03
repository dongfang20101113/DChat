#include "cli_render.h"

#include "chat_color.h"

namespace dchat {

RenderedLine RenderStatusLine(const std::string& text) {
    RenderedLine line;
    line.text = text;
    return line;
}

RenderedLine RenderChatMessage(const ChatMessage& message, bool colorEnabled) {
    RenderedLine out;
    if (!message.valid) {
        out.visible = false;
        return out;
    }

    switch (message.kind) {
        case ChatMessage::Kind::Say:
        case ChatMessage::Kind::Own: {
            // 有人 @ 我：整段"时间 + 昵称"加粗黄，正文照常
            if (message.mentionMe) out.text += kAnsiMention;
            out.text += "[" + message.time + "] <" + message.nick + "> ";
            if (message.mentionMe) out.text += AnsiReset();
            // 正文按色码切段上色。**色码解析和另外三端是同一份实现**
            // （client_core/chat_color.cpp），所以四端显示规则一致；
            // ANSI 序列的生成也复用那里的 AnsiForeground，不另写一份。
            for (const ColorSegment& segment :
                 ParseColorSegments(message.text, 0xDDDDDD, colorEnabled)) {
                if (segment.hasColor) out.text += AnsiForeground(segment.rgb);
                out.text += segment.text;
                if (segment.hasColor) out.text += AnsiReset();
            }
            return out;
        }
        case ChatMessage::Kind::Error:
            out.text = std::string(kAnsiError) + "[错误] " + message.text + AnsiReset();
            return out;
        case ChatMessage::Kind::System:
            out.text = std::string(kAnsiSystem) + "[系统] " + message.text + AnsiReset();
            return out;
        case ChatMessage::Kind::FileOffer:
            // 附件卡片：高亮一下，和普通发言区分开
            out.text = std::string(kAnsiMention) + message.text + AnsiReset();
            return out;
        case ChatMessage::Kind::Notice:
        default:
            out.text = message.text;
            return out;
    }
}

std::vector<std::string> WrapAnsi(const std::string& text, int width) {
    std::vector<std::string> lines;
    if (width <= 0) {
        lines.push_back(text);
        return lines;
    }
    std::string current;
    std::size_t visible = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b') {
            // ANSI 序列整段抄进当前行，且**不计宽度** ——
            // 从序列中间截断会让后面所有文字都用错颜色，是最难查的一类显示 bug。
            const std::size_t end = text.find('m', i);
            if (end == std::string::npos) break;
            current += text.substr(i, end - i + 1);
            i = end;
            continue;
        }
        current.push_back(text[i]);
        ++visible;
        if (visible >= static_cast<std::size_t>(width)) {
            lines.push_back(current);
            current.clear();
            visible = 0;
        }
    }
    if (!current.empty() || lines.empty()) lines.push_back(current);
    return lines;
}

}  // namespace dchat
