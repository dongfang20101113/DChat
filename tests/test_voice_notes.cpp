// 语音消息的测试：判定纯函数、WAV 拼装与解析，以及**端到端**的
// "PCM → 写 WAV → 读回来"。
//
// **刻意不碰麦克风**：录音设备在开发机 / CI 上经常没有（这台开发机就是
// `waveInGetNumDevs() == 0`，waveInOpen 直接返回 MMSYSERR_NODRIVER），
// 把它写进测试只会让测试随机失败。真正需要硬件的只有 waveInOpen 那一句，
// 那句靠人手在有麦克风的机器上点一次按钮验证。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mmreg.h>

#include "file_transfer.h"
#include "voice_notes.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

// 有些断言要带上实际值（"实际 123 vs 456"），那种消息是拼出来的 std::string
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

// 造一段正弦波 PCM，用来验证落盘和读回
std::vector<unsigned char> MakeTone(int seconds, int frequency = 440) {
    const int samples = dchat::kVoiceSampleRate * seconds;
    std::vector<unsigned char> pcm;
    pcm.reserve(static_cast<std::size_t>(samples) * 2);
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / dchat::kVoiceSampleRate;
        const short value = static_cast<short>(
            std::sin(2.0 * 3.14159265358979323846 * frequency * t) * 12000.0);
        pcm.push_back(static_cast<unsigned char>(value & 0xFF));
        pcm.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    }
    return pcm;
}

std::string TempPath(const char* name) {
    char dir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + "dchat-voice-test-" + name;
}

bool WriteFileBytes(const std::string& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
        written == bytes.size();
    CloseHandle(file);
    return ok;
}

}  // namespace

int main() {
    std::printf("== dchat voice notes tests ==\n");

    std::printf("[1] 音频扩展名识别\n");
    check(dchat::LooksLikeAudio("voice-1.wav"), "wav");
    check(dchat::LooksLikeAudio("voice.M4A"), "m4a，大小写不敏感");
    check(dchat::LooksLikeAudio("d:/received/语音.ogg"), "带路径、中文名也行");
    check(dchat::LooksLikeAudio("a.opus") && dchat::LooksLikeAudio("a.amr") &&
              dchat::LooksLikeAudio("a.3gp"),
          "安卓端可能录出来的容器都认");
    check(!dchat::LooksLikeAudio("note.txt"), "txt 不是音频");
    check(!dchat::LooksLikeAudio("voice"), "没有扩展名不算");
    check(!dchat::LooksLikeAudio("wav"), "只有扩展名本身（没点）不算");

    std::printf("[2] 时长格式化\n");
    check(dchat::FormatDuration(7) == "0:07", "个位数秒补零");
    check(dchat::FormatDuration(83) == "1:23", "分钟进位");
    check(dchat::FormatDuration(60) == "1:00", "整分钟");
    check(dchat::FormatDuration(0) == "0:00", "0 秒");
    check(dchat::FormatDuration(-1) == "0:00", "时长未知（-1）时不显示 -1:-1");

    std::printf("[3] 上限是按码率算出来的，不是手写的\n");
    check(dchat::kVoicePcmBytesPerSecond == 32000, "16 kHz 16 位单声道 = 32 KB/s");
    check(dchat::kVoicePcmBytesPerSecond * static_cast<unsigned long long>(dchat::kMaxVoiceSeconds) <=
              dchat::kMaxVoiceBytes,
          "录满上限的字节数不超过文件上限（" +
              std::to_string(dchat::kVoicePcmBytesPerSecond * dchat::kMaxVoiceSeconds) + " <= " +
              std::to_string(dchat::kMaxVoiceBytes) + "）");

    std::printf("[4] 语音能不能发\n");
    check(dchat::WhyCannotSendVoice("voice.wav", 48000, 3).empty(), "正常录音可以发");
    check(!dchat::WhyCannotSendVoice("voice.wav", 48000, 0).empty(), "0 秒被拦");
    check(dchat::WhyCannotSendVoice("voice.wav", 48000, 1).empty(),
          "刚好 1 秒可以发（边界要测两头）");
    check(dchat::WhyCannotSendVoice("voice.wav", 48000, dchat::kMaxVoiceSeconds).empty(),
          "刚好到时长上限可以发");
    check(!dchat::WhyCannotSendVoice("voice.wav", 48000, dchat::kMaxVoiceSeconds + 1).empty(),
          "超时长被拦");
    check(dchat::WhyCannotSendVoice("voice.wav", dchat::kMaxVoiceBytes, 30).empty(),
          "刚好到大小上限可以发");
    const std::string tooBig =
        dchat::WhyCannotSendVoice("voice.wav", dchat::kMaxVoiceBytes + 1, 30);
    check(!tooBig.empty(), "超大小被拦");
    check(tooBig.find("2.0 MB") != std::string::npos, "要说清上限是多少：" + tooBig);
    check(!dchat::WhyCannotSendVoice("voice.exe", 48000, 3).empty(), "不是音频格式被拦");
    check(!dchat::WhyCannotSendVoice("voice.wav", 0, 3).empty(), "0 字节被拦");
    check(dchat::FormatVoiceSize(9000) == "8.8 KB", "字节数说成人话：" +
                                                        dchat::FormatVoiceSize(9000));

    std::printf("[5] 自动下载的边界\n");
    check(dchat::ShouldAutoDownloadVoice("voice", "voice-1.m4a"),
          "种类是 voice 就自动下，不看扩展名");
    check(dchat::ShouldAutoDownloadVoice("voice", "奇怪的名字"),
          "种类是 voice 时文件名再怪也自动下");
    check(dchat::ShouldAutoDownloadVoice("VOICE", "a.wav"), "种类大小写不敏感");
    check(!dchat::ShouldAutoDownloadVoice("0", "报告.pdf"), "普通文件不自动下");
    check(!dchat::ShouldAutoDownloadVoice("1", "开心.png"),
          "贴纸在桌面端不自动下（没有内联渲染，下了也是堆垃圾）");
    check(dchat::ShouldAutoDownloadVoice("", "photo.png"),
          "老服务器（没有种类这一格）时退回原有行为：图片自动下");
    check(!dchat::ShouldAutoDownloadVoice("", "movie.mp4"),
          "老服务器时视频仍不自动下");

    std::printf("[6] WAV 拼装\n");
    {
        const std::vector<unsigned char> payload(1000, 0x5A);
        const std::vector<unsigned char> wav = dchat::BuildWav(
            payload, dchat::kVoiceSampleRate, dchat::kVoiceChannels, dchat::kVoiceBitsPerSample);
        check(wav.size() == 44 + payload.size(), "总长度 = 44 字节头 + 数据");
        check(std::memcmp(wav.data(), "RIFF", 4) == 0, "以 RIFF 开头");
        check(std::memcmp(wav.data() + 8, "WAVE", 4) == 0, "接着是 WAVE");
        check(std::memcmp(wav.data() + 12, "fmt ", 4) == 0, "fmt 块");
        check(std::memcmp(wav.data() + 36, "data", 4) == 0, "data 块");

        const unsigned int riffSize = static_cast<unsigned int>(wav[4]) |
                                      (static_cast<unsigned int>(wav[5]) << 8) |
                                      (static_cast<unsigned int>(wav[6]) << 16) |
                                      (static_cast<unsigned int>(wav[7]) << 24);
        check(riffSize == 36 + payload.size(), "RIFF 长度字段 = 36 + 数据长度");

        const std::string path = TempPath("pcm.wav");
        check(WriteFileBytes(path, wav), "写测试文件");
        unsigned short tag = 0;
        int rate = 0, seconds = 0;
        check(dchat::ReadWavFormat(path, &tag, &rate, &seconds), "读回来");
        check(tag == WAVE_FORMAT_PCM, "格式标记是 PCM");
        check(rate == dchat::kVoiceSampleRate, "采样率读对了");
        check(seconds == 0, "1000 字节不足 1 秒，报 0");
        DeleteFileA(path.c_str());
    }

    std::printf("[7] 端到端：3 秒录音 → 写盘 → 读回来\n");
    {
        const std::vector<unsigned char> pcm = MakeTone(3);
        check(pcm.size() == dchat::kVoicePcmBytesPerSecond * 3, "合成 3 秒 PCM 的长度正确");

        const std::vector<unsigned char> wav = dchat::BuildWav(
            pcm, dchat::kVoiceSampleRate, dchat::kVoiceChannels, dchat::kVoiceBitsPerSample);
        const std::string path = TempPath("tone.wav");
        check(WriteFileBytes(path, wav), "落盘");

        unsigned short tag = 0;
        int rate = 0, seconds = -1;
        check(dchat::ReadWavFormat(path, &tag, &rate, &seconds), "读回来");
        check(seconds == 3, "时长读出来是 3 秒（实际 " + std::to_string(seconds) + "）");
        check(dchat::WhyCannotSendVoice("voice-1.wav", wav.size(), seconds).empty(),
              "这段录音可以直接发（大小 " + dchat::FormatVoiceSize(wav.size()) + "）");

        // 真的能播：PlaySound 同步播一遍。这台机器上放 8 kHz/16 kHz PCM 是通的
        // （见 tools/acm_probe.cpp 的实测），所以这一条能验"文件格式是对的"。
        // 播不出来只记一笔，不算失败——无声卡的机器不该让测试变红。
        const BOOL played = PlaySoundA(path.c_str(), nullptr, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
        if (played) {
            check(true, "PlaySound 能播这个 WAV");
        } else {
            std::printf("  skip 这台机器放不出声（没有音频输出设备），跳过播放验证\n");
        }
        DeleteFileA(path.c_str());
    }

    std::printf("[8] 读不存在的文件 / 非 WAV 文件\n");
    {
        unsigned short tag = 0;
        int rate = 0, seconds = 0;
        check(!dchat::ReadWavFormat(TempPath("根本不存在.wav"), &tag, &rate, &seconds),
              "读不存在的文件返回 false，而不是崩");
        const std::string junk = TempPath("junk.wav");
        const std::vector<unsigned char> bytes(100, 0x41);
        WriteFileBytes(junk, bytes);
        check(!dchat::ReadWavFormat(junk, &tag, &rate, &seconds), "不是 RIFF 的文件也返回 false");
        DeleteFileA(junk.c_str());
    }

    std::printf("[9] 没有麦克风时给出的是一句人话\n");
    {
        // 这一条在**有**麦克风的机器上会真的开始录音，所以只在没有设备时验
        if (waveInGetNumDevs() == 0) {
            dchat::VoiceRecorder recorder;
            check(!recorder.Start(), "没有录音设备时 Start 返回 false");
            check(!recorder.LastError().empty(), "给了原因");
            check(recorder.LastError().find("麦克风") != std::string::npos,
                  "原因里明确提到麦克风：" + recorder.LastError());
            check(!recorder.IsRunning(), "状态没有变成录音中");
            check(recorder.ElapsedSeconds() == 0, "没录上任何时长");
            recorder.Cancel();  // 收尾不该崩
            check(true, "取消一次没开始的录音不会崩");
        } else {
            std::printf("  skip 这台机器有录音设备，跳过「没有麦克风」的分支\n");
        }
    }

    std::printf("[10] FILE_SEND 的字段数（服务器按位置解析，错了整条命令被拒）\n");
    {
        const std::string id = "f123_1";
        const std::string name = dchat::Base64Encode("语音 1.wav");

        // 普通文件、不带缩略图：3 格
        const std::string plain = dchat::BuildFileSendRest(id, name, 48000, "", false);
        check(std::count(plain.begin(), plain.end(), ' ') == 2, "普通文件是 3 格：" + plain);

        // 普通文件、带缩略图：4 格，最后一格是 "1"
        const std::string withThumb = dchat::BuildFileSendRest(id, name, 48000, "", true);
        check(std::count(withThumb.begin(), withThumb.end(), ' ') == 3, "带缩略图是 4 格");
        check(withThumb.size() >= 2 && withThumb.substr(withThumb.size() - 2) == " 1",
              "最后一格是缩略图标记：" + withThumb);

        // 语音：4 格，最后一格是 voice（**不是** "1"）
        const std::string voice = dchat::BuildFileSendRest(id, name, 48000, "voice", false);
        check(std::count(voice.begin(), voice.end(), ' ') == 3, "语音也是 4 格：" + voice);
        check(voice.size() >= 6 && voice.substr(voice.size() - 6) == " voice",
              "最后一格是 voice：" + voice);

        // 语音**同时**带缩略图时：种类优先，不能出现 5 格
        // （5 格会被服务器直接按"用法错误"拒绝，而且错误只在运行期出现）
        const std::string both = dchat::BuildFileSendRest(id, name, 48000, "voice", true);
        check(std::count(both.begin(), both.end(), ' ') == 3,
              "带种类时不发缩略图标记，仍是 4 格：" + both);
        check(both.substr(both.size() - 6) == " voice", "种类没被缩略图标记挤掉");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

