// 跨平台 socket 的薄封装：Windows 走 Winsock2，Linux 走 POSIX。
//
// 服务端原来直接用 Winsock 的类型和函数，移植时靠这层把它们收拢到一处，
// **业务代码一行不用改**。刻意只封服务端真正用到的那几个（十来种）——
// 封装面越大越容易在两端出现细微行为差异。
//
// 一个必须注意的差异：Linux 上往已关闭的连接写会收到 SIGPIPE，**默认直接杀进程**。
// 句柄类型 Socket 只是存文件描述符，任何能 send 的地方都要走 Send/SendAll（它们带了
// MSG_NOSIGNAL）。绕过去直接调 ::send 就可能把整个服务器打死。
#pragma once

// ⚠️ 顺序很关键：**任何标准库头之前**就要把 winsock2.h 包进来。
//
// 踩过的坑：`#include <string>` 会间接拉进 windows.h，而 windows.h 里包含了老的
// winsock.h；后者会定义 `_WINSOCKAPI_`，于是本文件里的 `winsock2.h` 被它自己的
// 保护宏挡掉 —— 结果 `SOCKET` / `sockaddr_in` 全都不认识，`namespace sock` 里的
// 类型别名直接编不过，报出来的却是"'sock' has not been declared"，很误导。
// （MSDN 的老规矩：winsock2.h 必须在 windows.h 之前。）
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>  // addrinfo / getaddrinfo / freeaddrinfo（服务端列本机地址要用）
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace dchat {
namespace sock {

#ifdef _WIN32
using Handle = SOCKET;
using Address = sockaddr_in;
inline constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
using Address = sockaddr_in;
inline constexpr Handle kInvalid = -1;
#endif

/**
 * 「往已关闭的连接写不要杀进程」这个能力，三个平台的表达方式都不同：
 *
 *   Windows  完全没有 SIGPIPE 这个概念，不用管
 *   Linux    send() 时传 MSG_NOSIGNAL 标志（per-call）
 *   macOS    没有 MSG_NOSIGNAL，改成给 socket 设 SO_NOSIGPIPE 选项（per-socket），
 *            所以**必须在建连/收连接之后就设上**，不能等到 send 时才管
 *
 * 抽成常量 + 一个设置函数，是为了让调用点不用到处写 #ifdef —— 漏掉一处的后果
 * 是整个服务器被一个已断开的客户端打死，而这种崩溃在测试里很难碰到。
 */
#ifdef _WIN32
inline constexpr int kSendFlags = 0;
#elif defined(__APPLE__)
inline constexpr int kSendFlags = 0;  // macOS 靠 SO_NOSIGPIPE，不是靠标志
#else
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

/**
 * 给一个 socket 设上「写已关闭连接不产生 SIGPIPE」。
 *
 * Linux 上是空操作（那边用 kSendFlags 解决），macOS 上设 SO_NOSIGPIPE。
 * **新建立的连接都要调它**，否则 macOS 上第一次写死连接就会收到 SIGPIPE
 * 而直接终止进程。
 */
bool SetNoSigpipe(Handle handle);

/** 进程启动时调一次；Windows 需要 WSAStartup，Linux 是空操作。 */
bool Startup();

/** 进程退出时调一次。 */
void Cleanup();

/** 关掉一个连接（两端都安全，传 kInvalid 直接返回）。 */
void Close(Handle handle);

/** 关闭写方向（对端会读到 EOF）。 */
void ShutdownWrite(Handle handle);

/** 建一个 TCP socket。 */
Handle CreateTcp();

/** 设 SO_REUSEADDR。 */
bool SetReuseAddress(Handle handle);

/** 设接收/发送超时（毫秒；0 表示不设）。 */
bool SetReceiveTimeout(Handle handle, int milliseconds);
bool SetSendTimeout(Handle handle, int milliseconds);

/** 开 TCP_NODELAY（聊天消息都是小包，禁掉 Nagle 才不会有延迟）。 */
bool SetNoDelay(Handle handle);

bool Bind(Handle handle, const Address& address);
bool Listen(Handle handle, int backlog);
Handle Accept(Handle listener, Address* peer);

/**
 * 发数据。返回实际发出的字节数；出错返回 -1。
 * **内部带 MSG_NOSIGNAL**，见文件头说明。
 */
std::ptrdiff_t Send(Handle handle, const void* data, std::size_t length);

/** 一次把 length 字节发完（内部循环补发）。全发出去返回 true。 */
bool SendAll(Handle handle, const void* data, std::size_t length);

/** 收数据。返回收到的字节数；0 = 对端关了；-1 = 出错。 */
std::ptrdiff_t Receive(Handle handle, void* buffer, std::size_t length);

/**
 * 上一次操作是不是"暂时没有数据/会阻塞"（而不是真错误）。
 *
 * 两端错误码不同：Windows 是 WSAEWOULDBLOCK，Linux 是 EAGAIN/EWOULDBLOCK。
 * 超时在 Linux 上报 EAGAIN、在 Windows 上报 WSAETIMEDOUT，所以两个都算。
 */
bool WouldBlock();

/** 上一次 socket 操作的错误码（用于日志）。 */
int LastError();

/** 错误码转成可读文字。 */
std::string ErrorText(int code);

/** 取对端 IP（点分十进制）。 */
std::string PeerIp(const Address& address);

/** 组一个监听地址。 */
Address MakeAddress(const std::string& host, int port);

}  // namespace sock
}  // namespace dchat
