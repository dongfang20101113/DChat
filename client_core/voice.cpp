#include "voice.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "file_transfer.h"  // FormatBytes
#include "voice_notes.h"   // 协议层定义的采样率/上限：两端必须用同一套常量

namespace dchat {
namespace {

/**
 * 录音与放音用的外部命令。
 *
 * 三个平台的工具不一样，但接口几乎一样（都是"读一个 WAV、写一个 WAV"），
 * 所以这里只换名字，其余代码（fork/exec、超时、SIGINT 收尾、时长校验）完全共用：
 *
 *   Linux   arecord -f S16_LE -r 16000 -c 1 -t wav -d N out.wav      （alsa-utils）
 *           aplay out.wav
 *   macOS   rec -q -r 16000 -c 1 -b 16 out.wav trim 0 N            （sox）
 *           play -q out.wav
 *
 * macOS 选 sox 而不是 afrecord/afplay：sox 的 rec 能直接写 16 kHz 单声道 WAV，
 * 参数写法与 arecord 一一对应；afrecord 的输出格式参数是另一套，且不一定装。
 */
#ifdef __APPLE__
constexpr const char* kRecordTool = "rec";
constexpr const char* kPlayTool = "play";
#else
constexpr const char* kRecordTool = "arecord";
constexpr const char* kPlayTool = "aplay";
#endif

/** 录音工具要不要 -d（秒）这种"录多久"参数。sox 用 `trim 0 N` 的位置参数。 */
#ifdef __APPLE__
constexpr bool kRecordUsesTrimArgument = true;
#else
constexpr bool kRecordUsesTrimArgument = false;
#endif

/** "有没有声卡"要看的路径：Linux 是 /dev/snd，macOS 走 CoreAudio 没有这个目录。 */
#ifdef __APPLE__
constexpr const char* kSoundDevicePath = "/System/Library/Frameworks/CoreAudio.framework";
#else
constexpr const char* kSoundDevicePath = "/dev/snd";
#endif

/** 工具在不在 PATH 里。 */
bool ToolExists(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string rest = path;
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::string dir = rest.substr(0, colon);
        rest = colon == std::string::npos ? std::string() : rest.substr(colon + 1);
        if (dir.empty()) continue;
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
}

/** 有没有 /dev/snd 下的声卡设备。 */
bool HasSoundDevice() {
    struct stat info {};
    if (::stat("/dev/snd", &info) != 0) return false;
    return S_ISDIR(info.st_mode);
}

/** 把当前时间写进文件（录音存到 /tmp，发送成功后由 FileTransfers 读走）。 */
std::string TempVoiceDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string base = tmp && *tmp ? tmp : "/tmp";
    if (base.back() == '/') base.pop_back();
    base += "/dchat-voice";
    ::mkdir(base.c_str(), 0700);
    return base;
}

}  // namespace

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool MicRecorder::HasCaptureDevice() {
    return ToolExists(kRecordTool) && HasSoundDevice();
}

bool MicRecorder::HasPlaybackDevice() {
    return ToolExists(kPlayTool) && HasSoundDevice();
}

std::string MakeVoiceFileName() {
    return TempVoiceDir() + "/voice-" + std::to_string(::getpid()) + "-" +
           std::to_string(NowMs()) + ".wav";
}

bool MicRecorder::Start(int maxSeconds, std::string* error) {
    if (Recording()) {
        if (error) *error = "已经在录音了";
        return false;
    }
    if (!ToolExists(kRecordTool)) {
        if (error) {
            *error = std::string("找不到 ") + kRecordTool + "（装一下：" +
#ifdef __APPLE__
                     "brew install sox）";
#else
                     "sudo apt-get install alsa-utils）";
#endif
        }
        return false;
    }
    if (!HasSoundDevice()) {
        if (error) {
            *error = std::string("这台机器没有可用的音频设备（找不到 ") + kSoundDevicePath +
                     "），录不了音";
        }
        return false;
    }
    const int limit = maxSeconds > 0 ? maxSeconds : kMaxVoiceSeconds;
    path_ = MakeVoiceFileName();

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = std::string("起不了录音进程：") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // 子进程：把标准输出和标准错误都丢掉（arecord 会往 stderr 打进度），
        // 否则它的输出会和终端界面混在一起，把输入行冲乱。
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            ::close(devNull);
        }
        // 录音命令的写法两端不同，但都是"16 kHz / 单声道 / 16 位 / 录 limit 秒"：
        //   Linux  arecord -q -f S16_LE -r 16000 -c 1 -t wav -d N out.wav
        //   macOS  rec -q -r 16000 -c 1 -b 16 out.wav trim 0 N
        const std::string rate = std::to_string(kVoiceSampleRate);
        const std::string channels = std::to_string(kVoiceChannels);
        const std::string seconds = std::to_string(limit);
        if (kRecordUsesTrimArgument) {
            // sox 的 rec：`trim 0 N` 表示从 0 秒录到 N 秒
            ::execlp(kRecordTool, kRecordTool, "-q", "-r", rate.c_str(), "-c", channels.c_str(),
                     "-b", "16", path_.c_str(), "trim", "0", seconds.c_str(),
                     static_cast<char*>(nullptr));
        } else {
            ::execlp(kRecordTool, kRecordTool, "-q", "-f", "S16_LE", "-r", rate.c_str(), "-c",
                     channels.c_str(), "-t", "wav", "-d", seconds.c_str(), path_.c_str(),
                     static_cast<char*>(nullptr));
        }
        ::_exit(127);  // execlp 失败才会到这
    }
    pid_ = static_cast<int>(pid);
    startedAtMs_ = NowMs();
    return true;
}

int MicRecorder::ElapsedSeconds() const {
    if (!Recording()) return 0;
    return static_cast<int>((NowMs() - startedAtMs_) / 1000);
}

bool MicRecorder::StopAndSave(std::string* path, std::string* error) {
    if (!Recording()) {
        if (error) *error = "当前没有在录音";
        return false;
    }
    const int pid = pid_;
    pid_ = -1;

    // 先送 SIGINT 让 arecord 自己收尾——它要**把 WAV 头的大小字段回填**。
    // 直接 SIGKILL 会留下一个头部写着 0 字节的文件，播放器打不开。
    ::kill(pid, SIGINT);
    int status = 0;
    bool reaped = false;
    for (int i = 0; i < 30; ++i) {  // 最多等 3 秒
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            reaped = true;
            break;
        }
        if (got < 0 && errno == ECHILD) {
            reaped = true;  // 已经被收掉了
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!reaped) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }

    struct stat info {};
    if (::stat(path_.c_str(), &info) != 0 || info.st_size <= 44) {
        // 44 = 标准 WAV 头长度；比它还小说明一个采样都没录到
        if (error) *error = "没录到声音（设备忙，或者麦克风被静音了）";
        ::unlink(path_.c_str());
        return false;
    }
    const unsigned long long bytes = static_cast<unsigned long long>(info.st_size);
    const int seconds = WavDurationSeconds(path_);
    const std::string why = WhyCannotSendVoice(path_, bytes, seconds);
    if (!why.empty()) {
        if (error) *error = why;
        ::unlink(path_.c_str());
        return false;
    }
    if (path) *path = path_;
    return true;
}

void MicRecorder::Discard() {
    if (Recording()) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
    }
}

MicRecorder::~MicRecorder() { Discard(); }

int WavDurationSeconds(const std::string& path) {
    // 自己读 WAV 头，不调外部工具：这里只是算个时长，不值得再起一个进程。
    // 布局：RIFF....WAVE + 若干 chunk，找到 fmt 拿字节率、找到 data 拿长度。
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return -1;
    unsigned char header[12] = {0};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
        std::fclose(file);
        return -1;
    }
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        std::fclose(file);
        return -1;
    }

    unsigned int byteRate = 0;
    unsigned long long dataBytes = 0;
    for (;;) {
        unsigned char chunk[8] = {0};
        if (std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        const unsigned int size = static_cast<unsigned int>(chunk[4]) |
                                  (static_cast<unsigned int>(chunk[5]) << 8) |
                                  (static_cast<unsigned int>(chunk[6]) << 16) |
                                  (static_cast<unsigned int>(chunk[7]) << 24);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            unsigned char format[16] = {0};
            if (std::fread(format, 1, sizeof(format), file) != sizeof(format)) break;
            byteRate = static_cast<unsigned int>(format[8]) |
                       (static_cast<unsigned int>(format[9]) << 8) |
                       (static_cast<unsigned int>(format[10]) << 16) |
                       (static_cast<unsigned int>(format[11]) << 24);
            // 跳过 fmt 里多出来的字节
            if (size > 16) std::fseek(file, static_cast<long>(size - 16), SEEK_CUR);
            continue;
        }
        if (std::memcmp(chunk, "data", 4) == 0) {
            dataBytes = size;
            break;
        }
        // 别的 chunk 跳过（size 是奇数时要补一个对齐字节）
        std::fseek(file, static_cast<long>(size + (size & 1)), SEEK_CUR);
    }
    std::fclose(file);
    if (byteRate == 0) return -1;
    return static_cast<int>(dataBytes / byteRate);
}

std::string WhyCannotSendVoice(const std::string& fileName, unsigned long long bytes, int seconds) {
    if (bytes > kMaxVoiceBytes) {
        // 用协议层的 FormatBytes（FormatVoiceSize 只在 Windows 那份实现里，
        // Linux 端链不到它）
        return "语音太大（" + FormatBytes(bytes) + "，上限 " + FormatBytes(kMaxVoiceBytes) + "）";
    }
    if (seconds >= 0 && seconds < kMinVoiceSeconds) {
        // 秒数已知且太短：点一下也会触发录音，全是噪音，直接拦掉
        return "太短了（不到 1 秒），再说一句试试";
    }
    if (fileName.empty()) return "文件名为空";
    return std::string();
}

std::string FormatDuration(int seconds) {
    if (seconds < 0) seconds = 0;
    const int minutes = seconds / 60;
    const int rest = seconds % 60;
    char buffer[32] = {0};
    // 分钟不补零、秒补零（和微信一致，也和另外两端的 FormatDuration 一致）
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, rest);
    return buffer;
}

bool PlayAudioFile(const std::string& path, int maxWaitMs, std::string* error) {
    if (!ToolExists(kPlayTool)) {
        if (error) {
            *error = std::string("找不到 ") + kPlayTool + "（装一下：" +
#ifdef __APPLE__
                     "brew install sox）";
#else
                     "sudo apt-get install alsa-utils）";
#endif
        }
        return false;
    }
    if (!HasSoundDevice()) {
        if (error) {
            *error = std::string("这台机器没有可用的音频设备（找不到 ") + kSoundDevicePath +
                     "），放不了音";
        }
        return false;
    }
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        if (error) *error = "文件不存在：" + path;
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = std::string("起不了播放进程：") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            ::close(devNull);
        }
        ::execlp(kPlayTool, kPlayTool, "-q", path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);
    for (;;) {
        int status = 0;
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid || (got < 0 && errno == ECHILD)) {
            return got == pid ? WIFEXITED(status) && WEXITSTATUS(status) == 0 : true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            if (error) *error = "播放超时（已停止）";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace dchat
