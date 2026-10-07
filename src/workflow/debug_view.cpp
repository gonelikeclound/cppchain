// cppchain —— src/workflow/debug_view.cpp
// wfr 调试台的导出与渲染（见头注释）。HTML 为自包含单文件：分层 SVG + 三栏可拖布局 +
// 树形日志（run 轮 → 节点 → 记录）+ 选中节点 JSON 描述 + 循环分组。
#include "cppchain/workflow/debug_view.hpp"

#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/node_type.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::workflow {

Json debug_view_graph(WorkflowIR const& ir) {
  Json nodes = Json::array();
  for (NodeIR const& n : ir.nodes) {
    std::string const kind = type_name_of(n);
    std::string text;
    if (NodeType const* t = node_type_of_name(kind); t != nullptr && t->describe)
      text = t->describe(n);
    else
      text = kind + " " + n.id;
    nodes.push_back(Json{{"id", n.id}, {"kind", kind}, {"text", text}, {"def", n.raw}});
  }
  Json edges = Json::array();
  for (NodeIR const& n : ir.nodes) {
    if (NodeType const* t = node_type_of_name(type_name_of(n)); t != nullptr && t->edges)
      for (NodeType::Edge const& e : t->edges(n)) {
        bool const terminal =
            e.target == "__end__" || e.target == "__fail__" || e.target == "_turn_end" ||
            e.target == "_fail";
        edges.push_back(Json{{"from", n.id},
                             {"to", e.target},
                             {"label", e.label},
                             {"forward", e.forward},
                             {"terminal", terminal}});
      }
  }
  return Json{{"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
}

Json debug_view_logs(std::vector<Json> const& records) {
  Json loop = Json::object();  // 重放中的 loop_counts 快照（右胜左覆盖合并，merge_slots 同款）
  std::vector<std::string> order;
  std::map<std::string, Json> by_run;

  for (Json const& r : records) {
    if (auto const it = r.find("update"); it != r.end() && it->is_object()) {
      if (auto const lc = it->find("loop_counts"); lc != it->end() && lc->is_object())
        for (auto k = lc->begin(); k != lc->end(); ++k) loop[k.key()] = k.value();
    }
    std::string run = r.value("run", "");
    std::string const key = run.empty() ? "?" : run;
    if (by_run.find(key) == by_run.end()) order.push_back(key);
    Json rec = r;
    rec["loop"] = loop;  // 该时刻 loop_counts 快照（每条记录内嵌，前端直接分组）
    by_run[key].push_back(std::move(rec));
  }
  Json out = Json::array();
  for (std::string const& k : order)
    out.push_back(Json{{"run", k}, {"records", by_run[k]}});
  return out;
}

Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records) {
  Json runs = debug_view_logs(records);
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
#graphwrap{flex:1 1 auto;overflow:auto;padding:8px;min-height:0}
#sh{flex:none;height:8px;border-top:1px solid var(--e)}
#detail{flex:none;height:220px;border-top:1px solid var(--e);overflow:auto;background:#fff;padding:8px 10px;min-height:0}
#log{grid-column:5;border-left:1px solid var(--e);padding:10px;overflow:auto;background:#fff;min-width:0;min-height:0}
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
.legend .item{display:flex;align-items:center;gap:5px;font-size:11px;color:#555}
.legend .dot{width:16px;height:11px;border-radius:3px;border:1.5px solid;background:transparent}
.nodeitem{padding:2px 4px;cursor:pointer;border-radius:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;display:flex;justify-content:space-between;gap:6px}
.nodeitem:hover,.nodeitem.on{background:var(--hl)}
.nodeitem .cnt{color:#999;font-weight:600}
.gototab{margin-top:4px}
.loopit{padding:2px 6px;border:1px solid #ccc;border-radius:4px;margin:2px 2px 2px 0;display:inline-block;cursor:pointer;background:#fff}
.loopit.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.small{color:#888;font:11px}
/* ---- 日志树：run 轮 → 节点 → 记录 ---- */
#loglist{margin-top:4px}
details.run{background:#fff;border:1px solid var(--e);border-radius:5px;margin:6px 0;overflow:hidden}
details.run>summary{list-style:none;cursor:pointer;padding:5px 8px;background:#f4f6f9;font-weight:600}
details.run>summary:hover{background:var(--hl)}
details.node{margin:2px 2px 2px 6px}
details.node>summary{list-style:none;cursor:pointer;padding:3px 6px;font-weight:600;color:#333;border-radius:3px}
details.node>summary:hover{background:var(--hl)}
summary::before{content:'▸';display:inline-block;width:14px;color:#888}
details[open]>summary::before{content:'▾'}
.cnt{color:#999;font-weight:400}
.rec{padding:3px 8px 3px 22px;border-top:1px dashed #e4e7ea;cursor:pointer;white-space:pre-wrap;word-break:break-all;font:11.5px ui-monospace,Menlo,Consolas,monospace}
details.node>.rec:first-of-type{border-top:none}
.rec:hover{background:var(--hl)}.rec.hit{background:#fdecea}
.rec b{font-weight:600;color:#222}
.ph{display:inline-block;min-width:64px;color:#666;font-weight:600}
.s{color:#1a73e8}.e2{color:#d93025}.iv{color:#b06000}
.lvl{color:#9e9e9e}.ts{color:#aaa;font-size:10.5px;float:right;margin-left:8px}
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
<h2>图例（节点颜色）</h2><div class="legend" id="legend"></div>
<h2>节点（导航）</h2><div id="nodelist"></div>
<h2>循环分组</h2><div id="gototabs"></div>
</aside>
<div id="sv1" class="split" title="拖动调整左栏宽度"></div>
<div id="mid">
  <div id="graphwrap"><svg id="canvas" width="700" height="400"></svg></div>
  <div id="sh" title="拖动调整详情区高度"></div>
  <div id="detail"><div id="detailhead">节点 <b id="dnode"></b><span id="dkind"></span>
    <button id="dcopy">复制</button></div><pre id="djson"></pre></div>
</div>
<div id="sv2" class="split" title="拖动调整右栏宽度"></div>
<section id="log"><h1>日志</h1><div id="loglist" class="small"></div></section>
<script>
const P = __DATA__;
const G = P.graph, RUNS = P.runs;
const NODES = {}; G.nodes.forEach(n => NODES[n.id] = n);
/* ---------- 布局 v2：BFS 分层 → 行排布；回边不推层；孤立节点单独末行 ---------- */
const layer = {};
const pos = {};
const NODE_W=138, NODE_H=36, GAP=20, LEVEL_Y=112;
const KIND_COLORS={
  label:{f:'#edf0f3',s:'#5f6368'}, tool:{f:'#d8e7fb',s:'#1a73e8'},
  expr:{f:'#fdeccb',s:'#f9ab00'}, goto:{f:'#fbdcd9',s:'#d93025'},
  model:{f:'#e8e0f4',s:'#7620bb'}, route:{f:'#d9f2dc',s:'#188038'},
  run_tools:{f:'#cdeee7',s:'#0d7a6f'}, load_history:{f:'#e2e3fb',s:'#5b6ee1'}
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
  const first = G.nodes[0] && G.nodes[0].id; if(!first) return {w:400,h:200};
  const indeg={}; G.nodes.forEach(n=>indeg[n.id]=0);
  const froms={}; G.edges.forEach(e=>{ (froms[e.from]=froms[e.from]||[]).push(e); if(e.to in indeg) indeg[e.to]++; });
  layer[first]=0; let cur=[first];
  while(cur.length){ const nx=[];
    for(const id of cur) for(const e of (froms[id]||[]))
      if(e.forward && !e.terminal && layer[e.to]===undefined){ layer[e.to]=layer[id]+1; nx.push(e.to); }
    cur=nx; }
  // 孤立节点（无入边、非入口）→ 末尾新行，避免与入口挤
  const isolated=G.nodes.map(n=>n.id).filter(id=>!(indeg[id]>0 || id===first));
  if(isolated.length){ const base=Math.max(0,...Object.values(layer)); isolated.forEach(id=>layer[id]=base+1); }
  G.nodes.forEach(n=>{ if(layer[n.id]===undefined) layer[n.id]=0; });
  const rows={}; G.nodes.forEach(n=>{ const l=layer[n.id]; (rows[l]=rows[l]||[]).push(n.id); });
  let maxRow=0;
  for(const arr of Object.values(rows)){ arr.sort(); maxRow=Math.max(maxRow,arr.length); }
  for(const [l,arr] of Object.entries(rows)) arr.forEach((id,i)=>{ pos[id]={x:64+i*(NODE_W+GAP), y:44+Number(l)*LEVEL_Y}; });
  const maxL=Math.max(0,...Object.values(layer));
  return {w:72+maxRow*(NODE_W+GAP)+NODE_W, h:60+(maxL+1)*LEVEL_Y};
}
/* ---------- SVG 绘制 v2：贝塞尔边（同行微拱/跨行等差）+ 回边弧线 + 类型配色 ---------- */
const svg=document.getElementById('canvas');
const NS='http://www.w3.org/2000/svg';
function el(t){ return document.createElementNS(NS,t); }
function renderSVG(){
  const size=layout();
  svg.setAttribute('width',size.w); svg.setAttribute('height',size.h); svg.setAttribute('viewBox','0 0 '+size.w+' '+size.h);
  // 箭头定义：forward 灰 / goto 红；terminal 无箭头（用端点圆点）
  const defs=el('defs');
  const mk=(id,fill)=>{ const m=el('marker'); m.setAttribute('id',id); m.setAttribute('viewBox','0 0 10 10');
    for(const [k,v] of Object.entries({refX:9,refY:5,markerWidth:6.5,markerHeight:6.5,orient:'auto'})) m.setAttribute(k,v);
    m.innerHTML='<path d="M0,0 L10,5 L0,10 z" fill="'+fill+'"/>'; defs.appendChild(m); };
  mk('arr','#5f6368'); mk('arrg','#d93025'); svg.appendChild(defs);
  G.edges.forEach(e=>{
    const a=pos[e.from]; if(!a) return;
    const x1=a.x+NODE_W, y1=a.y+NODE_H/2;
    const cls=e.forward ? (e.terminal?'e term':'e') : 'e goto';
    let d, ex, ey;
    if(pos[e.to]){
      const b=pos[e.to], x2=b.x, y2=b.y+NODE_H/2;
      ex=x2; ey=y2;
      const cx=(x1+x2)/2;
      const cy = !e.forward ? Math.min(a.y,b.y)-58 : (y1===y2 ? y1-46 : (y1+y2)/2);
      d='M'+x1+','+y1+' Q'+cx+','+cy+' '+x2+','+y2;
    } else {
      // 终端目标（_turn_end/_fail/__end__，图内无节点）：引到节点右侧的端点圆点
      ex=x1+34; ey=y1; const cx=(x1+ex)/2;
      d='M'+x1+','+y1+' Q'+cx+','+y1+' '+ex+','+ey;
      const dot=el('circle'); dot.setAttribute('cx',ex); dot.setAttribute('cy',ey); dot.setAttribute('r',3.5);
      dot.setAttribute('class',(/fail/.test(String(e.to))||e.label==='fail')?'eterm fail':'eterm');
      svg.appendChild(dot);
    }
    const p=el('path'); p.setAttribute('d',d); p.setAttribute('class',cls);
    if(pos[e.to]) p.setAttribute('marker-end', e.forward?'url(#arr)':'url(#arrg)');
    svg.appendChild(p);
    if(e.label && e.label!=='next'){
      const t=el('text'); const tgt=pos[e.to];
      t.setAttribute('x',(x1+(tgt?tgt.x:ex))/2);
      t.setAttribute('y',((tgt?Math.min(a.y,tgt.y):a.y)-16));
      t.setAttribute('text-anchor','middle'); t.setAttribute('class','elabel');
      t.textContent=e.label; svg.appendChild(t);
    }
  });
  G.nodes.forEach(n=>{
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
let curNode='', curGoto='', curIt='';
/* ---------- 日志树：run 轮 → 节点 → 记录（可开合，记录间分割线）---------- */
function renderLog(){
  const box=document.getElementById('loglist');
  const open={}; box.querySelectorAll('details[open]').forEach(d=>{ if(d.dataset.grp) open[d.dataset.grp]=1; });
  box.innerHTML='';
  if(!RUNS.length){ box.textContent='（无会话日志：先跑一轮 wfr <工作流名> 再重新 debug）'; return; }
  let shown=0;
  RUNS.forEach(run=>{
    const buckets={};
    run.records.forEach(rc=>{
      if(curGoto&&curIt){ const v=rc.loop&&rc.loop[curGoto]; if(v===undefined||Number(v)!==curIt) return; }
      const k=rc.node||'（引擎）'; (buckets[k]=buckets[k]||[]).push(rc);
    });
    const rd=document.createElement('details'); rd.className='run'; rd.dataset.grp='r:'+run.run;
    if(open['r:'+run.run]!==undefined ? open['r:'+run.run] : true) rd.open=true;
    const rsum=document.createElement('summary');
    rd.appendChild(rsum);
    let disp=0, nNodes=0;
    Object.keys(buckets).forEach(node=>{
      if(curNode && node!==curNode) return;
      const recs=buckets[node]; nNodes++; disp+=recs.length;
      const nd=document.createElement('details'); nd.className='node'; nd.dataset.grp='n:'+node;
      if(rd.open && (open['n:'+node]!==undefined ? open['n:'+node] : true)) nd.open=true;
      const nsum=document.createElement('summary');
      nsum.textContent=node+'（'+recs.length+' 条）';
      nd.appendChild(nsum);
      recs.forEach(r=>{
        const row=document.createElement('div'); row.className='rec';
        if(curNode && node===curNode) row.classList.add('hit');
        row.dataset.node=r.node||''; row.dataset.seq=String(r.seq!==undefined?r.seq:'');
        row.appendChild(recordRow(r));
        row.addEventListener('click',()=>gotoNode(r));
        nd.appendChild(row);
      });
      rd.appendChild(nd);
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
/* ---------- 联动 ---------- */
function highlightNode(id){ document.querySelectorAll('rect.n').forEach(r=>r.classList.toggle('active',r.dataset.id===id)); }
function pickNode(id){ curNode=(curNode===id?'':id); renderNodeList(); renderLog(); renderDetail(curNode); highlightNode(curNode); }
function cssq(s){ return String(s).replace(/\\/g,'\\\\').replace(/"/g,'\\"'); }
function gotoNode(r){
  curNode=r.node||''; curGoto=''; curIt='';
  renderNodeList(); renderLog(); renderDetail(curNode); highlightNode(curNode);
  if(r.run){ const rd=document.querySelector('details.run[data-grp="r:'+cssq(r.run)+'"]'); if(rd) rd.open=true;
    if(r.node){ const nd=rd.querySelector('details.node[data-grp="n:'+cssq(r.node)+'"]'); if(nd) nd.open=true; } }
  const row=document.querySelector('details.run[data-grp="r:'+cssq(r.run||'')+'"] .rec[data-seq="'+cssq(String(r.seq!==undefined?r.seq:''))+'"][data-node="'+cssq(r.node||'')+'"]');
  if(row) row.scrollIntoView({behavior:'smooth',block:'center'});
}
/* ---------- 节点 JSON 描述 ---------- */
function renderDetail(id){
  const hn=document.getElementById('dnode'), hk=document.getElementById('dkind'), pre=document.getElementById('djson');
  const n=id?NODES[id]:null;
  if(!id||!n){ hn.textContent=id||''; hk.textContent=n?(n.kind||''):'';
    pre.textContent=(id&&!n)?'(引擎合成节点，无原始定义 —— 如 __input__)':'(点击某个节点，其 JSON 定义显示在这里)'; return; }
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
  G.nodes.forEach(n=>{
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
  const gotos=[...new Set(G.edges.filter(e=>!e.forward).map(e=>e.from))];
  if(gotos.length===0){ box.textContent='（无循环 goto）'; return; }
  gotos.forEach(g=>{
    const wrap=document.createElement('div'); wrap.className='gototab';
    const cap=document.createElement('div'); cap.textContent='↺ '+g;
    wrap.appendChild(cap);
    const its=[...new Set(RUNS.flatMap(r=>r.records).map(r=>r.loop&&r.loop[g]).filter(v=>v&&!isNaN(v)))].sort((a,b)=>a-b);
    (its.length?its:[1]).forEach(it=>{
      const b=document.createElement('button'); b.className='loopit'; b.textContent=it;
      if(curGoto===g&&String(curIt)===String(it)) b.classList.add('on');
      b.addEventListener('click',()=>{ curGoto=g; curIt=(curGoto===g&&String(curIt)===String(it))?'':it; renderLog(); renderGotoTabs(); });
      wrap.appendChild(b);
    });
    box.appendChild(wrap);
  });
}
/* ---------- 分栏拖动 + 尺寸持久化（localStorage 尽力而为）---------- */
function loadSizes(){ try{ return JSON.parse(localStorage.getItem('wfr_dbg_size'))||{}; }catch(e){ return {}; } }
function saveSizes(){ try{ localStorage.setItem('wfr_dbg_size',
  JSON.stringify({wnav:document.documentElement.style.getPropertyValue('--wnav'),
                 wlog:document.documentElement.style.getPropertyValue('--wlog'),
                 dh:document.getElementById('detail').style.height})); }catch(e){} }
function bindSplitter(bar, axis, get, set, min, max){
  bar.addEventListener('pointerdown',e=>{
    e.preventDefault();
    document.body.classList.add(axis==='v'?'drag-v':'drag-h');
    const start=(axis==='v'?e.clientX:e.clientY), base=get();
    const move=ev=>{
      let v=base+(axis==='v'?ev.clientX:ev.clientY)-start;
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
  bindSplitter(document.getElementById('sv1'),'v',()=>gv('--wnav'),
    v=>document.documentElement.style.setProperty('--wnav',Math.round(v)+'px'),160,380);
  bindSplitter(document.getElementById('sv2'),'v',()=>gv('--wlog'),
    v=>document.documentElement.style.setProperty('--wlog',Math.round(v)+'px'),300,640);
  bindSplitter(document.getElementById('sh'),'h',()=>dt.offsetHeight,
    v=>dt.style.height=Math.round(v)+'px',96,0);   // h 轴 max 用窗口高度动态 clamp
}
/* ---------- 启动 ---------- */
try {
  layout(); renderSVG(); renderLegend(); renderNodeList(); renderGotoTabs(); renderLog(); renderDetail('');
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