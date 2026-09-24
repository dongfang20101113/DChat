// 账号与密码：注册、登录校验、账号文件的序列化。
//
// 密码不会明文保存，而是保存"随机盐 + PBKDF2-HMAC-SHA256 派生值"：
// 校验时用同样的盐和迭代次数重新推导再比对，因此文件被看到也无法直接得到原密码。
// 派生用 Windows 自带的加密库（bcrypt.dll），账号文件本身是纯文本，便于检视与测试。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dchat {

inline constexpr int kDefaultIterations = 12000;  // PBKDF2 迭代次数
inline constexpr std::size_t kMinPasswordChars = 6;
inline constexpr std::size_t kMaxPasswordChars = 64;

struct UserRecord {
    std::string name;         // 用户名（同时作为聊天昵称）
    std::string saltHex;      // 随机盐（十六进制）
    std::string hashHex;      // PBKDF2 派生值（十六进制）
    int iterations = kDefaultIterations;
};

enum class PasswordError { None, Empty, TooShort, TooLong, HasSpace };

// 密码规则：6-64 个字符、不能有空格（协议是一行一条、用空格分隔字段）
bool ValidatePassword(const std::string& password, PasswordError* error = nullptr);
const char* PasswordErrorText(PasswordError error);

// 生成随机盐（十六进制字符串）
std::string MakeSaltHex();

// PBKDF2-HMAC-SHA256，返回十六进制
std::string DeriveHashHex(const std::string& password, const std::string& saltHex, int iterations);

// 用密码创建一个账号记录
UserRecord MakeUser(const std::string& name, const std::string& password,
                    int iterations = kDefaultIterations);

// 校验密码是否匹配
bool CheckPassword(const UserRecord& user, const std::string& password);

// 改密码：重新生成盐并派生新的哈希（旧密码立刻失效）。
// 账号不存在返回 false。
bool ChangeUserPassword(std::vector<UserRecord>* users, const std::string& name,
                        const std::string& newPassword);

// ---- 账号文件（纯文本，一行一个账号：名字 盐 派生值 迭代次数）----
std::string SerializeUsers(const std::vector<UserRecord>& users);
bool ParseUsers(const std::string& text, std::vector<UserRecord>* users);

// 按名字查找（找不到返回 nullptr）
const UserRecord* FindUser(const std::vector<UserRecord>& users, const std::string& name);

}  // namespace dchat
