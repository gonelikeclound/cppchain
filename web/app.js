/* wfr CGI · 前端逻辑（原生 JS，调 CGI action；NDJSON 流式对话 + 二级树 + 双 tab） */
"use strict";

// CGI 入口：本地用 http.server --cgi 时的映射。/cgi-bin/ → web 根下 cgi-bin（symlink 到 ../cgi-bin）
const CGI = "cgi-bin/wfr_cgi";

const state = {
  tree: [],          // [{name, dir, sessions:[{name, pending?}], expanded}]
  ws: null,          // 选中 workspace 名
  session: null,     // 选中 session 全名（wfname.sessionid）
  tab: "chat",       // "chat" | "debug"
};

const $ = (id) => document.getElementById(id);

function qs(params) {
  const u = new URLSearchParams();
  for (const [k, v] of Object.entries(params)) if (v !== undefined && v !== null) u.set(k, v);
  return `${CGI}?${u.toString()}`;
}

async function raw(action, params) {
  const resp = await fetch(qs({ action, ...params }));
  const text = await resp.text();
  try { return JSON.parse(text); } catch { throw new Error("CGI 返回非 JSON: " + text.slice(0, 200)); }
}

function currentWs() {
  return state.tree.find(t => t.name === state.ws) || null;
}
function currentSession() {
  const w = currentWs();
  return w ? w.sessions.find(s => s.name === state.session) : null;
}

// ---- 轻量 Markdown 渲染（本地实现，零依赖；先 escape 再转标记防注入）----
function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
          .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}

// 行内语法：`code` **bold** *italic* [text](url)（按序处理，code 优先避免误吞）
function renderInline(s) {
  let out = "";
  // 用占位符保护行内代码，避免其中的 **/* 被后续误转
  const codes = [];
  s = s.replace(/`([^`]+)`/g, (_, c) => {
    codes.push("<code>" + escapeHtml(c) + "</code>");
    return "\u0000" + (codes.length - 1) + "\u0000";
  });
  // 链接（只放行 http/https，防 javascript:）
  s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
  // 粗体
  s = s.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
  // 斜体
  s = s.replace(/\*([^*]+)\*/g, "<i>$1</i>");
  // 还原行内代码
  out = s.replace(/\u0000(\d+)\u0000/g, (_, n) => codes[+n]);
  return out;
}

// 块级渲染：标题/引用/列表/代码块/表格/段落
function renderMarkdown(src) {
  const lines = String(src || "").replace(/\r\n/g, "\n").split("\n");
  const html = [];
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];

    // 代码块 ```lang / ```
    if (/^```/.test(line.trim())) {
      const lang = line.trim().slice(3).trim();
      const buf = [];
      ++i;
      while (i < lines.length && !/^```\s*$/.test(lines[i])) { buf.push(lines[i]); ++i; }
      ++i;  // 跳过闭合 ```
      html.push('<pre class="md-code"><code>' + escapeHtml(buf.join("\n")) + "</code></pre>");
      continue;
    }

    // 表格：连续的 | 行（首行表头 + --- 分隔）
    if (line.trim().startsWith("|") && i + 1 < lines.length && /^[\s|:-]+$/.test(lines[i + 1].trim())) {
      const rows = [];
      while (i < lines.length && lines[i].trim().startsWith("|")) { rows.push(lines[i].trim()); ++i; }
      html.push(renderTable(rows));
      continue;
    }

    // 标题
    const h = /^(#{1,6})\s+(.*)$/.exec(line.trim());
    if (h) {
      const lv = h[1].length;
      html.push(`<h${lv} class="md-h">` + renderInline(h[2]) + `</h${lv}>`);
      ++i;
      continue;
    }

    // 引用：连续 >
    if (/^>\s?/.test(line.trim())) {
      const buf = [];
      while (i < lines.length && /^>\s?/.test(lines[i].trim())) { buf.push(lines[i].trim().replace(/^>\s?/, "")); ++i; }
      html.push("<blockquote>" + buf.map(l => renderInline(l)).join("<br>") + "</blockquote>");
      continue;
    }

    // 无序列表：连续 - / * 项
    if (/^(\s*)[-*]\s+/.test(line)) {
      const buf = [];
      while (i < lines.length && /^(\s*)[-*]\s+/.test(lines[i])) { buf.push(lines[i].replace(/^\s*[-*]\s+/, "")); ++i; }
      html.push("<ul>" + buf.map(l => "<li>" + renderInline(l) + "</li>").join("") + "</ul>");
      continue;
    }

    // 空行跳过
    if (!line.trim()) { ++i; continue; }

    // 普通段落：收集到空行/下一个块级标记
    const buf = [line];
    ++i;
    while (i < lines.length && lines[i].trim() &&
           !/^```/.test(lines[i].trim()) && !/^#{1,6}\s/.test(lines[i].trim()) &&
           !/^>\s?/.test(lines[i].trim()) && !/^(\s*)[-*]\s+/.test(lines[i]) &&
           !lines[i].trim().startsWith("|")) {
      buf.push(lines[i]); ++i;
    }
    html.push("<p>" + buf.map(l => renderInline(l)).join("<br>") + "</p>");
  }
  return html.join("");
}

function renderTable(rows) {
  // rows[0]=表头；rows[1]=|---|；其余数据
  const cells = r => r.replace(/^\||\|$/g, "").split("|").map(c => c.trim());
  const head = cells(rows[0]);
  const body = rows.slice(2).map(r => "<tr>" + cells(r).map(c => "<td>" + renderInline(c) + "</td>").join("") + "</tr>").join("");
  return '<table class="md-table"><thead><tr>' +
         head.map(c => "<th>" + renderInline(c) + "</th>").join("") +
         "</tr></thead><tbody>" + body + "</tbody></table>";
}

// ---- 消息渲染 ----
function appendMsg(type, text, think) {
  const box = $("messages");
  const el = document.createElement("div");
  el.className = "msg " + type;
  if (think && think.trim()) {
    const t = document.createElement("div");
    t.className = "think"; t.textContent = "💭 " + think.trim();
    el.appendChild(t);
  }
  const body = document.createElement("div");
  body.className = "md-body";
  body.innerHTML = renderMarkdown(text);
  el.appendChild(body);
  box.appendChild(el);
  box.scrollTop = box.scrollHeight;
}

// 从 Message 的 content[] 提取可显示文本（Message JSON 里 text 在 content[].text，reasoning 在 content[].reasoning）
function msgText(m) {
  const parts = [];
  for (const blk of (m.content || [])) {
    if (typeof blk === "string") parts.push(blk);
    else if (blk && blk.type === "text" && blk.text) parts.push(blk.text);
    else if (blk && blk.type === "reasoning" && blk.reasoning) parts.push("[think] " + blk.reasoning);
  }
  return parts.join("\n");
}

// 用已读历史填充（user/ai 文本 + tool 折叠为灰色行）
function renderHistory(history) {
  $("messages").textContent = "";
  for (const m of history) {
    const text = msgText(m);
    if (m.type === "human") appendMsg("user", text, "");
    else if (m.type === "ai") appendMsg("ai", text, "");
    else if (m.type === "tool" && text) {
      const el = document.createElement("div");
      el.className = "tool-line";
      el.textContent = "🔧 工具结果： " + text.slice(0, 160) + (text.length > 160 ? "…" : "");
      $("messages").appendChild(el);
    }
  }
}

// ---- 二级树（workspace → session）----
async function loadTree() {
  const r = await raw("workspaces");
  const wss = r.workspaces || [];
  // 保留现有 expanded 状态
  const expandedPrev = {};
  for (const t of state.tree) expandedPrev[t.name] = t.expanded;
  state.tree = [];
  for (const w of wss) {
    const sessions = [];
    try {
      const sr = await raw("sessions", { ws: w.dir });
      for (const s of (sr.sessions || []))
        sessions.push({ name: s.name, pending: false });
    } catch (e) { /* 目录扫描失败：保留空 sessions */ }
    state.tree.push({ name: w.name, dir: w.dir, sessions, expanded: expandedPrev[w.name] !== undefined ? expandedPrev[w.name] : true });
  }
  if (state.ws && !state.tree.find(t => t.name === state.ws)) { state.ws = null; state.session = null; }
  if (!state.ws && state.tree.length) { state.ws = state.tree[0].name; state.session = null; }
  renderTree();
  // 若无选中会话，自动选第一个真实会话（同时设所属 workspace）
  const w = currentWs();
  if (w && w.sessions.length && !state.session) selectSession(w.sessions[0].name, w.name);
}

function renderTree() {
  const box = $("tree");
  box.textContent = "";
  if (!state.tree.length) { box.innerHTML = '<div class="empty-hint">暂无工作区，点【＋工作区】</div>'; return; }
  for (const t of state.tree) {
    const wsRow = document.createElement("div");
    wsRow.className = "ws-row" + (state.ws === t.name ? " active" : "");
    const arrow = document.createElement("span");
    arrow.className = "arrow"; arrow.textContent = t.expanded ? "▾" : "▸";
    wsRow.appendChild(arrow);
    const label = document.createElement("span");
    label.className = "ws-label"; label.textContent = t.name;
    wsRow.appendChild(label);
    const count = document.createElement("span");
    count.className = "ws-count"; count.textContent = t.sessions.length;
    wsRow.appendChild(count);
    wsRow.addEventListener("click", () => toggleWs(t.name));
    box.appendChild(wsRow);

    if (t.expanded) {
      if (!t.sessions.length)
        box.appendChild(elDiv("empty-hint", "无会话"));
      for (const s of t.sessions) {
        const sRow = document.createElement("div");
        sRow.className = "session-row" + (state.ws === t.name && state.session === s.name ? " active" : "");
        // 遗留无名会话（无点）置灰不可选
        const legacy = !s.name.includes(".");
        sRow.classList.toggle("legacy", legacy);
        const dot = document.createElement("span");
        dot.className = "dot"; dot.textContent = s.pending ? "⏳" : (legacy ? "•" : "●");
        sRow.appendChild(dot);
        const sLabel = document.createElement("span");
        sLabel.className = "s-label";
        sLabel.textContent = s.pending ? (s.name + " (未发送)") : s.name;
        sRow.appendChild(sLabel);
        // 所有 session 行都可点击选中（看历史）；legacy 仅样式置灰、debug 端拒绝
        sRow.addEventListener("click", () => selectSession(s.name, t.name));
        if (legacy) sRow.title = "遗留会话（未绑定 workflow，可查看历史但不能调试）";
        const del = document.createElement("button");
        del.className = "x"; del.textContent = "✕";
        del.addEventListener("click", (e) => { e.stopPropagation(); archiveSession(s.name); });
        sRow.appendChild(del);
        box.appendChild(sRow);
      }
    }
  }
}

function elDiv(cls, text) {
  const d = document.createElement("div");
  d.className = cls; d.textContent = text;
  return d;
}

function toggleWs(name) {
  const t = state.tree.find(x => x.name === name);
  if (!t) return;
  t.expanded = !t.expanded;
  state.ws = name;
  state.session = null;
  $("title-ws").textContent = name;
  renderTree();
  if (t.sessions.length) selectSession(t.sessions[0].name, name);
  else setTab("chat");
}

// ---- 会话 ----
// 选中会话：必须同时设所属 workspace（否则 currentWs() 可能指向别的工作区）
async function selectSession(name, wsName) {
  if (wsName) {
    const t = state.tree.find(x => x.name === wsName);
    if (t) t.expanded = true;
    state.ws = wsName;
    $("title-ws").textContent = wsName;
  }
  state.session = name;
  $("title-s").textContent = name;
  const w = currentWs();
  if (!w) return;
  // 清 debug 缓存（会话变了下次重载）
  $("dbg-frame").dataset.sid = "";
  const r = await raw("history", { ws: w.dir, s: name });
  renderHistory(r.messages || []);
  renderTree();  // 高亮当前行
}

function archiveSession(name) {
  const w = currentWs();
  if (w) w.sessions = w.sessions.filter(s => s.name !== name);
  if (state.session === name) { state.session = null; $("title-s").textContent = "—"; $("messages").textContent = ""; }
  renderTree();
}

// ---- 新建 workspace（modal）----
function openWsModal() {
  $("ws-name").value = "";
  $("ws-dir").value = "";
  $("ws-modal").classList.remove("hidden");
  $("ws-name").focus();
}
async function submitWs() {
  const name = $("ws-name").value.trim();
  const dir = $("ws-dir").value.trim();
  $("ws-modal").classList.add("hidden");
  try {
    const r = await raw("workspace_create", { name, dir });
    if (r.error) throw new Error(r.error);
    await loadTree();
    // 展开并选中新建 ws
    const t = state.tree.find(x => x.name === name);
    if (t) { t.expanded = true; state.ws = name; renderTree(); }
  } catch (e) {
    $("messages").innerHTML = '<div class="error-line">建工作区失败: ' + e.message + '</div>';
  }
}

// ---- 新建会话（modal：选 workflow）----
async function openSsModal() {
  const w = currentWs();
  if (!w) { alert("请先选中工作区"); return; }
  const sel = $("ss-wf");
  sel.textContent = "";
  try {
    const r = await raw("workflows", { ws: w.dir });
    const wfs = r.workflows || [];
    if (!wfs.length) { alert("该工作区没有可绑定的 workflow"); return; }
    for (const wf of wfs) {
      const o = document.createElement("option");
      o.value = wf.name;
      o.textContent = wf.name + "（" + wf.source + "）";
      sel.appendChild(o);
    }
    $("ss-modal").classList.remove("hidden");
  } catch (e) {
    alert("拉取 workflow 失败: " + e.message);
  }
}
async function submitSs() {
  const w = currentWs();
  if (!w) return;
  const wf = $("ss-wf").value;
  $("ss-modal").classList.add("hidden");
  const sid = "s" + Math.floor(Date.now() / 1000);
  const name = wf + "." + sid;
  // pending 占位：首条消息发出才真正写日志
  w.sessions.push({ name, pending: true });
  state.session = name;
  $("title-s").textContent = name;
  $("messages").textContent = "";
  renderTree();
  setTab("chat");
}

// ---- 双 tab ----
function setTab(t) {
  state.tab = t;
  $("tab-chat").classList.toggle("active", t === "chat");
  $("tab-debug").classList.toggle("active", t === "debug");
  $("chat-pane").classList.toggle("hidden", t !== "chat");
  $("debug-pane").classList.toggle("hidden", t !== "debug");
  if (t === "debug") updateDebugUI();        // 调试 tab：按钮按状态显示
  else $("dbg-refresh").classList.add("hidden");  // 对话 tab：隐藏调试按钮
}

// 调试 tab：工具条状态管理——能生成则显示「⚙ 生成」，已生成则显示「⟳ 刷新」
function canDebug() {
  // 可调试 = 已绑定 workflow（名字带点）的会话；pending 也可（未发消息时 debug 端返回"尚无记录"）
  const s = currentSession();
  return !!s && !!state.session && state.session.includes(".");
}

function updateDebugUI() {
  const frame = $("dbg-frame");
  const ph = $("dbg-placeholder");
  const ref = $("dbg-refresh");

  if (!canDebug()) {
    // 无法生成：隐藏 iframe 与生成按钮，只显示占位说明
    frame.classList.add("hidden"); frame.srcdoc = ""; frame.dataset.sid = "";
    ph.classList.remove("hidden");
    ref.classList.add("hidden");
    return;
  }
  // 可生成：显示按钮（未生成→「⚙ 生成」；已生成→「⟳ 刷新」）+ 隐藏占位
  ph.classList.add("hidden");
  ref.classList.remove("hidden");
  if (frame.dataset.sid !== state.session) {
    // 尚未生成：按钮是「生成」，iframe 隐藏
    frame.classList.add("hidden");
    ref.textContent = "⚙ 生成";
  } else {
    frame.classList.remove("hidden");
    ref.textContent = "⟳ 刷新";
  }
}

// 生成/刷新调试页：未生成（dataset.sid 未设）→ 生成；已生成 → 重新生成
function toggleDebug() {
  const w = currentWs();
  if (!canDebug()) return;  // 按钮只在可调试时显示，正常不会走到这
  const frame = $("dbg-frame");
  if (frame.dataset.sid === state.session) {
    // 刷新：清缓存重载
    frame.dataset.sid = "";
    frame.src = "";
  }
  frame.src = qs({ action: "debug", ws: w.dir, s: state.session });
  frame.dataset.sid = state.session;
  frame.classList.remove("hidden");
  $("dbg-placeholder").classList.add("hidden");
  $("dbg-refresh").textContent = "⟳ 刷新";
  frame.addEventListener("load", () => { frame.classList.remove("hidden"); }, { once: true });
}

// ---- 发送（流式：NDJSON 事件逐行渲染）----
function appendTool(text) {
  const box = $("messages");
  const el = document.createElement("div");
  el.className = "tool-line";
  el.textContent = "🔧 工具结果： " + (text || "").slice(0, 200) + ((text || "").length > 200 ? "…" : "");
  box.appendChild(el);
  box.scrollTop = box.scrollHeight;
}

// 解析一条事件：把 model/run_tools 的 node_end payload.messages 增量渲染（AI 文本 / 工具结果）
function renderStreamEvent(e) {
  if (e.type !== "node_end") return;
  // 跳过 load_history：它的 payload.messages 是**整段历史回放**（第 N 轮会把前 N-1 轮全量带上），
  // 若当增量渲染会导致历史反复叠加（"AbCd→AbCad"）。历史由 selectSession 的 renderHistory 负责。
  if (e.node_kind === "load_history") return;
  const msgs = (e.payload && e.payload.messages) || [];
  for (const m of msgs) {
    if (m.type === "ai") {
      // 有 tool_calls 的 AI：预告工具（下一事件会渲染 TOOL 结果）
      const calls = m.tool_calls || [];
      const txt = msgText(m);
      if (txt.trim()) appendMsg("ai", txt, "");
      for (const c of calls) {
        const el = document.createElement("div");
        el.className = "tool-line";
        el.textContent = "⚙️ 调用工具 " + (c.name || "?") + " " + JSON.stringify(c.args || {}).slice(0, 120);
        $("messages").appendChild(el);
      }
      $("messages").scrollTop = $("messages").scrollHeight;
    } else if (m.type === "tool") {
      appendTool(msgText(m));
    }
  }
}

async function streamChat(ws, s, q) {
  const resp = await fetch(qs({ action: "chat", ws, s, q }));
  if (!resp.ok) throw new Error("HTTP " + resp.status);
  if (!resp.body) { const r = await resp.json(); throw new Error(r.error || "无流"); }
  const reader = resp.body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let nl;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line || !line.startsWith("{")) continue;
      let e;
      try { e = JSON.parse(line); } catch { continue; }
      if (e.type === "end") return e.exit || 0;
      renderStreamEvent(e);
    }
  }
  return 0;
}

async function send() {
  const q = $("input").value.trim();
  if (!q) return;
  const w = currentWs();
  if (!w) return;
  // 必须有绑定 workflow 的 session（pending 占位也可——首条消息触发 wfr 真正写日志）
  if (!state.session || !state.session.includes(".")) {
    $("messages").appendChild(elDiv("error-line", "请先选 workflow 新建会话"));
    return;
  }
  appendMsg("user", q, "");
  $("input").value = "";
  setBusy(true);
  try {
    await streamChat(w.dir, state.session, q);
    // 刷新树：pending 占位被真实条目替换
    await refreshWs(w.name);
  } catch (e) {
    $("messages").appendChild(elDiv("error-line", "请求失败: " + e.message));
  } finally {
    setBusy(false);
  }
}

async function refreshWs(name) {
  const t = state.tree.find(x => x.name === name);
  if (!t) return;
  try {
    const r = await raw("sessions", { ws: t.dir });
    const real = (r.sessions || []).map(s => ({ name: s.name, pending: false }));
    t.sessions = real;
    renderTree();
  } catch (e) { /* 目录扫描失败：保留现状 */ }
}

function setBusy(b) {
  $("busy").classList.toggle("hidden", !b);
  $("send").disabled = b;
}

// ---- 事件 ----
$("new-workspace").addEventListener("click", openWsModal);
$("ws-cancel").addEventListener("click", () => $("ws-modal").classList.add("hidden"));
$("ws-ok").addEventListener("click", submitWs);
$("new-session").addEventListener("click", openSsModal);
$("ss-cancel").addEventListener("click", () => $("ss-modal").classList.add("hidden"));
$("ss-ok").addEventListener("click", submitSs);
$("tab-chat").addEventListener("click", () => setTab("chat"));
$("tab-debug").addEventListener("click", () => setTab("debug"));
$("dbg-refresh").addEventListener("click", toggleDebug);
$("send").addEventListener("click", send);
$("input").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); send(); }
});

// boot
loadTree().catch(e => {
  $("messages").innerHTML = '<div class="error-line">初始化失败: ' + e.message + '</div>';
});