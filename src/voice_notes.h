// 语音消息：录音、播放，以及"这段录音能不能发"的判定。
//
// ## 和贴纸/图片同一套路：复用文件通道
//
// 语音就是一段录音文件，走现成的文件通道，只在 FILE_SEND / FILE_OFFER 的最后一格
// 标上种类 `voice`。限速、大小限制、过期清理、加密全部原样复用。
//
// ## 格式：16 kHz 单声道 16 位 PCM，不压缩
//
// 选它是因为**它是 Windows 上唯一不用赌的格式**：waveIn 直接能采、PlaySound 直接能放。
// 试过的两条压缩路线都放弃了，理由记在下面（第 4 条最要命）：
//
//   1. ACM 的 GSM 6.10 驱动在这台开发机上 `acmStreamOpen` 返回 512
//      （MMSYSERR_ERROR），MS-ADPCM / G.711 也一样——**装不出编码器的机器上
//      语音会直接发不出去**，而这个功能的价值远大于省几百 KB。
//   2. 16 kHz 16 位单声道约 32 KB/s，2 MB 上限够录 **60 秒**，对讲一句话够用。
//   3. 降到 8 kHz 能翻倍到 2 分钟，但 8 kHz 的"电话音"听起来发闷，
//      而且这台机器没有录音设备、两种都验不了，宁可先用更常规的那一个。
//   4. **台式机经常没有麦克风**（实测这台就是 `waveInGetNumDevs() == 0`）。
//      所以"录不了"必须是**一句人话的提示**，不能崩、不能卡住不动。
//
// 判定与格式化是**纯函数**（[LooksLikeAudio]、[WhyCannotSendVoice]、[FormatDuration]），
// 不碰系统 API，能在普通单测里跑；真正碰硬件的是 [VoiceRecorder] / [VoicePlayer]。
#pragma once

#include <windows.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace dchat {

// ---------------------------------------------------------------------------
// 录音参数
// ---------------------------------------------------------------------------

inline constexpr int kVoiceSampleRate = 16000;
inline constexpr int kVoiceBitsPerSample = 16;
inline constexpr int kVoiceChannels = 1;

// 单个语音文件的大小上限（和安卓端 VoiceMessage.MAX_BYTES 一致）
inline constexpr unsigned long long kMaxVoiceBytes = 2ull * 1024 * 1024;

// 纯 PCM 每秒占多少字节
inline constexpr unsigned long long kVoicePcmBytesPerSecond =
    static_cast<unsigned long long>(kVoiceSampleRate) * (kVoiceBitsPerSample / 8) * kVoiceChannels;

// 2 MB 上限下最多能录多少秒（按上面的码率算出来，不再手写一个数字——
// 改了采样率却忘了改上限，就会出现"录满了却发不出去"）
inline constexpr int kMaxVoiceSeconds =
    static_cast<int>(kMaxVoiceBytes / kVoicePcmBytesPerSecond);

// 比这还短的直接丢掉：鼠标点一下也会触发一次录音，全是噪音。
inline constexpr int kMinVoiceSeconds = 1;

// ---------------------------------------------------------------------------
// 判定（纯函数，可单测）
// ---------------------------------------------------------------------------

// 这个文件名像不像音频（按扩展名，大小写不敏感）。这条只用于**兼容**：
// 正常路径上"是不是语音"由协议的种类字段说了算，不看扩展名。
bool LooksLikeAudio(const std::string& fileName);

// 收到一个附件时，要不要自动下载？
//
// kind 是协议透传的种类（"voice" / "1" / "sticker" / "0" / ""）。
//   - voice：**自动下**——语音的意义就是立刻能听，等用户点一下那个体验就废了；
//   - 这一格缺失（老服务器 / 老客户端发的贴纸写法）时，退回 image_preview.h 里的
//     [ShouldAutoDownload] 按扩展名判断，保持原有行为一个字不变。
//
// 为什么不让调用方"先判 kind、再判扩展名"：那种写法迟早会出现两处判断不一致，
// 而且只在特定消息上表现出来，极难排查。判断只留一个入口。
bool ShouldAutoDownloadVoice(const std::string& kind, const std::string& fileName);

// 这段录音能不能发？返回空串表示可以，否则是**给用户看的原因**。
// 时长、大小两条都在本地先拦，省一次"发出去被服务端拒绝"的往返。
std::string WhyCannotSendVoice(const std::string& fileName, unsigned long long bytes, int seconds);

// 秒数格式化成 `0:07` / `1:23`（分钟不补零、秒补零，和微信一致）。
// 负数夹成 0——时长未知时界面会拿到 -1。
std::string FormatDuration(int seconds);

// 480 KB / 1.2 MB。
std::string FormatVoiceSize(unsigned long long bytes);

// ---------------------------------------------------------------------------
// 录音
// ---------------------------------------------------------------------------

// 一次录音。所有方法都在**同一个线程**上调用（界面线程）：
// 采集回调只负责把 PCM 塞进来，写文件在 [Finished] 里做。
//
// 用法：
//     VoiceRecorder recorder;
//     if (!recorder.Start()) { 提示 LastError(); }
//     ... 定时看 ElapsedSeconds() 更新界面 ...
//     std::string path;
//     if (recorder.Finished(&path, &error)) { ... 发出去 ... }
class VoiceRecorder {
public:
    VoiceRecorder() = default;
    ~VoiceRecorder();
    VoiceRecorder(const VoiceRecorder&) = delete;
    VoiceRecorder& operator=(const VoiceRecorder&) = delete;

    // 打开麦克风开始录。失败返回 false（**原因一定是句人话**，见 LastError）。
    bool Start();

    // 已经录了多久（秒）。按收到的**样本数**算而不是墙钟时间：
    // 设备卡顿时墙钟还在走，那样算出来的时长和音频长度对不上。
    int ElapsedSeconds() const;

    // 已经录到多少字节（PCM）。
    unsigned long long PcmBytes() const;

    // 收尾：停设备、写成 WAV。成功时把路径写进 outPath 并返回 true。
    bool Finished(std::string* outPath, std::string* error);

    // 放弃这次录音：停设备、丢掉内存里的数据、不留文件。
    void Cancel();

    bool IsRunning() const { return running_; }

    const std::string& LastError() const { return lastError_; }

private:
    bool running_ = false;
    std::vector<unsigned char> pcm_;
    std::string lastError_;
    // 采集回调在系统音频线程上跑，waveIn 的细节藏在 .cpp 里
    void* session_ = nullptr;
    mutable std::mutex sinkMutex_;
};

// WAV 文件的拼装：把 PCM 数据包成 RIFF/WAVE。
// 单独抽出来是为了能单测（不依赖任何设备）。
std::vector<unsigned char> BuildWav(const std::vector<unsigned char>& payload, int sampleRate,
                                    int channels, int bitsPerSample);

// 读一个 WAV 的格式与时长（播放前用它算进度、判断能不能播）。失败返回 false。
bool ReadWavFormat(const std::string& path, unsigned short* formatTag, int* sampleRate,
                   int* seconds);

// ---------------------------------------------------------------------------
// 播放
//
// 用 PlaySoundW（winmm）：一行调用、不引任何解码器依赖。
// 代价是它**不报播放位置**，所以进度按"WAV 头里读出来的真实时长 + 自己计时"算。
// ---------------------------------------------------------------------------

class VoicePlayer {
public:
    VoicePlayer() = default;
    ~VoicePlayer();
    VoicePlayer(const VoicePlayer&) = delete;
    VoicePlayer& operator=(const VoicePlayer&) = delete;

    // 从头播放 [path]。返回总时长（秒）；**负数表示没播成**（原因在 LastError）。
    // 0 表示播上了但读不出时长——这两件事必须分开，
    // 否则"读不出时长的短音频"会被当成播放失败。
    int Play(const std::string& path);

    // 从暂停处接着播。没有暂停中的内容时返回 false。
    bool Resume();

    // 暂停：记住播到哪儿，[Resume] 接着播。
    void Pause();

    bool IsPlaying() const { return playing_; }
    bool IsPaused() const { return paused_; }

    // 已经播到第几秒（开始播的时刻 + 自己计时）。
    int CurrentSeconds() const;

    // 播完了吗？界面据此把"正在播"收回去。
    bool ReachedEnd() const;

    void Stop();

    const std::string& LastError() const { return lastError_; }

private:
    std::string path_;
    int durationSeconds_ = 0;
    bool playing_ = false;
    bool paused_ = false;
    int pausedAtSeconds_ = 0;
    unsigned long long startedAtTick_ = 0;
    std::string lastError_;
};

}  // namespace dchat
