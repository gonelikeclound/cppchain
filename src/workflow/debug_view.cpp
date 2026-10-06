// cppchain —— src/workflow/debug_view.cpp
// wfr 调试台的导出与渲染（见头注释）。HTML 为自包含单文件：分层 SVG + 双向联动 + 循环分组。
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
    nodes.push_back(Json{{"id", n.id}, {"kind", kind}, {"text", text}});
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

std::string render_debug_html(Json const& payload) {
  static constexpr char kHtml[] = R"html(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>wfr debug — __IR_ID__</title>
<style>
:root{--hl:#e8f0fe;--acc:#1a73e8;--g:780px}
body{font:13px system-ui,sans-serif;margin:0;display:grid;grid-template-columns:220px 1fr 360px;height:100vh}
aside{border-right:1px solid #ddd;padding:10px;background:#fafafa;overflow:auto}
main{padding:8px;overflow:auto}
#log{border-left:1px solid #ddd;padding:10px;overflow:auto;background:#fff}
h1{font-size:15px;margin:.3em 0} h2{font-size:12px;color:#666;margin:.8em 0 .3em}
select{width:100%;margin:.2em 0}
svg text{font:12px system-ui}
.n{border:1px solid #999;fill:#fff;stroke:#666;rx:6;cursor:pointer}
.n .t{stroke-width:0}.n .lbl{font:11px monospace}
.n.active{stroke:var(--acc);stroke-width:2.5;filter:drop-shadow(0 0 3px #1a73e855)}
.n.hit{stroke:#d93025;stroke-width:2.5}
path.e{fill:none;stroke:#666;stroke-width:1.3} path.e.goto{fill:none;stroke:#d93025;stroke-width:1.8} path.e.term{fill:none;stroke:#aaa}
.elabel{font:10px system-ui;fill:#999;pointer-events:none}
.nodeitem{padding:2px 4px;cursor:pointer;border-radius:3px;white-space:nowrap;overflow:hidden}
.nodeitem:hover,.nodeitem.on{background:var(--hl)}
.gototab{margin-top:4px}
.loopit{padding:2px 6px;border:1px solid #ccc;border-radius:4px;margin:2px 2px 2px 0;display:inline-block;cursor:pointer;background:#fff}
.loopit.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.lrow{font:12px monospace;padding:2px 4px;border-radius:3px;cursor:pointer;white-space:pre-wrap;word-break:break-all}
.lrow:hover{background:var(--hl)} .lrow.hit{background:#fdecea}
.phase{display:inline-block;width:52px;color:#666}
.s{color:#1a73e8}.e2{color:#d93025}.iv{color:#b06000}
.small{color:#888;font:11px}
</style></head><body>
<aside>
<h1>__IR_ID__</h1>
<div id="trim" class="small" style="display:none">⚠ 记录过多，已裁掉节点诊断</div>
<h2>运行（run）</h2><select id="runSel"></select>
<h2>节点</h2><div id="nodelist"></div>
<h2>循环分组</h2><div id="gototabs"></div>
</aside>
<main><svg id="canvas" width="700" height="400"></svg></main>
<section id="log"><h1>日志</h1><div id="loglist" class="small">未选中 run → 请在下拉选择</div></section>
<script>
const P = __DATA__;
const G = P.graph, RUNS = P.runs;
/* ---------- 布局 v2：BFS 分层 → 行排布；回边不推层；孤立节点单独末行 ---------- */
const layer = {};
const pos = {};
const NODES = {}; G.nodes.forEach(n => NODES[n.id] = n);
const NODE_W=138, NODE_H=48, GAP=20, LEVEL_Y=130;
function kindColor(k){
  if(k==='label') return '#f4f4f4';
  if(k==='tool') return '#e7f4ff';
  if(k==='expr') return '#fff5e0';
  if(k==='goto') return '#ffecec';
  if(k==='model') return '#eef0ff';
  if(k==='route') return '#ecf9ec';
  if(k==='run_tools') return '#f3edff';
  return '#f7f7f7';
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
  svg.innerHTML='';
  svg.setAttribute('width',size.w); svg.setAttribute('height',size.h); svg.setAttribute('viewBox','0 0 '+size.w+' '+size.h);
  const arrow=el('marker'); arrow.id='arr'; arrow.setAttribute('viewBox','0 0 10 10');
  for(const [k,v] of Object.entries({refX:9,refY:5,markerWidth:7,markerHeight:7,orient:'auto-start-reverse'})) arrow.setAttribute(k,v);
  arrow.innerHTML='<path d="M0,0 L10,5 L0,10 z" fill="#666"/>';
  svg.appendChild(arrow);
  G.edges.forEach(e=>{
    const a=pos[e.from], b=pos[e.to]; if(!a||!b) return;
    const x1=a.x+NODE_W, y1=a.y+NODE_H/2, x2=b.x, y2=b.y+NODE_H/2;
    const cx=(x1+x2)/2;
    const cy = !e.forward ? Math.min(a.y,b.y)-54
          : (y1===y2 ? y1-40 : (y1+y2)/2);
    const d='M'+x1+','+y1+' Q'+cx+','+cy+' '+x2+','+y2;
    const p=el('path'); p.setAttribute('d',d);
    p.setAttribute('class', e.forward ? (e.terminal?'e term':'e') : 'e goto');
    p.setAttribute('marker-end','url(#arr)');
    svg.appendChild(p);
    if(e.label && e.label!=='next'){                 // 边标签（else / goto …）
      const t=el('text'); t.setAttribute('x',cx); t.setAttribute('y',cy-8);
      t.setAttribute('text-anchor','middle'); t.setAttribute('class','elabel');
      t.textContent=e.label; svg.appendChild(t);
    }
  });
  G.nodes.forEach(n=>{
    const p=pos[n.id];
    const g=el('g'); g.setAttribute('transform','translate('+p.x+','+p.y+')');
    const rec=el('rect'); rec.setAttribute('width',NODE_W); rec.setAttribute('height',NODE_H);
    rec.setAttribute('rx',7); rec.setAttribute('style','fill:'+kindColor(n.kind));
    rec.setAttribute('class','n'); rec.setAttribute('data-id',n.id);
    const t1=el('text'); t1.setAttribute('x',10); t1.setAttribute('y',18); t1.setAttribute('class','lbl');
    t1.textContent=n.id;
    const t2=el('text'); t2.setAttribute('x',10); t2.setAttribute('y',38);
    t2.textContent=((n.kind||'')+' '+(NODES[n.id].text||'')).slice(0,26);
    g.appendChild(rec); g.appendChild(t1); g.appendChild(t2);
    g.addEventListener('click',()=>pickNode(n.id));
    svg.appendChild(g);
  });
}
/* ---------- 日志 ---------- */
let curRun=-1, curNode='', curGoto='', curIt='';
function logsOf(){ return curRun>=0 ? RUNS[curRun].records : []; }
function filtered(){
  let R=logsOf();
  if(curNode) R=R.filter(r=>r.node===curNode);
  if(curGoto&&curIt) R=R.filter(r=>{const v=r.loop&&r.loop[curGoto]; return v===Number(curIt);});
  return R;
}
function renderLog(){
  const box=document.getElementById('loglist');
  const R=filtered();
  if(curRun<0){ box.textContent='↓ 下拉选择一个 run 查看日志'; return; }
  if(R.length===0){ box.textContent='（无记录）'; return; }
  box.innerHTML='';
  R.forEach((r,i)=>{
    const d=document.createElement('div'); d.className='lrow';
    if(r.node===curNode) d.classList.add('hit');
    const ph=(r.phase||'')+(r.status||'');
    d.dataset.node=r.node||'';
    const up=('update' in r)?JSON.stringify(r.update).slice(0,120):'';
    const msg=(r.msg?(' ' + r.msg):'');
    d.innerHTML=`<span class="phase ${r.source==='node'?'s':r.status==='error'||r.source==='node'?'e2':''}">${ph}</span>`+
      `<b>${(r.node||'').padEnd(16)}</b>${r.level?' ['+r.level+']':''}${up}${msg.replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]))}`;
    d.addEventListener('click',()=>gotoNode(r));
    box.appendChild(d);
  });
  document.getElementById('loglist');
}
/* ---------- 联动 ---------- */
function highlightNode(id){ document.querySelectorAll('rect.n').forEach(r=>r.classList.toggle('active',r.dataset.id===id)); }
function pickNode(id){ curNode=(curNode===id?'':id); renderNodeList(); renderLog(); highlightNode(curNode); }
function gotoNode(r){
  // 点日志 → 定位节点：自动切到该 run 并高亮
  if(r.run&&!curRun&&RUNS[curRun]){}
  RUNS.forEach((x,i)=>{ if(x.run===r.run) curRun=i; });
  curNode=''; curGoto=''; curIt='';
  renderNodeList(); renderRunSel(); renderLog();
  if(r.node) highlightNode(r.node);
  const rect=document.querySelector(`rect.n[data-id="${r.node}"]`);
  if(rect&&rect.parentElement){ rect.parentElement.scrollIntoView({behavior:'smooth',block:'center'}); }
}
function renderRunSel(){
  const sel=document.getElementById('runSel');
  sel.innerHTML='';
  RUNS.forEach((r,i)=>{ const o=document.createElement('option');
    o.value=i; o.textContent=`${r.run}（${r.records.length} 条）`;
    if(i===curRun) o.selected=true; sel.appendChild(o); });
}
function renderNodeList(){
  const box=document.getElementById('nodelist'); box.innerHTML='';
  G.nodes.forEach(n=>{ const d=document.createElement('div'); d.className='nodeitem';
    if(n.id===curNode) d.classList.add('on');
    d.textContent=`${n.id}（${n.kind}）`;
    d.addEventListener('click',()=>pickNode(n.id)); box.appendChild(d); });
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
/* ---------- 启动 ---------- */
const sel=document.getElementById('runSel');
sel.addEventListener('change',()=>{ curRun=Number(sel.value); curNode='';curGoto='';curIt=''; renderLog(); renderNodeList(); renderGotoTabs(); });
try {
  layout(); renderSVG(); renderRunSel(); renderNodeList(); renderGotoTabs(); renderLog();
  if(P.trimmed_node_logs) document.getElementById('trim').style.display='block';
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