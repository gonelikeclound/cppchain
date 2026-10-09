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
function runPage(records) {
  const src = fs.readFileSync(path.join(ROOT, 'src/workflow/debug_view.cpp'), 'utf8');
  const a = src.indexOf('R"html(') + 'R"html('.length;
  const b = src.indexOf(')html"');
  let tpl = src.slice(a, b);
  const payload = { graph: GRAPH, ir: { id: 'wf_test' }, records, trimmed_node_logs: false };
  tpl = tpl.replace('__DATA__', JSON.stringify(payload)).replace(/__IR_ID__/g, 'wf_test');
  const script = tpl.slice(tpl.indexOf('<script>') + 8, tpl.lastIndexOf('</script>'));

  const cache = {};
  const document = {
    createElement: mkEl, createDocumentFragment: () => mkEl('#frag'),
    createElementNS: (_n, t) => mkEl(t),
    getElementById: (id) => (cache[id] = cache[id] || mkEl('div')),
    querySelectorAll: () => [], addEventListener() {}, body: mkEl('body'),
  };
  const ctx = vm.createContext({
    document, window: {}, console, Math, JSON, Number, Object, Array, String, isNaN,
    setTimeout: () => {}, Promise, parseFloat, parseInt,
  });
  vm.runInContext(script, ctx, { filename: 'debug_view.js' });
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

// ---- 用例 4：空记录 ----
{
  const ctx = runPage([]);
  eq(vm.runInContext('RUNS.length', ctx), 0, '空记录 → 没有 run');
}

console.log(`debug_derive_test：${checks - failures}/${checks} 通过`);
process.exit(failures ? 1 : 0);
