// 文件传输公共部分的单元测试：Base64 编解码、文件名清理、重名处理、字节数格式化，
// 以及"最坏情况下 FILE_DATA 一行不会超过协议上限"这条硬约束。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "file_transfer.h"
#include "protocol.h"

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

std::wstring TempDir() {
    wchar_t buffer[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, buffer);
    std::wstring dir = buffer;
    dir += L"dchat-file-transfer-test";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteFileBytes(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const char data[] = "x";
    DWORD written = 0;
    ::WriteFile(file, data, 1, &written, nullptr);
    ::CloseHandle(file);
}

std::wstring BaseName(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}
}  // namespace

int main() {
    std::printf("== dchat file transfer tests ==\n");

    {
        std::printf("[1] Base64 编码（RFC 4648 标准向量）\n");
        check(dchat::Base64Encode(std::string()) == "", "空数据编码成空串");
        check(dchat::Base64Encode(std::string("f")) == "Zg==", "f -> Zg==");
        check(dchat::Base64Encode(std::string("fo")) == "Zm8=", "fo -> Zm8=");
        check(dchat::Base64Encode(std::string("foo")) == "Zm9v", "foo -> Zm9v");
        check(dchat::Base64Encode(std::string("foob")) == "Zm9vYg==", "foob -> Zm9vYg==");
        check(dchat::Base64Encode(std::string("fooba")) == "Zm9vYmE=", "fooba -> Zm9vYmE=");
        check(dchat::Base64Encode(std::string("foobar")) == "Zm9vYmFy", "foobar -> Zm9vYmFy");
    }

    {
        std::printf("[2] Base64 解码与往返\n");
        std::vector<unsigned char> out;
        check(dchat::Base64Decode("", &out) && out.empty(), "空串解码成空数据");
        check(dchat::Base64Decode("Zm9vYmFy", &out) && std::string(out.begin(), out.end()) == "foobar",
              "foobar 往返正确");
        check(dchat::Base64Decode("Zg==", &out) && out.size() == 1 && out[0] == 'f', "单字节往返");
        check(dchat::Base64Decode("Zm8=", &out) && std::string(out.begin(), out.end()) == "fo",
              "两字节往返");

        // 0..255 全字节 + 各种长度都要能原样回来
        bool allRoundTrip = true;
        for (std::size_t len = 0; len <= 200; ++len) {
            std::string data;
            for (std::size_t i = 0; i < len; ++i) {
                data.push_back(static_cast<char>((i * 37 + len * 11) & 0xFF));
            }
            const std::string encoded = dchat::Base64Encode(data);
            std::vector<unsigned char> decoded;
            if (!dchat::Base64Decode(encoded, &decoded) ||
                std::string(decoded.begin(), decoded.end()) != data) {
                allRoundTrip = false;
                break;
            }
        }
        check(allRoundTrip, "长度 0..200 的任意字节都能原样往返");

        check(!dchat::Base64Decode("Zg=", &out), "长度不是 4 的倍数要拒绝");
        check(!dchat::Base64Decode("!!!!", &out), "非法字符要拒绝");
        check(!dchat::Base64Decode("Zg==Zg==", &out), "填充符只能出现在最后");
        check(!dchat::Base64Decode("Zm9v=", &out), "非法的尾部填充要拒绝");
        check(!dchat::Base64Decode("Z===", &out), "两个填充符却带三个字符要拒绝");
    }

    {
        std::printf("[3] 文件名清理\n");
        check(dchat::SanitizeFileName("../../evil.exe") == "evil.exe", "路径穿越只保留最后一段");
        check(dchat::SanitizeFileName("C:\\Windows\\system32\\cmd.exe") == "cmd.exe",
              "Windows 绝对路径只保留文件名");
        check(dchat::SanitizeFileName("a/b/c.txt") == "c.txt", "多层目录只保留文件名");
        check(dchat::SanitizeFileName("na<me>?.txt") == "na_me__.txt", "非法字符被替换");
        check(dchat::SanitizeFileName("报告 2026.txt") == "报告 2026.txt", "中文名原样保留");
        check(dchat::SanitizeFileName("name.exe...") == "name.exe", "结尾的点被去掉");
        check(dchat::SanitizeFileName("   ") == "file", "全是空格时兜底成 file");
        check(dchat::SanitizeFileName("..") == "file", "只有点号时兜底成 file");
        check(dchat::SanitizeFileName("") == "file", "空名字兜底成 file");
        check(dchat::SanitizeFileName("a\tb\nc.txt") == "a_b_c.txt", "控制字符被替换");

        const std::string longName(300, 'a');
        const std::string trimmed = dchat::SanitizeFileName(longName + ".txt");
        check(trimmed.size() <= dchat::kMaxFileNameBytes, "超长名字被截断到上限");
        check(trimmed.size() > 4 && trimmed.compare(trimmed.size() - 4, 4, ".txt") == 0,
              "截断后仍然保留扩展名");
        const std::string longCn(100, 'x');
        const std::string cn = dchat::SanitizeFileName(longCn + "中文名字很长很长.txt");
        check(cn.size() <= dchat::kMaxFileNameBytes, "中文长名字也被截断到上限（不会切坏字符）");
    }

    {
        std::printf("[4] 重名处理\n");
        const std::wstring dir = TempDir();
        ::DeleteFileW((dir + L"\\t.txt").c_str());
        ::DeleteFileW((dir + L"\\t (2).txt").c_str());
        ::DeleteFileW((dir + L"\\报告.txt").c_str());

        check(BaseName(dchat::MakeUniquePathW(dir, L"t.txt")) == L"t.txt",
              "没有同名文件时原样返回");
        check(BaseName(dchat::MakeUniquePathW(dir + L"\\", L"t.txt")) == L"t.txt",
              "目录结尾带分隔符也行");
        WriteFileBytes(dir + L"\\t.txt");
        check(BaseName(dchat::MakeUniquePathW(dir, L"t.txt")) == L"t (2).txt",
              "已存在同名文件时加 (2)");
        WriteFileBytes(dir + L"\\t (2).txt");
        check(BaseName(dchat::MakeUniquePathW(dir, L"t.txt")) == L"t (3).txt",
              "连 (2) 也占用了就加到 (3)");
        check(BaseName(dchat::MakeUniquePathW(dir, L"报告.txt")) == L"报告.txt",
              "中文文件名正常");
        check(BaseName(dchat::MakeUniquePathW(dir, L"没有扩展名")) == L"没有扩展名",
              "没有扩展名也行");

        ::DeleteFileW((dir + L"\\t.txt").c_str());
        ::DeleteFileW((dir + L"\\t (2).txt").c_str());
    }

    {
        std::printf("[5] 字节数格式化\n");
        check(dchat::FormatBytes(0) == "0 B", "0 字节");
        check(dchat::FormatBytes(1023) == "1023 B", "1023 字节");
        check(dchat::FormatBytes(1024) == "1.0 KB", "1024 -> 1.0 KB");
        check(dchat::FormatBytes(1536) == "1.5 KB", "1536 -> 1.5 KB");
        check(dchat::FormatBytes(1024 * 1024) == "1.0 MB", "1 MB");
        check(dchat::FormatBytes(5ull * 1024 * 1024 + 512 * 1024) == "5.5 MB", "5.5 MB");
    }

    {
        std::printf("[6] 分块与协议行长上限\n");
        // 一个 10 MB 的文件要切成多少块
        const unsigned long long size = 10ull * 1024 * 1024;
        const unsigned long long chunks =
            (size + dchat::kFileChunkBytes - 1) / dchat::kFileChunkBytes;
        check(chunks == 5120, "10 MB 按 2048 字节切成 5120 块");
        const unsigned long long maxChunks =
            (dchat::kMaxFileBytes + dchat::kFileChunkBytes - 1) / dchat::kFileChunkBytes;
        check(maxChunks == 32768, "64 MB 上限对应 32768 块，数量可控");

        // 最坏情况的 FILE_DATA 一行（命令名 + 最长昵称 + 最长传输 ID + 满块 Base64）
        std::vector<unsigned char> worst(dchat::kFileChunkBytes, 0xFF);
        const std::string encoded = dchat::Base64Encode(worst.data(), worst.size());
        const std::string nick(12, 'n');   // kMaxNickChars = 12
        const std::string id(64, 'i');     // 传输 ID 上限 64
        const std::string line = dchat::BuildLine("FILE_DATA", nick + " " + id + " " + encoded);
        check(line.size() <= dchat::kMaxLineBytes, "最坏情况的 FILE_DATA 不超过单行上限");
        check(line.size() <= dchat::kMaxLineBytes - 512, "而且留了 512 字节余量");

        // 服务器还要在更长的 FILE_FROM 里带 Base64 文件名
        const std::string nameEncoded = dchat::Base64Encode(std::string(300, 'x'));
        const std::string fromLine = dchat::BuildLine(
            "FILE_FROM", "23:59 " + nick + " " + id + " " + nameEncoded + " 68719476736");
        check(fromLine.size() <= dchat::kMaxLineBytes, "FILE_FROM 也在上限内");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
