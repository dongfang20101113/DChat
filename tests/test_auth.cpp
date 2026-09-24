// 账号与密码单元测试：密码规则、加盐哈希的确定性、账号文件读写。
#include <cstdio>
#include <string>
#include <vector>

#include "auth.h"

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
}  // namespace

int main() {
    std::printf("== dchat auth tests ==\n");

    {
        std::printf("[1] 密码规则\n");
        dchat::PasswordError error = dchat::PasswordError::None;
        check(dchat::ValidatePassword("secret123", &error) && error == dchat::PasswordError::None,
              "6 位以上、无空格 → 通过");
        check(!dchat::ValidatePassword("", &error) && error == dchat::PasswordError::Empty,
              "空密码被拒绝");
        check(!dchat::ValidatePassword("abc", &error) && error == dchat::PasswordError::TooShort,
              "太短被拒绝");
        check(!dchat::ValidatePassword(std::string(65, 'a'), &error) &&
                  error == dchat::PasswordError::TooLong,
              "太长被拒绝");
        check(!dchat::ValidatePassword("abc 123", &error) &&
                  error == dchat::PasswordError::HasSpace,
              "含空格被拒绝");
        check(std::string(dchat::PasswordErrorText(dchat::PasswordError::TooShort)).size() > 0,
              "错误提示非空");
    }

    {
        std::printf("[2] 加盐哈希\n");
        const std::string password = "my-secret";
        const dchat::UserRecord user = dchat::MakeUser("alice", password);
        check(!user.saltHex.empty() && !user.hashHex.empty(), "账号记录带盐与派生值");
        check(user.hashHex.find(password) == std::string::npos, "记录里不含明文密码");
        check(dchat::CheckPassword(user, password), "正确密码校验通过");
        check(!dchat::CheckPassword(user, "my-secret2"), "错误密码校验失败");
        check(!dchat::CheckPassword(user, ""), "空密码校验失败");

        const std::string again = dchat::DeriveHashHex(password, user.saltHex, user.iterations);
        check(again == user.hashHex, "同样的盐与迭代次数得到同样的派生值");
        const std::string other = dchat::DeriveHashHex(password, dchat::MakeSaltHex(),
                                                      user.iterations);
        check(other != user.hashHex, "换一个盐，派生值不同");
        const std::string weaker = dchat::DeriveHashHex(password, user.saltHex, 1);
        check(weaker != user.hashHex, "迭代次数不同，派生值不同");

        const dchat::UserRecord second = dchat::MakeUser("alice", password);
        check(second.saltHex != user.saltHex, "同一个密码两次注册的盐不同（避免彩虹表）");
    }

    {
        std::printf("[3] 账号文件读写\n");
        std::vector<dchat::UserRecord> users;
        users.push_back(dchat::MakeUser("alice", "password1"));
        users.push_back(dchat::MakeUser("小雷", "password2"));
        const std::string text = dchat::SerializeUsers(users);
        check(text.find("alice") != std::string::npos && text.find("小雷") != std::string::npos,
              "序列化包含用户名");
        check(text.find("password1") == std::string::npos, "序列化不含明文密码");

        std::vector<dchat::UserRecord> parsed;
        check(dchat::ParseUsers(text, &parsed), "解析成功");
        check(parsed.size() == 2, "解析出两条账号");
        check(parsed[0].name == "alice" && parsed[0].saltHex == users[0].saltHex &&
                  parsed[0].hashHex == users[0].hashHex &&
                  parsed[0].iterations == users[0].iterations,
              "往返后字段一致");
        check(dchat::CheckPassword(parsed[1], "password2"), "解析出来的账号仍能校验密码");
        check(parsed[1].name == "小雷", "中文用户名正确");

        check(dchat::FindUser(parsed, "alice") != nullptr, "能按名字找到账号");
        check(dchat::FindUser(parsed, "nobody") == nullptr, "找不到时返回空");
    }

    {
        std::printf("[4] 坏文件容错\n");
        std::vector<dchat::UserRecord> parsed;
        const std::string text =
            "# comment line\n"
            "\n"
            "broken-line\n"
            "alice 00112233445566778899aabbccddeeff aabbcc 100\n"
            "too few fields\n";
        check(dchat::ParseUsers(text, &parsed), "解析坏文件不崩溃");
        check(parsed.size() == 1, "只保留完整的一行");
        check(parsed[0].iterations == 100, "迭代次数被读出来");
        const std::string noIterations = "bob 00112233445566778899aabbccddeeff deadbeef\n";
        check(dchat::ParseUsers(noIterations, &parsed) && parsed.size() == 1 &&
                  parsed[0].iterations == dchat::kDefaultIterations,
              "缺少迭代次数时用默认值");
        check(dchat::ParseUsers("", &parsed) && parsed.empty(), "空文件解析成空列表");
    }

    {
        std::printf("[5] 改密码\n");
        std::vector<dchat::UserRecord> users;
        users.push_back(dchat::MakeUser("alice", "oldpass123"));
        users.push_back(dchat::MakeUser("bob", "bobpass123"));
        const std::string oldSalt = users[0].saltHex;

        check(!dchat::ChangeUserPassword(&users, "nobody", "newpass123"), "账号不存在时返回失败");
        check(!dchat::ChangeUserPassword(nullptr, "alice", "newpass123"), "空列表指针不崩溃");
        check(dchat::ChangeUserPassword(&users, "alice", "newpass123"), "改密码成功");
        check(!dchat::CheckPassword(users[0], "oldpass123"), "旧密码立刻失效");
        check(dchat::CheckPassword(users[0], "newpass123"), "新密码可以登录");
        check(users[0].saltHex != oldSalt, "改密码会换一个新的盐");
        check(dchat::CheckPassword(users[1], "bobpass123"), "改一个人的密码不影响别人");

        const std::string text = dchat::SerializeUsers(users);
        check(text.find("newpass123") == std::string::npos, "账号文件里依然没有明文密码");
        std::vector<dchat::UserRecord> reloaded;
        check(dchat::ParseUsers(text, &reloaded) && reloaded.size() == 2, "改完密码后文件仍能解析");
        const dchat::UserRecord* found = dchat::FindUser(reloaded, "alice");
        check(found != nullptr && dchat::CheckPassword(*found, "newpass123"),
              "写盘再读回来仍能用新密码登录");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
