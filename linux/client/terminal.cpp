#include "terminal.h"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>

namespace dchat {

#ifndef _WIN32
struct Terminal::TermiosState {
    termios original{};
};
#endif

Terminal::~Terminal() { Restore(); }

bool Terminal::EnterRawMode() {
    isTerminal_ = ::isatty(STDIN_FILENO) != 0;
    if (!isTerminal_) return false;
#ifdef _WIN32
    return false;
#else
    saved_ = new TermiosState();
    if (::tcgetattr(STDIN_FILENO, &saved_->original) != 0) {
        delete saved_;
        saved_ = nullptr;
        return false;
    }
    termios raw = saved_->original;
    // 关掉行缓冲和回显：要一个键一个键地读（Tab 补全、方向键翻历史都得这样）。
    // ISIG 保留着，Ctrl+C 还能正常退出。
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        delete saved_;
        saved_ = nullptr;
        return false;
    }
    rawMode_ = true;
    return true;
#endif
}

void Terminal::Restore() {
#ifndef _WIN32
    if (rawMode_ && saved_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_->original);
    }
    delete saved_;
    saved_ = nullptr;
#endif
    rawMode_ = false;
}

int Terminal::Width() const {
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
    return 80;
}

void Terminal::Write(const std::string& text) {
    if (text.empty()) return;
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void Terminal::ClearLine() {
    // \r 回到行首，\x1b[K 清到行尾
    Write("\r\x1b[K");
}

void Terminal::PrintAbove(const std::string& text, const std::string& prompt,
                          const std::string& input, std::size_t cursor) {
    ClearLine();
    std::string out = text;
    if (out.empty() || out.back() != '\n') out += '\n';
    Write(out);
    RedrawInput(prompt, input, cursor);
}

void Terminal::RedrawInput(const std::string& prompt, const std::string& input,
                           std::size_t cursor) {
    // 先把整行擦掉再重画。为什么不做"只更新变化部分"的优化：中文一个字占两格、
    // ANSI 序列不算宽度，增量更新非常容易算错，重画整行最稳。
    ClearLine();
    std::string line = prompt + input;
    Write(line);
    // 光标退回到 cursor 处（注意：这里按字节算，中文会有偏差，
    // 所以只用于 ASCII 输入；中文编辑时按"格"数算在 UiLoop 里处理）
    const std::size_t tail = input.size() - cursor;
    if (tail > 0) {
        char buffer[32] = {0};
        std::snprintf(buffer, sizeof(buffer), "\x1b[%zuD", tail);
        Write(buffer);
    }
}

int Terminal::ReadKey(int timeoutMs) {
    if (pending_ >= 0) {
        const int key = pending_;
        pending_ = -1;
        return key;
    }
#ifndef _WIN32
    pollfd item{};
    item.fd = STDIN_FILENO;
    item.events = POLLIN;
    const int ready = ::poll(&item, 1, timeoutMs);
    if (ready <= 0) return 0;
    unsigned char ch = 0;
    const ssize_t got = ::read(STDIN_FILENO, &ch, 1);
    if (got != 1) return 0;
    // 方向键 / 功能键是 ESC 开头的多字节序列：等一小会儿把剩下的读进来，
    // 拼成一个"伪按键码"返回（\x1b[A = 上，\x1b[B = 下 ...）。
    if (ch == 0x1B) {
        std::string sequence;
        sequence.push_back(static_cast<char>(ch));
        while (sequence.size() < 8) {
            pollfd more{};
            more.fd = STDIN_FILENO;
            more.events = POLLIN;
            if (::poll(&more, 1, 15) <= 0) break;
            unsigned char next = 0;
            if (::read(STDIN_FILENO, &next, 1) != 1) break;
            sequence.push_back(static_cast<char>(next));
            if (next >= 'A' && next <= 'Z') break;
            if (next == '~') break;
        }
        if (sequence.size() == 1) return 0x1B;
        // 把序列编码成一个大于 0x100 的整数，调用方按整数比
        int code = 0x100;
        for (char item2 : sequence) code = code * 128 + static_cast<unsigned char>(item2);
        return code;
    }
    return ch;
#else
    return 0;
#endif
}

}  // namespace dchat
