// dchat 网页版网关（dchat-webd）
//
// ## 为什么必须有这一层
//
// dchat 的协议是 **5555 端口上的裸 TCP** + 自定义 ECDH/AES-GCM 握手。浏览器的
// WebSocket / fetch 都跑不到裸 TCP 上，所以"网页直接连服务器"是做不到的。
//
// 这个网关一边用 HTTP/WebSocket 对浏览器，一边用**真实的 dchat 协议**连服务器。
// 关键点是它**不复刻协议**：直接复用 client_core 的 ChatCore + ClientConnection，
// 也就是桌面端和 Linux 端用的同一份实现。带来的好处有两个：
//   1. 网页版天然具备全部能力（群聊、私聊、文件、黑名单、审核……），
//      因为浏览器发过来的每一行最终都进了 ChatCore::SubmitInput；
//   2. 协议只在**一个地方**实现。哪天协议改了，网关照旧跟着变，不会多出一份要对齐的代码。
//
// 浏览器侧**一行加密代码都没有**：加密在网关和服务器之间，浏览器只看到明文文本。
//
// ## 对浏览器的协议（行式文本，Tab 分隔）
//
// 浏览器 → 网关
//   LOGIN \t <用户> \t <密码> \t <0|1 注册>     建一条 dchat 连接并登录
//   INPUT \t <一行文本>                          等价于在客户端输入框里敲这行
//   QUIT
//
// 网关 → 浏览器
//   STATE \t <未连接|连接中|已连接|已登录> \t <说明>
//   SAY   \t <昵称> \t <时间> \t <正文> \t <0|1 是不是自己> \t <0|1 有没有 @我>
//   SYS   \t <正文>          （服务器通知：进出、设置变更、公告……）
//   ERROR \t <正文>
//   INFO  \t <正文>          （网关自己的提示）
//
// 用法：
//   dchat-webd --port 8080 --server 127.0.0.1:5555 --www ./webd-page
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chat_core.h"
#include "file_transfer.h"
#include "socket_util.h"

namespace {

// ---------------------------------------------------------------- SHA-1
//
// WebSocket 握手要把 Sec-WebSocket-Key 和固定 GUID 拼起来做 SHA-1 再 Base64。
// 这里手写一份（三十来行）而不是链 OpenSSL：WebSocket 只需要**这一处**哈希，
// 为它引入一个平台相关的加密后端不值得 —— 而且手写之后这个网关在 Windows 上
// 也能编、也能测（真正部署是 Linux，但开发和验证都在 Windows 上做）。
struct Sha1 {
    unsigned int h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    static unsigned int Rol(unsigned int v, int bits) { return (v << bits) | (v >> (32 - bits)); }

    void Block(const unsigned char* p) {
        unsigned int w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<unsigned int>(p[i * 4]) << 24) |
                   (static_cast<unsigned int>(p[i * 4 + 1]) << 16) |
                   (static_cast<unsigned int>(p[i * 4 + 2]) << 8) |
                   static_cast<unsigned int>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            unsigned int f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const unsigned int temp = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string Digest(const std::string& text) {
        std::vector<unsigned char> data(text.begin(), text.end());
        const unsigned long long bitLength = static_cast<unsigned long long>(data.size()) * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0);
        for (int i = 7; i >= 0; --i) {
            data.push_back(static_cast<unsigned char>((bitLength >> (i * 8)) & 0xFF));
        }
        for (std::size_t i = 0; i + 63 < data.size(); i += 64) Block(&data[i]);
        std::string out;
        for (int i = 0; i < 5; ++i) {
            out.push_back(static_cast<char>((h[i] >> 24) & 0xFF));
            out.push_back(static_cast<char>((h[i] >> 16) & 0xFF));
            out.push_back(static_cast<char>((h[i] >> 8) & 0xFF));
            out.push_back(static_cast<char>(h[i] & 0xFF));
        }
        return out;
    }
};

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

bool SendAll(dchat::sock::Handle handle, const void* data, std::size_t length) {
    return dchat::sock::SendAll(handle, data, length);
}

// ---------------------------------------------------------------- 一个浏览器会话
class Session : public dchat::ChatCoreDelegate,
                public std::enable_shared_from_this<Session> {
public:
    Session(dchat::sock::Handle handle, const std::string& serverHost, int serverPort)
        : handle_(handle), serverHost_(serverHost), serverPort_(serverPort) {}
    ~Session() override {
        if (core_) core_->Disconnect();
        if (handle_ != dchat::sock::kInvalid) dchat::sock::Close(handle_);
    }

    // ---- WebSocket 的收 ----
    void Start() { SendLine(std::string("INFO\t网页版已就绪，输入账号登录")); }

    /** 处理浏览器发来的一行（见文件头的协议说明）。 */
    void OnBrowserLine(const std::string& line) {
        const std::size_t tab = line.find('\t');
        const std::string command = Lower(Trim(tab == std::string::npos ? line : line.substr(0, tab)));
        const std::string rest = (tab == std::string::npos) ? std::string() : line.substr(tab + 1);

        if (command == "login") {
            std::vector<std::string> parts;
            std::size_t from = 0;
            while (true) {
                const std::size_t at = rest.find('\t', from);
                parts.push_back(rest.substr(from, at == std::string::npos ? std::string::npos
                                                                         : at - from));
                if (at == std::string::npos) break;
                from = at + 1;
            }
            if (parts.size() < 2) {
                SendLine(std::string("ERROR\t登录需要用户名和密码"));
                return;
            }
            const std::string user = parts[0];
            const std::string password = parts[1];
            const bool wantRegister = (parts.size() > 2 && parts[2] == "1");
            DoLogin(user, password, wantRegister);
            return;
        }
        if (command == "input") {
            if (!core_ || !core_->LoggedIn()) {
                SendLine(std::string("ERROR\t还没登录"));
                return;
            }
            core_->SubmitInput(rest);
            return;
        }
        if (command == "quit") {
            Close();
            return;
        }
        SendLine(std::string("ERROR\t不认识的消息：") + command);
    }

    void Close() {
        closed_ = true;
        if (core_) core_->Disconnect();
        if (handle_ != dchat::sock::kInvalid) {
            dchat::sock::Close(handle_);
            handle_ = dchat::sock::kInvalid;
        }
    }

    // ---- ChatCoreDelegate：把核心层的事件翻成浏览器的行 ----
    void OnChatMessage(const dchat::ChatMessage& message) override {
        switch (message.kind) {
            case dchat::ChatMessage::Kind::Say:
            case dchat::ChatMessage::Kind::Own: {
                std::string line = "SAY\t" + message.nick + "\t" + message.time + "\t" +
                                   message.text + "\t" +
                                   (message.kind == dchat::ChatMessage::Kind::Own ? "1" : "0") + "\t" +
                                   (message.mentionMe ? "1" : "0");
                SendLine(line);
                return;
            }
            case dchat::ChatMessage::Kind::Error:
                SendLine("ERROR\t" + message.text);
                return;
            case dchat::ChatMessage::Kind::FileOffer:
                SendLine("INFO\t附件 " + message.fileId + "：" + message.text +
                         "（在输入框里发 /get " + message.fileId + " 下载）");
                return;
            case dchat::ChatMessage::Kind::System:
            case dchat::ChatMessage::Kind::Notice:
            default:
                SendLine("SYS\t" + message.text);
                return;
        }
    }
    void OnConnected(bool encrypted, const std::string& fingerprint,
                     const dchat::TrustDecision&) override {
        SendLine("STATE\t连接中\t" + std::string(encrypted ? "已加密" : "明文") + "，服务器指纹 " +
                 fingerprint);
    }
    void OnLoggedIn(const std::string& nick) override {
        SendLine("STATE\t已登录\t" + nick);
    }
    void OnDisconnected(const std::string& reason) override {
        SendLine("STATE\t未连接\t" + reason);
    }
    void OnOnlineNicks(const std::vector<std::string>&) override {}
    void OnTransferProgress(const std::string& text) override { SendLine("SYS\t" + text); }
    void OnColorSettingChanged(bool enabled, const std::string&) override {
        SendLine("SYS\t彩色聊天：" + std::string(enabled ? "开" : "关"));
    }
    void OnVoiceCommand(const std::string&) override {
        SendLine(std::string("ERROR\t网页版暂不支持录音"));
    }

    /** 发一帧 WebSocket 文本帧。**加锁**：接收线程和 WS 线程都会调。 */
    void SendLine(const std::string& text);

    bool closed() const { return closed_; }
    dchat::sock::Handle handle() const { return handle_; }

private:
    void DoLogin(const std::string& user, const std::string& password, bool wantRegister) {
        if (core_) {
            core_->Disconnect();
            core_.reset();
        }
        core_ = std::make_unique<dchat::ChatCore>(this);
        dchat::ConnectOptions options;
        options.host = serverHost_;
        options.port = serverPort_;
        options.user = user;
        options.password = password;
        options.wantRegister = wantRegister;
        std::string error;
        SendLine(std::string("STATE\t连接中\t正在连 ") + serverHost_ + ":" +
                 std::to_string(serverPort_));
        if (!core_->Connect(options, &error)) {
            SendLine("ERROR\t连接失败：" + error);
            SendLine(std::string("STATE\t未连接\t连接失败"));
            core_.reset();
        }
    }

    dchat::sock::Handle handle_ = dchat::sock::kInvalid;
    std::string serverHost_;
    int serverPort_ = 5555;
    std::unique_ptr<dchat::ChatCore> core_;
    std::mutex sendMutex_;
    bool closed_ = false;
};

std::mutex g_sessionsMutex;
std::vector<std::shared_ptr<Session>> g_sessions;

/** 组一个 WebSocket 文本帧。载荷小于 126 字节时用 7 位长度，够用（一行聊天不会超过）。 */
std::string MakeWsFrame(const std::string& text) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + opcode=1（文本）
    const std::size_t size = text.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((size >> (i * 8)) & 0xFF));
    }
    frame += text;
    return frame;
}

void Session::SendLine(const std::string& text) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (handle_ == dchat::sock::kInvalid) return;
    const std::string frame = MakeWsFrame(text);
    SendAll(handle_, frame.data(), frame.size());
}

}  // namespace

// ---------------------------------------------------------------- 内嵌页面
//
// 页面**嵌进二进制**而不是从磁盘读：部署时只拷一个可执行文件，不会出现
// "忘了拷 webd-page 目录，首页 404"这种事。要改界面就重新编，代价可以接受。
const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>dchat 网页版</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; font:14px/1.5 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
         background:#1b1b1f; color:#e6e6e6; height:100vh; display:flex; flex-direction:column; }
  header { padding:10px 16px; background:#232329; border-bottom:1px solid #33333a;
           display:flex; align-items:center; gap:12px; flex-wrap:wrap; }
  header b { font-size:16px; }
  #state { color:#8a8a94; font-size:12px; }
  #state.on { color:#4caf7d; }
  main { flex:1; display:flex; min-height:0; }
  #log { flex:1; overflow-y:auto; padding:12px 16px; }
  .row { margin:6px 0; white-space:pre-wrap; word-break:break-word; }
  .nick { color:#7aa7ff; }
  .time { color:#666; font-size:12px; margin:0 6px; }
  .own .nick { color:#4caf7d; }
  .sys { color:#8a8a94; font-size:13px; }
  .err { color:#ff6b6b; }
  #login { position:fixed; inset:0; background:#1b1b1fee; display:flex; align-items:center;
           justify-content:center; }
  #login form { background:#232329; padding:24px; border-radius:12px; width:320px;
                display:flex; flex-direction:column; gap:10px; border:1px solid #33333a; }
  input { padding:9px 11px; border-radius:8px; border:1px solid #3a3a42;
          background:#2a2a31; color:#e6e6e6; font:inherit; }
  button { padding:9px 14px; border-radius:8px; border:0; background:#2f6fed; color:#fff;
           font:inherit; cursor:pointer; }
  button.ghost { background:#2f2f37; }
  footer { display:flex; gap:8px; padding:10px 16px; background:#232329; border-top:1px solid #33333a; }
  footer input { flex:1; }
  a { color:#7aa7ff; }
</style></head><body>
<header>
  <b>dchat</b><span id="state">未连接</span>
  <span style="flex:1"></span>
  <a href="/download" style="font-size:13px">下载客户端</a>
</header>
<main><div id="log"></div></main>
<footer>
  <input id="input" placeholder="说点什么…（/help 看指令，/rooms 看会话，/get <id> 下载文件）" disabled>
  <button id="send" disabled>发送</button>
</footer>
<div id="login"><form id="loginForm">
  <b style="font-size:16px">登录 dchat</b>
  <input id="user" placeholder="昵称" autocomplete="username">
  <input id="pass" type="password" placeholder="密码" autocomplete="current-password">
  <input id="host" value="dchat.xin:5555" placeholder="服务器:端口">
  <label style="font-size:13px;color:#8a8a94">
    <input type="checkbox" id="reg" style="width:auto"> 没有账号，注册一个新的
  </label>
  <button type="submit">连接</button>
</form></div>
<script>
const logEl = document.getElementById('log');
const stateEl = document.getElementById('state');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
let ws = null;

function add(kind, text) {
  const d = document.createElement('div');
  d.className = 'row ' + kind;
  d.textContent = text;
  logEl.appendChild(d);
  logEl.scrollTop = logEl.scrollHeight;
}
function setState(text, on) {
  stateEl.textContent = text;
  stateEl.className = on ? 'on' : '';
}
function send(line) { if (ws && ws.readyState === 1) ws.send(line); }

document.getElementById('loginForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const hostEl = document.getElementById('host');
  const parts = hostEl.value.split(':');
  // 服务器地址交给网关：网关按自己的默认值连，这里只把 host:port 透传过去
  document.getElementById('login').style.display = 'none';
  setState('连接中…', false);
  add('sys', '正在连接 ' + hostEl.value + ' …');
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?server=' + encodeURIComponent(hostEl.value));
  ws.onopen = () => {
    setState('已连接', true);
    const reg = document.getElementById('reg').checked ? '1' : '0';
    send('LOGIN\t' + document.getElementById('user').value + '\t' +
         document.getElementById('pass').value + '\t' + reg);
  };
  ws.onmessage = (ev) => {
    const f = ev.data.split('\t');
    switch (f[0]) {
      case 'SAY': {
        const own = f[4] === '1';
        const d = document.createElement('div');
        d.className = 'row' + (own ? ' own' : '');
        d.innerHTML = '';
        const n = document.createElement('span'); n.className = 'nick'; n.textContent = f[1]; 
        const t = document.createElement('span'); t.className = 'time'; t.textContent = f[2];
        const x = document.createElement('span'); x.textContent = f[3];
        d.appendChild(n); d.appendChild(t); d.appendChild(x);
        logEl.appendChild(d); logEl.scrollTop = logEl.scrollHeight;
        break;
      }
      case 'SYS':   add('sys', f[1]); break;
      case 'ERROR': add('err', f[1]); break;
      case 'INFO':  add('sys', f[1]); break;
      case 'STATE':
        setState(f[1] + (f[2] ? '　' + f[2] : ''), f[1] === '已登录');
        if (f[1] === '已登录') { inputEl.disabled = false; sendEl.disabled = false; inputEl.focus(); }
        break;
    }
  };
  ws.onclose = () => { setState('已断开', false); inputEl.disabled = true; sendEl.disabled = true; };
  ws.onerror = () => add('err', '连接出错');
});

function submit() {
  const text = inputEl.value.trim();
  if (!text) return;
  send('INPUT\t' + text);
  inputEl.value = '';
}
sendEl.addEventListener('click', submit);
inputEl.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });
</script></body></html>
)HTML";

const char* kDownloadHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>下载 dchat</title>
<style>
  body { margin:0; font:15px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
         background:#1b1b1f; color:#e6e6e6; }
  .wrap { max-width:760px; margin:0 auto; padding:40px 20px; }
  h1 { font-size:26px; margin:0 0 6px; }
  p.sub { color:#8a8a94; margin:0 0 28px; }
  .card { background:#232329; border:1px solid #33333a; border-radius:12px; padding:18px 20px;
          margin-bottom:14px; }
  .card h2 { font-size:16px; margin:0 0 6px; }
  .meta { color:#8a8a94; font-size:13px; margin-bottom:10px; }
  a.btn { display:inline-block; padding:9px 16px; border-radius:8px; background:#2f6fed;
          color:#fff; text-decoration:none; font-size:14px; }
  a.btn.alt { background:#2f2f37; }
  code { background:#2a2a31; padding:2px 6px; border-radius:5px; font-size:13px; }
  .foot { color:#666; font-size:12px; margin-top:26px; }
</style></head><body><div class="wrap">
<h1>dchat</h1>
<p class="sub">选一个端开始用。都是同一台服务器：<code>dchat.xin:5555</code></p>
<div id="list"></div>
<div class="foot">网页版就在你现在这个页面：<a href="/">打开</a>　·　
  源码 <a href="https://github.com/dongfang20101113/DChat">GitHub</a></div>
</div>
<script>
const items = [
  { t:'Windows 桌面版', d:'解压即用，不用安装。群聊 / 私聊 / 语音 / 文件都在这里。',
    f:'dchat-windows-x64.zip' },
  { t:'Android 版', d:'APK，直接装。', f:'dchat-android.apk' },
  { t:'Linux 服务端', d:'自己搭一台服务器用。解压后 ./dchat_server --port 5555', f:'dchat-linux-x86_64.tar.gz' },
];
const list = document.getElementById('list');
for (const it of items) {
  const card = document.createElement('div');
  card.className = 'card';
  card.innerHTML = '<h2></h2><div class="meta"></div>';
  card.querySelector('h2').textContent = it.t;
  card.querySelector('.meta').textContent = it.d;
  const a = document.createElement('a');
  a.className = 'btn';
  a.href = '/dl/' + it.f;
  a.textContent = '下载';
  card.appendChild(a);
  list.appendChild(card);
}
</script></body></html>
)HTML";

namespace {

struct Config {
    int port = 8080;
    std::string serverHost = "127.0.0.1";
    int serverPort = 5555;
    std::string dlDir = "./dchat-downloads";
};

Config g_config;

bool ReadRequest(dchat::sock::Handle handle, std::string* out) {
    std::string buffer;
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        const std::ptrdiff_t got = dchat::sock::Receive(handle, chunk, sizeof(chunk));
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(got));
        if (buffer.size() > 64 * 1024) return false;  // 请求头不该这么大
    }
    *out = buffer;
    return true;
}

void ServeConnection(dchat::sock::Handle handle) {
    dchat::sock::SetNoSigpipe(handle);
    dchat::sock::SetNoDelay(handle);
    dchat::sock::SetReceiveTimeout(handle, 30000);

    std::string request;
    if (!ReadRequest(handle, &request)) {
        dchat::sock::Close(handle);
        return;
    }

    // ---- 第一行：请求行 ----
    const std::size_t lineEnd = request.find("\r\n");
    std::string firstLine = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);
    std::string method;
    std::string path;
    {
        const std::size_t sp1 = firstLine.find(' ');
        const std::size_t sp2 = firstLine.rfind(' ');
        if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1) {
            method = firstLine.substr(0, sp1);
            path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }
    const std::string headers = Lower(request);

    // ---- WebSocket 升级 ----
    if (headers.find("upgrade: websocket") != std::string::npos) {
        std::string key;
        {
            const std::string needle = "sec-websocket-key:";
            const std::size_t at = headers.find(needle);
            if (at != std::string::npos) {
                std::size_t begin = at + needle.size();
                while (begin < headers.size() && (headers[begin] == ' ' || headers[begin] == '\t')) {
                    ++begin;
                }
                const std::size_t end = headers.find("\r\n", begin);
                key = request.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            }
        }
        if (key.empty()) {
            dchat::sock::Close(handle);
            return;
        }
        Sha1 sha;
        const std::string accept =
            dchat::Base64Encode(sha.Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        const std::string reply = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        SendAll(handle, reply.data(), reply.size());

        // ?server=host:port 允许前端指定服务器；默认用命令行给的
        std::string host = g_config.serverHost;
        int port = g_config.serverPort;
        const std::size_t q = path.find("server=");
        if (q != std::string::npos) {
            std::string value = path.substr(q + 7);
            const std::size_t andAt = value.find('&');
            if (andAt != std::string::npos) value = value.substr(0, andAt);
            const std::size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                host = value.substr(0, colon);
                const int parsed = std::atoi(value.c_str() + colon + 1);
                if (parsed > 0 && parsed < 65536) port = parsed;
            } else if (!value.empty()) {
                host = value;
            }
        }

        auto session = std::make_shared<Session>(handle, host, port);
        {
            std::lock_guard<std::mutex> lock(g_sessionsMutex);
            g_sessions.push_back(session);
        }
        session->Start();

        // ---- WebSocket 帧循环：浏览器发来的帧一律带掩码（RFC 6455 要求） ----
        std::string pending;
        char buffer[4096];
        while (!session->closed()) {
            const std::ptrdiff_t got = dchat::sock::Receive(handle, buffer, sizeof(buffer));
            if (got <= 0) break;
            pending.append(buffer, static_cast<std::size_t>(got));
            while (pending.size() >= 2) {
                const unsigned char b0 = static_cast<unsigned char>(pending[0]);
                const unsigned char b1 = static_cast<unsigned char>(pending[1]);
                const int opcode = b0 & 0x0F;
                std::size_t length = b1 & 0x7F;
                std::size_t offset = 2;
                if (length == 126) {
                    if (pending.size() < 4) break;
                    length = (static_cast<unsigned char>(pending[2]) << 8) |
                             static_cast<unsigned char>(pending[3]);
                    offset = 4;
                } else if (length == 127) {
                    if (pending.size() < 10) break;
                    length = 0;
                    for (int i = 0; i < 8; ++i) {
                        length = (length << 8) | static_cast<unsigned char>(pending[2 + i]);
                    }
                    offset = 10;
                }
                const bool masked = (b1 & 0x80) != 0;
                const std::size_t maskBytes = masked ? 4 : 0;
                if (pending.size() < offset + maskBytes + length) break;
                std::string payload = pending.substr(offset + maskBytes, length);
                if (masked) {
                    for (std::size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<char>(payload[i] ^
                                                       pending[offset + (i % 4)]);
                    }
                }
                pending.erase(0, offset + maskBytes + length);
                if (opcode == 0x8) {  // close
                    session->Close();
                    break;
                }
                if (opcode == 0x9) {  // ping -> pong
                    std::string pong;
                    pong.push_back(static_cast<char>(0x8A));
                    pong.push_back(static_cast<char>(payload.size()));
                    pong += payload;
                    SendAll(handle, pong.data(), pong.size());
                    continue;
                }
                if (opcode == 0x1 || opcode == 0x0) session->OnBrowserLine(payload);
            }
        }
        session->Close();
        {
            std::lock_guard<std::mutex> lock(g_sessionsMutex);
            g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), session),
                             g_sessions.end());
        }
        return;
    }

    // ---- 普通 HTTP：页面 + 静态下载 ----
    auto reply = [&](const std::string& status, const std::string& type,
                     const std::string& body) {
        const std::string head = "HTTP/1.1 " + status +
                                 "\r\nContent-Type: " + type +
                                 "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        SendAll(handle, head.data(), head.size());
        SendAll(handle, body.data(), body.size());
    };

    if (path == "/" || path == "/index.html") {
        reply("200 OK", "text/html; charset=utf-8", kIndexHtml);
    } else if (path == "/download") {
        reply("200 OK", "text/html; charset=utf-8", kDownloadHtml);
    } else if (path.rfind("/dl/", 0) == 0) {
        // 下载：文件名里不许出现 .. 或 /
        std::string name = path.substr(4);
        const std::size_t q = name.find('?');
        if (q != std::string::npos) name = name.substr(0, q);
        if (name.empty() || name.find("..") != std::string::npos ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            reply("400 Bad Request", "text/plain; charset=utf-8", "bad name");
        } else {
            const std::string full = g_config.dlDir + "/" + name;
            FILE* file = std::fopen(full.c_str(), "rb");
            if (file == nullptr) {
                reply("404 Not Found", "text/plain; charset=utf-8",
                      "没有这个文件：" + name + "（放在 " + g_config.dlDir + " 里）");
            } else {
                std::fseek(file, 0, SEEK_END);
                const long size = std::ftell(file);
                std::fseek(file, 0, SEEK_SET);
                const std::string head = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: application/octet-stream\r\n"
                                         "Content-Disposition: attachment; filename=\"" + name +
                                         "\"\r\nContent-Length: " + std::to_string(size) +
                                         "\r\nConnection: close\r\n\r\n";
                SendAll(handle, head.data(), head.size());
                char chunk[65536];
                std::size_t left = static_cast<std::size_t>(size);
                while (left > 0) {
                    const std::size_t want = std::min(left, sizeof(chunk));
                    const std::size_t got = std::fread(chunk, 1, want, file);
                    if (got == 0) break;
                    if (!SendAll(handle, chunk, got)) break;
                    left -= got;
                }
                std::fclose(file);
            }
        }
    } else {
        reply("404 Not Found", "text/plain; charset=utf-8", "not found");
    }
    (void)method;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--port") {
            g_config.port = std::atoi(next().c_str());
        } else if (arg == "--server") {
            const std::string value = next();
            const std::size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                g_config.serverHost = value.substr(0, colon);
                g_config.serverPort = std::atoi(value.c_str() + colon + 1);
            } else {
                g_config.serverHost = value;
            }
        } else if (arg == "--dl") {
            g_config.dlDir = next();
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：dchat-webd [--port 8080] [--server 127.0.0.1:5555] [--dl 目录]\n");
            return 0;
        }
    }
    if (g_config.port <= 0 || g_config.port > 65535) g_config.port = 8080;

    if (!dchat::sock::Startup()) {
        std::fprintf(stderr, "socket 初始化失败\n");
        return 1;
    }
    const dchat::sock::Handle listener = dchat::sock::CreateTcp();
    if (listener == dchat::sock::kInvalid) {
        std::fprintf(stderr, "建监听 socket 失败\n");
        return 1;
    }
    dchat::sock::SetReuseAddress(listener);
    const dchat::sock::Address address = dchat::sock::MakeAddress("0.0.0.0", g_config.port);
    if (!dchat::sock::Bind(listener, address) || !dchat::sock::Listen(listener, 32)) {
        std::fprintf(stderr, "监听 %d 失败\n", g_config.port);
        return 1;
    }
    std::printf("dchat-webd 已启动：http://0.0.0.0:%d/  （上游 dchat %s:%d，下载目录 %s）\n",
                g_config.port, g_config.serverHost.c_str(), g_config.serverPort,
                g_config.dlDir.c_str());
    std::fflush(stdout);

    while (true) {
        dchat::sock::Address peer{};
        const dchat::sock::Handle client = dchat::sock::Accept(listener, &peer);
        if (client == dchat::sock::kInvalid) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::thread([client]() {
            ServeConnection(client);
            dchat::sock::Close(client);
        }).detach();
    }
}

