// 调试台页面 JS 的推导测试（wfr-㉟）。
//
// 为什么在 node 里测：日志的推导（按 run 分组 / loop 迭代号 / 走过的边）已经搬到**页面里的
// JS**（唯一实现 —— 因为"把日志文件拖进页面"必须在浏览器里推导）。C++ 侧只剩"IR → 图"。
// 这里把 src/workflow/debug_view.cpp 里的 HTML 模板抠出来、灌一份夹具数据、在最小 DOM 垫片
// 下**真跑**那段脚本，断言推导结果。测的就是发布出去的那份代码。
//
// 用法：node debug_derive_test.mjs <仓库根>
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const ROOT = process.argv[2] || path.resolve(new URL('.', import.meta.url).pathname, '../..');

// ---- 夹具：与 tests/wf/debug_view_test.cpp 的 loop_node_ir_json 同构 ----
const GRAPH = {
  nodes: [
    { id: 'poll', kind: 'loop', text: '', def: {} },
    { id: 'fetch', kind: 'tool', text: '', def: {}, loop: 'poll' },
    { id: 'ck', kind: 'expr', text: '', def: {}, loop: 'poll' },
    { id: 'notify', kind: 'tool', text: '', def: {} },
  ],
  edges: [
    { from: 'poll', to: 'fetch', label: 'body', forward: true, terminal: false },
    { from: 'poll', to: 'notify', label: 'next', forward: true, terminal: false },
    { from: 'poll', to: '_turn_end', label: 'on_max', forward: true, terminal: true },
    { from: 'fetch', to: 'ck', label: 'next', forward: true, terminal: false },
    { from: 'ck', to: 'notify', label: 'then', forward: true, terminal: false },
    { from: 'ck', to: 'poll', label: 'else', forward: false, terminal: false },
    { from: 'notify', to: '_turn_end', label: 'next', forward: true, terminal: true },
  ],
};
const rec = (node, phase, update, extra = {}) =>
  Object.assign({ run: 's1/r-1', node, phase, source: 'engine' }, update ? { update } : {}, extra);

let failures = 0, checks = 0;
function eq(actual, expected, what) {
  ++checks;
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) {
    ++failures;
    console.log(`  ✗ ${what}\n      实际 ${a}\n      期望 ${e}`);
  }
}

// ---- 最小 DOM 垫片：只要能让页面脚本跑到底，并暴露 RUNS/deriveRuns ----
function mkEl(tag) {
  const el = {
    tagName: tag, className: '', textContent: '', innerHTML: '', open: false, value: '',
    style: {}, dataset: {}, children: [], _attr: {}, files: [],
    classList: {
      _s: new Set(),
      add(c) { this._s.add(c); }, remove(c) { this._s.delete(c); },
      contains(c) { return this._s.has(c); },
      toggle(c, on) { on ? this._s.add(c) : this._s.delete(c); },
    },
    appendChild(c) { this.children.push(c); return c; },
    append(...cs) { this.children.push(...cs); },
    addEventListener() {}, setAttribute(k, v) { this._attr[k] = String(v); },
    removeAttribute(k) { delete this._attr[k]; }, getAttribute(k) { return this._attr[k]; },
    querySelectorAll() { return []; }, querySelector() { return null; },
    insertBefore(c) { this.children.push(c); return c; },
    getBoundingClientRect() { return { width: 800, height: 600, left: 0, top: 0 }; },
  };
  el.firstChild = { nodeValue: '' };
  return el;
}
// 内嵌 JS 的语法检查：用**真解析器**（vm.Script）。浏览器里语法错误 = 整页空白，
// 而 C++ 编译期与 gtest 都看不出来 —— 这里是最靠得住的一道闸。
// （原先在 gtest 里放了个手写的括号平衡扫描，正则字面量会把它带偏、误报，已删。）
let parsedOnce = false;
function assertParses(script) {
  if (parsedOnce) return;
  parsedOnce = true;
  try {
    new vm.Script(script);
  } catch (e) {
    ++failures; ++checks;
    console.log(`  ✗ 内嵌 JS 语法错误（浏览器里会整页空白）：${e.message}`);
  }
}

function runPage(records, extraGlobals = {}, graph = GRAPH) {
  const src = fs.readFileSync(path.join(ROOT, 'src/workflow/debug_view.cpp'), 'utf8');
  const a = src.indexOf('R"html(') + 'R"html('.length;
  const b = src.indexOf(')html"');
  let tpl = src.slice(a, b);
  const payload = { graph, ir: { id: 'wf_test' }, records, trimmed_node_logs: false };
  tpl = tpl.replace('__DATA__', JSON.stringify(payload)).replace(/__IR_ID__/g, 'wf_test');
  const script = tpl.slice(tpl.indexOf('<script>') + 8, tpl.lastIndexOf('</script>'));
  assertParses(script);

  const cache = {};
  const document = {
    createElement: mkEl, createDocumentFragment: () => mkEl('#frag'),
    createElementNS: (_n, t) => mkEl(t),
    getElementById: (id) => (cache[id] = cache[id] || mkEl('div')),
    querySelectorAll: () => [], addEventListener() {}, body: mkEl('body'),
  };
  const ctx = vm.createContext(Object.assign({
    document, window: {}, console, Math, JSON, Number, Object, Array, String, isNaN,
    setTimeout: () => {}, Promise, parseFloat, parseInt,
  }, extraGlobals));
  vm.runInContext(script, ctx, { filename: 'debug_view.js' });
  ctx.__cache = cache;
  return ctx;
}

// ---- 用例 1：每条记录的 loop 归属与迭代号 ----
{
  const records = [
    rec('recall', 'exit'),                                     // [0] 循环外
    rec('poll', 'enter'),                                      // [1] 第 1 轮：enter 在自增之前
    rec('poll', 'exit', { loop_counts: { poll: 1 } }),          // [2]
    rec('fetch', 'exit'),                                      // [3]
    rec('ck', 'exit'),                                         // [4]
    rec('poll', 'enter'),                                      // [5] 第 2 轮
    rec('poll', 'exit', { loop_counts: { poll: 2 } }),          // [6]
    rec('fetch', 'exit'),                                      // [7]
  ];
  const ctx = runPage(records);
  const runs = vm.runInContext('RUNS', ctx);
  eq(runs.length, 1, '只应有一个 run');
  const rs = runs[0].records;
  eq(rs.length, 8, '记录条数');
  eq(rs[0].loop_id, undefined, '循环外的记录没有 loop_id');
  eq(rs[0].iter, undefined, '循环外的记录没有 iter');
  eq(rs[1].loop, {}, '首轮 enter 时还没计数');
  eq(rs[2].loop, { poll: 1 }, '第 1 轮快照');
  eq(rs[6].loop, { poll: 2 }, '第 2 轮快照');
  eq([1, 2, 3, 4].map(i => rs[i].iter), [1, 1, 1, 1], '第 1 轮的迭代号（含节点头 enter）');
  eq([5, 6, 7].map(i => rs[i].iter), [2, 2, 2], '第 2 轮的迭代号');
  eq(rs.slice(1).map(r => r.loop_id), ['poll', 'poll', 'poll', 'poll', 'poll', 'poll', 'poll'],
     '循环内记录都归属 poll');
}

// ---- 用例 2：本轮走过的边（含终端边）----
{
  const records = [
    rec('__input__', 'enter'), rec('__input__', 'exit'),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 1 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit'),
    rec('notify', 'enter'), rec('notify', 'exit'),
  ];
  const ctx = runPage(records);
  const edges = vm.runInContext('RUNS[0].edges', ctx);
  const count = (f, t) => { const e = edges.find(x => x.from === f && x.to === t); return e ? e.count : 0; };
  eq(count('__input__', 'poll'), 1, '起点进循环容器（poll 是入口）');
  eq(count('poll', 'fetch'), 1, '节点头把控制流送进 body');
  eq(count('fetch', 'ck'), 1, 'body 内前向');
  eq(count('ck', 'notify'), 1, 'ck 的 _break → loop.next');
  eq(count('notify', '_turn_end'), 1, '终端边（唯一 terminal 出边）应被补上');
}

// ---- 用例 3：重复计次 + route 发布的 target 优先 ----
{
  const records = [
    rec('__input__', 'enter'), rec('__input__', 'exit'),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 1 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit'),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 2 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit'),
    rec('notify', 'enter'), rec('notify', 'exit'),
    // 最后节点的 exit 里发布了 results.target（route 就是这么发的）
    rec('ck', 'exit', { results: { ck: { target: '_turn_end' } } }),
  ];
  const ctx = runPage(records);
  const edges = vm.runInContext('RUNS[0].edges', ctx);
  const count = (f, t) => { const e = edges.find(x => x.from === f && x.to === t); return e ? e.count : 0; };
  eq(count('poll', 'fetch'), 2, '节点头 → body 走了两轮');
  eq(count('fetch', 'ck'), 2, 'fetch → ck 两轮');
  eq(count('ck', 'poll'), 1, '第 1 轮 ck 走 _continue 回节点头');
  eq(count('ck', 'notify'), 1, '第 2 轮 ck 走 _break 去 notify');
  eq(count('ck', '_turn_end'), 0, 'ck 在图上没有 _turn_end 出边 → 不会凭空标');
}

// ---- 用例 5：同一节点多条 exit —— 只有**最后一条**的 target 才算收尾边 ----
// 循环里每个节点每轮都有一条 exit。若取第一条（而不是最后一条），上一轮的 target 会被
// 当成"本轮收尾"，凭空多标一条边 —— 这个 bug 真机上出现过（route→act 被算成 2 次、
// route→_turn_end 丢掉），所以在这里钉住。
{
  const rec = (node, phase, update) => Object.assign({ run: 's1/r-1', node, phase, source: 'engine' },
                                                      update ? { update } : {});
  const records = [
    rec('__input__', 'enter'), rec('__input__', 'exit'),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 1 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit', { results: { ck: { target: 'notify' } } }),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 2 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit', { results: { ck: { target: 'notify' } } }),
    rec('poll', 'enter'), rec('poll', 'exit', { loop_counts: { poll: 3 } }),
    rec('fetch', 'enter'), rec('fetch', 'exit'),
    rec('ck', 'enter'), rec('ck', 'exit', { results: { ck: { target: '_turn_end' } } }),
  ];
  const ctx = runPage(records);
  const edges = vm.runInContext('RUNS[0].edges', ctx);
  const count = (f, t) => { const e = edges.find(x => x.from === f && x.to === t); return e ? e.count : 0; };
  eq(count('ck', 'poll'), 2, '应恰好两轮 _continue 的回边（取第一条 exit 会变成 3）');
  eq(count('ck', 'notify'), 0, '前两轮的 target 不算收尾（ck 从没真正走到 notify）');
  eq(count('poll', 'fetch'), 3, '节点头三轮都进了 body');
}

// ---- 用例 6：会话名抽取（拖入文件与 URL 走同一个函数）----
{
  const ctx = runPage([]);
  const f = (x) => vm.runInContext(`sessionNameOf(${JSON.stringify(x)})`, ctx);
  eq(f('session-s1.log.jsonl'), 's1', '本地文件名');
  eq(f('http://host:8787/logs/session-abc.log.jsonl'), 'abc', 'URL 取最后一段');
  eq(f('https://host/x/a.jsonl'), 'a', '普通 jsonl');
  eq(f('session-x.log.jsonl?v=2'), 'x', '带 query 也认');
  eq(f('weird'), 'weird', '认不出来就原样当会话名');
}

// ---- 用例 7：拖入/URL 的文本 → 记录（会话前缀与选取一致）----
{
  const ctx = runPage([]);
  const text = [
    JSON.stringify({ run: 'r-1', node: '__input__', phase: 'exit', source: 'engine', seq: 1 }),
    JSON.stringify({ run: 'r-2', node: 'recall', phase: 'exit', source: 'engine', seq: 2 }),
    '',                                    // 空行跳过
    '{ 坏行',                               // 坏行跳过（与 C++ 读日志同判）
    JSON.stringify({ run: 'r-2', node: 'recall', phase: 'enter', source: 'engine', seq: 3 }),
  ].join('\n');
  vm.runInContext(`ingestTexts([{name:'session-zz.log.jsonl', text:${JSON.stringify(text)}}])`, ctx);
  const runs = vm.runInContext('RUNS', ctx);
  eq(runs.map(r => r.run), ['zz/r-1', 'zz/r-2'], 'run 带上会话前缀');
  eq(vm.runInContext('Object.keys(SESS)', ctx), ['zz'], '会话桶');
  eq(runs[1].records.length, 2, '坏行与空行被跳过');
  eq(runs[1].records.map(r => r.seq), [2, 3], '同一会话内按 seq 归并');
}

// ---- 用例 8：从 URL 读取（喂假 fetch）----
{
  const body = [
    JSON.stringify({ run: 'r-1', node: '__input__', phase: 'enter', source: 'engine', seq: 1 }),
    JSON.stringify({ run: 'r-1', node: '__input__', phase: 'exit', source: 'engine', seq: 2 }),
  ].join('\n');
  const ok = runPage([], {
    fetch: (u) => Promise.resolve({ ok: true, status: 200, text: () => Promise.resolve(body) }),
  });
  vm.runInContext("loadFromUrl('http://h/logs/session-uu.log.jsonl')", ok);
  // fetch 是异步的：等一轮微任务
  await new Promise(r => setTimeout(r, 0));
  eq(vm.runInContext('RUNS.map(r=>r.run)', ok), ['uu/r-1'], 'URL 会话名从路径推出并加前缀');
  eq(vm.runInContext("document.getElementById('urlhint').textContent", ok), '', '成功后不报错');

  // 失败：HTTP 500 → 提示里要说清楚（含 CORS 那句），且不污染已有数据
  const bad = runPage([], {
    fetch: () => Promise.resolve({ ok: false, status: 500, text: () => Promise.resolve('') }),
  });
  vm.runInContext("loadFromUrl('http://h/session-bad.log.jsonl')", bad);
  await new Promise(r => setTimeout(r, 0));
  eq(vm.runInContext('RUNS.length', bad), 0, '读取失败不改动已有数据');
  const hint = vm.runInContext("document.getElementById('urlhint').textContent", bad);
  eq(hint.includes('HTTP 500'), true, '提示里带 HTTP 状态');
  eq(hint.includes('CORS'), true, '提示里说明 CORS 这一常见原因');
}

// ---- 用例 9：正交走线不穿节点（跨层走两侧通道、横向段只走行间隙）----
// 夹具刻意构造出全部五种走线：相邻层 / 同层 / 跨层前向（到终端）/ 单层回边 / 跨层回边。
{
  const N = (id) => ({ id, kind: 'tool', text: '', def: {} });
  const E = (from, to, forward = true, terminal = false) =>
    ({ from, to, label: 'next', forward, terminal });
  const g = {
    nodes: [N('a'), N('b'), N('c'), N('d'), N('e')],
    edges: [
      E('a', 'b'), E('a', 'c'),            // b、c 同层
      E('b', 'd'), E('c', 'e'),            // d、e 同层（下一层）
      E('d', 'e'),                         // ★ 同层边
      E('a', '_turn_end', true, true),     // ★ 跨层前向（a 在第 0 层，终端在最底层）
      E('d', 'c', false),                  // ★ 单层回边
      E('e', 'a', false),                  // ★ 跨层回边
    ],
  };
  const ctx = runPage([], {}, g);
  const pos = vm.runInContext('pos', ctx);
  const W = vm.runInContext('NODE_W', ctx), H = vm.runInContext('NODE_H', ctx);
  const svg = ctx.__cache['canvas'];
  const paths = svg.children.filter(c => c.tagName === 'path')
                            .map(c => c.getAttribute('d')).filter(Boolean);
  eq(paths.length > 0, true, '应该有边被画出来');

  const verts = (d) => {
    const ns = (d.match(/-?\d+(?:\.\d+)?/g) || []).map(Number), out = [];
    for (let i = 0; i + 1 < ns.length; i += 2) out.push({ x: ns[i], y: ns[i + 1] });
    return out;   // Q 的控制点就是拐角顶点，取它做折线顶点即可（贝塞尔必在该折线围成的区域内）
  };
  const boxes = Object.entries(pos).map(([id, p]) =>
    ({ id, x1: p.x, y1: p.y, x2: p.x + W, y2: p.y + H }));
  const EPS = 0.5;   // 起止点贴在框边上，不算"穿"
  const crosses = (s, b) => {
    const x1 = b.x1 + EPS, y1 = b.y1 + EPS, x2 = b.x2 - EPS, y2 = b.y2 - EPS;
    if (x2 <= x1 || y2 <= y1) return false;
    const lo = { x: Math.min(s.a.x, s.b.x), y: Math.min(s.a.y, s.b.y) };
    const hi = { x: Math.max(s.a.x, s.b.x), y: Math.max(s.a.y, s.b.y) };
    return !(hi.x <= x1 || lo.x >= x2 || hi.y <= y1 || lo.y >= y2);
  };
  let bad = 0;
  paths.forEach((d, pi) => {
    const v = verts(d);
    for (let i = 1; i < v.length; ++i) {
      const s = { a: v[i - 1], b: v[i] };
      boxes.forEach(b => { if (crosses(s, b)) { ++bad;
        console.log(`      边#${pi} ${JSON.stringify(s.a)}→${JSON.stringify(s.b)} 穿过 ${b.id}`); } });
    }
  });
  eq(bad, 0, '任何线段都不应穿过节点框');
  // 圆角折线：路径里应出现 Q（倒角），且不该再有 C（三次贝塞尔曲线段）
  eq(paths.some(d => d.includes(' Q')), true, '拐角应有圆角');
  eq(paths.some(d => d.includes(' C')), false, '不该再用曲线');
}

// ---- 用例 10：连线文字不重叠（也不压节点）----
// 夹具刻意让多条边的横向段落在**同一条行间隙**里（a 同时连向同层的 b/c/d），
// 这正是原先文字叠在一起的场景。
{
  const N = (id) => ({ id, kind: 'tool', text: '', def: {} });
  const E = (from, to, label, forward = true, terminal = false) =>
    ({ from, to, label, forward, terminal });
  const g = {
    nodes: [N('a'), N('b'), N('c'), N('d'), N('e')],
    edges: [
      E('a', 'b', 'cases.tool_calls'), E('a', 'c', 'cases.text'), E('a', 'd', 'default'),
      E('b', 'e', 'then'), E('c', 'e', 'else'), E('d', 'e', 'on_max'),
      E('e', 'a', 'else_back', false),                    // 回边（走左侧通道）
      E('a', '_turn_end', 'on_max2', true, true),         // 跨层前向（走右侧通道）
    ],
  };
  const ctx = runPage([], {}, g);
  const pos = vm.runInContext('pos', ctx);
  const W = vm.runInContext('NODE_W', ctx), H = vm.runInContext('NODE_H', ctx);
  const tw = (t) => vm.runInContext(`textWidth(${JSON.stringify(t)})`, ctx);
  const svg = ctx.__cache['canvas'];

  // 注意用 includes：本轮走过的边会被 applyEdgeUsage 改成 'elabel took'
  const labels = svg.children
    .filter(c => c.tagName === 'text' && String(c.getAttribute('class')).includes('elabel'))
    .map(c => ({ t: c.textContent, x: Number(c.getAttribute('x')), y: Number(c.getAttribute('y')),
                 w: tw(c.textContent) }));
  eq(labels.length, 8, '八条边都应带文字');

  // 两两不重叠（用与页面同一套宽度估算；高度按 12px 半高 + 2px 余量）
  let clash = 0;
  for (let i = 0; i < labels.length; ++i)
    for (let j = i + 1; j < labels.length; ++j) {
      const A = labels[i], B = labels[j], PAD = 1;
      const hit = !(A.x + A.w / 2 + PAD < B.x - B.w / 2 || A.x - A.w / 2 - PAD > B.x + B.w / 2 ||
                    A.y + 10 + PAD < B.y - 10 || A.y - 10 - PAD > B.y + 10);
      if (hit) { ++clash; console.log(`      「${A.t}」与「${B.t}」重叠`); }
    }
  eq(clash, 0, '任意两条边的文字都不应重叠');

  // 也不该压到节点框
  const boxes = Object.entries(pos).map(([id, p]) =>
    ({ id, x1: p.x, y1: p.y, x2: p.x + W, y2: p.y + H }));
  let onNode = 0;
  labels.forEach(L => {
    boxes.forEach(b => {
      const hit = !(L.x + L.w / 2 <= b.x1 || L.x - L.w / 2 >= b.x2 ||
                    L.y + 10 <= b.y1 || L.y - 10 >= b.y2);
      if (hit) { ++onNode; console.log(`      「${L.t}」压在节点 ${b.id} 上`); }
    });
  });
  eq(onNode, 0, '文字不应压在节点上');
}

// ---- 用例 11：按走向选最近的**面**（引出/接入点）----
// 横向为主（同层，或横向位移 > 纵向位移）→ 侧面中点；纵向为主 → 上下边中点。
// 夹具的分层是算出来的（layout 用 BFS + "首节点必为入口"），所以先确认：
//   a=0 层；b、c=1 层；d=2 层 → b→c 同层（右）、c→b 同层（左）、
//   b→d 同列跨层（纵）、c→d 横差 158 > 纵差 112（横）。
{
  const N = (id) => ({ id, kind: 'tool', text: '', def: {} });
  const E = (from, to, label = 'next', forward = true, terminal = false) =>
    ({ from, to, label, forward, terminal });
  const g = {
    nodes: [N('a'), N('b'), N('c'), N('d')],
    edges: [E('a', 'b'), E('a', 'c', 'right-ish'), E('b', 'c', 'same-right'),
            E('c', 'b', 'same-left'), E('b', 'd', 'down'), E('c', 'd', 'horiz')],
  };
  const ctx = runPage([], {}, g);
  const pos = vm.runInContext('pos', ctx);
  const W = vm.runInContext('NODE_W', ctx), H = vm.runInContext('NODE_H', ctx);
  const L = vm.runInContext('layer', ctx);
  eq([L.a, L.b, L.c, L.d], [0, 1, 1, 2], '夹具分层如预期（否则后面的断言没意义）');

  const route = (f, t) => JSON.parse(vm.runInContext(
    `JSON.stringify(routeEdge(${JSON.stringify(f)},${JSON.stringify(t)}))`, ctx));
  const sideOf = (pt, id) => {
    const p = pos[id], eps = 0.6;
    if (Math.abs(pt.x - (p.x + W)) < eps && Math.abs(pt.y - (p.y + H / 2)) < eps) return 'right';
    if (Math.abs(pt.x - p.x) < eps && Math.abs(pt.y - (p.y + H / 2)) < eps) return 'left';
    if (Math.abs(pt.x - (p.x + W / 2)) < eps && Math.abs(pt.y - p.y) < eps) return 'top';
    if (Math.abs(pt.x - (p.x + W / 2)) < eps && Math.abs(pt.y - (p.y + H)) < eps) return 'bottom';
    return '?';
  };
  const at = (f, t) => { const p = route(f, t); return [sideOf(p[0], f), sideOf(p[p.length - 1], t)]; };

  eq(at('b', 'c'), ['right', 'left'], '同层向右：右边引出、左边接入');
  eq(at('c', 'b'), ['left', 'right'], '同层向左：左边引出、右边接入');
  eq(at('b', 'd'), ['bottom', 'top'], '同列跨层：下边引出、上边接入');
  eq(at('c', 'd'), ['left', 'right'], '横向位移更大（158>112）：走侧面而非上下');
}

// ---- 用例 4：空记录 ----
{
  const ctx = runPage([]);
  eq(vm.runInContext('RUNS.length', ctx), 0, '空记录 → 没有 run');
}

console.log(`debug_derive_test：${checks - failures}/${checks} 通过`);
process.exit(failures ? 1 : 0);
