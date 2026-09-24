#include "auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>

namespace dchat {

namespace {

constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;

std::string ToHex(const std::vector<unsigned char>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(digits[(byte >> 4) & 0xF]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

bool FromHex(const std::string& hex, std::vector<unsigned char>* bytes) {
    if (hex.size() % 2 != 0) return false;
    bytes->clear();
    bytes->reserve(hex.size() / 2);
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = value(hex[i]);
        const int low = value(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes->push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool DeriveBytes(const std::string& password, const std::vector<unsigned char>& salt, int iterations,
                 std::vector<unsigned char>* out) {
    if (iterations <= 0) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }
    out->assign(kHashBytes, 0);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iterations), out->data(),
        static_cast<ULONG>(out->size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        std::string line =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return lines;
}

}  // namespace

bool ValidatePassword(const std::string& password, PasswordError* error) {
    auto fail = [&](PasswordError kind) {
        if (error) *error = kind;
        return false;
    };
    if (password.empty()) return fail(PasswordError::Empty);
    if (password.size() < kMinPasswordChars) return fail(PasswordError::TooShort);
    if (password.size() > kMaxPasswordChars) return fail(PasswordError::TooLong);
    for (char c : password) {
        if (c == ' ' || c == '\t') return fail(PasswordError::HasSpace);
    }
    if (error) *error = PasswordError::None;
    return true;
}

const char* PasswordErrorText(PasswordError error) {
    switch (error) {
        case PasswordError::Empty:
            return "密码不能为空";
        case PasswordError::TooShort:
            return "密码至少 6 个字符";
        case PasswordError::TooLong:
            return "密码最多 64 个字符";
        case PasswordError::HasSpace:
            return "密码不能包含空格";
        case PasswordError::None:
        default:
            return "";
    }
}

std::string MakeSaltHex() {
    std::vector<unsigned char> salt(kSaltBytes, 0);
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // 极端情况下退化成基于时间的盐（仍然比固定盐好）
        const auto ticks = static_cast<unsigned long long>(GetTickCount64());
        for (std::size_t i = 0; i < salt.size(); ++i) {
            salt[i] = static_cast<unsigned char>((ticks >> ((i % 8) * 8)) & 0xFF);
        }
    }
    return ToHex(salt);
}

std::string DeriveHashHex(const std::string& password, const std::string& saltHex, int iterations) {
    std::vector<unsigned char> salt;
    if (!FromHex(saltHex, &salt)) return std::string();
    std::vector<unsigned char> hash;
    if (!DeriveBytes(password, salt, iterations, &hash)) return std::string();
    return ToHex(hash);
}

UserRecord MakeUser(const std::string& name, const std::string& password, int iterations) {
    UserRecord user;
    user.name = name;
    user.iterations = iterations;
    user.saltHex = MakeSaltHex();
    user.hashHex = DeriveHashHex(password, user.saltHex, iterations);
    return user;
}

bool CheckPassword(const UserRecord& user, const std::string& password) {
    const std::string candidate = DeriveHashHex(password, user.saltHex, user.iterations);
    if (candidate.empty() || candidate.size() != user.hashHex.size()) return false;
    // 定长比较，不提前退出
    unsigned char diff = 0;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        diff |= static_cast<unsigned char>(candidate[i] ^ user.hashHex[i]);
    }
    return diff == 0;
}

bool ChangeUserPassword(std::vector<UserRecord>* users, const std::string& name,
                        const std::string& newPassword) {
    if (!users) return false;
    for (UserRecord& user : *users) {
        if (user.name != name) continue;
        user = MakeUser(name, newPassword);  // 换一个盐，旧密码立刻失效
        return true;
    }
    return false;
}

std::string SerializeUsers(const std::vector<UserRecord>& users) {
    std::string out;
    out += "# dchat-server accounts (v1): name salt pbkdf2-hash iterations\n";
    for (const UserRecord& user : users) {
        out += user.name + " " + user.saltHex + " " + user.hashHex + " " +
               std::to_string(user.iterations) + "\n";
    }
    return out;
}

bool ParseUsers(const std::string& text, std::vector<UserRecord>* users) {
    if (!users) return false;
    users->clear();
    for (const std::string& line : SplitLines(text)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            const std::size_t begin = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > begin) fields.push_back(line.substr(begin, i - begin));
        }
        if (fields.size() < 3) continue;  // 坏行直接跳过
        UserRecord user;
        user.name = fields[0];
        user.saltHex = fields[1];
        user.hashHex = fields[2];
        if (fields.size() > 3) {
            try {
                user.iterations = std::stoi(fields[3]);
            } catch (...) {
                user.iterations = kDefaultIterations;
            }
        }
        if (user.iterations <= 0) user.iterations = kDefaultIterations;
        if (user.name.empty() || user.saltHex.empty() || user.hashHex.empty()) continue;
        // 盐和派生值必须是合法的十六进制，否则视为坏行
        std::vector<unsigned char> probe;
        if (!FromHex(user.saltHex, &probe) || probe.empty()) continue;
        if (!FromHex(user.hashHex, &probe) || probe.empty()) continue;
        users->push_back(user);
    }
    return true;
}

const UserRecord* FindUser(const std::vector<UserRecord>& users, const std::string& name) {
    for (const UserRecord& user : users) {
        if (user.name == name) return &user;
    }
    return nullptr;
}

}  // namespace dchat
