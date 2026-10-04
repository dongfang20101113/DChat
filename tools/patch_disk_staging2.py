"""补上第一版补丁没匹配上的三处（缩进是 4 空格，不是 8）。

教训：Select-String -Context 的输出会带额外填充，**不要拿它当锚点原文**，
要用 read 工具读出来的内容。
"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

ANCHOR = '''// 清掉过期文件；返回被清掉的描述（用于日志）
std::vector<std::string> ExpireFilesLocked() {'''

HELPERS = '''// ---- 暂存目录 ----
// 放在工作目录下，和 dchat-history.txt 一致：管理员知道去哪找，也方便单独做磁盘配额。
std::string g_filesDir = "dchat-files";
// 上传临时文件的序号，只用来保证 .part 名字唯一（正式附件的名字由 StoredFile::id 决定）
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

// 删掉一个暂存文件；删不掉只记日志 —— 清理失败不该影响主流程。
void RemoveStoredFileQuiet(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) Log("failed to remove staged file " + path + "：" + ec.message());
}

// 启动时清空暂存目录。
// 文件本来就不跨重启（和以前"内存暂存"的行为一致：重启后旧附件不可下载），
// 顺带把上次崩溃留下的 .part 残留一起清掉。
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

OLD_EXPIRE = '''        if ((*it)->expires <= now) {
            expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
            it = g_files.erase(it);'''

NEW_EXPIRE = '''        if ((*it)->expires <= now) {
            expired.push_back((*it)->id + "（" + (*it)->owner + " 上传）");
            // 磁盘上那份也要删，否则"过期"只清内存、文件会一直堆在磁盘上
            RemoveStoredFileQuiet((*it)->path);
            it = g_files.erase(it);'''

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
        // 淘汰时把磁盘文件一起删掉，否则"腾地方"只腾了内存、磁盘越攒越多
        RemoveStoredFileQuiet(g_files.front()->path);
        g_files.erase(g_files.begin());
    }
    auto file = std::make_shared<StoredFile>();
    file->id = "F" + std::to_string(++g_fileSeq);
    file->owner = owner;
    file->nameB64 = nameB64;
    file->size = size;
    file->path = path;  // 内容已经在磁盘上了，这里只登记位置
    file->thumb = thumb;'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as fh:
        text = fh.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    ok = 0
    for old, new, label in [
        (ANCHOR, HELPERS, "新增磁盘辅助函数"),
        (OLD_EXPIRE, NEW_EXPIRE, "过期时删磁盘文件"),
        (OLD_STORE, NEW_STORE, "StoreFile 改收路径"),
    ]:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        ok += 1
        print("  ✅", label)

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("共 %d / 3 处" % ok)


if __name__ == "__main__":
    main()
