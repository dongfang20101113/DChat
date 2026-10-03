// Linux 客户端的 TOFU（首次信任）判定。
//
// 语义和 Windows 端、安卓端一致：记住第一次见到的服务器指纹，以后每次比对。
//   - 没见过（known 为空）      -> 首次，接受并记下来
//   - 和记住的一样               -> 没变，静默通过
//   - 和记住的不一样             -> **警告**（可能是换了密钥，也可能是有人劫持）
//
// 做成纯函数是为了能单测：这段逻辑一旦错了，"中间人换钥匙"就悄无声息地通过了。
#pragma once

#include <string>

namespace dchat {

enum class TrustKind {
    FirstUse,  // 第一次见
    Unchanged, // 和记住的一致
    Changed,   // 变了（要警告）
    None,      // 没指纹可比（明文连接）
};

struct TrustDecision {
    TrustKind kind = TrustKind::None;
    std::string known;
    std::string current;
};

TrustDecision DecideTrust(const std::string& known, const std::string& current);

/** 给用户看的一句话说明。 */
std::string DescribeTrust(const TrustDecision& decision);

/** 备忘文件：`已知服务器指纹` 那一行的读写（键是 `主机:端口`）。 */
std::string KnownServerKey(const std::string& host, int port);
/** 从文件内容里取出某个键记住的指纹；没有返回空串。 */
std::string LookupKnownFingerprint(const std::string& fileContent, const std::string& key);
/** 更新文件内容：同一个键覆盖，没有就追加。 */
std::string UpsertKnownFingerprint(const std::string& fileContent, const std::string& key,
                                   const std::string& fingerprint);

}  // namespace dchat
