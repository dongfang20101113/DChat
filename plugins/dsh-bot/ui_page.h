// 管理界面的那一页 HTML（内联进二进制，和 webd 的做法一致）
//
// 为什么不拆成 .html 文件：那样部署时要多管一个"别弄丢"的文件，而它只有一页。
// 拼进二进制以后，`dchat-dsh-bot` 拷到哪都能直接开界面。
//
// 前端刻意不用任何框架：一页表单，用原生 fetch + DOM 就够；引入 React 等于
// 给一个改三个开关的界面加一整套构建流程。
#pragma once

namespace dchat {
namespace dshbot {

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
  input[type=text], input[type=number] { background:#0d1015; color:var(--text); border:1px solid var(--line);
         border-radius:8px; padding:7px 10px; font:inherit; min-width:0; }
  input[type=text] { flex:1; }
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
    <span style="flex:1"></span>
    <button id="refresh">刷新</button>
  </header>

  <div class="card">
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

  <div class="card">
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

  <div class="card">
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

  <div class="card">
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
    var opt = { method: body ? 'POST' : 'GET', headers: { 'X-Dchat-Bot': '1' } };
    if (body) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }
    return fetch(path, opt).then(function (r) { return r.json(); });
  }
  function setMsg(text, cls) {
    var m = el('msg'); m.textContent = text || ''; m.className = cls || '';
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
    el('maxChars').value = state.typing.maxChars;
    el('minChars').value = state.typing.minChars;
    el('minGap').value = state.typing.minGapMs;
    el('perChar').value = state.typing.perCharMs;
    el('maxGap').value = state.typing.maxGapMs;
    el('ack').value = state.ackText;
    el('log').textContent = (state.log && state.log.length) ? state.log.join('\n') : '（还没有日志）';
  }
  function load(keepMsg) {
    return api('/api/state').then(function (data) {
      if (!data.ok) { setMsg(data.error || '读取失败', 'bad'); return; }
      state = data;
      dirty = false; el('save').disabled = true; el('reload').disabled = true;
      if (!keepMsg) setMsg('');
      render();
    }).catch(function (e) { setMsg('连不上机器人：' + e, 'bad'); });
  }
  el('refresh').onclick = function () { load(); };
  el('reload').onclick = function () { load(); };
  document.querySelectorAll('#mode button').forEach(function (b) {
    b.onclick = function () { state.replyMode = b.dataset.m; render(); markDirty(); };
  });
  el('mentionOnly').onclick = function () {
    state.mentionOnly = !state.mentionOnly;
    el('mentionOnly').className = 'switch' + (state.mentionOnly ? ' on' : '');
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
      maxChars: parseInt(el('maxChars').value, 10),
      minChars: parseInt(el('minChars').value, 10),
      minGapMs: parseInt(el('minGap').value, 10),
      perCharMs: parseInt(el('perChar').value, 10),
      maxGapMs: parseInt(el('maxGap').value, 10)
    };
  }
  el('save').onclick = function () {
    el('save').disabled = true;
    setMsg('保存中…');
    api('/api/config', collect()).then(function (data) {
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
    api('/api/search', { q: q }).then(function (data) {
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
    api('/api/join', { room: id }).then(function (data) {
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
