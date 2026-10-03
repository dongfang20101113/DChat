// 语音（录制 / 播放）。
//
// 格式和另外两端**必须一致**，否则发出去的语音对方放不出来：
//   16 kHz / 单声道 / 16 位 PCM WAV（见 src/voice_notes.h 的 kVoice* 常量）
//   上限 2 MB（按上面的码率算出来约 64 秒），短于 1 秒的不发（误触全是噪音）
//
// 实现方式是调外部工具，而不是链 ALSA 库：
//   arecord -f S16_LE -r 16000 -c 1 -t wav
//   aplay   <文件>
// 理由：链 libasound 会让这个客户端多一个编译期依赖，而 arecord/aplay 在几乎所有
// Linux 发行版上都是 alsa-utils 包自带的。代价是录音时必须管好子进程（别留僵尸、
// 别把 Ctrl+C 吃掉），这部分在 .cpp 里处理。
//
// 另外**不能假设真有声卡**：容器和服务器上经常没有 /dev/snd，那时 arecord 会失败。
// 所以每个入口都要能优雅地报"没有可用录音设备"，而不是崩掉。
#pragma once

#include <string>
#include <vector>

namespace dchat {

/**
 * 一次录音的会话（后台跑 arecord，不阻塞界面）。
 *
 * 名字特意不叫 VoiceRecorder：协议层 src/voice_notes.h 里已经有一个同名的
 * Windows waveIn 实现，两个类都在 dchat 命名空间下，重名会直接编不过。
 */
class MicRecorder {
public:
    MicRecorder() = default;
    ~MicRecorder();

    MicRecorder(const MicRecorder&) = delete;
    MicRecorder& operator=(const MicRecorder&) = delete;

    /** 录音设备在不在（没有就早点告诉用户，别等按下录音才报错）。 */
    static bool HasCaptureDevice();

    /** 放音设备在不在。 */
    static bool HasPlaybackDevice();

    /**
     * 开始录音。失败返回 false，原因写进 error。
     * `maxSeconds` 到点自动停（0 表示用协议上限）。
     */
    bool Start(int maxSeconds, std::string* error);

    /** 正在录吗。 */
    bool Recording() const { return pid_ > 0; }

    /** 已经录了多少秒（按墙钟算，够界面显示用）。 */
    int ElapsedSeconds() const;

    /**
     * 停止录音并收尾。成功时把落盘的路径写进 path。
     *
     * 会检查时长和大小：太短/太大都返回 false 并给出**给用户看的原因**
     * （本地先拦，省一次"发出去被服务端拒绝"的往返）。
     */
    bool StopAndSave(std::string* path, std::string* error);

    /** 放弃这次录音并删掉临时文件。 */
    void Discard();

private:
    int pid_ = -1;
    std::string path_;
    long long startedAtMs_ = 0;
};

/** 播放一个音频文件（阻塞，最长等 maxWaitMs）。 */
bool PlayAudioFile(const std::string& path, int maxWaitMs, std::string* error);

/** 这个音频文件有多少秒（读 WAV 头；读不出来返回 -1）。 */
int WavDurationSeconds(const std::string& path);

/** 这段录音能不能发？空串表示可以，否则是给用户看的原因。 */
std::string WhyCannotSendVoice(const std::string& fileName, unsigned long long bytes, int seconds);

/** 秒数格式化成 `0:07` / `1:23`。 */
std::string FormatDuration(int seconds);

/** 临时目录里给录音用的文件名。 */
std::string MakeVoiceFileName();

/** 当前毫秒时间戳（单调时钟）。 */
long long NowMs();

}  // namespace dchat
