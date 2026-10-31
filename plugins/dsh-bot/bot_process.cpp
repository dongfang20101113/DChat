#include "bot_process.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace dchat {
namespace dshbot {

namespace {

/** 把缓冲里的完整行交给回调，返回 false 表示调用方叫停。 */
bool DrainLines(std::string* buffer, const std::function<bool(const std::string&)>& onLine) {
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = buffer->find('\n', start);
        if (newline == std::string::npos) break;
        std::string line = buffer->substr(start, newline - start);
        start = newline + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!onLine(line)) {
            buffer->erase(0, start);
            return false;
        }
    }
    buffer->erase(0, start);
    return true;
}

/**
 * 子进程退出时把缓冲区里**没有换行结尾**的最后一段也交出去。
 *
 * 为什么需要：`DrainLines` 只认完整的行。curl 拿一个 JSON 响应时正文后面没有
 * 换行，最后那段就永远留在缓冲里 —— 表现为"请求明明成功了，拿回来的却是空串"
 * （api 模式第一次真机联调就是这么卡住的）。
 */
bool FlushTail(std::string* buffer, const std::function<bool(const std::string&)>& onLine) {
    if (buffer->empty()) return true;
    std::string line = *buffer;
    buffer->clear();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return onLine(line);
}

void AppendTail(std::string* tail, const char* data, std::size_t size) {
    const std::size_t kMaxTail = 4096;
    tail->append(data, size);
    if (tail->size() > kMaxTail) tail->erase(0, tail->size() - kMaxTail);
}

}  // namespace

std::vector<std::string> BuildArgv(const std::vector<std::string>& command,
                                   const std::string& prompt, const std::string& session) {
    std::vector<std::string> argv;
    for (const std::string& token : command) {
        if (token == "{prompt}") {
            argv.push_back(prompt);
            continue;
        }
        if (token == "{session}") {
            // 没有会话 id 时**整个消失**，不能留一个空的 --session-id（dsh 会当成
            // "adopt 名为空串的会话"直接报错，而这个错误看起来像插件坏了）
            if (!session.empty()) {
                argv.push_back("--session-id");
                argv.push_back(session);
            }
            continue;
        }
        argv.push_back(token);
    }
    return argv;
}

#ifdef _WIN32

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::wstring QuoteArgument(const std::wstring& arg, bool force) {
    if (!force && !arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

bool HasExeExtension(const std::string& path) {
    if (path.size() < 4) return false;
    std::string tail = path.substr(path.size() - 4);
    for (char& ch : tail) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return tail == ".exe";
}

std::wstring BuildEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::wstring block;
    LPWCH current = GetEnvironmentStringsW();
    if (current != nullptr) {
        for (LPWCH p = current; *p != L'\0'; p += std::wcslen(p) + 1) {
            const std::wstring entry = p;
            const std::size_t equals = entry.find(L'=');
            bool skip = false;
            if (equals != std::wstring::npos && equals > 0) {
                const std::wstring key = entry.substr(0, equals);
                for (const auto& pair : overrides) {
                    if (_wcsicmp(key.c_str(), Widen(pair.first).c_str()) == 0) {
                        skip = true;
                        break;
                    }
                }
            }
            if (skip || entry.empty() || entry[0] == L'=') continue;  // 跳过隐藏的驱动器变量
            block += entry;
            block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(current);
    }
    for (const auto& pair : overrides) {
        block += Widen(pair.first);
        block.push_back(L'=');
        block += Widen(pair.second);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

bool RunBrain(const ProcessSpec& spec, const std::string& prompt,
              const std::function<bool(const std::string&)>& onLine, BrainRunResult* result,
              std::string* error) {
    if (result == nullptr) return false;
    *result = BrainRunResult();
    if (spec.argv.empty()) {
        if (error != nullptr) *error = "brain.command 是空的";
        return false;
    }

    // 命令行：.cmd/.bat 必须经过 cmd.exe 转发，否则 CreateProcess 只认 .exe，
    // 而 `dsh` 在 PATH 里正是一个 .cmd（见 DSH 的 cli/bin/dsh.cmd）。
    //
    // cmd 的引号规则是个坑：`cmd /s /c` 会**无条件剥掉首尾各一个引号**，
    // 所以要写成 `""C:\路径\x.cmd" 参数…"` —— 开头一个引号给 /s 剥，紧跟的
    // 那对引号是程序路径自己的。少一个引号，cmd 会把整串当成一个命令名去找，
    // 报"不是内部或外部命令"（这个错误消息完全没提引号，很容易查错方向）。
    const bool viaCmd = !HasExeExtension(spec.argv[0]);
    std::wstring inner;
    for (std::size_t i = 0; i < spec.argv.size(); ++i) {
        if (i > 0) inner.push_back(L' ');
        // 走 cmd 时程序路径**无论如何都要加引号**，否则上面那条 /s 规则就不成立
        inner += QuoteArgument(Widen(spec.argv[i]), viaCmd && i == 0);
    }
    std::wstring commandLine = inner;
    if (viaCmd) commandLine = L"cmd.exe /d /s /c \"" + inner + L"\"";

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    auto cleanup = [&]() {
        if (stdinRead) CloseHandle(stdinRead);
        if (stdinWrite) CloseHandle(stdinWrite);
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        if (process) CloseHandle(process);
        if (thread) CloseHandle(thread);
    };

    if (!CreatePipe(&stdinRead, &stdinWrite, &attributes, 0) ||
        !CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
        if (error != nullptr) *error = "创建管道失败";
        cleanup();
        return false;
    }
    // 父进程这一侧不能被子进程继承
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;

    const std::wstring environment = BuildEnvironmentBlock(spec.env);
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');
    const std::wstring workingDir = Widen(spec.cwd);

    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   const_cast<wchar_t*>(environment.c_str()),
                                   workingDir.empty() ? nullptr : workingDir.c_str(), &startup,
                                   &info);
    if (!ok) {
        if (error != nullptr) {
            *error = "起不了子进程（错误码 " + std::to_string(GetLastError()) + "）：" + spec.argv[0];
        }
        cleanup();
        return false;
    }
    process = info.hProcess;
    thread = info.hThread;
    // 子进程已经拿到了自己的句柄副本，父进程把子进程那一端关掉，否则读不到 EOF
    CloseHandle(stdinRead);
    stdinRead = nullptr;
    CloseHandle(stdoutWrite);
    stdoutWrite = nullptr;
    CloseHandle(stderrWrite);
    stderrWrite = nullptr;
    result->started = true;

    if (!prompt.empty()) {
        DWORD written = 0;
        WriteFile(stdinWrite, prompt.data(), static_cast<DWORD>(prompt.size()), &written, nullptr);
    }
    CloseHandle(stdinWrite);
    stdinWrite = nullptr;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeoutMs);
    std::string outBuffer;
    std::string errBuffer;
    bool stop = false;
    bool childDone = false;
    while (true) {
        char chunk[4096];
        bool gotAnything = false;
        DWORD available = 0;
        for (int which = 0; which < 2 && !stop; ++which) {
            HANDLE readEnd = (which == 0) ? stdoutRead : stderrRead;
            if (readEnd == nullptr) continue;
            if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) {
                CloseHandle(readEnd);
                if (which == 0) {
                    stdoutRead = nullptr;
                } else {
                    stderrRead = nullptr;
                }
                continue;
            }
            if (available == 0) continue;
            DWORD got = 0;
            const DWORD want = available > sizeof(chunk) ? sizeof(chunk) : available;
            if (!ReadFile(readEnd, chunk, want, &got, nullptr) || got == 0) continue;
            gotAnything = true;
            if (which == 0) {
                outBuffer.append(chunk, got);
                if (!DrainLines(&outBuffer, onLine)) stop = true;
            } else {
                AppendTail(&errBuffer, chunk, got);
            }
        }
        if (stop) {
            TerminateProcess(process, 1);
            break;
        }
        const DWORD wait = WaitForSingleObject(process, 0);
        if (wait == WAIT_OBJECT_0) {
            childDone = true;
            if (!gotAnything) break;  // 再读一轮也可能还有尾包
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result->timedOut = true;
            result->error = "子进程超时（" + std::to_string(spec.timeoutMs) + " ms）";
            TerminateProcess(process, 1);
            break;
        }
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - std::chrono::steady_clock::now())
                                              .count());
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20, std::max(1, left))));
    }

    WaitForSingleObject(process, 5000);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process, &exitCode)) result->exitCode = static_cast<int>(exitCode);
    if (!outBuffer.empty()) {
        DrainLines(&outBuffer, onLine);
        FlushTail(&outBuffer, onLine);   // 最后一段可能没有换行（curl 的 JSON 响应就是这样）
    }
    result->stderrTail = errBuffer;
    (void)childDone;
    cleanup();
    return true;
}

#else  // POSIX

namespace {

/** 在 PATH 里找可执行文件（execve 不搜 PATH，而 `dsh` 通常就是个 PATH 里的脚本）。 */
bool ResolveExecutable(const std::string& name, std::string* out) {
    if (name.find('/') != std::string::npos) {
        if (access(name.c_str(), X_OK) == 0) {
            *out = name;
            return true;
        }
        return false;
    }
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string text = path;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        const std::string dir =
            (colon == std::string::npos) ? text.substr(start) : text.substr(start, colon - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) {
                *out = candidate;
                return true;
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

bool WriteAllFd(int fd, const std::string& text) {
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

/** 环境：父进程的 environ + 覆盖项。在 fork 之前就拼好，子进程里只做 execve。 */
std::vector<std::string> BuildEnvironment(const std::vector<std::pair<std::string, std::string>>& env) {
    std::vector<std::string> out;
    // environ 由 <unistd.h> 在**全局作用域**声明。这里必须写 ::environ ——
    // 在命名空间里写 `extern char** environ;` 会声明出一个新的、没人定义过的符号，
    // Linux 上链接期才发现（g++ 甚至只给一条 "undefined-internal" 警告）。
    for (char** p = ::environ; p != nullptr && *p != nullptr; ++p) {
        std::string entry = *p;
        const std::size_t equals = entry.find('=');
        bool skip = false;
        if (equals != std::string::npos && equals > 0) {
            const std::string key = entry.substr(0, equals);
            for (const auto& pair : env) {
                if (pair.first == key) {
                    skip = true;
                    break;
                }
            }
        }
        if (!skip) out.push_back(entry);
    }
    for (const auto& pair : env) out.push_back(pair.first + "=" + pair.second);
    return out;
}

}  // namespace

bool RunBrain(const ProcessSpec& spec, const std::string& prompt,
              const std::function<bool(const std::string&)>& onLine, BrainRunResult* result,
              std::string* error) {
    if (result == nullptr) return false;
    *result = BrainRunResult();
    if (spec.argv.empty()) {
        if (error != nullptr) *error = "brain.command 是空的";
        return false;
    }
    std::string program;
    if (!ResolveExecutable(spec.argv[0], &program)) {
        if (error != nullptr) *error = "找不到可执行文件：" + spec.argv[0] + "（PATH 里没有）";
        return false;
    }

    // argv / envp / cwd 全部在 fork 之前准备好：fork 之后只允许 async-signal-safe 的调用，
    // 而 malloc、std::string 在多线程进程的 fork 之后都可能死锁。
    std::vector<std::string> argvStorage = spec.argv;
    argvStorage[0] = program;
    std::vector<char*> argv;
    for (std::string& item : argvStorage) argv.push_back(const_cast<char*>(item.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envStorage = BuildEnvironment(spec.env);
    std::vector<char*> envp;
    for (std::string& item : envStorage) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        if (error != nullptr) *error = "创建管道失败";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) close(fd);
        }
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        if (error != nullptr) *error = "fork 失败";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) close(fd);
        }
        return false;
    }
    if (pid == 0) {
        // 子进程：只做 async-signal-safe 的事
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) _exit(127);
        execve(program.c_str(), argv.data(), envp.data());
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    if (!prompt.empty()) WriteAllFd(inPipe[1], prompt);
    close(inPipe[1]);
    result->started = true;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeoutMs);
    std::string outBuffer;
    std::string errBuffer;
    bool stop = false;
    while (true) {
        struct pollfd fds[2];
        int count = 0;
        if (outPipe[0] >= 0) {
            fds[count].fd = outPipe[0];
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (errPipe[0] >= 0) {
            fds[count].fd = errPipe[0];
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (count == 0) break;
        const int ready = poll(fds, static_cast<nfds_t>(count), 50);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0) {
            for (int k = 0; k < count; ++k) {
                if ((fds[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
                char chunk[4096];
                const ssize_t got = read(fds[k].fd, chunk, sizeof(chunk));
                if (got > 0) {
                    if (fds[k].fd == outPipe[0]) {
                        outBuffer.append(chunk, static_cast<std::size_t>(got));
                        if (!DrainLines(&outBuffer, onLine)) {
                            stop = true;
                            break;
                        }
                    } else {
                        AppendTail(&errBuffer, chunk, static_cast<std::size_t>(got));
                    }
                } else if (got == 0 || (got < 0 && errno != EINTR)) {
                    close(fds[k].fd);
                    if (fds[k].fd == outPipe[0]) {
                        outPipe[0] = -1;
                    } else {
                        errPipe[0] = -1;
                    }
                }
            }
        }
        if (stop) {
            kill(pid, SIGKILL);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result->timedOut = true;
            result->error = "子进程超时（" + std::to_string(spec.timeoutMs) + " ms）";
            kill(pid, SIGKILL);
            break;
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        result->exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->exitCode = 128 + WTERMSIG(status);
    }
    result->stderrTail = errBuffer;
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (errPipe[0] >= 0) close(errPipe[0]);
    // 收尾：最后一行可能没有换行符，也要交出去
    if (!outBuffer.empty()) {
        if (!outBuffer.empty() && outBuffer.back() == '\n') outBuffer.pop_back();
        if (!outBuffer.empty() && outBuffer.back() == '\r') outBuffer.pop_back();
        if (!outBuffer.empty()) onLine(outBuffer);
    }
    return true;
}

#endif

}  // namespace dshbot
}  // namespace dchat
