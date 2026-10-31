// 管理界面的那一页 HTML（内联进二进制，和 webd 的做法一致）//
// 为什么不拆成 .html 文件：那样部署时要多管一个"别弄丢"的文件，而它只有一页。
// 拼进二进制以后，`dchat-dsh-bot` 拷到哪都能直接开界面。
//
// 前端刻意不用任何框架：一页表单，用原生 fetch + DOM 就够；引入 React 等于
// 给一个改三个开关的界面加一整套构建流程。
#pragma once

namespace dchat {
namespace dshbot {

// 登录页：管理界面现在可以用 dchat 账号登录后从公网打开。
// 只有 op，或者机器人所加入的群的群主，才能进去改东西。
inline constexpr const char* kLoginPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>dchat 机器人 · 登录</title>
<style>
  body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
       background:#16181d;color:#e6e8ee;font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
  .box{width:min(92vw,360px);background:#1e2129;border:1px solid #2b303b;border-radius:14px;padding:26px}
  h1{margin:0 0 4px;font-size:19px}
  p.tip{margin:0 0 18px;color:#8b93a7;font-size:13px}
  label{display:block;margin:14px 0 6px;color:#aab2c5;font-size:13px}
  input{width:100%;box-sizing:border-box;padding:10px 12px;border-radius:9px;border:1px solid #333a48;
        background:#14161b;color:#e6e8ee;font-size:15px}
  button{margin-top:20px;width:100%;padding:11px;border:0;border-radius:9px;background:#3b82f6;
         color:#fff;font-size:15px;cursor:pointer}
  button:disabled{opacity:.6;cursor:default}
  #msg{margin-top:14px;min-height:20px;font-size:13px;color:#f87171}
</style></head><body>
<div class="box">
  <h1>dchat 机器人</h1>
  <p class="tip">用你的 dchat 账号登录。只有 <b>op</b> 或<br>机器人所在群的 <b>群主</b> 能进来。</p>
  <form id="f">
    <label>昵称</label><input id="nick" autocomplete="username" autofocus>
    <label>密码</label><input id="pass" type="password" autocomplete="current-password">
    <button id="go" type="submit">登录</button>
  </form>
  <div id="msg"></div>
</div>
<script>
(function () {
  var go = document.getElementById('go');
  var msg = document.getElementById('msg');

  // 登录接口**必须用绝对路径**：这一页既可能是 /bot/，也可能是 /room/<群>/bot，
  // 相对写法在后者会解析成 /room/<群>/api/login（网关那边是 404 纯文本，
  // 前端 r.json() 直接抛 SyntaxError —— 就是那个 "Unexpected token 'o'"）。
  var LOGIN_URL = '/bot/api/login';

  function submit(nick, password, silent) {
    go.disabled = true;
    if (!silent) msg.textContent = '正在登录…';
    return fetch(LOGIN_URL, {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Dchat-Bot': '1'},
      body: JSON.stringify({nick: nick, password: password})
    }).then(function (r) {
      // 网关出错时可能回纯文本（404/502），先按文本读再尝试解析，
      // 免得一个非 JSON 响应就把整页报成"连不上"。
      return r.text().then(function (body) {
        var data = null;
        try { data = JSON.parse(body); } catch (e) { data = null; }
        if (data === null) {
          throw new Error('服务器返回了非 JSON（HTTP ' + r.status + '）');
        }
        return data;
      });
    });
  }

  function afterLogin(data) {
    // 统一进总览页：总览里有"每个群的入口"，从那儿再点进单群。
    // 之前"只管理一个群就直接跳单群页"会让人找不到总览（用户实测就困在单群页里）。
    location.href = '/bot/';
  }

  document.getElementById('f').addEventListener('submit', function (e) {
    e.preventDefault();
    msg.textContent = '';
    submit(document.getElementById('nick').value.trim(), document.getElementById('pass').value, false)
      .then(function (data) {
        if (data.ok) { afterLogin(data); return; }
        msg.textContent = data.error || '登录失败';
        go.disabled = false;
      })
      .catch(function (err) {
        msg.textContent = '登录失败：' + err.message;
        go.disabled = false;
      });
  });

  // 已经登录过聊天室的，直接用那份账号密码登录 —— 同一个站点、同一份 localStorage，
  // 用户不该为了看机器人设置再输一遍密码。
  try {
    var saved = JSON.parse(localStorage.getItem('dchat_login') || 'null');
    if (saved && saved.user && saved.pass) {
      document.getElementById('nick').value = saved.user;
      msg.textContent = '用聊天室记住的账号自动登录…';
      submit(saved.user, saved.pass, true).then(function (data) {
        if (data.ok) { afterLogin(data); return; }
        msg.textContent = data.error || '自动登录失败，请手动输入密码';
        go.disabled = false;
      }).catch(function (err) {
        msg.textContent = '自动登录失败：' + err.message;
        go.disabled = false;
      });
    }
  } catch (e) { /* 隐私模式没有 localStorage，忽略 */ }
})();
</script>
</body></html>)HTML";

inline constexpr const char* kUiPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>dchat 机器人 · 设置</title>
<style>
  :root { --bg:#0f1115; --card:#171a21; --line:#262b36; --text:#e6e9ef; --muted:#8b93a5;
          --accent:#5b8cff; --ok:#3fb950; --bad:#f85149; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--text);
         font:14px/1.6 "Segoe UI","Microsoft YaHei",system-ui,sans-serif; }
  .wrap { max-width:860px; margin:0 auto; padding:22px 16px 90px; }
  header { display:flex; align-items:center; gap:12px; margin-bottom:18px; flex-wrap:wrap; }
  header h1 { font-size:19px; margin:0; font-weight:600; }
  .badge { font-size:12px; padding:2px 9px; border-radius:99px; border:1px solid var(--line); color:var(--muted); }
  .badge.on { color:var(--ok); border-color:#20432b; background:#122117; }
  .badge.off { color:var(--bad); border-color:#4a1f1f; background:#1e1212; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:16px; margin-bottom:14px; }
  .card h2 { font-size:14px; margin:0 0 4px; font-weight:600; }
  .card p.hint { margin:0 0 12px; color:var(--muted); font-size:12.5px; }
  .row { display:flex; align-items:center; gap:10px; padding:7px 0; border-top:1px solid rgba(255,255,255,.04); }
  .row:first-of-type { border-top:0; }
  .row .name { flex:1; min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .row .id { color:var(--muted); font-size:12px; }
  .switch { width:42px; height:24px; border-radius:99px; background:#2a2f3b; border:1px solid var(--line);
            position:relative; cursor:pointer; transition:.15s; flex:none; }
  .switch::after { content:""; position:absolute; top:2px; left:2px; width:18px; height:18px; border-radius:50%;
                   background:#9aa3b5; transition:.15s; }
  .switch.on { background:#1f3a6b; border-color:#2f5ea8; }
  .switch.on::after { left:20px; background:var(--accent); }
  .lg { display:flex; gap:8px; flex-wrap:wrap; margin-top:8px; }
  /* 密码框、下拉框、文本域也要走深色主题：之前只写了 text/number，
     其余控件用的是浏览器默认白底 —— 深色页面上特别刺眼（用户截图里那几块白）。 */
  input[type=text], input[type=number], input[type=password], select, textarea {
         background:#0d1015; color:var(--text); border:1px solid var(--line);
         border-radius:8px; padding:7px 10px; font:inherit; min-width:0;
         outline:none; -webkit-appearance:none; appearance:none; }
  input:focus, select:focus, textarea:focus { border-color:#3a5ea8; }
  input::placeholder, textarea::placeholder { color:#5c657a; }
  select { cursor:pointer; background-image:none; }
  select option { background:#0d1015; color:var(--text); }
  textarea { width:100%; resize:vertical; line-height:1.5;
             font-family:Consolas,"Cascadia Mono",monospace; font-size:13px; }
  input[type=text], input[type=password] { flex:1; }
  input[type=number] { width:96px; }
  button { background:#1d2330; color:var(--text); border:1px solid var(--line); border-radius:8px;
           padding:7px 13px; font:inherit; cursor:pointer; }
  button:hover { border-color:#3a455c; }
  button.primary { background:var(--accent); border-color:var(--accent); color:#07101f; font-weight:600; }
  button:disabled { opacity:.5; cursor:default; }
  .seg { display:inline-flex; border:1px solid var(--line); border-radius:9px; overflow:hidden; }
  .seg button { border:0; border-radius:0; background:transparent; }
  .seg button.act { background:#22304d; color:#cfe0ff; }
  .tags { display:flex; gap:6px; flex-wrap:wrap; margin-top:10px; }
  .tag { display:inline-flex; align-items:center; gap:6px; background:#202737; border:1px solid var(--line);
         border-radius:99px; padding:3px 10px; font-size:12.5px; }
  .tag b { cursor:pointer; color:var(--muted); font-weight:400; }
  .tag b:hover { color:var(--bad); }
  footer { position:fixed; left:0; right:0; bottom:0; background:rgba(15,17,21,.92);
           border-top:1px solid var(--line); backdrop-filter:blur(6px); }
  footer .inner { max-width:860px; margin:0 auto; padding:11px 16px; display:flex; gap:12px; align-items:center; }
  #msg { flex:1; color:var(--muted); font-size:13px; }
  #msg.ok { color:var(--ok); } #msg.bad { color:var(--bad); }
  pre#log { margin:0; max-height:230px; overflow:auto; font:12px/1.5 Consolas,monospace; color:#a8b0c0;
            white-space:pre-wrap; word-break:break-all; }
  .muted { color:var(--muted); }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>dchat 机器人</h1>
    <span id="st" class="badge">…</span>
    <span id="addr" class="badge"></span>
    <a id="backAll" href="/bot/" style="display:none;font-size:13px;color:var(--accent);text-decoration:none">← 总览</a>
    <span style="flex:1"></span>
    <button id="refresh">刷新</button>
  </header>

  <div class="card" id="cardRoomManage">
    <h2>群管理</h2>
    <p class="hint">每个群有自己的管理页：群主和群管理员能在那一页改自己群的 Key，看不到别的群。</p>
    <div id="roomLinks" class="tags"></div>
  </div>

  <div class="card" id="cardRooms">
    <h2>接入的群</h2>
    <p class="hint">只回这里打开的群；没打开的群就算有人 @ 它也不理。机器人不是群成员时收不到消息，所以要先加群。</p>
    <div id="rooms"></div>
    <div class="lg">
      <input id="addRoom" type="text" placeholder="群 ID（例如 test）">
      <button id="btnAddRoom">加入并接入</button>
    </div>
    <div class="lg">
      <input id="searchQ" type="text" placeholder="搜索公开的群（关键词）">
      <button id="btnSearch">搜索</button>
    </div>
    <div id="hits" class="tags"></div>
  </div>

  <div class="card" id="cardReply">
    <h2>谁能让它开口</h2>
    <p class="hint">@ 它的人还必须在允许范围内，否则它保持沉默（日志里会记一句）。</p>
    <div class="seg" id="mode">
      <button data-m="all">谁都回</button>
      <button data-m="whitelist">只回白名单</button>
      <button data-m="blacklist">名单里的人不回</button>
    </div>
    <div class="tags" id="list"></div>
    <div class="lg">
      <input id="addWho" type="text" placeholder="昵称">
      <button id="btnAddWho">加入名单</button>
    </div>
  </div>

  <div class="card" id="cardTrigger">
    <h2>触发方式</h2>
    <div class="row">
      <span class="name">只有被 @ 时才回复</span>
      <div id="mentionOnly" class="switch"></div>
    </div>
    <p class="hint" style="margin-top:10px">额外别名（不带 @）——别人 @ds 也算叫它。</p>
    <div class="tags" id="aliases"></div>
    <div class="lg">
      <input id="addAlias" type="text" placeholder="别名">
      <button id="btnAddAlias">添加别名</button>
    </div>
  </div>

  <div class="card" id="cardLocal">
    <h2>本机操作</h2>
    <p class="hint">打开后它能在这个目录里读文件、改文件、改名、跑命令，也能把目录里的文件发到群里。
      群里<strong>任何能 @ 它的人</strong>都能让它做这些 —— 只在信任这个群的时候打开。</p>
    <div class="row">
      <span class="name">允许改本机文件（关掉 = 只能聊天）</span>
      <div id="brainMode" class="switch"></div>
    </div>
    <div class="row"><span class="name">工作目录（它能碰到的全部范围）</span><input id="workDir" type="text" placeholder="例如 D:\codes\dchat-bot\work"></div>
    <div class="row"><span class="name">单文件上限（MB，0 = 不许发文件）</span><input id="fileMaxMb" type="number" min="0" max="256"></div>
    <div class="row"><span class="name">额外可发送的目录（只读，分号分隔）</span><input id="fileRoots" type="text" placeholder="例如 C:\Users\你\Desktop"></div>
    <p class="hint" id="patchHint" style="margin-top:10px"></p>
  </div>

  <div class="card" id="cardBrain">
    <h2>大脑</h2>
    <p class="hint">「本机 DeepSeek Harness」= 起子进程跑 dsh（需要本机装 DSH）；
      「DeepSeek API」= 直接调接口，工具由机器人自己做 —— 这样它可以整个跑在服务器上，
      改的也是服务器上的文件。API Key 只保存在这台机器的配置文件里（权限 600），
      页面永远不回显。</p>
    <div class="row" id="rowBrainType">
      <span class="name">用哪个大脑</span>
      <select id="brainType">
        <option value="dsh">本机 DeepSeek Harness（子进程）</option>
        <option value="api">DeepSeek API（直连，不需要装 DSH）</option>
      </select>
    </div>
    <div class="row" id="rowApiUrl"><span class="name">接口地址</span><input id="apiUrl" type="text" placeholder="https://api.deepseek.com/chat/completions"></div>
    <div class="row" id="rowApiModel"><span class="name">模型</span>
      <select id="apiModel">
        <option value="deepseek-chat">deepseek-chat（快，日常聊天）</option>
        <option value="deepseek-reasoner">deepseek-reasoner（会推理，慢一些）</option>
        <option value="__custom__">其它（自己填）</option>
      </select>
      <input id="apiModelCustom" type="text" placeholder="模型名" style="display:none;max-width:220px">
    </div>
    <div class="row" id="rowApiKey"><span class="name">默认 API Key（只写：留空 = 不修改，填 - 清空）</span><input id="apiKey" type="password" autocomplete="off" placeholder="sk-…"></div>
    <div class="row" id="rowRoomKey" style="display:block">
      <span class="name" id="roomKeyLabel">每个群单独的 Key（一行一个：<code>房间ID = sk-...</code>；<code>房间ID = -</code> 清空；留空用默认）</span>
      <textarea id="apiRoomKeyText" rows="3" autocomplete="off" placeholder="test = sk-xxxxxxxx" style="width:100%;margin-top:6px"></textarea>
    </div>
    <!-- 单群页用这个：只填 key，群名由页面决定，不需要（也不允许）手写房间 ID -->
    <div class="row" id="rowSingleRoomKey" style="display:none">
      <span class="name" id="singleRoomKeyLabel">这个群的 API Key（只写：留空 = 不修改，填 - 清空）</span>
      <input id="singleRoomKey" type="password" autocomplete="off" placeholder="sk-…">
    </div>
    <p class="hint" id="apiKeyHint" style="margin-top:6px"></p>
  </div>

  <div class="card" id="cardTyping">
    <h2>回答怎么分条发</h2>
    <p class="hint">字太多会刷屏，字太少像卡壳。改动保存后立刻生效。</p>
    <div class="row"><span class="name">每条最多几个字</span><input id="maxChars" type="number" min="8" max="4000"></div>
    <div class="row"><span class="name">多短的句子并到下一句</span><input id="minChars" type="number" min="0" max="4000"></div>
    <div class="row"><span class="name">两条之间最少等（毫秒）</span><input id="minGap" type="number" min="0" max="60000"></div>
    <div class="row"><span class="name">每多一个字多等（毫秒）</span><input id="perChar" type="number" min="0" max="5000"></div>
    <div class="row"><span class="name">间隔上限（毫秒）</span><input id="maxGap" type="number" min="0" max="60000"></div>
    <div class="row"><span class="name">被 @ 后先说一句</span><input id="ack" type="text" placeholder="留空 = 不说"></div>
  </div>

  <div class="card">
    <h2>最近的日志</h2>
    <pre id="log">…</pre>
  </div>
</div>

<footer><div class="inner">
  <span id="msg"></span>
  <button id="reload" disabled>撤销修改</button>
  <button id="save" class="primary" disabled>保存</button>
</div></footer>

<script>
(function () {
  var state = null, dirty = false;
  var el = function (id) { return document.getElementById(id); };
  function api(path, body) {
    // 单群页挂在 /room/<群>/bot 上（经网关转发），相对路径会解析成 /room/<群>/api/...，
    // 所以这种情况下一律走 /bot/ 前缀 —— 网关认得它。
    var base = window.__ROOM__ ? '/bot/' : '';
    var opt = { method: body ? 'POST' : 'GET', headers: { 'X-Dchat-Bot': '1' } };
    if (body) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }
    return fetch(base + path, opt).then(function (r) { return r.json(); });
  }
  function setMsg(text, cls) {    var m = el('msg'); m.textContent = text || ''; m.className = cls || '';
  }
  function markDirty() {
    dirty = true; el('save').disabled = false; el('reload').disabled = false;
    if (!el('msg').textContent) setMsg('有未保存的修改');
  }
  function tags(host, items, onRemove) {
    host.innerHTML = '';
    if (!items.length) {
      var none = document.createElement('span');
      none.className = 'muted'; none.textContent = '（空）'; host.appendChild(none); return;
    }
    items.forEach(function (item) {
      var t = document.createElement('span'); t.className = 'tag';
      t.appendChild(document.createTextNode(item));
      var x = document.createElement('b'); x.textContent = '×'; x.title = '移除';
      x.onclick = function () { onRemove(item); };
      t.appendChild(x); host.appendChild(t);
    });
  }
  function render() {
    el('st').textContent = state.online ? '在线' : '离线';
    el('st').className = 'badge ' + (state.online ? 'on' : 'off');
    el('addr').textContent = state.nick + ' @ ' + state.server;

    // 单群页（/room/<群>/bot）：只留「这个群」能改的东西。
    // 群主/群管理员看到的就这一块；全局设置整块藏掉，免得以为改得动。
    if (window.__ROOM__) {
      var hideIds = ['cardRooms', 'cardReply', 'cardTrigger', 'cardLocal', 'cardTyping'];
      hideIds.forEach(function (id) {
        var node = document.getElementById(id);
        if (node) node.style.display = 'none';
      });
      ['rowBrainType', 'rowApiUrl', 'rowApiModel', 'rowApiKey', 'rowRoomKey'].forEach(function (id) {
        var node = document.getElementById(id);
        if (node) node.style.display = 'none';
      });
      // 单群页只要一个"这个群的 key"输入框：群名由地址决定，
      // 不让用户再手写 `房间 = sk-...`（写错就是跨群改配置）。
      var single = document.getElementById('rowSingleRoomKey');
      if (single) single.style.display = '';
      var singleLabel = document.getElementById('singleRoomKeyLabel');
      if (singleLabel) {
        singleLabel.innerHTML = '「' + window.__ROOM__ + '」的 API Key（只写：留空=不改，填 <code>-</code> 清空）';
      }
      var h2 = document.querySelector('#cardBrain h2');
      if (h2) h2.textContent = '群「' + window.__ROOM__ + '」的设置';
      // 能管所有群的人，从单群页要能回总览（否则点进来就出不去了）
      var back = document.getElementById('backAll');
      if (back && state.canManageAll) back.style.display = '';
    } else if (!state.canManageAll) {
      // 只管理某些群的人打开总览：全局设置他也改不了，藏掉，只留"群管理"和日志
      ['cardRooms', 'cardReply', 'cardTrigger', 'cardLocal', 'cardTyping', 'cardBrain']
        .forEach(function (id) {
          var node = document.getElementById(id);
          if (node) node.style.display = 'none';
        });
      var saveBtn = document.getElementById('save');
      if (saveBtn) saveBtn.style.display = 'none';
      var reloadBtn = document.getElementById('reload');
      if (reloadBtn) reloadBtn.style.display = 'none';
    }

    // 群管理入口：管理员看到所有群，群主/群管理员只看到自己那几个
    var linksHost = document.getElementById('roomLinks');
    if (linksHost) {
      linksHost.innerHTML = '';
      var canAll = !!state.canManageAll;
      var mine = state.opRooms || [];
      var list = (state.known || []).filter(function (room) {
        if (canAll) return true;
        for (var i = 0; i < mine.length; i++) {
          if (mine[i].toLowerCase() === String(room.id).toLowerCase()) return true;
        }
        return false;
      });
      if (!list.length) {
        var none = document.createElement('span');
        none.className = 'muted'; none.textContent = '（没有你能管理的群）';
        linksHost.appendChild(none);
      } else {
        list.forEach(function (room) {
          var a = document.createElement('a');
          a.className = 'tag';
          a.style.textDecoration = 'none';
          a.href = '/room/' + encodeURIComponent(room.id) + '/bot';
          a.textContent = (room.name || room.id) + ' ›';
          linksHost.appendChild(a);
        });
      }
    }

    var rooms = el('rooms'); rooms.innerHTML = '';
    state.known.forEach(function (room) {
      var row = document.createElement('div'); row.className = 'row';
      var name = document.createElement('span'); name.className = 'name';
      name.textContent = room.name || room.id;
      var id = document.createElement('span'); id.className = 'id'; id.textContent = room.kind === 'hall' ? '大厅' : room.id;
      var sw = document.createElement('div');
      sw.className = 'switch' + (state.rooms.indexOf(room.id) >= 0 ? ' on' : '');
      sw.title = '点击切换是否接入';
      sw.onclick = function () {
        var at = state.rooms.indexOf(room.id);
        if (at >= 0) state.rooms.splice(at, 1); else state.rooms.push(room.id);
        sw.classList.toggle('on'); markDirty();
      };
      row.appendChild(name); row.appendChild(id); row.appendChild(sw);
      rooms.appendChild(row);
    });
    if (!state.known.length) rooms.innerHTML = '<p class="muted">还没加入任何群。下面按群 ID 加入，或先搜索。</p>';

    document.querySelectorAll('#mode button').forEach(function (b) {
      b.classList.toggle('act', b.dataset.m === state.replyMode);
    });
    tags(el('list'), state.replyList, function (nick) {
      state.replyList = state.replyList.filter(function (x) { return x !== nick; });
      render(); markDirty();
    });
    tags(el('aliases'), state.aliases, function (a) {
      state.aliases = state.aliases.filter(function (x) { return x !== a; });
      render(); markDirty();
    });
    el('mentionOnly').className = 'switch' + (state.mentionOnly ? ' on' : '');
    el('brainMode').className = 'switch' + (state.brainMode === 'local' ? ' on' : '');
    el('workDir').value = state.workDir || '';
    el('fileMaxMb').value = state.fileMaxMb;
    el('fileRoots').value = (state.fileRoots || []).join(';');
    el('patchHint').textContent = state.patch
      ? '当前生效的 DSH 补丁：' + state.patch
      : '没有生效的补丁：headless 下没人能点"允许"，需要补丁才能让工具真的执行。';
    el('brainType').value = state.brainType || 'dsh';
    el('apiUrl').value = state.apiUrl || '';
    // 模型用下拉：常见两个直接选，"其它"才出现输入框
    var knownModels = ['deepseek-chat', 'deepseek-reasoner'];
    var model = state.apiModel || 'deepseek-chat';
    if (knownModels.indexOf(model) >= 0) {
      el('apiModel').value = model;
      el('apiModelCustom').style.display = 'none';
      el('apiModelCustom').value = '';
    } else {
      el('apiModel').value = '__custom__';
      el('apiModelCustom').style.display = '';
      el('apiModelCustom').value = model;
    }
    // API Key 是**只写**的：页面永远拿不到 key 的任何片段，只能设置/替换/清空。
    // 想看 key 只有一条路 —— 上服务器看 /etc/dchat-bot/bot.conf（权限 600）。
    el('apiKey').value = '';
    el('apiKey').placeholder = state.apiKeySet
      ? '已设置（不可查看；留空=不改，填 - 清空）'
      : '还没设置，粘贴 sk-… 后保存';
    var roomKeyRooms = (state.apiRoomKeys || []).map(function (item) { return item.room; });
    el('apiRoomKeyText').value = '';
    if (window.__ROOM__) {
      // 单群页：这个文本框只管当前这一个群，别群的 key 在这里改不了
      el('apiRoomKeyText').placeholder = window.__ROOM__ + ' = sk-xxxxxxxx（只管这个群）';
      el('apiKeyHint').textContent =
        '这一页只管「' + window.__ROOM__ + '」这个群。群主/群管理员能改这里的 Key；' +
        '其余设置（白名单、分条、工作目录…）归管理员。' +
        (roomKeyRooms.indexOf(window.__ROOM__) >= 0 ? ' 这个群已经单独配了 key（要换就重写一行，写 - 清空）。' : '');
    } else {
      el('apiRoomKeyText').placeholder = 'test = sk-xxxxxxxx（一行一个，只写不回显）';
      el('apiKeyHint').textContent = (state.brainType === 'api'
        ? '当前用 API 直连；工具（读写文件、跑命令、发文件）由机器人自己执行，范围就是上面的「工作目录」。'
        : '当前用本机 DSH；要跑在服务器上就切到 API 直连。') +
        (roomKeyRooms.length ? ' 已单独配了 key 的群：' + roomKeyRooms.join('、') + '（要改就重新写一行，写 - 清空）' : '');
      el('apiKeyHint').textContent += ' Key 只保存在服务器上的配置文件里（权限 600），界面不回显。';
    }
    el('maxChars').value = state.typing.maxChars;
    el('minChars').value = state.typing.minChars;
    el('minGap').value = state.typing.minGapMs;
    el('perChar').value = state.typing.perCharMs;
    el('maxGap').value = state.typing.maxGapMs;
    el('ack').value = state.ackText;
    el('log').textContent = (state.log && state.log.length) ? state.log.join('\n') : '（还没有日志）';
  }
  function load(keepMsg) {
    return api('api/state').then(function (data) {
      if (!data.ok) { setMsg(data.error || '读取失败', 'bad'); return; }
      state = data;
      dirty = false; el('save').disabled = true; el('reload').disabled = true;
      if (!keepMsg) setMsg('');
      render();
    }).catch(function (e) { setMsg('连不上机器人：' + e, 'bad'); });
  }
  el('refresh').onclick = function () { load(); };
  el('reload').onclick = function () { load(); };
  // 任何输入框 / 下拉框 / 开关一动就点亮"保存"。
  // 之前只有少数几个控件手写了 markDirty，**文本输入框一个都没绑** ——
  // 改了字按钮还是灰的，用户只会觉得"保存按钮坏了"（实测就卡在这）。
  document.querySelectorAll('input, textarea, select').forEach(function (node) {
    node.addEventListener('input', markDirty);
    node.addEventListener('change', markDirty);
  });
  document.querySelectorAll('#mode button').forEach(function (b) {
    b.onclick = function () { state.replyMode = b.dataset.m; render(); markDirty(); };
  });
  el('mentionOnly').onclick = function () {
    state.mentionOnly = !state.mentionOnly;
    el('mentionOnly').className = 'switch' + (state.mentionOnly ? ' on' : '');
    markDirty();
  };
  el('brainMode').onclick = function () {
    state.brainMode = (state.brainMode === 'local') ? 'chat' : 'local';
    el('brainMode').className = 'switch' + (state.brainMode === 'local' ? ' on' : '');
    markDirty();
  };
  function collect() {
    return {
      rooms: state.rooms,
      mentionOnly: state.mentionOnly,
      aliases: state.aliases,
      replyMode: state.replyMode,
      replyList: state.replyList,
      ackText: el('ack').value,
      brainMode: state.brainMode,
      brainType: el('brainType').value,
      apiUrl: el('apiUrl').value.trim(),
      apiModel: (el('apiModel').value === '__custom__'
                 ? el('apiModelCustom').value.trim()
                 : el('apiModel').value.trim()),
      apiKey: el('apiKey').value.trim(),
      // 单群页：key 单独一个框，群名用地址里的那个，不让手写房间 ID
      apiRoomKeyText: window.__ROOM__
        ? (function () {
            var v = el('singleRoomKey').value.trim();
            if (!v) return '';                       // 留空 = 不改
            return window.__ROOM__ + ' = ' + v;      // 后端会再校验一次群名
          })()
        : el('apiRoomKeyText').value,
      // 单群页：告诉后端"这一页只管这个群"，后端据此只放行这个群的改动
      room: window.__ROOM__ || '',
      workDir: el('workDir').value,
      fileMaxMb: parseInt(el('fileMaxMb').value, 10),
      fileRoots: el('fileRoots').value.split(';').map(function (s) { return s.trim(); })
                   .filter(function (s) { return s.length > 0; }),
      maxChars: parseInt(el('maxChars').value, 10),
      minChars: parseInt(el('minChars').value, 10),
      minGapMs: parseInt(el('minGap').value, 10),
      perCharMs: parseInt(el('perChar').value, 10),
      maxGapMs: parseInt(el('maxGap').value, 10)
    };
  }
  el('apiModel').onchange = function () {
    var custom = el('apiModel').value === '__custom__';
    el('apiModelCustom').style.display = custom ? '' : 'none';
    if (custom) el('apiModelCustom').focus();
    markDirty();
  };
  el('save').onclick = function () {
    el('save').disabled = true;
    setMsg('保存中…');
    api('api/config', collect()).then(function (data) {
      if (!data.ok) { setMsg('保存失败：' + data.error, 'bad'); el('save').disabled = false; return; }
      state = data.state;
      dirty = false; el('reload').disabled = true;
      render();
      setMsg('已保存，立刻生效', 'ok');
      setTimeout(function () { if (!dirty) setMsg(''); }, 2500);
    }).catch(function (e) { setMsg('保存失败：' + e, 'bad'); el('save').disabled = false; });
  };
  el('btnAddWho').onclick = function () {
    var v = el('addWho').value.trim(); if (!v) return;
    if (state.replyList.indexOf(v) < 0) state.replyList.push(v);
    el('addWho').value = ''; render(); markDirty();
  };
  el('btnAddAlias').onclick = function () {
    var v = el('addAlias').value.trim(); if (!v) return;
    v = v.replace(/^@/, '');
    if (state.aliases.indexOf(v) < 0) state.aliases.push(v);
    el('addAlias').value = ''; render(); markDirty();
  };
  el('btnSearch').onclick = function () {
    var q = el('searchQ').value.trim(); if (!q) return;
    setMsg('搜索中…');
    api('api/search', { q: q }).then(function (data) {
      setMsg('');
      var hits = el('hits'); hits.innerHTML = '';
      if (!data.ok) { setMsg('搜索失败：' + data.error, 'bad'); return; }
      if (!data.hits.length) { hits.innerHTML = '<span class="muted">（没搜到）</span>'; return; }
      data.hits.forEach(function (hit) {
        var b = document.createElement('button');
        b.textContent = hit.name + '（' + hit.members + ' 人）· 加入并接入';
        b.onclick = function () { joinRoom(hit.id, b); };
        hits.appendChild(b);
      });
    });
  };
  function joinRoom(id, btn) {
    if (btn) btn.disabled = true;
    setMsg('正在加入 ' + id + '…');
    api('api/join', { room: id }).then(function (data) {
      if (!data.ok) { setMsg('加入失败：' + data.error, 'bad'); if (btn) btn.disabled = false; return; }
      setMsg(data.note || '已加入并接入', 'ok');
      return load(true);
    });
  }
  el('btnAddRoom').onclick = function () {
    var v = el('addRoom').value.trim(); if (!v) return;
    joinRoom(v.toLowerCase(), el('btnAddRoom'));
    el('addRoom').value = '';
  };
  load();
})();
</script>
</body>
</html>)HTML";

}  // namespace dshbot
}  // namespace dchat
