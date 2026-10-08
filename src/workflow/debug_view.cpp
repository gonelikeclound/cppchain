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

Json debug_view_logs(std::vector<Json> const& records, WorkflowIR const& ir) {
  // 第一遍：逐条重放 loop_counts，记下每条**之后**（含它自己的 update）的快照 ——
  // 后面的迭代号判定要用它。
  std::vector<Json> after;
  after.reserve(records.size());
  Json loop = Json::object();
  for (Json const& r : records) {
    if (auto const it = r.find("update"); it != r.end() && it->is_object()) {
      if (auto const lc = it->find("loop_counts"); lc != it->end() && lc->is_object())
        for (auto k = lc->begin(); k != lc->end(); ++k) loop[k.key()] = k.value();
    }
    after.push_back(loop);
  }

  // 节点归属：body 节点 → 所属 loop id；loop 节点头 → 它自己（节点头不带 `loop` 字段）。
  auto const owner = loop_owner(ir);
  std::unordered_map<std::string, bool> is_head;
  for_each_node(ir, [&](NodeIR const& n, LoopScope const*) {
    if (n.kind == NodeKind::Loop) is_head[n.id] = true;
    return true;
  });

  // 某条记录属于 loop L 的第几轮。
  auto iter_of = [&](std::size_t i, std::string const& L) -> std::optional<long> {
    std::string const node = records[i].value("node", "");
    if (is_head.count(node) != 0) {
      // 节点头：enter 在自增之前、exit 才是那次自增 → 取"此后第一次自增后的值"，
      // 于是 enter/exit 这对记录都归到第 N 轮；跑满后的最后一次判定没有自增 →
      // 回落到快照值（= max，归到第 max 轮，作为那一轮的收尾）。
      for (std::size_t j = i; j < records.size(); ++j) {
        if (auto const it = records[j].find("update"); it != records[j].end() && it->is_object())
          if (auto const lc = it->find("loop_counts"); lc != it->end() && lc->is_object())
            if (lc->contains(L) && (*lc)[L].is_number()) return (*lc)[L].get<long>();
      }
    }
    if (after[i].contains(L) && after[i][L].is_number()) return after[i][L].get<long>();
    // 兜底（还没进过循环）：向后取第一个已定义值
    for (std::size_t j = i; j < records.size(); ++j)
      if (after[j].contains(L) && after[j][L].is_number()) return after[j][L].get<long>();
    return std::nullopt;
  };

  std::vector<std::string> order;
  std::map<std::string, Json> by_run;
  for (std::size_t i = 0; i < records.size(); ++i) {
    Json const& r = records[i];
    std::string run = r.value("run", "");
    std::string const key = run.empty() ? "?" : run;
    if (by_run.find(key) == by_run.end()) order.push_back(key);
    Json rec = r;
    rec["loop"] = after[i];  // 该时刻 loop_counts 快照（前端"循环分组"页签用）
    // 归属：body 节点看 owner；loop 节点头看它自己
    std::string const node = r.value("node", "");
    std::string lid;
    if (auto const it = owner.find(node); it != owner.end()) lid = it->second;
    else if (is_head.count(node) != 0) lid = node;
    if (!lid.empty()) {
      if (auto const n = iter_of(i, lid)) {
        rec["loop_id"] = lid;
        rec["iter"] = *n;
      }
    }
    by_run[key].push_back(std::move(rec));
  }
  Json out = Json::array();
  for (std::string const& k : order)
    out.push_back(Json{{"run", k}, {"records", by_run[k]}});
  return out;
}

Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records) {
  Json runs = debug_view_logs(records, ir);
  bool trimmed = false;
  if (records.size() > 20000) {  // 超大会话：裁掉节点诊断，保结构事件
    for (Json& run : runs) {
      Json keep = Json::array();
      for (Json const& rec : run["records"])
        if (rec.value("source", "") != "node") keep.push_back(rec);
      run["records"] = std::move(keep);
    }
    trimmed = true;
  }
  return Json{{"graph", debug_view_graph(ir)},
              {"ir", Json{{"id", ir.id}}},
              {"runs", std::move(runs)},
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
.gototab{margin-top:4px}
.loopit{padding:2px 6px;border:1px solid #ccc;border-radius:4px;margin:2px 2px 2px 0;display:inline-block;cursor:pointer;background:#fff}
.loopit.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.small{color:#888;font:11px}
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
details.loopg{margin:3px 2px 3px 7px}
details.loopg>summary{list-style:none;cursor:pointer;padding:3px 6px;font-weight:600;color:#5b2c8e;background:#f6f2fd;border-radius:3px;font-size:12px}
details.loopg>summary:hover{background:var(--hl)}
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
const G = P.graph, RUNS = P.runs;
const NODES = {}; G.nodes.forEach(n => NODES[n.id] = n);
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
  echo:{f:'#fde7d9',s:'#f9672a'}, loop:{f:'#e6dcf7',s:'#6a1b9a'}
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
/* ---------- SVG 绘制 v2：贝塞尔边（同行微拱/跨行等差）+ 回边弧线 + 类型配色 ---------- */
const svg=document.getElementById('canvas');
const NS='http://www.w3.org/2000/svg';
function el(t){ return document.createElementNS(NS,t); }
// 从中心 (cx,cy) 沿方向 (dx,dy) 到矩形节点边界交点（让箭头停在节点边界，不压住文字）
function boxEdge(cx,cy,dx,dy){
  const t=Math.min((NODE_W/2)/Math.abs(dx),(NODE_H/2)/Math.abs(dy));
  return {x:cx+dx*t, y:cy+dy*t};
}
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
  REDGES.forEach(e=>{
    const a=pos[e.from]; if(!a) return;
    const ca={x:a.x+NODE_W/2, y:a.y+NODE_H/2};
    const b=pos[e.to];
    let d, lx, ly;
    if(!b){
      // 兜底：目标不存在（非常规）→ 右缘短线段 + 端点圆点
      const y1=ca.y, x0=ca.x+NODE_W/2, x2=x0+40;
      d='M'+x0+','+y1+' L'+x2+','+y1;
      lx=x2-6; ly=y1-6;
      const dot=el('circle'); dot.setAttribute('cx',x2); dot.setAttribute('cy',y1); dot.setAttribute('r',3.5);
      dot.setAttribute('class',(/fail/.test(String(e.to))||e.label==='fail')?'eterm fail':'eterm');
      svg.appendChild(dot);
    } else {
      // 中心到中心的三次贝塞尔：两个控制点都落在两中心之间的中层（连线从中间穿过）
      const cb={x:b.x+NODE_W/2, y:b.y+NODE_H/2};
      const dx=cb.x-ca.x, dy=cb.y-ca.y;
      const s=boxEdge(ca.x,ca.y,dx,dy), t=boxEdge(cb.x,cb.y,-dx,-dy);
      const my=(ca.y+cb.y)/2, mx=(s.x+t.x)/2;
      if (dy < 0) {
        // 回边（向上）→ 绕到左侧弧线：不沿主线列走，避免与同列的竖直前向边完全重叠
        const bx = Math.min(s.x, t.x) - NODE_W * 0.55 - 4;
        d='M'+s.x+','+s.y+' C'+bx+','+(my-32)+' '+bx+','+(my+32)+' '+t.x+','+t.y;
        lx=bx; ly=my-12;
      } else {
        d='M'+s.x+','+s.y+' C'+s.x+','+my+' '+t.x+','+my+' '+t.x+','+t.y;
        lx=mx; ly=my-8;
      }
    }
    const cls=e.forward ? (e.terminal?'e term':'e') : 'e goto';
    const p=el('path'); p.setAttribute('d',d); p.setAttribute('class',cls);
    if(b && !e.terminal) p.setAttribute('marker-end', e.forward?'url(#arr)':'url(#arrg)');
    svg.appendChild(p);
    if(e.label && e.label!=='next'){
      const t=el('text'); t.setAttribute('x',lx); t.setAttribute('y',ly);
      t.setAttribute('text-anchor','middle'); t.setAttribute('class','elabel');
      t.textContent=e.label; svg.appendChild(t);
    }
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
const countByNode = {};
RUNS.forEach(run=>run.records.forEach(rc=>{
  const k = rc.node || '（引擎）'; countByNode[k] = (countByNode[k]||0) + 1;
}));
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
    // 轮次(会话/run) → **loop 迭代**（↺ loop:1 / ↺ loop:2 /（循环外））→ workflow → 节点 → 记录。
    // loop 归属与迭代号由 C++ 侧算好（rec.loop_id / rec.iter），前端不再自己推断 ——
    // loop 节点头的 enter 在自增之前，只有 C++ 那边知道该归到哪一轮。
    const groups={}, gorder=[];
    run.records.forEach(rc=>{
      if(curGoto&&curIt){
        const v=rc.loop&&rc.loop[curGoto]; if(v===undefined||Number(v)!==curIt) return;
        if(rc.loop_id!==curGoto) return;   // 该 loop 自己的节点头也算本轮（它有 loop_id）
      }
      const gk=rc.loop_id ? (rc.loop_id+'#'+rc.iter) : '';
      let g=groups[gk];
      if(!g){
        g=groups[gk]={lid:rc.loop_id||'', iter:(typeof rc.iter==='number'?rc.iter:-1),
                      label:(rc.loop_id ? ('↺ '+rc.loop_id+':'+rc.iter) : '（循环外）'), wfs:{}};
        gorder.push(gk);
      }
      const n0=rc.node?NODES[rc.node]:null;
      // 归属优先级：节点 origin_wf（import 子链）→ 记录徽标（父 workflow）→ 按节点/页推断
      const wf=(n0&&n0.def&&n0.def.origin_wf)
          ? n0.def.origin_wf
          : (rc.workflow||(n0?PAGEID:'（其它）'));
      const bucket=(g.wfs[wf]=(g.wfs[wf]||{}));
      const k=rc.node||'（引擎）';
      (bucket[k]=(bucket[k]||[])).push(rc);
    });
    // 排序：循环组按（loop id, 迭代号）升序；"（循环外）"永远排最后
    gorder.sort((a,b)=>{
      const A=groups[a], B=groups[b];
      if(A.lid==='') return 1;
      if(B.lid==='') return -1;
      return A.lid===B.lid ? (A.iter-B.iter) : (A.lid<B.lid?-1:1);
    });
    const rd=document.createElement('details'); rd.className='run'; rd.dataset.grp='r:'+run.run;
    if(open['r:'+run.run]!==undefined ? open['r:'+run.run] : true) rd.open=true;
    const rsum=document.createElement('summary');
    rd.appendChild(rsum);
    let disp=0, nNodes=0;
    gorder.forEach(gk=>{
      const g=groups[gk];
      const gd=document.createElement('details'); gd.className='loopg'; gd.dataset.grp='g:'+gk;
      if(rd.open && (open['g:'+gk]!==undefined ? open['g:'+gk] : true)) gd.open=true;
      const gsum=document.createElement('summary');
      gd.appendChild(gsum);
      const wfKeys=Object.keys(g.wfs);
      let gcount=0;
      wfKeys.forEach(wf=>{
        const nodesMap=g.wfs[wf];
        let parent=gd;
        if(wfKeys.length>1){   // 多个 workflow（import 子链）时才多这一层，单链场景不添噪音
          const wd=document.createElement('details'); wd.className='wf'; wd.dataset.grp='wf:'+gk+':'+wf;
          if(gd.open && (open['wf:'+gk+':'+wf]!==undefined ? open['wf:'+gk+':'+wf] : true)) wd.open=true;
          const wsum=document.createElement('summary');
          wsum.textContent=wf;
          wd.appendChild(wsum); gd.appendChild(wd); parent=wd;
        }
        Object.keys(nodesMap).forEach(node=>{
          const recs=nodesMap[node]; nNodes++; gcount+=recs.length; disp+=recs.length;
          const nd=document.createElement('details'); nd.className='node'; nd.dataset.grp='n:'+gk+':'+node;
          if(parent.open && (open['n:'+gk+':'+node]!==undefined ? open['n:'+gk+':'+node] : true)) nd.open=true;
          const nsum=document.createElement('summary');
          nsum.textContent=node+'（'+recs.length+' 条）';
          nd.appendChild(nsum);
          recs.forEach(r=>{
            const row=document.createElement('div'); row.className='rec';
            if(curNode && node===curNode) row.classList.add('hit');
            if(curLog && curLog===recKey(r)) row.classList.add('sel');
            row.dataset.node=r.node||''; row.dataset.seq=String(r.seq!==undefined?r.seq:'');
            row.appendChild(recordRow(r));
            row.addEventListener('click',()=>pickRecord(r));
            nd.appendChild(row);
          });
          parent.appendChild(nd);
        });
      });
      gsum.innerHTML=esc(g.label)+' <span class="cnt">（'+gcount+' 条）</span>';
      rd.appendChild(gd);
    });
    if(nNodes===0) return;                       // 本 run 被过滤空 → 不显示
    rsum.innerHTML=esc(run.run)+' <span class="cnt">（'+disp+' 条 · '+nNodes+' 节点）</span>';
    shown+=disp;
    box.appendChild(rd);
  });
  if(shown===0) box.textContent='（无记录）';
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
const SESS = {};
RUNS.forEach((r,i)=>{ const p=(r.run.split('/')[0])||r.run; (SESS[p]=SESS[p]||[]).push(i); });
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
    renderRunSelectors(); renderLog(); renderGotoTabs();
  });
  rs.addEventListener('change',()=>{
    curRunIdx=Number(rs.value); renderLog(); renderGotoTabs();
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
/* ---------- 启动 ---------- */
try {
  layout(); renderSVG(); renderLegend(); renderRunSelectors(); bindRunSelectors();
  renderNodeList(); renderGotoTabs(); renderLog(); renderDetail(''); renderLogDetail(null);
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