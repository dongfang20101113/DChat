#include "bot_http.h"

#include "socket_util.h"  // 必须最先：winsock2.h 要排在 windows.h 之前

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace dchat {
namespace dshbot {

namespace {

// 请求体上限：管理界面的请求就是几个开关，64 KB 已经非常宽松了。
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

std::string Lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexValue(text[i + 1]);
            const int lo = HexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& key) {
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair =
            (amp == std::string::npos) ? query.substr(start) : query.substr(start, amp - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            if (UrlDecode(pair.substr(0, equals)) == key) return UrlDecode(pair.substr(equals + 1));
        } else if (UrlDecode(pair) == key) {
            return std::string();
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return std::string();
}

int ParseHttpRequest(const std::string& buffer, HttpRequest* out, std::size_t* totalLength) {
    if (out == nullptr || totalLength == nullptr) return -1;
    const std::size_t headerEnd = buffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return -1;
        return 0;  // 头部还没收全
    }
    if (headerEnd > kMaxHeaderBytes) return -1;

    HttpRequest request;
    std::string head = buffer.substr(0, headerEnd);
    std::size_t lineStart = 0;
    bool first = true;
    while (lineStart <= head.size()) {
        const std::size_t lineEnd = head.find("\r\n", lineStart);
        const std::string line = (lineEnd == std::string::npos)
                                     ? head.substr(lineStart)
                                     : head.substr(lineStart, lineEnd - lineStart);
        if (first) {
            first = false;
            // "GET /api/state?x=1 HTTP/1.1"
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) return -1;
            request.method = line.substr(0, sp1);
            const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::size_t question = target.find('?');
            request.path = UrlDecode((question == std::string::npos) ? target : target.substr(0, question));
            request.query = (question == std::string::npos) ? std::string() : target.substr(question + 1);
            if (request.method != "GET" && request.method != "POST" && request.method != "OPTIONS") {
                return -1;
            }
        } else {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                request.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
            }
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 2;
    }

    std::size_t bodyBytes = 0;
    const auto it = request.headers.find("content-length");
    if (it != request.headers.end() && !it->second.empty()) {
        try {
            bodyBytes = static_cast<std::size_t>(std::stoul(it->second));
        } catch (...) {
            return -1;
        }
    }
    if (bodyBytes > kMaxBodyBytes) return -1;
    const std::size_t total = headerEnd + 4 + bodyBytes;
    if (buffer.size() < total) {
        *totalLength = total;
        return 0;
    }
    request.body = buffer.substr(headerEnd + 4, bodyBytes);
    *out = request;
    *totalLength = total;
    return 1;
}

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(const std::string& host, int port, Handler handler, std::string* error) {
    if (listenSocket_ >= 0) return true;
    handler_ = std::move(handler);
    sock::Startup();
    const sock::Handle handle = sock::CreateTcp();
    if (handle == sock::kInvalid) {
        if (error != nullptr) *error = "建 socket 失败";
        return false;
    }
    sock::SetReuseAddress(handle);
    const sock::Address address = sock::MakeAddress(host, port);
    if (!sock::Bind(handle, address) || !sock::Listen(handle, 8)) {
        if (error != nullptr) {
            *error = "端口 " + std::to_string(port) + " 绑不上（可能已被别的程序占用）";
        }
        sock::Close(handle);
        return false;
    }
    // 监听端口可能传的是 0（系统分配），把真实端口取回来告诉调用方
    sockaddr_in actual{};
    socklen_t length = sizeof(actual);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
        boundPort_ = ntohs(actual.sin_port);
    } else {
        boundPort_ = port;
    }
    listenSocket_ = static_cast<intptr_t>(handle);
    thread_ = new std::thread([this] { Loop(); });
    return true;
}

void HttpServer::Loop() {
    const sock::Handle listener = static_cast<sock::Handle>(listenSocket_);
    while (!stopping_) {
        sockaddr_in peer{};
        const sock::Handle client = sock::Accept(listener, &peer);
        if (client == sock::kInvalid) {
            if (stopping_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        sock::SetNoDelay(client);
        sock::SetReceiveTimeout(client, 2000);
        std::string buffer;
        HttpResponse response;
        bool answered = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
            char chunk[4096];
            const std::ptrdiff_t got = sock::Receive(client, chunk, sizeof(chunk));
            if (got <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(got));
            HttpRequest request;
            std::size_t total = 0;
            const int status = ParseHttpRequest(buffer, &request, &total);
            if (status < 0) {
                response.status = 400;
                response.body = "{\"ok\":false,\"error\":\"bad request\"}";
                answered = true;
                break;
            }
            if (status == 1) {
                buffer.resize(total);
                if (request.method == "OPTIONS") {
                    // 只回"不支持跨域"，不返回任何 CORS 头：管理界面同源访问，用不到
                    response.status = 405;
                    response.body = "{\"ok\":false,\"error\":\"method not allowed\"}";
                } else {
                    response = handler_ ? handler_(request) : HttpResponse{};
                }
                answered = true;
                break;
            }
        }
        if (!answered) {
            response.status = 408;
            response.body = "{\"ok\":false,\"error\":\"timeout\"}";
        }
        std::string header = "HTTP/1.1 " + std::to_string(response.status) + " OK\r\n" +
                             "Content-Type: " + response.contentType + "\r\n" +
                             "Content-Length: " + std::to_string(response.body.size()) + "\r\n" +
                             "Cache-Control: no-store\r\n"
                             "X-Content-Type-Options: nosniff\r\n";
        for (const auto& extra : response.headers) {
            header += extra.first + ": " + extra.second + "\r\n";
        }
        header += "Connection: close\r\n\r\n";
        sock::SendAll(client, header.data(), header.size());
        sock::SendAll(client, response.body.data(), response.body.size());
        sock::Close(client);
    }
}

void HttpServer::Stop() {
    if (listenSocket_ < 0) return;
    stopping_ = true;
    const sock::Handle listener = static_cast<sock::Handle>(listenSocket_);
    listenSocket_ = -1;
    sock::Close(listener);
    if (thread_ != nullptr) {
        auto* thread = static_cast<std::thread*>(thread_);
        thread_ = nullptr;
        if (thread->joinable()) thread->join();
        delete thread;
    }
}

}  // namespace dshbot
}  // namespace dchat
