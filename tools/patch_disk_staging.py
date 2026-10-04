"""把 dchat 服务端的文件数据从内存改成磁盘暂存。

为什么必须改：StoredFile::data 是 std::string，整个文件内容都驻留内存。
在只有 1.6GB 内存的阿里云轻量服务器上，传几个大文件就会被 OOM 杀掉
（原来的闸门 maxservertemp=32768MB 是物理内存的 20 倍，形同虚设）。

设计：
  - 上传：FILE_SEND 就建 dchat-files/upload-<n>.part，FILE_CHUNK 直接追加写，
    收齐后改名去掉 .part，再把**路径**登记进 StoredFile（不再碰内容）。
  - 下载：FILE_GET 打开文件、seekg 到 offset，按块读。
  - 清理：过期/超预算/被淘汰时 unlink 磁盘文件，否则磁盘只涨不降。
  - 启动：清空暂存目录（文件本来就不跨重启，和以前"内存暂存"行为一致），
    顺便清掉上次崩溃留下的 .part。
  - 缩略图仍然放内存：只有几 KB，而且下载/预览路径到处都在用它。

用 std::filesystem / std::ifstream / std::ofstream 而不是 POSIX open/read：
服务端在 Windows、Linux、macOS 三端都要编译。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

# ---------------------------------------------------------------- 1) 两个结构体
OLD_STRUCTS = '''// ---- 文件暂存（QQ 式"点击下载"）----
// 发送方先把文件传给服务器，服务器在**内存**里暂存一段时间；其他人在卡片上点「下载」
// 时才把数据发给他（FILE_GET）。过期或超出容量会删掉，所以服务器不会无限膨胀。
struct PendingUpload {          // 正在上传的文件（每个连接最多一个）
    std::string id;             // 客户端自己的传输 ID（只用于日志）
    std::string nameB64;
    unsigned long long declared = 0;
    std::string data;
    std::string thumb;          // 缩略图（PNG，图片缩小图 / 视频第一帧），可能为空
    bool expectThumb = false;   // 发送方声明"会带缩略图"
    // 发送方声明的**附件种类**，原样透传给接收方。
    //   0 / file = 普通文件   1 / sticker = 贴纸   voice = 语音消息
    // 服务端不解释这个值——它只负责透传。三种附件走完全相同的传输，
    // 区别只在客户端收到后怎么显示、以及要不要自动下载。
    std::string kind = "0";
};

struct StoredFile {             // 已经传完、等人下载的文件
    std::string id;             // 服务器分配（F1、F2…）
    std::string owner;
    std::string nameB64;
    unsigned long long size = 0;
    std::string data;
    std::string thumb;          // 缩略图（PNG），可能为空
    std::chrono::steady_clock::time_point expires;
};'''

NEW_STRUCTS = '''// ---- 文件暂存（QQ 式"点击下载"）----
// 发送方先把文件传给服务器，服务器**在磁盘上**暂存一段时间；其他人在卡片上点「下载」
// 时才把数据发给他（FILE_GET）。过期或超出容量会连磁盘文件一起删掉。
//
// 为什么是磁盘而不是内存：原来 StoredFile::data 是 std::string，整个文件内容
// 都驻留内存。这台服务器只有 1.6GB 内存，传几个大文件就会被 OOM 杀掉
// （而且当时 maxservertemp 设成了 32GB，是物理内存的 20 倍，那道闸门形同虚设）。
// 语音消息也是走这条路径（kind=voice），所以一起落盘。
struct PendingUpload {          // 正在上传的文件（每个连接最多一个）
    std::string id;             // 客户端自己的传输 ID（只用于日志）
    std::string nameB64;
    unsigned long long declared = 0;
    // ---- 磁盘暂存 ----
    // FILE_SEND 时建 .part 文件，FILE_CHUNK 直接往里追加。收齐后改名去掉 .part
    // 并把路径登记进 StoredFile；**半途中断（断线、取消、校验失败）由析构删掉**，
    // 这样无论从哪条路径退出，都不会在磁盘上留下垃圾。
    std::string partPath;
    std::ofstream out;
    unsigned long long received = 0;  // 已落盘字节数，用来校验完整性
    bool committed = false;           // 改名成功、已交给 StoredFile 管，析构不再删
    std::string thumb;          // 缩略图（PNG，图片缩小图 / 视频第一帧），可能为空
    bool expectThumb = false;   // 发送方声明"会带缩略图"
    // 发送方声明的**附件种类**，原样透传给接收方。
    //   0 / file = 普通文件   1 / sticker = 贴纸   voice = 语音消息
    // 服务端不解释这个值——它只负责透传。三种附件走完全相同的传输，
    // 区别只在客户端收到后怎么显示、以及要不要自动下载。
    std::string kind = "0";

    PendingUpload() = default;
    PendingUpload(const PendingUpload&) = delete;
    PendingUpload& operator=(const PendingUpload&) = delete;
    ~PendingUpload() {
        if (out.is_open()) out.close();
        // committed 为假 = 这份数据没有变成正式附件，磁盘上那份必须清掉
        if (!committed && !partPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(partPath, ec);
        }
    }
};

struct StoredFile {             // 已经传完、等人下载的文件
    std::string id;             // 服务器分配（F1、F2…）
    std::string owner;
    std::string nameB64;
    unsigned long long size = 0;
    std::string path;           // 文件内容在磁盘上的位置（dchat-files/upload-N）
    std::string thumb;          // 缩略图（PNG，仍放内存：只有几 KB，且预览路径到处在用）
    std::chrono::steady_clock::time_point expires;
};'''

# ---------------------------------------------------------------- 2) 磁盘辅助函数
ANCHOR_HELPERS = '''    // 清掉过期文件；返回被清掉的描述（用于日志）
    std::vector<std::string> ExpireFilesLocked() {'''

NEW_HELPERS = '''    // ---- 暂存目录 ----
    // 放在工作目录下，和 dchat-history.txt 一致：管理员知道去哪找、也方便做磁盘配额。
    std::string g_filesDir = "dchat-files";
    /// 上传临时文件的序号，只用来保证 .part 名字唯一（正式附件的名字由 StoredFile::id 决定）
    unsigned long long g_uploadSeq = 0;
    constexpr const char* kPartSuffix = ".part";

    bool EnsureFilesDir() {
        std::error_code ec;
        std::filesystem::create_directories(g_filesDir, ec);
        if (ec) {
            Log("cannot create file staging dir " + g_filesDir + "：" + ec.message());
            return false;
        }
        return true;
    }

    /// 删掉一个暂存文件；删不掉只记日志 —— 清理失败不该影响主流程。
    void RemoveStoredFileQuiet(const std::string& path) {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) Log("failed to remove staged file " + path + "：" + ec.message());
    }

    /// 启动时清空暂存目录。
    /// 文件本来就不跨重启（和以前"内存暂存"的行为一致，重启后旧文件不可下载），
    /// 顺带把上次崩溃留下的 .part 残留一起清掉。
    void ClearFilesDir() {
        std::error_code ec;
        std::size_t removed = 0;
        if (std::filesystem::exists(g_filesDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(g_filesDir, ec)) {
                std::error_code rm;
                if (std::filesystem::remove_all(entry.path(), rm)) ++removed;
            }
        }
        if (removed > 0) {
            Log("cleared " + std::to_string(removed) + " leftover file(s) in " + g_filesDir +
                "（暂存不跨重启）");
        }
        EnsureFilesDir();
    }

    // 清掉过期文件；返回被清掉的描述（用于日志）
    std::vector<std::string> ExpireFilesLocked() {'''

# ---------------------------------------------------------------- 3) 过期时删磁盘文件
OLD_EXPIRE = '''            if ((*it)->expires <= now) {
                expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
                it = g_files.erase(it);'''

NEW_EXPIRE = '''            if ((*it)->expires <= now) {
                expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
                // 磁盘上那份也要删，否则过期只清内存、文件会一直堆着
                RemoveStoredFileQuiet((*it)->path);
                it = g_files.erase(it);'''

# ---------------------------------------------------------------- 4) StoreFile
OLD_STORE = '''std::string StoreFile(const std::string& owner, const std::string& nameB64,
                          const std::string& data, const std::string& thumb) {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        for (const std::string& line : ExpireFilesLocked()) Log("stored file expired: " + line);
        const unsigned long long budget = TempBudgetBytes();
        if (data.size() > budget) return std::string();  // 单个就超总量，存不下
        // 腾地方：先按最旧的删，直到数量和总量都满足
        while (!g_files.empty() && (g_files.size() >= kMaxStoredFiles ||
                                    StoredBytesLocked() + data.size() > budget)) {
            Log("stored file evicted (no room): " + g_files.front()->id + "（" +
                g_files.front()->owner + " 上传）");
            g_files.erase(g_files.begin());
        }
        auto file = std::make_shared<StoredFile>();
        file->id = "F" + std::to_string(++g_fileSeq);
        file->owner = owner;
        file->nameB64 = nameB64;
        file->size = data.size();
        file->data = data;
        file->thumb = thumb;'''

NEW_STORE = '''std::string StoreFile(const std::string& owner, const std::string& nameB64,
                          unsigned long long size, const std::string& path,
                          const std::string& thumb) {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        for (const std::string& line : ExpireFilesLocked()) Log("stored file expired: " + line);
        const unsigned long long budget = TempBudgetBytes();
        if (size > budget) return std::string();  // 单个就超总量，存不下
        // 腾地方：先按最旧的删，直到数量和总量都满足
        while (!g_files.empty() && (g_files.size() >= kMaxStoredFiles ||
                                    StoredBytesLocked() + size > budget)) {
            Log("stored file evicted (no room): " + g_files.front()->id + "（" +
                g_files.front()->owner + " 上传）");
            // 淘汰时把磁盘文件一起删掉，不然"腾地方"只腾了内存、磁盘越攒越多
            RemoveStoredFileQuiet(g_files.front()->path);
            g_files.erase(g_files.begin());
        }
        auto file = std::make_shared<StoredFile>();
        file->id = "F" + std::to_string(++g_fileSeq);
        file->owner = owner;
        file->nameB64 = nameB64;
        file->size = size;
        file->path = path;   // 内容已经在磁盘上了，这里只登记位置
        file->thumb = thumb;'''

# ---------------------------------------------------------------- 5) EnforceTempBudget
OLD_BUDGET = '''        while (!g_files.empty() && StoredBytesLocked() > budget) {
            Log("stored file evicted (over maxservertemp): " + g_files.front()->id);
            g_files.erase(g_files.begin());
        }'''

NEW_BUDGET = '''        while (!g_files.empty() && StoredBytesLocked() > budget) {
            Log("stored file evicted (over maxservertemp): " + g_files.front()->id);
            RemoveStoredFileQuiet(g_files.front()->path);
            g_files.erase(g_files.begin());
        }'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as fh:
        text = fh.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    if "g_filesDir" in text:
        raise SystemExit("看起来已经打过补丁了")

    edits = [
        (OLD_STRUCTS, NEW_STRUCTS, "两个结构体改磁盘"),
        (ANCHOR_HELPERS, NEW_HELPERS, "新增磁盘辅助函数"),
        (OLD_EXPIRE, NEW_EXPIRE, "过期时删磁盘文件"),
        (OLD_STORE, NEW_STORE, "StoreFile 改收路径"),
        (OLD_BUDGET, NEW_BUDGET, "超预算淘汰时删磁盘文件"),
    ]
    ok = 0
    for old, new, label in edits:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        ok += 1
        print("  ✅", label)

    # 补 include
    if "#include <filesystem>" not in text:
        text = text.replace("#include <fstream>", "#include <filesystem>\n#include <fstream>", 1)
        print("  ✅ include <filesystem>")
    if "#include <cstdio>" not in text:
        text = text.replace("#include <filesystem>", "#include <cstdio>\n#include <filesystem>", 1)
        print("  ✅ include <cstdio>")

    # main：启动时清空暂存目录
    if "ClearFilesDir();" not in text:
        text = text.replace("    LoadRules();", "    LoadRules();\n    ClearFilesDir();", 1)
        print("  ✅ main 调用 ClearFilesDir")

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("共 %d / %d 处" % (ok, len(edits)))


if __name__ == "__main__":
    main()
