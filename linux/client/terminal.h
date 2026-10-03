// 终端控制：原始模式、光标、清行，以及"历史区 + 固定输入行"的布局。
//
// 终端里没有真正的窗口，界面是"自己画的"：接收到的消息往上滚，输入行永远留在下面。
// 所以每次要输出消息，都得先把输入行擦掉、打印消息、再把输入行重画一遍。
#pragma once

#include <string>
#include <vector>

namespace dchat {

class Terminal {
public:
    Terminal() = default;
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /** 进原始模式（一个字符一个字符读，不等回车）。失败返回 false（例如不是终端）。 */
    bool EnterRawMode();

    /** 恢复原来的终端设置。析构时也会调，异常退出不至于把终端搞坏。 */
    void Restore();

    /** 终端宽度（拿不到时返回 80）。 */
    int Width() const;

    /** 输出一段文字（不换行），会自动处理 ANSI 序列。 */
    void Write(const std::string& text);

    /** 擦掉当前这一行。 */
    void ClearLine();

    /**
     * 把历史区刷新：先擦掉输入行，打印 text（可多行），再重画输入行。
     * `prompt` + `input` 就是当前输入框的内容。
     */
    void PrintAbove(const std::string& text, const std::string& prompt, const std::string& input,
                    std::size_t cursor);

    /** 只重画输入行（用户敲字时用）。 */
    void RedrawInput(const std::string& prompt, const std::string& input, std::size_t cursor);

    /** 读一个按键。返回 0 表示超过 timeoutMs 没有输入。 */
    int ReadKey(int timeoutMs);

    /** 取出并清空挂起的按键（用于一次读多个字节）。 */
    bool HasPendingKey() const { return pending_ >= 0; }

    /** 提示"还有内容在输入缓冲区里"（Tab 循环时用）。 */
    void SetPendingKey(int key) { pending_ = key; }

    bool IsTerminal() const { return isTerminal_; }

private:
    bool isTerminal_ = false;
    bool rawMode_ = false;
    int pending_ = -1;
#ifdef _WIN32
#else
    struct TermiosState;
    TermiosState* saved_ = nullptr;
#endif
};

}  // namespace dchat
