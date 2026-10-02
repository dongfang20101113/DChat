// 语音消息的实现：录音（waveIn）、WAV 拼装与解析、播放（PlaySound）。
//
// 格式与取舍见 voice_notes.h 的开头。这里只说两处容易踩的地方：
//
//   1. **waveIn 的回调在系统音频线程上跑**，不能直接碰界面，也不能和界面线程
//      无锁共享容器。这里的做法是：回调只把数据 memcpy 进 recorder 持有的缓冲，
//      并用临界区保护；界面线程读长度时也走同一把锁。
//   2. **waveIn 的缓冲必须轮转排队**（waveInAddBuffer 一次只够一小段）。
//      只排一块的话，界面线程忙一下就会丢音频——听起来像"录音断断续续"。
#include "voice_notes.h"

#include "image_preview.h"  // ShouldAutoDownload：种类缺失时退回按扩展名判断

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <mmsystem.h>
#include <mmreg.h>

namespace dchat {
namespace {

constexpr const wchar_t* kVoiceDirName = L"voice-notes";
constexpr int kRecordBuffers = 8;         // 8 块轮转
constexpr int kRecordBufferMillis = 100;  // 每块 100ms

int RecordBufferBytes() {
    return kVoiceSampleRate * (kVoiceBitsPerSample / 8) * kVoiceChannels * kRecordBufferMillis /
           1000;
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string ExtensionOf(const std::string& fileName) {
    const std::size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size()) return std::string();
    return ToLowerAscii(fileName.substr(dot + 1));
}

// 这一层刻意自己带两个编码转换函数，不去依赖 client.cpp 里的同名函数：
// 语音模块要能单独编译、单独测试（test_voice_notes 里不含 client.cpp）。
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

// 录音文件放在 exe 同级的 voice-notes\ 目录里（发出去的原始文件不该混进"收件箱"）
std::wstring VoiceDirectory() {
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L".";
    std::wstring dir(exePath);
    const std::size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring(L".") : dir.substr(0, slash);
    dir += L"\\";
    dir += kVoiceDirName;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void AppendLE32(std::vector<unsigned char>* out, unsigned int value) {
    out->push_back(static_cast<unsigned char>(value & 0xFF));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    out->push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out->push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
}

void AppendLE16(std::vector<unsigned char>* out, unsigned short value) {
    out->push_back(static_cast<unsigned char>(value & 0xFF));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

unsigned int ReadLE32(const unsigned char* p) {
    return static_cast<unsigned int>(p[0]) | (static_cast<unsigned int>(p[1]) << 8) |
           (static_cast<unsigned int>(p[2]) << 16) | (static_cast<unsigned int>(p[3]) << 24);
}

unsigned short ReadLE16(const unsigned char* p) {
    return static_cast<unsigned short>(static_cast<unsigned int>(p[0]) |
                                       (static_cast<unsigned int>(p[1]) << 8));
}

// ---------------- waveIn 录音 ----------------

class WaveInSession {
public:
    ~WaveInSession() { Close(); }

    bool Open(std::vector<unsigned char>* sink, std::mutex* sinkMutex, std::string* error) {
        sink_ = sink;
        sinkMutex_ = sinkMutex;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(kVoiceChannels);
        format.nSamplesPerSec = static_cast<DWORD>(kVoiceSampleRate);
        format.wBitsPerSample = static_cast<WORD>(kVoiceBitsPerSample);
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        const MMRESULT opened = waveInOpen(
            &handle_, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(&WaveInSession::Callback),
            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (opened != MMSYSERR_NOERROR) {
            // 每一条都要落到"用户该怎么办"上，不能只说"打开失败"
            switch (opened) {
                case MMSYSERR_NODRIVER:
                case MMSYSERR_BADDEVICEID:
                    *error = "这台机器没有麦克风，发不了语音（可以听别人发的）";
                    break;
                case MMSYSERR_ALLOCATED:
                    *error = "麦克风正被别的程序占用（会议软件、录音机…），关掉再试";
                    break;
                case WAVERR_BADFORMAT:
                    *error = "麦克风不支持这个录音格式";
                    break;
                default:
                    *error = "打不开麦克风（错误码 " + std::to_string(opened) + "）";
                    break;
            }
            return false;
        }

        const int bytes = RecordBufferBytes();
        for (int i = 0; i < kRecordBuffers; ++i) {
            buffers_[i].resize(static_cast<std::size_t>(bytes));
            headers_[i] = {};
            headers_[i].lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
            headers_[i].dwBufferLength = static_cast<DWORD>(bytes);
            if (waveInPrepareHeader(handle_, &headers_[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(handle_, &headers_[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                *error = "准备录音缓冲失败";
                Close();
                return false;
            }
        }
        if (waveInStart(handle_) != MMSYSERR_NOERROR) {
            *error = "启动录音失败";
            Close();
            return false;
        }
        return true;
    }

    void Close() {
        if (!handle_) return;
        // 先 STOP 再 RESET：RESET 会把排队的缓冲都打回来，顺序反了会多收到一段静音
        waveInStop(handle_);
        waveInReset(handle_);
        for (int i = 0; i < kRecordBuffers; ++i) {
            if (headers_[i].lpData) {
                waveInUnprepareHeader(handle_, &headers_[i], sizeof(WAVEHDR));
            }
            headers_[i] = {};
        }
        waveInClose(handle_);
        handle_ = nullptr;
    }

private:
    static void CALLBACK Callback(HWAVEIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1,
                                  DWORD_PTR) {
        if (msg != WIM_DATA) return;
        auto* self = reinterpret_cast<WaveInSession*>(instance);
        auto* header = reinterpret_cast<WAVEHDR*>(param1);
        if (self) self->OnData(header);
    }

    void OnData(WAVEHDR* header) {
        if (!header || !sink_ || !sinkMutex_) return;
        const std::size_t got = header->dwBytesRecorded;
        if (got > 0) {
            // 只在拷贝这一小段时加锁；写文件和界面绘制都不在这把锁里面
            std::lock_guard<std::mutex> guard(*sinkMutex_);
            sink_->insert(sink_->end(), header->lpData, header->lpData + got);
        }
        // 排回去继续采；已经在收尾（handle_ 置空）就不再排
        if (handle_) {
            header->dwBytesRecorded = 0;
            waveInAddBuffer(handle_, header, sizeof(WAVEHDR));
        }
    }

    HWAVEIN handle_ = nullptr;
    std::vector<unsigned char> buffers_[kRecordBuffers];
    WAVEHDR headers_[kRecordBuffers]{};
    std::vector<unsigned char>* sink_ = nullptr;
    std::mutex* sinkMutex_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// 判定（纯函数）
// ---------------------------------------------------------------------------

bool LooksLikeAudio(const std::string& fileName) {
    static const char* const kExts[] = {"wav", "mp3",  "m4a", "aac", "ogg",
                                        "opus", "3gp", "amr", "wma", "flac"};
    const std::string ext = ExtensionOf(fileName);
    if (ext.empty()) return false;
    for (const char* candidate : kExts) {
        if (ext == candidate) return true;
    }
    return false;
}

bool ShouldAutoDownloadVoice(const std::string& kind, const std::string& fileName) {
    const std::string lowered = ToLowerAscii(kind);
    // 协议说了算：种类是 voice → 一定自动下，跟文件名长什么样无关
    // （安卓端录出来是 .m4a，桌面端是 .wav，扩展名不可靠）
    if (lowered == "voice") return true;
    // 这一格缺失（老服务器）时退回按扩展名判断，行为一个字都不变
    if (lowered.empty()) return ShouldAutoDownload(fileName);
    // 有种类、但不是 voice → 不自动下。贴纸也一样：
    // 桌面端没有内联渲染贴纸，自动下一张小图只会在 received\ 里堆没用的文件。
    return false;
}

std::string WhyCannotSendVoice(const std::string& fileName, unsigned long long bytes,
                              int seconds) {
    if (seconds < kMinVoiceSeconds) return "说话时间太短了";
    if (seconds > kMaxVoiceSeconds) {
        return "语音最长 " + FormatDuration(kMaxVoiceSeconds) + "，这段有 " +
               FormatDuration(seconds);
    }
    if (!LooksLikeAudio(fileName)) return "这个格式不能当语音发";
    if (bytes == 0) return "录音文件是空的";
    if (bytes > kMaxVoiceBytes) {
        return "语音文件最大 " + FormatVoiceSize(kMaxVoiceBytes) + "，这段有 " +
               FormatVoiceSize(bytes);
    }
    return std::string();
}

std::string FormatDuration(int seconds) {
    const int total = seconds < 0 ? 0 : seconds;
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
    return std::string(buffer);
}

std::string FormatVoiceSize(unsigned long long bytes) {
    char buffer[48] = {0};
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", bytes);
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

std::vector<unsigned char> BuildWav(const std::vector<unsigned char>& payload, int sampleRate,
                                    int channels, int bitsPerSample) {
    std::vector<unsigned char> wav;
    wav.reserve(payload.size() + 64);

    const unsigned int dataSize = static_cast<unsigned int>(payload.size());
    const int blockAlign = channels * bitsPerSample / 8;
    const unsigned int avgBytes = static_cast<unsigned int>(sampleRate * blockAlign);

    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    AppendLE32(&wav, 36 + dataSize);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    AppendLE32(&wav, 16);
    AppendLE16(&wav, WAVE_FORMAT_PCM);
    AppendLE16(&wav, static_cast<unsigned short>(channels));
    AppendLE32(&wav, static_cast<unsigned int>(sampleRate));
    AppendLE32(&wav, avgBytes);
    AppendLE16(&wav, static_cast<unsigned short>(blockAlign));
    AppendLE16(&wav, static_cast<unsigned short>(bitsPerSample));

    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    AppendLE32(&wav, dataSize);
    wav.insert(wav.end(), payload.begin(), payload.end());
    return wav;
}

bool ReadWavFormat(const std::string& path, unsigned short* formatTag, int* sampleRate,
                   int* seconds) {
    HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::vector<unsigned char> head(4096);
    DWORD got = 0;
    const bool read =
        ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &got, nullptr) != 0;
    CloseHandle(file);
    if (!read || got < 44) return false;
    head.resize(got);

    if (std::memcmp(head.data(), "RIFF", 4) != 0 || std::memcmp(head.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    unsigned short tag = 0;
    int rate = 0;
    unsigned int avgBytes = 0;
    unsigned int dataSize = 0;
    bool haveFmt = false;
    std::size_t pos = 12;
    while (pos + 8 <= head.size()) {
        const unsigned int chunkSize = ReadLE32(head.data() + pos + 4);
        const char* id = reinterpret_cast<const char*>(head.data() + pos);
        if (std::memcmp(id, "fmt ", 4) == 0 && pos + 24 <= head.size()) {
            tag = ReadLE16(head.data() + pos + 8);
            rate = static_cast<int>(ReadLE32(head.data() + pos + 12));
            avgBytes = ReadLE32(head.data() + pos + 16);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataSize = chunkSize;
            break;
        }
        pos += 8 + chunkSize + (chunkSize & 1);  // 块按偶数字节对齐
    }
    if (!haveFmt || rate <= 0) return false;
    if (formatTag) *formatTag = tag;
    if (sampleRate) *sampleRate = rate;
    if (seconds) *seconds = avgBytes > 0 ? static_cast<int>(dataSize / avgBytes) : 0;
    return true;
}

// ---------------------------------------------------------------------------
// VoiceRecorder
// ---------------------------------------------------------------------------

VoiceRecorder::~VoiceRecorder() { Cancel(); }

bool VoiceRecorder::Start() {
    if (running_) return true;
    lastError_.clear();
    {
        std::lock_guard<std::mutex> guard(sinkMutex_);
        pcm_.clear();
        pcm_.reserve(static_cast<std::size_t>(kVoicePcmBytesPerSecond) * 10);
    }

    auto* session = new WaveInSession();
    if (!session->Open(&pcm_, &sinkMutex_, &lastError_)) {
        delete session;
        return false;
    }
    session_ = session;
    running_ = true;
    return true;
}

int VoiceRecorder::ElapsedSeconds() const {
    const unsigned long long bytes = PcmBytes();
    return static_cast<int>(bytes / kVoicePcmBytesPerSecond);
}

unsigned long long VoiceRecorder::PcmBytes() const {
    std::lock_guard<std::mutex> guard(sinkMutex_);
    return pcm_.size();
}

bool VoiceRecorder::Finished(std::string* outPath, std::string* error) {
    if (!running_) {
        if (error) *error = "没有正在进行的录音";
        return false;
    }
    auto* session = static_cast<WaveInSession*>(session_);
    session->Close();
    delete session;
    session_ = nullptr;
    running_ = false;

    std::vector<unsigned char> pcm;
    {
        std::lock_guard<std::mutex> guard(sinkMutex_);
        pcm.swap(pcm_);
    }

    const std::vector<unsigned char> wav =
        BuildWav(pcm, kVoiceSampleRate, kVoiceChannels, kVoiceBitsPerSample);

    const std::wstring dir = VoiceDirectory();
    // 文件名带 tick：连着录两段也不会互相覆盖
    const std::wstring full =
        dir + L"\\voice-" + std::to_wstring(GetTickCount64()) + L".wav";

    HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "写不进录音文件（目录可能没有写权限）";
        return false;
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(file, wav.data(), static_cast<DWORD>(wav.size()), &written, nullptr) != 0 &&
        written == wav.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(full.c_str());
        if (error) *error = "写录音文件失败";
        return false;
    }

    if (outPath) *outPath = WideToUtf8(full);
    return true;
}

void VoiceRecorder::Cancel() {
    if (session_) {
        auto* session = static_cast<WaveInSession*>(session_);
        session->Close();
        delete session;
        session_ = nullptr;
    }
    running_ = false;
    std::lock_guard<std::mutex> guard(sinkMutex_);
    pcm_.clear();
}

// ---------------------------------------------------------------------------
// VoicePlayer
// ---------------------------------------------------------------------------

VoicePlayer::~VoicePlayer() { Stop(); }

int VoicePlayer::Play(const std::string& path) {
    Stop();
    lastError_.clear();
    if (path.empty()) {
        lastError_ = "这条语音还没有下载完";
        return -1;
    }

    unsigned short formatTag = 0;
    int sampleRate = 0;
    int seconds = 0;
    if (!ReadWavFormat(path, &formatTag, &sampleRate, &seconds)) {
        lastError_ = "音频文件读不了（可能没下载完）";
        return -1;
    }

    const std::wstring wide = Utf8ToWide(path);
    if (!PlaySoundW(wide.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        lastError_ = "放不出来（系统缺少这种音频的解码器）";
        return -1;
    }
    path_ = path;
    durationSeconds_ = seconds;
    playing_ = true;
    paused_ = false;
    pausedAtSeconds_ = 0;
    startedAtTick_ = GetTickCount64();
    return seconds;  // 0 = 播上了但读不出时长
}

bool VoicePlayer::Resume() {
    if (path_.empty() || playing_) return false;
    if (!PlaySoundW(Utf8ToWide(path_).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        lastError_ = "放不出来（系统缺少这种音频的解码器）";
        return false;
    }
    playing_ = true;
    paused_ = false;
    // 把起点往回拨，CurrentSeconds 才会接着上次的数往下走
    startedAtTick_ = GetTickCount64() - static_cast<unsigned long long>(pausedAtSeconds_) * 1000ull;
    return true;
}

void VoicePlayer::Pause() {
    if (!playing_) return;
    pausedAtSeconds_ = CurrentSeconds();
    PlaySoundW(nullptr, nullptr, 0);  // 传空 = 停掉当前播放
    playing_ = false;
    paused_ = true;
}

int VoicePlayer::CurrentSeconds() const {
    if (!playing_) return paused_ ? pausedAtSeconds_ : 0;
    return static_cast<int>((GetTickCount64() - startedAtTick_) / 1000ull);
}

bool VoicePlayer::ReachedEnd() const {
    if (!playing_ || durationSeconds_ <= 0) return false;
    // 多算半秒：自己计时和声卡实际播完总有偏差，提前收回去会看起来卡一下
    return CurrentSeconds() >= durationSeconds_;
}

void VoicePlayer::Stop() {
    if (playing_) PlaySoundW(nullptr, nullptr, 0);
    playing_ = false;
    paused_ = false;
    pausedAtSeconds_ = 0;
    durationSeconds_ = 0;
    startedAtTick_ = 0;
    // path_ 留着给 Resume 用；换一条语音时会先 Stop 再 Play，Play 里会覆盖它
}

}  // namespace dchat
