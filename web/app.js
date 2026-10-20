/* wfr CGI · 前端逻辑（原生 JS，调 CGI action；同步阻塞，无流式） */
"use strict";

// CGI 入口：本地用 http.server --cgi 时的映射。/cgi-bin/ → web 根下 cgi-bin（symlink 到 ../cgi-bin）
const CGI = "cgi-bin/wfr_cgi";

const state = {
  workspaces: [],   // [{name,dir}]
  ws: null,         // 当前工作区名
  session: null,    // 当前会话名
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

// ---- 工作区 ----
async function loadWorkspaces() {
  const r = await raw("workspaces");
  state.workspaces = r.workspaces || [];
  const sel = $("ws-select");
  sel.textContent = "";
  for (const w of state.workspaces) {
    const o = document.createElement("option");
    o.value = w.name; o.textContent = w.name;
    sel.appendChild(o);
  }
  if (state.workspaces.length && !state.ws) selectWorkspace(state.workspaces[0].name);
}

async function selectWorkspace(name) {
  state.ws = name;
  state.session = null;
  $("title-ws").textContent = name;
  await loadSessions();
  if (state.workspaces.find(w => w.name === name)?.dir) {
    // 自动进最近会话（若有）
    const rows = state.sessions || [];
    if (rows.length) selectSession(rows[rows.length - 1].name);
    else { await ensureSession(); loadSessions(); }
  }
}

// ---- 会话 ----
async function loadSessions() {
  const w = state.workspaces.find(x => x.name === state.ws);
  if (!w) return;
  const r = await raw("sessions", { ws: w.dir });
  state.sessions = r.sessions || [];
  const ul = $("session-list");
  ul.textContent = "";
  if (!state.sessions.length) ul.innerHTML = '<div class="empty-hint">暂无会话，点【＋新会话】</div>';
  for (const s of state.sessions) {
    const li = document.createElement("li");
    li.textContent = s.name || "(默认)";
    li.addEventListener("click", () => selectSession(s.name));
    if (state.session === s.name) li.classList.add("active");
    const del = document.createElement("button");
    del.className = "x"; del.textContent = "✕";
    del.addEventListener("click", (e) => { e.stopPropagation(); archiveSession(s.name); });
    li.appendChild(del);
    ul.appendChild(li);
  }
}

async function selectSession(name) {
  state.session = name;
  $("title-s").textContent = name || "(默认)";
  const w = state.workspaces.find(x => x.name === state.ws);
  Array.from(document.querySelectorAll("#session-list li")).forEach(li => li.classList.toggle("active", li.textContent === name));
  if (w) {
    const r = await raw("history", { ws: w.dir, s: name });
    renderHistory(r.messages || []);
  }
}

async function newSession() {
  const w = state.workspaces.find(x => x.name === state.ws);
  if (!w) return;
  const name = "s" + Math.floor(Date.now() / 1000);
  state.session = name;
  $("title-s").textContent = name;
  $("messages").textContent = "";
  await loadSessions();
  // 会话在第一条消息发出时才真正建（wfr 首次写 session 日志）；这里只是占位
}

async function ensureSession() {
  if (state.session) return;
  const name = "s" + Math.floor(Date.now() / 1000);
  state.session = name;
  $("title-s").textContent = name;
}

function archiveSession(name) {
  // 简易归档：仅从列表移除（日志不动）
  state.sessions = (state.sessions || []).filter(s => s.name !== name);
  if (state.session === name) { state.session = null; $("messages").textContent = ""; $("title-s").textContent = "—"; }
  loadSessions();
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
  if (!q || !state.ws) return;
  const w = state.workspaces.find(x => x.name === state.ws);
  if (!w) return;
  await ensureSession();  // 首个会话由首条消息自然创建（wfr 首次写 session 日志）
  appendMsg("user", q, "");
  $("input").value = "";
  setBusy(true);
  try {
    await streamChat(w.dir, state.session, q);
    loadSessions();
  } catch (e) {
    const el = document.createElement("div"); el.className = "error-line"; el.textContent = "请求失败: " + e.message;
    $("messages").appendChild(el);
  } finally {
    setBusy(false);
  }
}

function setBusy(b) {
  $("busy").classList.toggle("hidden", !b);
  $("send").disabled = b;
}

// ---- 事件 ----
$("ws-select").addEventListener("change", (e) => selectWorkspace(e.target.value));
$("new-session").addEventListener("click", newSession);
$("send").addEventListener("click", send);
$("input").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); send(); }
});

// boot
loadWorkspaces().catch(e => {
  $("messages").innerHTML = '<div class="error-line">初始化失败: ' + e.message + '</div>';
});