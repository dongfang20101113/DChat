// 附件（FileOffer）的解析与显示。纯逻辑，不依赖网络/文件系统，便于单测。
#pragma once

#include <string>

namespace dchat {

/** 收到的一条附件信息（FILE_OFFER 解析结果）。 */
struct FileOffer {
    std::string owner;         // 上传者昵称（老服务器可能不发这一格）
    std::string id;            // 服务器分配的附件 ID（形如 F1）
    std::string name;          // 已从 Base64 解出来的显示名
    unsigned long long size = 0;
    std::string kind;          // 空/0 = 普通文件；voice = 语音；1/sticker = 贴纸
    bool hasThumbnail = false;
    bool valid = false;

    bool IsVoice() const;
    /** 显示用："📎 文件 名字 (1.2 MB)"。 */
    std::string Describe() const;
};

/**
 * 解析 FILE_OFFER 的字段。
 *
 * 服务器实际发的顺序（**按位置**，错一格就会把昵称当成 ID）：
 *
 *     FILE_OFFER <上传者> <附件ID> <文件名B64> <字节数> <有缩略图 0|1> [种类]
 *
 * 第一格"上传者"是最容易漏的：漏了就会把昵称当成附件 ID、把 ID 当成文件名去
 * Base64 解码（一定失败），表现是"附件卡片不显示"。这里做兼容：**按位置和内容
 * 双重判断**——如果第二格长得像附件 ID（字母+数字、长度合理）而第三格是合法
 * Base64，就认为带上传者；否则退回不带上传者的老写法。
 */
FileOffer ParseFileOffer(const std::string& rest);

}  // namespace dchat
