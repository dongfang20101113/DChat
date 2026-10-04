"""磁盘暂存改造的第 3 部分：上传三处理器 + 下载按偏移读。"""
import io
import os

TARGET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "src", "server.cpp")

# ---------------------------------------------------------------- FILE_SEND
OLD_SEND = '''            client->upload = std::make_shared<PendingUpload>();
            client->upload->id = words[0];
            client->upload->nameB64 = words[1];
            client->upload->declared = bytes;'''

NEW_SEND = '''            // 建临时文件开始接收：**不再把文件内容攒在内存里**。
            // 这台服务器只有 1.6GB 内存，几个大文件就能把它 OOM 掉。
            if (!EnsureFilesDir()) {
                client->SendLine(Timed("ERROR", "服务器无法准备暂存目录，上传取消"));
                return true;
            }
            unsigned long long uploadSeq = 0;
            {
                std::lock_guard<std::mutex> lock(g_filesMutex);
                uploadSeq = ++g_uploadSeq;
            }
            const std::string partPath =
                g_filesDir + "/upload-" + std::to_string(uploadSeq) + kPartSuffix;
            std::ofstream partFile(partPath, std::ios::binary | std::ios::trunc);
            if (!partFile) {
                client->SendLine(Timed("ERROR", "服务器无法建立临时文件，上传取消"));
                Log("upload failed (cannot create part file): " + partPath);
                return true;
            }
            client->upload = std::make_shared<PendingUpload>();
            client->upload->id = words[0];
            client->upload->nameB64 = words[1];
            client->upload->declared = bytes;
            client->upload->partPath = partPath;
            client->upload->out = std::move(partFile);'''

# ---------------------------------------------------------------- FILE_CHUNK
OLD_CHUNK = '''            if (client->upload->data.size() + chunk.size() > client->upload->declared) {
                client->SendLine(Timed("ERROR", "收到的数据超过了声明的大小，上传已中止"));
                Log("upload aborted (too much data): " + client->nick);
                client->upload.reset();
                return true;
            }
            client->upload->data.append(chunk.begin(), chunk.end());'''

NEW_CHUNK = '''            if (client->upload->received + chunk.size() > client->upload->declared) {
                client->SendLine(Timed("ERROR", "收到的数据超过了声明的大小，上传已中止"));
                Log("upload aborted (too much data): " + client->nick);
                client->upload.reset();  // 析构会把 .part 删掉
                return true;
            }
            if (!chunk.empty()) {
                client->upload->out.write(reinterpret_cast<const char*>(chunk.data()),
                                          static_cast<std::streamsize>(chunk.size()));
                if (!client->upload->out) {
                    client->SendLine(Timed("ERROR", "写入暂存文件失败，上传已中止（磁盘可能满了）"));
                    Log("upload aborted (disk write failed): " + client->nick);
                    client->upload.reset();
                    return true;
                }
                client->upload->received += chunk.size();
            }'''

# ---------------------------------------------------------------- FILE_END
OLD_END = '''            if (upload->data.size() != upload->declared) {
                client->SendLine(Timed("ERROR", "文件不完整，上传取消（收到 " +
                                                  dchat::FormatBytes(upload->data.size()) +
                                                  "，声明 " +
                                                  dchat::FormatBytes(upload->declared) + "）"));
                Log("upload aborted (incomplete): " + client->nick);
                return true;
            }
            const std::string fileId = StoreFile(client->nick, upload->nameB64, upload->data,
                                                 upload->thumb);
            if (fileId.empty()) {
                client->SendLine(Timed("ERROR", "服务器暂时存不下这个文件，稍后再试"));
                return true;
            }'''

NEW_END = '''            if (upload->received != upload->declared) {
                client->SendLine(Timed("ERROR", "文件不完整，上传取消（收到 " +
                                                  dchat::FormatBytes(upload->received) +
                                                  "，声明 " +
                                                  dchat::FormatBytes(upload->declared) + "）"));
                Log("upload aborted (incomplete): " + client->nick);
                return true;  // upload 析构时会把 .part 删掉
            }
            upload->out.close();
            if (!upload->out) {
                client->SendLine(Timed("ERROR", "暂存文件收尾失败（磁盘可能满了），上传取消"));
                Log("upload aborted (close failed): " + client->nick);
                return true;
            }
            // 改名去掉 .part：**先把文件落定、再登记**，
            // 这样进 g_files 的路径一定指向一个完整的文件。
            const std::string partSuffix = kPartSuffix;
            const std::string finalPath =
                upload->partPath.substr(0, upload->partPath.size() - partSuffix.size());
            if (std::rename(upload->partPath.c_str(), finalPath.c_str()) != 0) {
                client->SendLine(Timed("ERROR", "暂存文件改名失败，上传取消"));
                Log("upload aborted (rename failed): " + upload->partPath);
                return true;
            }
            upload->committed = true;  // 已交给 StoredFile 管，析构不要再删它
            const std::string fileId = StoreFile(client->nick, upload->nameB64, upload->received,
                                                 finalPath, upload->thumb);
            if (fileId.empty()) {
                client->SendLine(Timed("ERROR", "服务器暂时存不下这个文件，稍后再试"));
                RemoveStoredFileQuiet(finalPath);  // 没登记成功，磁盘上那份不能留
                return true;
            }'''

# ---------------------------------------------------------------- 下载：打开文件
OLD_OPEN = '''        for (unsigned long long offset = resumeFrom; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {'''

NEW_OPEN = '''        // 打开磁盘上的暂存文件，按 offset 顺序读出来发走（内容不再驻留内存）
        std::ifstream in(file->path, std::ios::binary);
        if (!in) {
            client->SendLine(dchat::BuildLine("FILE_FAIL", file->id + " 文件已不在暂存目录里"));
            Log("download failed (staged file missing): " + file->path);
            return true;
        }
        if (resumeFrom > 0) in.seekg(static_cast<std::streamoff>(resumeFrom));
        for (unsigned long long offset = resumeFrom; ok && offset < file->size;
             offset += dchat::kFileChunkBytes) {'''

# ---------------------------------------------------------------- 下载：读一块
OLD_READ = '''            const std::string chunk = dchat::Base64Encode(
                reinterpret_cast<const unsigned char*>(file->data.data() + offset), length);'''

NEW_READ = '''            std::string buffer(length, '\\0');
            in.read(&buffer[0], static_cast<std::streamsize>(length));
            if (in.gcount() != static_cast<std::streamsize>(length)) {
                // 暂存文件在下载过程中被别的线程过期/淘汰掉了。
                // 客户端会收到不完整的数据并可以重试 —— 现在有断点续传，代价不大。
                Log("download read failed (staged file gone?): " + file->id);
                ok = false;
                break;
            }
            const std::string chunk = dchat::Base64Encode(
                reinterpret_cast<const unsigned char*>(buffer.data()), length);'''


def main():
    with io.open(TARGET, encoding="utf-8", newline="") as fh:
        text = fh.read()
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")

    ok = 0
    for old, new, label in [
        (OLD_SEND, NEW_SEND, "FILE_SEND 建 .part"),
        (OLD_CHUNK, NEW_CHUNK, "FILE_CHUNK 追加写盘"),
        (OLD_END, NEW_END, "FILE_END 校验+改名+登记"),
        (OLD_OPEN, NEW_OPEN, "下载打开文件"),
        (OLD_READ, NEW_READ, "下载按偏移读"),
    ]:
        if old not in text:
            print("  ⚠ 未匹配:", label)
            continue
        text = text.replace(old, new, 1)
        ok += 1
        print("  ✅", label)

    # 剩下的 upload->data.size() 是广播/日志里的字节数，改用 received
    n1 = text.count("std::to_string(upload->data.size()) + \" \" +")
    text = text.replace("std::to_string(upload->data.size()) + \" \" +",
                        "std::to_string(upload->received) + \" \" +")
    n2 = text.count("dchat::FormatBytes(upload->data.size()) + \"）\");")
    text = text.replace("dchat::FormatBytes(upload->data.size()) + \"）\");",
                        "dchat::FormatBytes(upload->received) + \"）\");")
    print("  ✅ 字节数引用改用 received（%d + %d 处）" % (n1, n2))

    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("共 %d / 5 处" % ok)


if __name__ == "__main__":
    main()
