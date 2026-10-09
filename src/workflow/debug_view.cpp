// cppchain —— src/workflow/debug_view.cpp
// wfr 调试台的导出与渲染（见头注释）。HTML 为自包含单文件：分层 SVG + 三栏可拖布局 +
// 树形日志（run 轮 → 节点 → 记录）+ 选中节点 JSON 描述 + 循环分组。
#include "cppchain/workflow/debug_view.hpp"

#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/ir_walk.hpp"
#include "cppchain/workflow/node_type.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cppchain::workflow {

// 图是**平铺**的（loop 不做容器框）：loop 节点就是普通节点，body 里的节点额外带一个
// `loop` 归属字段（与 import 的 origin_wf 同一套"节点级扁平归属"），前端据此做迭代分组。
Json debug_view_graph(WorkflowIR const& ir) {
  Json nodes = Json::array();
  Json edges = Json::array();

  for_each_node(ir, [&](NodeIR const& n, LoopScope const* scope) {
    std::string const kind = type_name_of(n);
    std::string text;
    if (NodeType const* t = node_type_of_name(kind); t != nullptr && t->describe)
      text = t->describe(n);
    else
      text = kind + " " + n.id;
    Json node{{"id", n.id}, {"kind", kind}, {"text", text}, {"def", n.raw}};
    if (scope != nullptr) node["loop"] = scope->loop_id;  // 归属（前端分组维度）
    nodes.push_back(std::move(node));

    NodeType const* t = node_type_of_name(kind);
    if (t == nullptr || !t->edges) return true;
    for (NodeType::Edge const& e : t->edges(n)) {
      // 保留名在这里就地解析成图目标：_continue → loop 节点头（回边）、
      // _break → loop 的 next。与 validator/compiler 同一套解析，画出来的边才和跑的一致。
      std::string to = e.target;
      bool forward = e.forward;
      if (scope != nullptr && e.target == kContinueName) {
        to = scope->loop_id;
        forward = false;  // 回边：前端据此画绕行弧线
      } else if (scope != nullptr && e.target == kBreakName) {
        to = scope->exit_target;
      }
      bool const terminal = to == "__end__" || to == "__fail__" || to == "_turn_end" ||
                            to == "_fail";
      edges.push_back(Json{{"from", n.id},
                           {"to", std::move(to)},
                           {"label", e.label},
                           {"forward", forward},
                           {"terminal", terminal}});
    }
    return true;
  });

  return Json{{"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
}

Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records) {
  // payload 里**只放原始记录**：分组 / loop 迭代号 / 边使用这些推导搬到页面里的 JS 了
  //（wfr-㉟）。因为"把日志文件拖进页面"这条路径必须在浏览器里推导 —— 一份实现同时服务
  // 静态导出与拖入，没有第二份口径。
  Json recs = Json::array();
  bool trimmed = false;
  for (Json const& r : records) {
    if (records.size() > 20000 && r.value("source", "") == "node") {  // 超大会话：裁节点诊断
      trimmed = true;
      continue;
    }
    recs.push_back(r);
  }
  return Json{{"graph", debug_view_graph(ir)},
              {"ir", Json{{"id", ir.id}}},
              {"records", std::move(recs)},
              {"trimmed_node_logs", trimmed}};
}

// ---- 单 HTML 渲染 ----

// 内嵌到 <script> 前的转义：把 JSON 串里的 `</script`/`<`/`>`/`&` 逐字符换掉，
// 浏览器读回时 \uXXXX 会还原成原字符（JS 字符串字面量语义不变）。
std::string script_data_escape(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '<': out += "\\u003c"; break;
      case '>': out += "\\u003e"; break;
      case '&': out += "\\u0026"; break;
      default: out += c;
    }
  }
  return out;
}

// 整体页式：body 为三列 grid（导航 | 分割条 | 中：图+详情 | 分割条 | 日志），
// 中列再纵向分成 图 / 横分割条 / 节点 JSON 详情。日志为 run→节点→记录的可开合树。
std::string render_debug_html(Json const& payload) {
  static constexpr char kHtml[] = R"html(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>wfr debug — __IR_ID__</title>
<style>
:root{--hl:#e8f0fe;--acc:#1a73e8;--e:#ddd;--wnav:230px;--wlog:380px}
*{box-sizing:border-box}
body{font:13px system-ui,sans-serif;margin:0;height:100vh;overflow:hidden;
  display:grid;grid-template-columns:var(--wnav) 8px 1fr 8px var(--wlog);grid-template-rows:1fr}
#nav{grid-column:1;border-right:1px solid var(--e);padding:10px;background:#fafafa;overflow:auto;min-width:0;min-height:0}
#sv1{grid-column:2}#sv2{grid-column:4}
#mid{grid-column:3;display:flex;flex-direction:column;min-width:0;min-height:0;background:#fbfbfb}
#graphwrap{flex:1 1 auto;display:flex;flex-direction:column;min-width:0;min-height:0}
#canvasbox{flex:1 1 auto;overflow:auto;padding:8px;min-height:0}
#sh{flex:none;height:8px;border-top:1px solid var(--e)}
#detail{flex:none;height:220px;border-top:1px solid var(--e);overflow:auto;background:#fff;padding:8px 10px;min-height:0}
#log{grid-column:5;border-left:1px solid var(--e);padding:10px 10px 8px;background:#fff;min-width:0;min-height:0;display:flex;flex-direction:column}
#log h1{flex:none}
.split{background:#e8eaed;cursor:col-resize}#sh{cursor:row-resize}
.split:hover{background:#b6bcc4}#sh:hover{background:#b6bcc4}
body.drag-v,body.drag-v *{cursor:col-resize!important;user-select:none}
body.drag-h,body.drag-h *{cursor:row-resize!important;user-select:none}
h1{font-size:15px;margin:.2em 0}h2{font-size:12px;color:#666;margin:.8em 0 .3em}
svg text{font:12px system-ui}
.n{fill:#f1f3f4;stroke:#9aa0a6;stroke-width:1.5;cursor:pointer}
.n.kind-label{fill:#edf0f3;stroke:#5f6368}
.n.kind-tool{fill:#d8e7fb;stroke:#1a73e8}
.n.kind-expr{fill:#fdeccb;stroke:#f9ab00}
.n.kind-goto{fill:#fbdcd9;stroke:#d93025}
.n.kind-model{fill:#e8e0f4;stroke:#7620bb}
.n.kind-route{fill:#d9f2dc;stroke:#188038}
.n.kind-run_tools{fill:#cdeee7;stroke:#0d7a6f}
.n.kind-load_history{fill:#e2e3fb;stroke:#5b6ee1}
.n.kind-echo{fill:#fde7d9;stroke:#f9672a}
.n.kind-loop{fill:#e6dcf7;stroke:#6a1b9a;stroke-width:2}
.n.kind-summarize{fill:#d7f0f5;stroke:#00838f;stroke-width:2}
.n.kind-refine_history{fill:#e6f4d7;stroke:#558b2f;stroke-width:2}
.n.kind-input,.n.kind-end{stroke-dasharray:5 3}
.n.active{stroke:var(--acc);stroke-width:2.5;filter:drop-shadow(0 0 3px #1a73e833)}
.n.hit{stroke:#d93025;stroke-width:2.5}
.n .lbl{font:12px ui-monospace,monospace;font-weight:600;fill:#111}
path.e{fill:none;stroke:#5f6368;stroke-width:1.6;stroke-linecap:round}
path.e.goto{fill:none;stroke:#d93025;stroke-width:2}
path.e.term{fill:none;stroke:#b0b3b8;stroke-width:1.2;stroke-dasharray:5 4}
.eterm{fill:#fff;stroke:#5f6368;stroke-width:1.5}
.eterm.fail{fill:#fff;stroke:#d93025;stroke-width:1.5}
.elabel{font:10px system-ui;fill:#9aa0a6;pointer-events:none;font-weight:600}
/* ---- 节点图例（种类→颜色的对照）---- */
.legend{display:flex;flex-wrap:wrap;gap:4px 12px;margin:2px 0 8px}
.graph-legend{flex:none;border-top:1px solid var(--e);padding:5px 10px;background:#fafafa;margin:0;align-items:center}
.legend .item{display:flex;align-items:center;gap:5px;font-size:11px;color:#555}
.legend .dot{width:16px;height:11px;border-radius:3px;border:1.5px solid;background:transparent}
.nodeitem{padding:2px 4px;cursor:pointer;border-radius:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;display:flex;justify-content:space-between;gap:6px}
.nodeitem:hover,.nodeitem.on{background:var(--hl)}
.nodeitem .cnt{color:#999;font-weight:600}
.e.took{stroke:var(--acc);stroke-width:2.8}
.elabel.took{fill:var(--acc);font-weight:700}
.ecount{font:10px system-ui;fill:#fff;font-weight:700}
.ecountbg{fill:var(--acc)}
.gototab{margin-top:4px}
.loopit{padding:2px 6px;border:1px solid #ccc;border-radius:4px;margin:2px 2px 2px 0;display:inline-block;cursor:pointer;background:#fff}
.loopit.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.small{color:#888;font:11px}
.drop{margin:2px 0 6px;padding:8px 6px;border:1.5px dashed #b9c2cc;border-radius:6px;text-align:center;
  font-size:11.5px;color:#5f6368;cursor:pointer;background:#fafbfc}
.drop:hover{background:var(--hl)}
.drop.over{border-color:var(--acc);background:#e8f0fe;color:var(--acc)}
.drop.err{border-color:#d93025;background:#fdecea;color:#d93025}
.urlrow{display:flex;gap:4px;margin:0 0 2px}
.urlrow input{flex:1 1 auto;min-width:0;font-size:11.5px;padding:3px 5px;border:1px solid var(--e);border-radius:4px}
.urlrow button{flex:none;font-size:11.5px;padding:3px 8px;border:1px solid var(--e);border-radius:4px;
  background:#fff;cursor:pointer}
.urlrow button:hover{background:var(--hl)}
#urlhint{min-height:12px;margin-bottom:4px}
/* ---- 日志树：run 轮 → loop 迭代 → 节点 → 记录 ---- */
#loglist{flex:1 1 auto;overflow:auto;margin-top:4px;min-height:0}
#sh2{flex:none;height:6px;border-top:1px solid var(--e);margin-top:6px;cursor:row-resize}
#logdetail{flex:none;height:200px;margin-top:0;display:flex;flex-direction:column;border-top:1px solid var(--e);padding-top:6px;min-height:0}
details.run{background:#fff;border:1px solid var(--e);border-radius:5px;margin:6px 0;overflow:hidden}
details.run>summary{list-style:none;cursor:pointer;padding:5px 8px;background:#f4f6f9;font-weight:600}
details.run>summary:hover{background:var(--hl)}
details.node{margin:2px 2px 2px 6px}
details.node>summary{list-style:none;cursor:pointer;padding:3px 6px;font-weight:600;color:#333;border-radius:3px}
details.node>summary:hover{background:var(--hl)}
details.loopnode>summary{color:#5b2c8e}
details.iter{margin:2px 2px 2px 8px}
details.iter>summary{list-style:none;cursor:pointer;padding:2px 6px;font-weight:600;color:#5b2c8e;background:#f6f2fd;border-radius:3px;font-size:12px}
details.iter>summary:hover{background:var(--hl)}
details.wf{margin:1px 2px 1px 10px}
details.wf>summary{list-style:none;cursor:pointer;padding:2px 6px;font-weight:600;color:#444;border-radius:3px;font-size:12px}
details.wf>summary:hover{background:var(--hl)}
summary::before{content:'▸';display:inline-block;width:14px;color:#888}
details[open]>summary::before{content:'▾'}
.cnt{color:#999;font-weight:400}
.rec{padding:3px 8px 3px 22px;border-top:1px dashed #e4e7ea;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font:11.5px ui-monospace,Menlo,Consolas,monospace}
details.node>.rec:first-of-type{border-top:none}
.rec:hover{background:var(--hl)}
.rec.hit{background:#fdecea}
.rec.sel{background:#e8f0fe;box-shadow:inset 2px 0 0 var(--acc)}
.rec b{font-weight:600;color:#222}
.ph{display:inline-block;min-width:64px;color:#666;font-weight:600}
.s{color:#1a73e8}.e2{color:#d93025}.iv{color:#b06000}
.lvl{color:#9e9e9e}
.ts{color:#aaa;font-size:10.5px;margin-left:8px}
/* ---- 日志选中记录的下半区 JSON 美化查看 ---- */
#ldhead{font:11.5px ui-monospace,monospace;color:#666;margin-bottom:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#ldbody{flex:1 1 auto;overflow:auto;background:#fbfbfb;border:1px solid var(--e);border-radius:4px;padding:6px 8px}
#ldbody pre{margin:0;font:11px ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all}
.j-key{color:#1a6db5;font-weight:600}.j-str{color:#1e8e3e}.j-num{color:#c5221f}.j-kw{color:#f9ab00}.j-pun{color:#666}
/* ---- 节点详情 ---- */
#detailhead{display:flex;align-items:center;gap:8px;margin-bottom:6px;color:#666}
#detailhead b{color:#111;font-size:13px}
#dcopy{margin-left:auto;font-size:11px;padding:2px 8px;border:1px solid #ccc;border-radius:4px;background:#fff;cursor:pointer}
#dcopy:hover{background:var(--hl)}
#djson{margin:0;font:11.5px ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all;color:#064a2a}
</style></head><body>
<aside id="nav">
<h1>__IR_ID__</h1>
<div id="trim" class="small" style="display:none">⚠ 记录过多，已裁掉节点诊断</div>
<h2>会话 / 轮次（日志区只显示所选 run）</h2>
<div id="drop" class="drop">把 .log.jsonl 拖到这里<br><span class="small">或点击选择多个文件（可反复追加）</span>
  <input id="filepick" type="file" multiple accept=".jsonl,.json,.log" style="display:none"></div>
<div id="urlrow" class="urlrow">
  <input id="urlbox" type="text" placeholder="或填日志 URL：http(s)://…/session-x.log.jsonl">
  <button id="urlgo" title="从该 URL 读取日志">读取</button>
</div>
<div id="urlhint" class="small"></div>
<select id="sessSel"></select>
<select id="runSel"></select>
<h2>节点（导航）</h2><div id="nodelist"></div>
<h2>循环分组</h2><div id="gototabs"></div>
</aside>
<div id="sv1" class="split" title="拖动调整左栏宽度"></div>
<div id="mid">
  <div id="graphwrap">
    <div id="canvasbox"><svg id="canvas" width="700" height="400"></svg></div>
    <div id="legend" class="legend graph-legend"></div>
  </div>
  <div id="sh" title="拖动调整详情区高度"></div>
  <div id="detail"><div id="detailhead">节点 <b id="dnode"></b><span id="dkind"></span>
    <button id="dcopy">复制</button></div><pre id="djson"></pre></div>
</div>
<div id="sv2" class="split" title="拖动调整右栏宽度"></div>
<section id="log"><h1>日志</h1><div id="loglist" class="small"></div>
  <div id="sh2" title="拖动调整日志详情高度"></div>
  <div id="logdetail"><div id="ldhead"></div><div id="ldbody"></div></div>
</section>
<script>
const P = __DATA__;
const G = P.graph;
let RUNS = [];   // 由 P.records 推导（见 deriveRuns）；拖入日志后会重建
const NODES = {}; G.nodes.forEach(n => NODES[n.id] = n);

/* ---------- 从**原始会话日志**推导运行视图 ----------
   这是唯一的一份实现：原先在 C++（debug_view_logs），wfr-㉟ 搬到页面里 —— 因为"把日志
   文件拖进页面"这条路径必须在浏览器里推导，一份实现同时服务静态导出与拖入，没有第二份口径。
   推导四件事：① 逐条 loop_counts 快照 ② 每条记录的 loop 归属与迭代号 ③ 本轮走过的边与次数
   ④ 按 run 分组。 */
function deriveRuns(records){
  const isHead={}, owner={};
  G.nodes.forEach(n=>{ if(n.kind==='loop') isHead[n.id]=true; if(n.loop) owner[n.id]=n.loop; });
  const outs={};
  G.edges.forEach(e=>{ (outs[e.from]=outs[e.from]||[]).push(e.to); });
  const num=v=>(typeof v==='number' && isFinite(v)) ? v : null;
  const lcOf=r=>{ const u=r.update; return (u && typeof u==='object' && u.loop_counts &&
                    typeof u.loop_counts==='object') ? u.loop_counts : null; };

  // ① 逐条快照（重放 update.loop_counts，右胜左）
  const after=[]; const loop={};
  records.forEach(r=>{
    const lc=lcOf(r);
    if(lc) Object.keys(lc).forEach(k=>{ loop[k]=lc[k]; });
    after.push(Object.assign({}, loop));
  });

  // ② 迭代号。loop 节点头特殊：它的 enter 在自增**之前**、exit 才是那次自增 ——
  //    所以对节点头取"此后第一次自增后的值"，enter/exit 这对记录就都落在第 N 轮。
  const iterOf=(i,L)=>{
    const node=records[i].node||'';
    if(isHead[node]){
      for(let j=i;j<records.length;++j){
        const lc=lcOf(records[j]); const v=lc?num(lc[L]):null;
        if(v!==null) return v;
      }
    }
    const v1=num((after[i]||{})[L]); if(v1!==null) return v1;
    for(let j=i;j<records.length;++j){ const v=num((after[j]||{})[L]); if(v!==null) return v; }
    return null;
  };

  // ④a 先按 run 分组，并把归属/迭代号贴到每条记录上
  const order=[], byRun={};
  records.forEach((r,i)=>{
    const key=r.run||'?';
    if(!byRun[key]){ byRun[key]=[]; order.push(key); }
    const rec=Object.assign({}, r);
    rec.loop=after[i];
    const node=r.node||'';
    const lid=owner[node]||(isHead[node]?node:null);
    if(lid){ const it=iterOf(i,lid); if(it!==null){ rec.loop_id=lid; rec.iter=it; } }
    byRun[key].push(rec);
  });

  // ③ 本轮走过的边：引擎 enter 序列里相邻两个执行节点 = 一条边（首个节点只记起点）。
  //    终端目标（_turn_end/_fail）不产生 enter，靠最后节点的 exit 里发布的 results.target，
  //    或"该节点在图上只有一条 terminal 出边"补 —— 两者都不明确就不猜。
  const isTerm=t=>(t==='__end__'||t==='__fail__'||t==='_turn_end'||t==='_fail');
  const out=[];
  order.forEach(k=>{
    const recs=byRun[k];
    const pairs=[]; let last=null;
    recs.forEach(r=>{
      if(r.source!=='engine'||r.phase!=='enter') return;
      const cur=r.node||''; if(!cur) return;
      if(last!==null && last!==cur) pairs.push([last,cur]);
      last=cur;
    });
    if(last!==null){
      const cand=outs[last]||[];
      let picked=null;
      // 取**最后一条**该节点的 exit（后写覆盖）：循环里同一个节点每轮都有一条 exit，
      // 只有收尾那条的 target 才是真正结束本轮的那条边。
      recs.forEach(r=>{
        if(r.source!=='engine'||r.phase!=='exit'||r.node!==last) return;
        const rs=(r.update&&typeof r.update==='object')?r.update.results:null;
        const f=(rs&&typeof rs==='object')?rs[last]:null;
        if(f&&typeof f==='object'&&typeof f.target==='string') picked=f.target;
      });
      if(picked===null){
        const terms=cand.filter(isTerm);
        const lastRec=recs.length?recs[recs.length-1]:null;
        if(terms.length===1 && lastRec && lastRec.source==='engine' && lastRec.phase==='exit')
          picked=terms[0];
      }
      if(picked!==null && cand.indexOf(picked)>=0) pairs.push([last,picked]);
    }
    const agg={}, list=[];
    pairs.forEach(([a,b])=>{ const kk=a+'|'+b; if(agg[kk]===undefined){agg[kk]=0; list.push([a,b]);} agg[kk]++; });
    out.push({run:k, records:recs, edges:list.map(([a,b])=>({from:a,to:b,count:agg[a+'|'+b]}))});
  });
  return out;
}
// —— 合成内置节点：__input__ 入口 + 各终端目标（_turn_end / _fail / __end__）——
const TERMS=[...new Set(G.edges.map(e=>e.to).filter(t=>t&&!NODES[t]))];
const BUILTIN=[{id:'__input__',kind:'input',text:'输入'}]
  .concat(TERMS.map(t=>({id:t,kind:'end',text:'结束'})));
BUILTIN.forEach(n=>{ NODES[n.id]=n; });
const ALLNODES=G.nodes.concat(BUILTIN);
const PAGEID=(P.ir&&P.ir.id)||'';   // 本页 workflow 名（记录无 workflow 徽标时的归属推断）
/* ---------- 布局 v2：BFS 分层 → 行排布；回边不推层；孤立节点单独末行 ---------- */
const layer = {};
const pos = {};
const NODE_W=138, NODE_H=36, GAP=20, LEVEL_Y=112;
const KIND_COLORS={
  label:{f:'#edf0f3',s:'#5f6368'}, tool:{f:'#d8e7fb',s:'#1a73e8'},
  expr:{f:'#fdeccb',s:'#f9ab00'}, goto:{f:'#fbdcd9',s:'#d93025'},
  model:{f:'#e8e0f4',s:'#7620bb'}, route:{f:'#d9f2dc',s:'#188038'},
  run_tools:{f:'#cdeee7',s:'#0d7a6f'}, load_history:{f:'#e2e3fb',s:'#5b6ee1'},
  input:{f:'#e7e9ec',s:'#5f6368'}, end:{f:'#f0f0f0',s:'#3c4043'},
  echo:{f:'#fde7d9',s:'#f9672a'}, loop:{f:'#e6dcf7',s:'#6a1b9a'},
  summarize:{f:'#d7f0f5',s:'#00838f'}, refine_history:{f:'#e6f4d7',s:'#558b2f'}
};
function kindColor(k){ return KIND_COLORS[k]||{f:'#f1f3f4',s:'#9aa0a6'}; }
function renderLegend(){
  const box=document.getElementById('legend');
  Object.entries(KIND_COLORS).forEach(([k,c])=>{
    const it=document.createElement('span'); it.className='item';
    const d=document.createElement('span'); d.className='dot';
    d.style.background=c.f; d.style.borderColor=c.s;
    const t=document.createElement('span'); t.textContent=k;
    it.appendChild(d); it.appendChild(t); box.appendChild(it);
  });
  const hint=document.createElement('span'); hint.className='item';
  const hd=document.createElement('span'); hd.className='dot';
  hd.style.background='#1a73e8'; hd.style.borderColor='#1a73e8';
  const ht=document.createElement('span'); ht.textContent='加粗蓝线 = 本轮走过的边（×N 为次数）';
  hint.appendChild(hd); hint.appendChild(ht); box.appendChild(hint);
}
function layout(){
  const ids=G.nodes.map(n=>n.id); const first=ids[0];
  if(!first) return {w:400,h:200};
  const indeg={}; ids.forEach(id=>indeg[id]=0);
  const froms={};
  G.edges.forEach(e=>{
    (froms[e.from]=froms[e.from]||[]).push(e);
    if(e.forward && !e.terminal && e.to in indeg) indeg[e.to]++;   // 只数前向入边（goto 回边不推层）
  });
  // 多源 BFS：所有前向入度为 0 的入口（含只有 goto 回边的 l0）都从第 0 层起排
  const q=ids.filter(id=>indeg[id]===0);
  q.forEach(id=>layer[id]=0);
  if(!q.includes(first)){ layer[first]=0; q.push(first); }
  for(let i=0;i<q.length;i++){
    const id=q[i];
    for(const e of (froms[id]||[]))
      if(e.forward && !e.terminal && e.to in indeg && layer[e.to]===undefined) {
        layer[e.to]=layer[id]+1; q.push(e.to);
      }
  }
  // 未分层（孤立）→ 已有最大层下方新行
  const rest=ids.filter(id=>layer[id]===undefined);
  if(rest.length){ const base=Math.max(0,...Object.values(layer)); rest.forEach(id=>layer[id]=base+1); }
  // 内置节点置顶（__input__）与置底（各终端目标）
  const realL=G.nodes.map(n=>layer[n.id]);
  if(realL.length){ const mL=Math.min(...realL), xL=Math.max(...realL);
    layer['__input__']=mL-1; TERMS.forEach(t=>layer[t]=xL+1); }
  else { layer['__input__']=0; TERMS.forEach(t=>layer[t]=1); }
  const topL=Math.min(...Object.values(layer));
  const rows={}; ALLNODES.forEach(n=>{ const l=layer[n.id]; (rows[l]=rows[l]||[]).push(n.id); });
  let maxRow=0;
  for(const arr of Object.values(rows)){ arr.sort(); maxRow=Math.max(maxRow,arr.length); }
  for(const [l,arr] of Object.entries(rows)) arr.forEach((id,i)=>{ pos[id]={x:64+i*(NODE_W+GAP), y:44+(Number(l)-topL)*LEVEL_Y}; });
  const maxL2=Math.max(0,...Object.values(layer));
  return {w:72+maxRow*(NODE_W+GAP)+NODE_W, h:60+(maxL2-topL+1)*LEVEL_Y};
}
/* ---------- SVG 绘制 v3：**正交圆角折线**（走行间隙带 / 两侧通道，不压节点）+ 类型配色 ---------- */
const svg=document.getElementById('canvas');
const NS='http://www.w3.org/2000/svg';
function el(t){ return document.createElementNS(NS,t); }
// 从中心 (cx,cy) 沿方向 (dx,dy) 到矩形节点边界交点（让箭头停在节点边界，不压住文字）

/* ---------- 正交走线（圆角折线）----------
   全部线段只落在两类位置之一：
     · **行间隙带**（两行之间 y∈[rowBot,rowBot+LEVEL_Y-NODE_H] 的中线）—— 横向段走这里，
       不可能压到任何节点框；
     · **两侧通道**（所有节点框的左/右之外 26px）—— 跨多层的纵向段走这里。
   于是"线与节点不重叠"是一条可证的性质，而不是靠调参凑出来的（页面上有测试守着）。 */
const MID=(LEVEL_Y-NODE_H)/2;                 // 行边到间隙中线的距离
function laneX(side){
  let m = side==='right' ? -1e9 : 1e9;
  ALLNODES.forEach(n=>{ const p=pos[n.id]; if(!p) return;
    m = side==='right' ? Math.max(m, p.x+NODE_W) : Math.min(m, p.x); });
  return side==='right' ? m+26 : m-26;
}
function routeEdge(s,t){
  const A=pos[s], B=pos[t];
  const ax=A.x+NODE_W/2, bx=B.x+NODE_W/2;
  const aTop=A.y, aBot=A.y+NODE_H, bTop=B.y, bBot=B.y+NODE_H;
  const ls=layer[s], lt=layer[t];
  // 「按走向选最近的面」：横向为主（同层，或横向位移大于纵向位移）→ 从**侧面中点**
  // 引出/接入；纵向为主 → 仍走上下边中点（下面是那套行间隙 + 两侧通道的走法）。
  // 横向走线用**列间隙通道**：这个布局每行共用同一套 x 网格，所以列间隙（宽 GAP、
  // 中心在 x+W+GAP/2）在**所有行**里都是空的 —— 纵向跑多远都不会压到节点。
  if(lt===ls || Math.abs(bx-ax) > Math.abs(bTop-aTop)){
    const right = bx > ax;
    const ay=A.y+NODE_H/2, by=B.y+NODE_H/2;
    const xA = right ? A.x+NODE_W : A.x;              // 引出面中点
    const xB = right ? B.x : B.x+NODE_W;              // 接入面中点（朝向来源那一侧）
    const c1 = right ? A.x+NODE_W+GAP/2 : A.x-GAP/2;  // A 旁边的列间隙
    const c2 = right ? B.x-GAP/2 : B.x+NODE_W+GAP/2;  // B 旁边的列间隙
    if(Math.abs(c1-c2) < 1)                            // 相邻列：同一条通道里直上/直下
      return [{x:xA,y:ay},{x:c1,y:ay},{x:c1,y:by},{x:xB,y:by}];
    const band = (lt<ls) ? (A.y-MID) : (A.y+NODE_H+MID);   // 横向那一段退回行间隙走
    return [{x:xA,y:ay},{x:c1,y:ay},{x:c1,y:band},{x:c2,y:band},{x:c2,y:by},{x:xB,y:by}];
  }
  if(lt===ls){                                  // 同层：绕本行**下方**的间隙（不横穿本行的节点）
    const g=aBot+MID;
    return [{x:ax,y:aBot},{x:ax,y:g},{x:bx,y:g},{x:bx,y:bBot}];
  }
  if(lt>ls){
    if(lt-ls===1){                              // 相邻层：下 → 横（两行之间的间隙）→ 下
      const g=aBot+MID;
      return [{x:ax,y:aBot},{x:ax,y:g},{x:bx,y:g},{x:bx,y:bTop}];
    }
    const L=laneX('right');                     // 跨多层：走右侧通道，不穿中间那些行
    const g1=aBot+MID, g2=bTop-MID;
    return [{x:ax,y:aBot},{x:ax,y:g1},{x:L,y:g1},{x:L,y:g2},{x:bx,y:g2},{x:bx,y:bTop}];
  }
  if(ls-lt===1){                                // 回边一层：上 → 横 → 上（进目标的下边）
    const g=aTop-MID;
    return [{x:ax,y:aTop},{x:ax,y:g},{x:bx,y:g},{x:bx,y:bBot}];
  }
  const L=laneX('left');                        // 回边多层：走左侧通道
  const g1=aTop-MID, g2=bBot+MID;
  return [{x:ax,y:aTop},{x:ax,y:g1},{x:L,y:g1},{x:L,y:g2},{x:bx,y:g2},{x:bx,y:bBot}];
}
// 折线 → 圆角路径（每个拐角用二次贝塞尔倒角，半径按相邻段长的一半夹住）
function roundedPath(pts, r){
  const p=[];
  pts.forEach(q=>{ const l=p[p.length-1];
    if(l && Math.abs(l.x-q.x)<0.01 && Math.abs(l.y-q.y)<0.01) return;
    // 共线顶点也去掉：方向没变的拐点留着只会多出一条冗余命令
    if(l && p.length>=2){
      const m=p[p.length-2];
      const sameX=Math.abs(m.x-l.x)<0.01 && Math.abs(l.x-q.x)<0.01;
      const sameY=Math.abs(m.y-l.y)<0.01 && Math.abs(l.y-q.y)<0.01;
      if(sameX||sameY){ p[p.length-1]=q; return; }
    }
    p.push(q); });
  if(p.length<2) return '';
  if(p.length===2) return 'M'+p[0].x+','+p[0].y+' L'+p[1].x+','+p[1].y;
  let d='M'+p[0].x+','+p[0].y;
  for(let i=1;i<p.length-1;++i){
    const a=p[i-1], b=p[i], c=p[i+1];
    const v1={x:b.x-a.x,y:b.y-a.y}, v2={x:c.x-b.x,y:c.y-b.y};
    const l1=Math.abs(v1.x)+Math.abs(v1.y), l2=Math.abs(v2.x)+Math.abs(v2.y);
    const rr=Math.min(r, l1/2, l2/2);
    if(l1<0.01||l2<0.01||rr<1){ d+=' L'+b.x+','+b.y; continue; }
    d+=' L'+(b.x-v1.x/l1*rr)+','+(b.y-v1.y/l1*rr)+
       ' Q'+b.x+','+b.y+' '+(b.x+v2.x/l2*rr)+','+(b.y+v2.y/l2*rr);
  }
  const e=p[p.length-1];
  return d+' L'+e.x+','+e.y;
}
// 文本宽度估算（浏览器里没法在画之前度量）：CJK 按 12px、其余按 6.2px
function textWidth(s){
  let w=0;
  for(const ch of String(s)) w += (ch.codePointAt(0) > 0x2E7F) ? 12 : 6.2;
  return w;
}
// 标签锚点：**优先落在横向段的中点**（横向段都在行间隙里，于是标签天然不压节点，
// 也便于按"带"防重叠）；整条都是竖线（直上直下的边）时才贴到线的右侧。
function labelAnchor(pts, s, t){
  const midY=(pts[0].y+pts[pts.length-1].y)/2;
  let best=null;
  for(let i=1;i<pts.length;++i){
    if(Math.abs(pts[i].x-pts[i-1].x) < 30) continue;   // 侧面的短接头放不下文字，跳过
    const y=(pts[i].y+pts[i-1].y)/2, x=(pts[i].x+pts[i-1].x)/2;
    const d=Math.abs(y-midY);
    if(!best || d<best.d) best={d:d, x:x, y:y};
  }
  if(best) return {x:best.x, y:best.y-5, vertical:false};
  // 没有够长的横向段（侧面引出/接入的走法）→ 贴在竖线旁，且 **y 取行间隙带**：
  // 那里没有节点，文字横向铺开也不会压到东西。
  const A=pos[s];
  const gapY = (layer[t] < layer[s]) ? (A.y-MID) : (A.y+NODE_H+MID);
  return {x:pts[0].x+10, y:gapY, vertical:true};
}
// 标签防重叠：按"间隙带"（y 相近的一组）分组，带内用轨道错开（间隙高 76、标签盒高 24
// → 刚好三层 0/±24），同一轨道再撞就沿 x 往右推。竖线标签贴线右侧、不参与避让。
function placeLabels(items){
  // 半高 9 + 余量 2 → 轨道间距须 ≥22；间隙高 76、标签居中 → 0/±22 三层都落在间隙里
  //（贴竖线右侧的标签也走同一套避让 —— 它同样会与别人撞）。
  const PAD=2, HH=9;
  const placed=[];
  const box=(x,y,w)=>({x:x, y:y, w:w});
  const hits=(a,b)=> !(a.x+a.w/2+PAD < b.x-b.w/2 || a.x-a.w/2-PAD > b.x+b.w/2 ||
                       a.y+HH+PAD < b.y-HH || a.y-HH-PAD > b.y+HH);
  items.sort((a,b)=> (a.y-b.y) || (a.x-b.x));
  items.forEach(it=>{
    for(const dy of [0,-22,22]){
      const c=box(it.x, it.y+dy, it.w);
      if(!placed.some(p=>hits(c,p))){ it.at=c; placed.push(c); return; }
    }
    let x=it.x;                                   // 三条轨道都占满 → 沿 x 往右推
    for(let g=0; g<40; ++g){
      const c=box(x, it.y, it.w);
      if(!placed.some(p=>hits(c,p))){ it.at=c; placed.push(c); return; }
      x+=18;
    }
    it.at=box(it.x,it.y,it.w); placed.push(it.at);
  });
}
const EDGE_EL=[];   // 每条边画出来的元素（按轮次标「走过」用）
function renderSVG(){
  const size=layout();
  svg.setAttribute('width',size.w); svg.setAttribute('height',size.h); svg.setAttribute('viewBox','0 0 '+size.w+' '+size.h);
  // 箭头定义：forward 灰 / goto 红；terminal 无箭头（用端点圆点）
  const defs=el('defs');
  const mk=(id,fill)=>{ const m=el('marker'); m.setAttribute('id',id); m.setAttribute('viewBox','0 0 10 10');
    for(const [k,v] of Object.entries({refX:9,refY:5,markerWidth:6.5,markerHeight:6.5,orient:'auto'})) m.setAttribute(k,v);
    m.innerHTML='<path d="M0,0 L10,5 L0,10 z" fill="'+fill+'"/>'; defs.appendChild(m); };
  mk('arr','#5f6368'); mk('arrg','#d93025'); svg.appendChild(defs);
  // 输入 → 各入口 的内置边（只画，不参与分层）
  const rl=G.nodes.map(n=>layer[n.id]);
  const eMin=Math.min(...rl);
  const REDGES=G.edges.concat(
    G.nodes.filter(n=>layer[n.id]===eMin)
           .map(n=>({from:'__input__',to:n.id,forward:true,label:'next',terminal:false}))
  );
  // 两趟：先算每条边的折线与标签**锚点**，统一做一次防重叠，再建元素 ——
  // 标签必须一起排（同一条行间隙里的标签互相不知道，逐个画必然重叠）。
  const specs=[];
  REDGES.forEach(e=>{
    const a=pos[e.from]; if(!a) return;
    const b=pos[e.to];
    if(!b){
      // 兜底：目标不存在（非常规）→ 右缘短线段 + 端点圆点
      const ca={x:a.x+NODE_W/2, y:a.y+NODE_H/2};
      const x0=ca.x+NODE_W/2, x2=x0+40;
      specs.push({e:e, d:'M'+x0+','+ca.y+' L'+x2+','+ca.y,
                  anchor:{x:x2-6, y:ca.y-6, vertical:false}, dot:{x:x2,y:ca.y}});
      return;
    }
    // 正交圆角折线：走行间隙带 / 两侧通道（见 routeEdge 的说明）
    const pts=routeEdge(e.from, e.to);
    specs.push({e:e, d:roundedPath(pts, 8), anchor:labelAnchor(pts, e.from, e.to)});
  });
  const items=[];
  specs.forEach(sp=>{
    if(!sp.e.label || sp.e.label==='next') return;   // 只给有意义的边标文字
    items.push({x:sp.anchor.x, y:sp.anchor.y, vertical:sp.anchor.vertical,
                w:Math.max(24, textWidth(sp.e.label)), owner:sp});
  });
  placeLabels(items);
  items.forEach(it=>{ it.owner.anchor.x=it.at.x; it.owner.anchor.y=it.at.y; });

  specs.forEach(sp=>{
    const e=sp.e;
    const cls=e.forward ? (e.terminal?'e term':'e') : 'e goto';
    const p=el('path'); p.setAttribute('d',sp.d); p.setAttribute('class',cls);
    if(pos[e.to] && !e.terminal) p.setAttribute('marker-end', e.forward?'url(#arr)':'url(#arrg)');
    svg.appendChild(p);
    if(sp.dot){
      const dot=el('circle'); dot.setAttribute('cx',sp.dot.x); dot.setAttribute('cy',sp.dot.y);
      dot.setAttribute('r',3.5);
      dot.setAttribute('class',(/fail/.test(String(e.to))||e.label==='fail')?'eterm fail':'eterm');
      svg.appendChild(dot);
    }
    const lx=sp.anchor.x, ly=sp.anchor.y;
    let lt=null;
    if(e.label && e.label!=='next'){
      lt=el('text'); lt.setAttribute('x',lx); lt.setAttribute('y',ly);
      lt.setAttribute('text-anchor','middle'); lt.setAttribute('class','elabel');
      lt.textContent=e.label; svg.appendChild(lt);
    }
    // 次数角标：本轮走过才显示（内容由 applyEdgeUsage 填）
    const bg=el('rect'); bg.setAttribute('rx',6); bg.setAttribute('class','ecountbg');
    bg.setAttribute('height',12); bg.setAttribute('width',0);
    bg.setAttribute('x',lx-6); bg.setAttribute('y',ly+3);
    const cn=el('text'); cn.setAttribute('text-anchor','middle'); cn.setAttribute('class','ecount');
    cn.setAttribute('x',lx); cn.setAttribute('y',ly+12);
    svg.appendChild(bg); svg.appendChild(cn);
    EDGE_EL.push({key:e.from+'|'+e.to, path:p, label:lt, badge:bg, text:cn, x:lx, y:ly});
  });
  ALLNODES.forEach(n=>{
    const p=pos[n.id];
    const g=el('g'); g.setAttribute('transform','translate('+p.x+','+p.y+')');
    const rec=el('rect'); rec.setAttribute('width',NODE_W); rec.setAttribute('height',NODE_H);
    rec.setAttribute('rx',NODE_H/2);
    rec.setAttribute('class','n kind-'+n.kind); rec.setAttribute('data-id',n.id);
    const t1=el('text'); t1.setAttribute('x',NODE_W/2); t1.setAttribute('y',NODE_H/2+4);
    t1.setAttribute('text-anchor','middle'); t1.setAttribute('class','lbl');
    t1.textContent=n.id;              // 只显示节点 id；种类用颜色（见左栏图例）
    g.appendChild(rec); g.appendChild(t1);
    g.addEventListener('click',()=>pickNode(n.id));
    svg.appendChild(g);
  });
}
/* ---------- 安全转义（记录文本全部走 textContent，这里仅兜底）---------- */
function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
/* ---------- 节点记录统计（导航计数）---------- */
let countByNode = {};
function rebuildCounts(){
  countByNode = {};
  RUNS.forEach(run=>run.records.forEach(rc=>{
    const k = rc.node || '（引擎）'; countByNode[k] = (countByNode[k]||0) + 1;
  }));
}
/* ---------- 状态 ---------- */
let curNode='', curGoto='', curIt='', curLog='';
let curRunIdx = RUNS.length ? 0 : -1;   // 选中轮次（会话/run）→ 日志区只显示这一轮
/* ---------- 日志树：run 轮 → 节点 → 记录（可开合，记录间分割线）---------- */
function renderLog(){
  const box=document.getElementById('loglist');
  const open={}; box.querySelectorAll('details[open]').forEach(d=>{ if(d.dataset.grp) open[d.dataset.grp]=1; });
  box.innerHTML='';
  if(!RUNS.length){ box.textContent='（无会话日志：先跑一轮 wfr <工作流名> 再重新 debug）'; return; }
  let shown=0;
  RUNS.forEach((run,i)=>{
    if(curRunIdx>=0 && i!==curRunIdx) return;   // 只渲染选中会话下的选中 run（轮次）
    // 轮次(会话/run) → **按执行顺序的真实层级**：
    //   普通节点直接成组；循环的记录收进"该 loop 节点"这个容器，容器下再按迭代分组。
    //   容器名 = **真实的 loop 节点 id**；顶层顺序 = 各实体**首次出现**的顺序（即执行顺序），
    //   所以 __input__ / recall 排在 loop 之上、循环之后的节点排在 loop 之下。
    //   loop 归属与迭代号由 C++ 侧算好（rec.loop_id / rec.iter）。
    const tops=new Map(), torder=[];
    const touch=(key, make)=>{ let e=tops.get(key); if(!e){ e=make(); tops.set(key,e); torder.push(key); } return e; };
    const bucketOf=(maps, wf, node)=>{ const b=(maps[wf]=(maps[wf]||{})); (b[node]=(b[node]||[])).push; return b; };
    run.records.forEach(rc=>{
      if(curGoto&&curIt){
        const v=rc.loop&&rc.loop[curGoto]; if(v===undefined||Number(v)!==curIt) return;
        if(rc.loop_id!==curGoto) return;   // 该 loop 自己的节点头也算本轮（它有 loop_id）
      }
      const n0=rc.node?NODES[rc.node]:null;
      // 归属优先级：节点 origin_wf（import 子链）→ 记录徽标（父 workflow）→ 按节点/页推断
      const wf=(n0&&n0.def&&n0.def.origin_wf)
          ? n0.def.origin_wf
          : (rc.workflow||(n0?PAGEID:'（其它）'));
      const node=rc.node||'（引擎）';
      const put=(maps)=>{ const b=(maps[wf]=maps[wf]||{}); (b[node]=b[node]||[]).push(rc); };
      if(rc.loop_id){
        const e=touch('L:'+rc.loop_id, ()=>({kind:'loop', lid:rc.loop_id, iters:new Map()}));
        let it=e.iters.get(rc.iter);
        if(!it){ it={iter:rc.iter, wfs:{}}; e.iters.set(rc.iter, it); }
        put(it.wfs);
      } else {
        const e=touch('N:'+node, ()=>({kind:'node', node:node, wfs:{}}));
        put(e.wfs);
      }
    });

    // 渲染一个 {wf → {node → [recs]}} 集合：多于一个 wf 时才多一层（import 子链才需要）
    function renderMaps(parent, scope, wfs){
      const wfKeys=Object.keys(wfs);
      let cnt=0, nodes=0;
      wfKeys.forEach(wf=>{
        let holder=parent;
        if(wfKeys.length>1){
          const wk='wf:'+scope+':'+wf;
          const wd=document.createElement('details'); wd.className='wf'; wd.dataset.grp=wk;
          if(parent.open && (open[wk]!==undefined ? open[wk] : true)) wd.open=true;
          const ws=document.createElement('summary'); ws.textContent=wf;
          wd.appendChild(ws); parent.appendChild(wd); holder=wd;
        }
        Object.keys(wfs[wf]).forEach(node=>{
          const recs=wfs[wf][node]; cnt+=recs.length; ++nodes;
          const nk='n:'+scope+':'+wf+':'+node;
          const nd=document.createElement('details'); nd.className='node'; nd.dataset.grp=nk;
          if(holder.open && (open[nk]!==undefined ? open[nk] : true)) nd.open=true;
          const ns=document.createElement('summary');
          ns.textContent=node+'（'+recs.length+' 条）';
          nd.appendChild(ns);
          recs.forEach(r=>{
            const row=document.createElement('div'); row.className='rec';
            if(curNode && node===curNode) row.classList.add('hit');
            if(curLog && curLog===recKey(r)) row.classList.add('sel');
            row.dataset.node=r.node||''; row.dataset.seq=String(r.seq!==undefined?r.seq:'');
            row.appendChild(recordRow(r));
            row.addEventListener('click',()=>pickRecord(r));
            nd.appendChild(row);
          });
          holder.appendChild(nd);
        });
      });
      return {disp:cnt, nodes:nodes};
    }

    const rd=document.createElement('details'); rd.className='run'; rd.dataset.grp='r:'+run.run;
    if(open['r:'+run.run]!==undefined ? open['r:'+run.run] : true) rd.open=true;
    const rsum=document.createElement('summary');
    rd.appendChild(rsum);
    let disp=0, nNodes=0;
    torder.forEach(tk=>{
      const e=tops.get(tk);
      if(e.kind==='node'){
        // 普通节点（含 __input__ / recall / 循环之后的节点）
        const st=renderMaps(rd, tk, e.wfs);
        disp+=st.disp; nNodes+=st.nodes;
        return;
      }
      // 循环容器：名字就是真实的 loop 节点 id
      const iters=[...e.iters.keys()].sort((a,b)=>a-b);
      const ck='loop:'+e.lid;
      const cd=document.createElement('details'); cd.className='node loopnode'; cd.dataset.grp=ck;
      if(rd.open && (open[ck]!==undefined ? open[ck] : true)) cd.open=true;
      const cs=document.createElement('summary');
      cd.appendChild(cs);
      let cnt=0, nodes=0;
      iters.forEach(it=>{
        const ik=ck+'/'+it;
        const id=document.createElement('details'); id.className='iter'; id.dataset.grp=ik;
        if(cd.open && (open[ik]!==undefined ? open[ik] : true)) id.open=true;
        const is=document.createElement('summary');
        id.appendChild(is);
        const st=renderMaps(id, ik, e.iters.get(it).wfs);
        is.innerHTML=esc(e.lid+'/'+it)+' <span class="cnt">（'+st.disp+' 条）</span>';
        cd.appendChild(id);
        cnt+=st.disp; nodes+=st.nodes;
      });
      cs.innerHTML='↺ '+esc(e.lid)+' <span class="cnt">（'+iters.length+' 轮 · '+cnt+' 条）</span>';
      rd.appendChild(cd);
      disp+=cnt; nNodes+=nodes;
    });
    if(nNodes===0) return;                       // 本 run 被过滤空 → 不显示
    rsum.innerHTML=esc(run.run)+' <span class="cnt">（'+disp+' 条 · '+nNodes+' 节点）</span>';
    shown+=disp;
    box.appendChild(rd);
  });
  if(shown===0) box.textContent='（无记录）';
}
// 按**当前选中的轮次**标出真实走过的边与次数（数据来自 C++ 侧从引擎 enter 序列推出的
// run.edges；终端边靠「最后一个节点发布的 target」或「唯一 terminal 出边」补上）。
function applyEdgeUsage(){
  const run=(curRunIdx>=0)?RUNS[curRunIdx]:null;
  const map={};
  if(run && run.edges) run.edges.forEach(u=>{ map[u.from+'|'+u.to]=u.count; });
  EDGE_EL.forEach(E=>{
    const c=map[E.key]||0;
    const base=E.path.getAttribute('class').replace(' took','');
    if(c>0){
      E.path.setAttribute('class',base+' took');
      if(E.label) E.label.setAttribute('class','elabel took');
      const s2='×'+c, w=s2.length*7+6;
      E.text.textContent=s2;
      E.badge.setAttribute('width',String(w));
      E.badge.setAttribute('x',String(E.x-w/2));
    } else {
      E.path.setAttribute('class',base);
      if(E.label) E.label.setAttribute('class','elabel');
      E.text.textContent='';
      E.badge.setAttribute('width',0);
    }
  });
}
function recordRow(r){
  const frag=document.createDocumentFragment();
  let ph=r.phase||''; if(r.status) ph=ph?(ph+'·'+r.status):r.status;
  let cls='s'; if(r.status==='error') cls='e2'; else if(!r.phase&&r.source==='node') cls='iv';
  if(!ph){ ph=r.source==='node'?(r.level||'node'):'event'; cls='iv'; }
  const sp=document.createElement('span'); sp.className='ph '+cls; sp.textContent=ph; frag.appendChild(sp);
  const b=document.createElement('b'); b.textContent=r.node||'（引擎）'; frag.appendChild(b);
  if(r.level){ const l=document.createElement('span'); l.className='lvl'; l.textContent=' ['+r.level+']'; frag.appendChild(l); }
  if('update' in r){ const u=document.createElement('span'); u.className='lvl'; u.textContent=' '+JSON.stringify(r.update).slice(0,140); frag.appendChild(u); }
  else if('data' in r){ const u=document.createElement('span'); u.className='lvl'; u.textContent=' '+JSON.stringify(r.data).slice(0,140); frag.appendChild(u); }
  if(r.msg){ const ms=document.createElement('span'); ms.textContent=' '+r.msg; frag.appendChild(ms); }
  const ts=document.createElement('span'); ts.className='ts';
  ts.textContent=(r.ms!=null?r.ms+'ms ':'')+(r.ts?new Date(r.ts).toLocaleTimeString():''); frag.appendChild(ts);
  return frag;
}
/* ---------- 选中记录 → 使用说明 JSON 美化（下半区）---------- */
function recKey(r){ return (r.run||'')+'\u0001'+(r.node||'')+'\u0001'+String(r.seq!==undefined?r.seq:''); }
function pickRecord(r){
  gotoNode(r);                       // 保持跳图/展开定位
  const k=recKey(r);
  curLog=(curLog===k)?null:k;        // 再点同一行 → 取消
  renderLog();                       // 重挂 .sel（保留 open 状态）
  renderLogDetail(curLog?r:null);
}
function renderLogDetail(r){
  const head=document.getElementById('ldhead'), body=document.getElementById('ldbody');
  if(!r){ head.textContent='点日志行 → 下半区显示该条完整 JSON'; body.textContent=''; return; }
  head.textContent=(r.run||'?')+' / '+(r.node||'（引擎）')+' / '+(r.phase||r.level||'event')
    +((r.phase&&r.status)?('·'+r.status):'')+(r.ms!=null?('  ['+r.ms+'ms]'):'');
  body.innerHTML='<pre>'+hl(JSON.stringify(r,null,2))+'</pre>';
}
// 极简 JSON 语法高亮：key/字符串/数字/布尔/null 着色；文本全经过 esc 防注入
function hl(s){
  const num=/^-?\d+(\.\d+)?([eE][+-]?\d+)?/, kw=/^(true|false|null)/;
  let out='', i=0, n=s.length;
  while(i<n){
    const ch=s[i];
    if(ch==='"'){
      let j=i+1; while(j<n&&s[j]!=='"'){ if(s[j]==='\\')j+=2; else j++; } j++;
      const tok=s.slice(i,j); i=j;
      let k=i; while(k<n&&/\s/.test(s[k])) k++;
      out+='<span class="'+(s[k]===':'?'j-key':'j-str')+'">'+esc(tok)+'</span>';
      continue;
    }
    if(/[-\d]/.test(ch)){ const m=num.exec(s.slice(i)); if(m){ out+='<span class="j-num">'+esc(m[0])+'</span>'; i+=m[0].length; continue; } }
    const m=kw.exec(s.slice(i)); if(m){ out+='<span class="j-kw">'+esc(m[0])+'</span>'; i+=m[0].length; continue; }
    out+=esc(ch); i++;
  }
  return out;
}
/* ---------- 联动 ---------- */
function highlightNode(id){ document.querySelectorAll('rect.n').forEach(r=>r.classList.toggle('active',r.dataset.id===id)); }
function pickNode(id){ curNode=(curNode===id?'':id); renderNodeList(); renderLog(); renderDetail(curNode); highlightNode(curNode);
  if(curNode) focusNodeInLog(curNode); }
function focusNodeInLog(id){
  let nd=null; document.querySelectorAll('details.node').forEach(d=>{ if(!nd&&d.dataset.grp==='n:'+id) nd=d; });
  if(!nd) return;
  const run=nd.parentElement; if(run) run.open=true; nd.open=true;
  if(nd.scrollIntoView) nd.scrollIntoView({behavior:'smooth',block:'center'});
}
function cssq(s){ return String(s).replace(/\\/g,'\\\\').replace(/"/g,'\\"'); }
function gotoNode(r){
  curNode=r.node||''; curGoto=''; curIt='';
  renderNodeList(); renderLog(); renderDetail(curNode); highlightNode(curNode);
  if(r.run){ const rd=document.querySelector('details.run[data-grp="r:'+cssq(r.run)+'"]');
    if(rd){ rd.open=true;
      if(r.node){ const nd=rd.querySelector('details.node[data-grp="n:'+cssq(r.node)+'"]'); if(nd) nd.open=true; }
    } }
  const row=document.querySelector('details.run[data-grp="r:'+cssq(r.run||'')+'"] .rec[data-seq="'+cssq(String(r.seq!==undefined?r.seq:''))+'"][data-node="'+cssq(r.node||'')+'"]');
  if(row) row.scrollIntoView({behavior:'smooth',block:'center'});
}
/* ---------- 节点 JSON 描述 ---------- */
function renderDetail(id){
  const hn=document.getElementById('dnode'), hk=document.getElementById('dkind'), pre=document.getElementById('djson');
  const n=id?NODES[id]:null;
  if(!id||!n){ hn.textContent=id||''; hk.textContent=n?(n.kind||''):'';
    pre.textContent=(id&&!n)?'(引擎合成节点，无原始定义 —— 如 __input__)':'(点击某个节点，其 JSON 定义显示在这里)'; return; }
  if(n.def===undefined||n.def===null){ hn.textContent=id; hk.textContent='· '+(n.kind||'');
    pre.textContent='（内置合成节点，无原始定义 —— 点右侧日志里对应记录行可看完整 JSON）'; return; }
  hn.textContent=id; hk.textContent=n.kind||'';
  const js=JSON.stringify(n.def!=null?n.def:null,null,2);
  pre.textContent=js.length>20000?js.slice(0,20000)+'\n…（已截断）':js;
}
const dcopy=document.getElementById('dcopy');
dcopy.addEventListener('click',()=>{
  const t=document.getElementById('djson').textContent;
  try{ navigator.clipboard.writeText(t); }
  catch(e){ const a=document.createElement('textarea'); a.value=t; document.body.appendChild(a); a.select(); document.execCommand('copy'); a.remove(); }
});
/* ---------- 导航列表 ---------- */
function renderNodeList(){
  const box=document.getElementById('nodelist'); box.innerHTML='';
  ALLNODES.forEach(n=>{
    const d=document.createElement('div'); d.className='nodeitem';
    if(n.id===curNode) d.classList.add('on');
    const lab=document.createElement('span'); lab.textContent=n.id+'（'+n.kind+'）';
    d.appendChild(lab);
    const c=document.createElement('span'); c.className='cnt'; c.textContent=countByNode[n.id]||0;
    d.appendChild(c);
    d.addEventListener('click',()=>pickNode(n.id)); box.appendChild(d);
  });
}
function renderGotoTabs(){
  const box=document.getElementById('gototabs'); box.innerHTML='';
  // 分组维度 = loop 节点（wfr-㉚）。loop_counts 的键就是 loop 节点 id，所以迭代号直接可读。
  const gotos=[...new Set(G.nodes.filter(n=>n.kind==='loop').map(n=>n.id))];
  if(gotos.length===0){ box.textContent='（无循环）'; return; }
  gotos.forEach(g=>{
    const wrap=document.createElement('div'); wrap.className='gototab';
    const cap=document.createElement('div'); cap.textContent='↺ '+g;
    wrap.appendChild(cap);
    const selRecs = (curRunIdx>=0 && RUNS[curRunIdx]) ? RUNS[curRunIdx].records
                  : RUNS.flatMap(r=>r.records);   // 循环分组只看选中轮次
    const its=[...new Set(selRecs.map(r=>r.loop&&r.loop[g]).filter(v=>v&&!isNaN(v)))].sort((a,b)=>a-b);
    (its.length?its:[1]).forEach(it=>{
      const b=document.createElement('button'); b.className='loopit'; b.textContent=it;
      if(curGoto===g&&String(curIt)===String(it)) b.classList.add('on');
      b.addEventListener('click',()=>{ curGoto=g; curIt=(curGoto===g&&String(curIt)===String(it))?'':it; renderLog(); renderGotoTabs(); });
      wrap.appendChild(b);
    });
    box.appendChild(wrap);
  });
}
/* ---------- 会话 / 轮次选择器（左栏）：session → run ---------- */
let SESS = {};
function rebuildSess(){
  SESS = {};
  RUNS.forEach((r,i)=>{ const p=(r.run.split('/')[0])||r.run; (SESS[p]=SESS[p]||[]).push(i); });
}
function renderRunSelectors(){
  const ss=document.getElementById('sessSel'), rs=document.getElementById('runSel');
  const sessions=Object.keys(SESS);
  if(!sessions.length) return;
  const curRunLabel = (curRunIdx>=0 && RUNS[curRunIdx]) ? RUNS[curRunIdx].run : '';
  const curSess = curRunLabel.split('/')[0];
  const activeSess = (curSess && SESS[curSess]) ? curSess : sessions[0];
  ss.innerHTML='';
  sessions.forEach(p=>{
    const o=document.createElement('option'); o.value=p;
    o.textContent = p + (SESS[p].length>1 ? '（'+SESS[p].length+' 轮）' : '');
    if(p===activeSess) o.selected=true; ss.appendChild(o);
  });
  const runs=(SESS[activeSess]||[]);
  if(!runs.includes(curRunIdx)) curRunIdx = runs.length? runs[0] : -1;
  rs.innerHTML='';
  runs.forEach(idx=>{
    const o=document.createElement('option'); o.value=String(idx);
    o.textContent=RUNS[idx].run.split('/')[1]||RUNS[idx].run;
    if(idx===curRunIdx) o.selected=true; rs.appendChild(o);
  });
}
function bindRunSelectors(){
  const ss=document.getElementById('sessSel'), rs=document.getElementById('runSel');
  ss.addEventListener('change',()=>{
    const runs=(SESS[ss.value]||[]); curRunIdx = runs.length? runs[0] : -1;
    renderRunSelectors(); renderLog(); renderGotoTabs(); applyEdgeUsage();
  });
  rs.addEventListener('change',()=>{
    curRunIdx=Number(rs.value); renderLog(); renderGotoTabs(); applyEdgeUsage();
  });
}
/* ---------- 分栏拖动 + 尺寸持久化（localStorage 尽力而为）---------- */
function loadSizes(){ try{ return JSON.parse(localStorage.getItem('wfr_dbg_size'))||{}; }catch(e){ return {}; } }
function saveSizes(){ try{ localStorage.setItem('wfr_dbg_size',
  JSON.stringify({wnav:document.documentElement.style.getPropertyValue('--wnav'),
                 wlog:document.documentElement.style.getPropertyValue('--wlog'),
                 dh:document.getElementById('detail').style.height,
                 dh2:document.getElementById('logdetail').style.height})); }catch(e){} }
function bindSplitter(bar, axis, get, set, min, max, dir){
  dir = (dir===undefined)?1:dir;   // dir=-1：该分割条锚定在拖动方向的反侧（右/下锚定）
  bar.addEventListener('pointerdown',e=>{
    e.preventDefault();
    document.body.classList.add(axis==='v'?'drag-v':'drag-h');
    const start=(axis==='v'?e.clientX:e.clientY), base=get();
    const move=ev=>{
      const delta=(axis==='v'?ev.clientX:ev.clientY)-start;
      let v=dir>0?base+delta:base-delta;
      v=axis==='v'?Math.min(max,Math.max(min,v)):Math.min(window.innerHeight-200,Math.max(min,v));
      set(v);
    };
    const up=()=>{ window.removeEventListener('pointermove',move); window.removeEventListener('pointerup',up);
                   document.body.classList.remove(axis==='v'?'drag-v':'drag-h'); saveSizes(); };
    window.addEventListener('pointermove',move); window.addEventListener('pointerup',up);
  });
}
function initSplitters(){
  const gv=(n)=>parseFloat(getComputedStyle(document.documentElement).getPropertyValue(n))||200;
  const sizes=loadSizes();
  if(sizes.wnav) document.documentElement.style.setProperty('--wnav',sizes.wnav);
  if(sizes.wlog) document.documentElement.style.setProperty('--wlog',sizes.wlog);
  const dt=document.getElementById('detail'); if(sizes.dh&&parseFloat(sizes.dh)>0) dt.style.height=sizes.dh;
  const dt2=document.getElementById('logdetail'); if(sizes.dh2&&parseFloat(sizes.dh2)>0) dt2.style.height=sizes.dh2;
  bindSplitter(document.getElementById('sv1'),'v',()=>gv('--wnav'),
    v=>document.documentElement.style.setProperty('--wnav',Math.round(v)+'px'),160,380,1);
  bindSplitter(document.getElementById('sv2'),'v',()=>gv('--wlog'),
    v=>document.documentElement.style.setProperty('--wlog',Math.round(v)+'px'),300,640,-1);   // 右锚定→反向
  bindSplitter(document.getElementById('sh'),'h',()=>dt.offsetHeight,
    v=>dt.style.height=Math.round(v)+'px',96,0,-1);   // 底锚定→反向
  bindSplitter(document.getElementById('sh2'),'h',()=>dt2.offsetHeight,
    v=>dt2.style.height=Math.round(v)+'px',80,0,-1);   // 日志详情区（日志栏内）→ 底锚定反向
}
/* ---------- 拖入 / 选择日志文件（wfr-㉟：页面自己拿数据，不必重新生成网页）----------
   每个文件按会话解析（文件名 session-<名>.log.jsonl → 会话名），run 加上会话前缀 ——
   与 --all 采集时同一套约定，于是多会话并排可选。可反复拖入追加。 */
// 文件名 / URL 路径 → 会话名：session-<名>.log.jsonl → <名>
function sessionNameOf(p){
  const base=String(p||'log').replace(/[?#].*$/,'').replace(/^.*\//,'');
  return base.replace(/^session-/,'').replace(/\.log\.jsonl$/,'').replace(/\.jsonl$/,'').replace(/\.json$/,'')||'log';
}
// 从 URL 读一份日志。注意：file:// 打开的页面发跨源 fetch 会被浏览器按 CORS 拦掉
//（file 源是 null）—— 失败原因会明确写出来，不静默吞。
function loadFromUrl(url){
  const hint=document.getElementById('urlhint');
  hint.textContent='读取中…'; hint.style.color='#888';
  fetch(url).then(res=>{
    if(!res.ok) throw new Error('HTTP '+res.status);
    return res.text();
  }).then(text=>{
    try{ localStorage.setItem('wfr_dbg_url', url); }catch(e){}
    hint.textContent=''; ingestTexts([{name:sessionNameOf(url), text:text}]);
  }).catch(e=>{
    hint.style.color='#d93025';
    hint.textContent='读取失败：'+String(e&&(e.message||e))+
      '（file:// 打开的页面会被 CORS 拦；把日志放到同源 http 服务下，或直接拖文件进来）';
  });
}
function ingestTexts(entries){        // entries: [{name, text}]
  const dropped=[];
  entries.forEach(({name,text})=>{
    const sess=sessionNameOf(name);
    text.split('\n').forEach(line=>{
      const t=line.trim(); if(!t) return;
      let r; try{ r=JSON.parse(t); }catch(e){ return; }   // 半行/坏行跳过（与 C++ 读日志同判）
      if(!r || typeof r!=='object') return;
      r.session=sess;
      const run=r.run||'';
      if(run && run.indexOf(sess+'/')!==0) r.run=sess+'/'+run;
      dropped.push(r);
    });
  });
  if(!dropped.length){ flashDrop('没解析出记录（要 .log.jsonl，每行一条 JSON）', true); return; }
  // 同一会话按 seq 归并：拖多个文件/多次追加也能得到确定顺序
  dropped.sort((a,b)=>{
    const sa=String(a.session||''), sb=String(b.session||'');
    if(sa!==sb) return sa<sb?-1:1;
    return Number(a.seq||0)-Number(b.seq||0);
  });
  setRecords((P.records||[]).concat(dropped));
  flashDrop('已载入 '+dropped.length+' 条记录（共 '+RUNS.length+' 轮）');
}
function flashDrop(msg, bad){
  const d=document.getElementById('drop');
  d.classList.remove('over'); if(bad) d.classList.add('err'); else d.classList.remove('err');
  d.dataset.msg=msg;
  d.firstChild.nodeValue=msg;
  setTimeout(()=>{ const dd=document.getElementById('drop');
    dd.firstChild.nodeValue='把 .log.jsonl 拖到这里';
    dd.classList.remove('err'); dd.classList.remove('over'); }, 2600);
}
function setRecords(raw){
  RUNS = deriveRuns(raw||[]);
  P.records = raw||[];
  rebuildCounts(); rebuildSess();
  curRunIdx = RUNS.length ? 0 : -1;
  curGoto=''; curIt=''; curLog='';
  renderRunSelectors(); renderNodeList(); renderGotoTabs(); renderLog(); applyEdgeUsage();
}
function bindUrlRow(){
  const box=document.getElementById('urlbox'), go=document.getElementById('urlgo');
  try{ const last=localStorage.getItem('wfr_dbg_url'); if(last && !box.value) box.value=last; }catch(e){}
  go.addEventListener('click',()=>{ const u=box.value.trim(); if(u) loadFromUrl(u); });
  box.addEventListener('keydown',e=>{ if(e.key==='Enter'){ const u=box.value.trim(); if(u) loadFromUrl(u); } });
}
function bindDrop(){
  const d=document.getElementById('drop'), pick=document.getElementById('filepick');
  d.addEventListener('click',()=>pick.click());
  pick.addEventListener('change',()=>{
    const fs=[...pick.files]; if(!fs.length) return;
    Promise.all(fs.map(f=>f.text().then(text=>({name:f.name,text})))).then(ingestTexts);
    pick.value='';
  });
  d.addEventListener('dragover',e=>{ e.preventDefault(); d.classList.add('over'); });
  d.addEventListener('dragleave',()=>d.classList.remove('over'));
  d.addEventListener('drop',e=>{
    e.preventDefault(); d.classList.remove('over');
    const fs=[...(e.dataTransfer?e.dataTransfer.files:[])];
    if(!fs.length) return;
    Promise.all(fs.map(f=>f.text().then(text=>({name:f.name,text})))).then(ingestTexts);
  });
}

/* ---------- 启动 ---------- */
try {
  layout(); renderSVG(); renderLegend(); bindRunSelectors(); bindDrop(); bindUrlRow();
  setRecords(P.records||[]);
  renderDetail(''); renderLogDetail(null);
  if(P.trimmed_node_logs) document.getElementById('trim').style.display='block';
  initSplitters();
} catch (e) {
  const box=document.getElementById('loglist');
  box.innerHTML='<b>初始化失败</b><br>'+String(e && (e.message||e))+'<br>（打开控制台看堆栈）';
  throw e;
}
</script>
</body></html>)html";

  std::string out(kHtml);
  auto const idx = out.find("__DATA__");
  std::string const data = script_data_escape(payload.dump());
  std::string const ir_id = payload["ir"].value("id", "");
  std::string const ir_escaped = script_data_escape(ir_id);
  out.replace(idx, 8, data);
  // 两处 marker（<title> 与 <h1>）各自显式替换。注意 "__IR_ID__" 是 **9** 个字符
  //（过替换 10 会把紧跟的 `</title>` 左尖括号一并吃掉 → title 永不闭合、head 崩）。
  std::size_t const pTitle = out.find("__IR_ID__");
  if (pTitle != std::string::npos) out.replace(pTitle, 9, ir_escaped);
  std::size_t const pH1 = out.find("__IR_ID__");
  if (pH1 != std::string::npos) out.replace(pH1, 9, ir_escaped);
  return out;
}

}  // namespace cppchain::workflow