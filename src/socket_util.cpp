#include "socket_util.h"

#include <cstring>

namespace dchat {
namespace sock {

namespace {

#ifdef _WIN32
// 超时参数在 Windows 上就是 DWORD 毫秒
bool SetTimeout(Handle handle, int option, int milliseconds) {
    if (milliseconds <= 0) return true;
    const DWORD value = static_cast<DWORD>(milliseconds);
    return ::setsockopt(handle, SOL_SOCKET, option, reinterpret_cast<const char*>(&value),
                        sizeof(value)) != SOCKET_ERROR;
}
#else
// Linux 上要的是 struct timeval
bool SetTimeout(Handle handle, int option, int milliseconds) {
    if (milliseconds <= 0) return true;
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    return ::setsockopt(handle, SOL_SOCKET, option, &value, sizeof(value)) == 0;
}
#endif

}  // namespace

#ifdef _WIN32

bool Startup() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void Cleanup() { WSACleanup(); }

void Close(Handle handle) {
    if (handle == kInvalid) return;
    ::closesocket(handle);
}

void ShutdownWrite(Handle handle) {
    if (handle != kInvalid) ::shutdown(handle, SD_SEND);
}

Handle CreateTcp() { return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }

// Windows 没有 SIGPIPE，空实现
bool SetNoSigpipe(Handle handle) {
    (void)handle;
    return true;
}

bool SetReuseAddress(Handle handle) {
    const BOOL on = TRUE;
    return ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                        sizeof(on)) != SOCKET_ERROR;
}

bool SetReceiveTimeout(Handle handle, int milliseconds) {
    return SetTimeout(handle, SO_RCVTIMEO, milliseconds);
}

bool SetSendTimeout(Handle handle, int milliseconds) {
    return SetTimeout(handle, SO_SNDTIMEO, milliseconds);
}

bool SetNoDelay(Handle handle) {
    const BOOL on = TRUE;
    return ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
                        sizeof(on)) != SOCKET_ERROR;
}

bool Bind(Handle handle, const Address& address) {
    return ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
           SOCKET_ERROR;
}

bool Listen(Handle handle, int backlog) { return ::listen(handle, backlog) != SOCKET_ERROR; }

Handle Accept(Handle listener, Address* peer) {
    if (!peer) return ::accept(listener, nullptr, nullptr);
    int length = sizeof(Address);
    return ::accept(listener, reinterpret_cast<sockaddr*>(peer), &length);
}

std::ptrdiff_t Send(Handle handle, const void* data, std::size_t length) {
    const int written =
        ::send(handle, static_cast<const char*>(data), static_cast<int>(length), 0);
    return written == SOCKET_ERROR ? -1 : written;
}

std::ptrdiff_t Receive(Handle handle, void* buffer, std::size_t length) {
    const int got = ::recv(handle, static_cast<char*>(buffer), static_cast<int>(length), 0);
    return got == SOCKET_ERROR ? -1 : got;
}

bool WouldBlock() {
    const int code = WSAGetLastError();
    return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT;
}

int LastError() { return WSAGetLastError(); }

std::string ErrorText(int code) {
    switch (code) {
        case WSAEWOULDBLOCK:
            return "操作会阻塞（或超时）";
        case WSAETIMEDOUT:
            return "连接超时";
        case WSAECONNRESET:
            return "连接被对端重置";
        case WSAECONNABORTED:
            return "连接中断";
        case WSAHOST_NOT_FOUND:
            return "找不到主机";
        default:
            return "错误码 " + std::to_string(code);
    }
}

std::string PeerIp(const Address& address) { return ::inet_ntoa(address.sin_addr); }

Address MakeAddress(const std::string& host, int port) {
    Address address{};
    address.sin_family = AF_INET;
    // 注意：**不能写 ::htons**。macOS 上 htons 是宏
    // （#define htons(x) __DARWIN_OSSwapInt16(x)），加了 :: 会展开成
    // "::((__uint16_t)...)" 这种非法语法。Windows / Linux 上它是函数，
    // 加不加 :: 都行 —— 所以统一不加，三端通吃。
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        address.sin_addr.s_addr = ::inet_addr(host.c_str());
    }
    return address;
}

#else  // POSIX

bool Startup() { return true; }  // POSIX 不需要初始化
void Cleanup() {}

void Close(Handle handle) {
    if (handle == kInvalid) return;
    ::close(handle);
}

void ShutdownWrite(Handle handle) {
    if (handle != kInvalid) ::shutdown(handle, SHUT_WR);
}

Handle CreateTcp() { return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }

bool SetNoSigpipe(Handle handle) {
#ifdef __APPLE__
    // macOS 没有 MSG_NOSIGNAL，只能给 socket 设 SO_NOSIGPIPE。
    // 不设的话：往已断开的连接写 -> SIGPIPE -> 进程直接被杀。
    const int on = 1;
    return ::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
    // Linux 上用 send 的 MSG_NOSIGNAL 标志解决，这里不用做事。
    // 但入口保留：调用点不必区分平台。
    (void)handle;
    return true;
#endif
}

bool SetReuseAddress(Handle handle) {
    const int on = 1;
    return ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
}

bool SetReceiveTimeout(Handle handle, int milliseconds) {
    return SetTimeout(handle, SO_RCVTIMEO, milliseconds);
}

bool SetSendTimeout(Handle handle, int milliseconds) {
    return SetTimeout(handle, SO_SNDTIMEO, milliseconds);
}

bool SetNoDelay(Handle handle) {
    const int on = 1;
    return ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
}

bool Bind(Handle handle, const Address& address) {
    return ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

bool Listen(Handle handle, int backlog) { return ::listen(handle, backlog) == 0; }

Handle Accept(Handle listener, Address* peer) {
    if (!peer) return ::accept(listener, nullptr, nullptr);
    socklen_t length = sizeof(Address);
    return ::accept(listener, reinterpret_cast<sockaddr*>(peer), &length);
}

std::ptrdiff_t Send(Handle handle, const void* data, std::size_t length) {
    // kSendFlags 在 Linux 上是 MSG_NOSIGNAL（对端关闭时不产生 SIGPIPE），
    // 在 macOS 上是 0 —— 那边靠 SetNoSigpipe 设的 SO_NOSIGPIPE 解决。
    const ssize_t written = ::send(handle, data, length, kSendFlags);
    return written < 0 ? -1 : written;
}

std::ptrdiff_t Receive(Handle handle, void* buffer, std::size_t length) {
    const ssize_t got = ::recv(handle, buffer, length, 0);
    return got < 0 ? -1 : got;
}

bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }

int LastError() { return errno; }

std::string ErrorText(int code) { return std::strerror(code); }

std::string PeerIp(const Address& address) {
    char buffer[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return buffer;
}

Address MakeAddress(const std::string& host, int port) {
    Address address{};
    address.sin_family = AF_INET;
    // 注意：**不能写 ::htons**。macOS 上 htons 是宏
    // （#define htons(x) __DARWIN_OSSwapInt16(x)），加了 :: 会展开成
    // "::((__uint16_t)...)" 这种非法语法。Windows / Linux 上它是函数，
    // 加不加 :: 都行 —— 所以统一不加，三端通吃。
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        ::inet_pton(AF_INET, host.c_str(), &address.sin_addr);
    }
    return address;
}

#endif

bool SendAll(Handle handle, const void* data, std::size_t length) {
    const char* cursor = static_cast<const char*>(data);
    std::size_t left = length;
    while (left > 0) {
        const std::ptrdiff_t written = Send(handle, cursor, left);
        if (written <= 0) return false;
        cursor += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace sock
}  // namespace dchat
