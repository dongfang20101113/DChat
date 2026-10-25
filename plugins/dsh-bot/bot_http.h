// 插件自带的一个**极小** HTTP 服务：只给本机浏览器上的管理界面用
//
// 为什么是这个形态：机器人跑在用户自己的机器上（DSH 装在哪就得在哪跑），
// 而用户已经有一个浏览器。再写一个 Win32 窗口的话，等于把"改三个设置项"
// 变成一套新的界面代码 + 布局 + 高 DPI + 主题适配，收益为零。
//
// 刻意只做这几件事：
//   * 只监听 127.0.0.1（可配），不对外网开放；
//   * 只支持 GET / POST，一次请求一条连接，处理完就关（不需要 keep-alive）；
//   * 请求体上限 64 KB，读完头部开始计时，2 秒没读完就断开 ——
//     本机界面的正常请求都是几毫秒的事，慢的一定是异常连接。
#pragma once

#include <functional>
#include <map>
#include <string>

namespace dchat {
namespace dshbot {

struct HttpRequest {
    std::string method;   // Get / POST（统一大写）
    std::string path;     // 不含查询串，已做百分号解码
    std::string query;    // "?" 之后的原始串（未解码）
    std::map<std::string, std::string> headers;  // 键统一小写
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

/**
 * 解析一段可能不完整的 HTTP 请求。
 *
 * 返回 1 = 解析完成（*totalLength 是从缓冲开头到请求结束的总字节数），
 *      0 = 数据还不够，需要继续读，
 *     -1 = 格式非法（调用方直接 400 并断开）。
 *
 * 抽成纯函数是为了能单测：真起 socket 才能测的话，这类解析器基本不会有人测，
 * 而它的 bug 表现为"点了保存没反应"——最难查的那种。
 */
int ParseHttpRequest(const std::string& buffer, HttpRequest* out, std::size_t* totalLength);

/** 百分号解码（%E4%BD%A0 和 '+' 都能解）。 */
std::string UrlDecode(const std::string& text);

/** 从 "a=1&b=2" 里取一个参数，没有就返回空串。 */
std::string QueryParam(const std::string& query, const std::string& key);

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer();

    /** 启动。失败返回 false 并写 error（端口被占用是最常见的）。 */
    bool Start(const std::string& host, int port, Handler handler, std::string* error);
    void Stop();

    /** 实际监听的端口（port 传 0 时由系统分配，这里拿到真实值）。 */
    int BoundPort() const { return boundPort_; }

private:
    void Loop();

    void* listenSocket_ = nullptr;  // sock::Handle（头文件里不拉平台 socket 头）
    int boundPort_ = 0;
    bool stopping_ = false;
    Handler handler_;
    void* thread_ = nullptr;  // std::thread*
};

}  // namespace dshbot
}  // namespace dchat
