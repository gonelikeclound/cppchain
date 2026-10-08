// cppchain —— src/workflow/validator.cpp
// IR 全量校验。规则编号（实现对照）：
//   V1  顶层三键齐全（parse 层，见 WorkflowIR::parse）；此处补 id 合法
//   V2  nodes 非空、元素为对象（parse 层）
//   V3  id 不得为保留/内部键
//   VT1 trigger.source == "time"（v1 只放行 time）
//   VT2 trigger 必须带 timing.kind == "schedule"（v1 不产 at/interval/calendar）
//   VT3 trigger 里禁止任何 condition 键（业务条件下沉到 expr 节点）
//   VT4 trigger 禁止 filter 键（过滤/STRICT_3V 层 v1 不实现）
//   VT5 schedule 为合法 5 字段 cron（拒绝秒字段/@宏/L/W/#//步进；允许逗号列表与区间）
//   VT6 timezone（若有）非空
//   VT7 trigger 未知键即错；enabled 为 bool
//   VN1 id 匹配 [A-Za-z][A-Za-z0-9_]*（无前导 _）
//   VN2 id 全局唯一，且不在保留名/内部键里
//   VN3 节点类型按 §3 优先级判定（parse 层）
//   VN4 歧义节点即错（parse 层）
//   VN5 label 节点除 id/kind 外不得带任何字段
//   VN6 tool 节点：tool 在目录、parameters 为对象、next 非空
//   VN7 parameters 键 ⊆ 工具规格 properties；规格必填项必须出现
//   VN8 output_schema：对象、键为标识符、值 ∈ {number,string,boolean,*}
//   VN9 expr 节点：expr 可解析、then/else 非空
//   VR1 所有 $/@config 片段语法合法
//   VR2 $<id>.result.f：id 存在且为 tool 节点、下标在上游、字段已声明（或 schema 为 *）
//   VR3 $_.result.f：字段被 ≥1 个上游 tool 声明（或 *）
//   VR4 @config.k：键为标识符（parse 层保证）；若宿主声明了 config 契约，键必须在其列
//   VN10 M12 节点：model 组件名/tools 工具集名必须在注册表；route 的 on/cases 键走封闭词表，
//        且 cases 必须覆盖全部 outcome 或显式给 default（禁止"漏一档静默走 else"）
//   VN11 M12 history 节点：path/next 必填
//   VN13 wfr-㉙ model 节点：params 必须是对象且不含请求体保留键（extra 合并是右胜）；
//        reasoning_echo 为字面量时 ∈ {drop,tool_calls,always}（整串 @config 引用留给运行期）
//   VN14 wfr-㉙ load_history 节点：mode 为字面量时 ∈ {full,drop_round,keep_evidence}；
//        budget 为字面量时须为正整数（引用留给运行期，缺省→full/200、非法→硬错）
//   VG9  load_history 必须在消费 messages 的节点之前（Q8：硬拒绝，读了没人用就是错）
//   VH1 on_error：retry ≥ 0；fallback 为存在节点 id 或 _turn_end
//   VG1 指针目标：next/then/else 为存在节点或 _turn_end；goto 必须指向 label
//   VG2 前向边（tool.next / expr.then/else 替换 label 后）无环（循环只能经 goto）
//   VG3 每个 label 有续接节点（其"后第一个非 label 节点"）
//   VG6 入口 = nodes[0]；为 label 时取它的续接
//   VG7 除入口外每个节点可达（前向边），或作为某可达节点 on_error.fallback 的目标
//   VG8 每个 label 至少被一个 goto / next / then / else 指向
#include "cppchain/workflow/validator.hpp"

#include "cppchain/workflow/ir_walk.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/node_type.hpp"

namespace cppchain::workflow {

namespace {

// 规范 §3 里的终止目标 id（编译期会映射到 graph::kEnd）。validator 不依赖 graph 层。
inline constexpr char kTurnEnd[] = "_turn_end";
inline constexpr char kEndId[] = "__end__";

// on_error.retry 的上界。retry 是在**一次节点调用内**同步循环的（compiler.cpp），
// graph 的 max_steps 只数 super-step、完全管不到这里 —— 没有上界就等于能把进程挂死。
inline constexpr int kMaxRetry = 10;

// ---- 小工具 ----

bool is_ident(std::string_view s) {
  if (s.empty()) return false;
  if (!std::isalpha(static_cast<unsigned char>(s.front()))) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}


bool is_internal_key(std::string_view id) {
  return id == kResultsKey || id == kLastExecKey || id == kLoopCountsKey || id == kConfigKey;
}

// ⑤：框架对节点的**统一按名分发**（内置或导入的动态节点都走这里）。
// 所有"要调这个节点的 validate/edges/ref_texts"的地方用它，别再用 node_type_of(kind)。
NodeType const* node_type_of_ir(NodeIR const& n) { return node_type_of_name(type_name_of(n)); }

// ---- cron（VT5）----

// 数值 token："*" | N | N-M（都在范围内）；不带 "/"（步进拒绝）。
bool cron_token(std::string_view tok, int lo, int hi) {
  if (tok == "*") return true;
  if (tok.empty()) return false;
  std::string_view cur = tok;
  if (auto dash = cur.find('-'); dash != std::string_view::npos) {
    auto a = cur.substr(0, dash);
    auto b = cur.substr(dash + 1);
    auto num = [](std::string_view s) -> std::optional<long> {
      if (s.empty()) return std::nullopt;
      long v = 0;
      for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        v = v * 10 + (c - '0');
      }
      return v;
    };
    auto na = num(a);
    auto nb = num(b);
    if (!na || !nb) return false;
    return *na >= lo && *na <= hi && *nb >= lo && *nb <= hi && *na <= *nb;
  }
  long v = 0;
  for (char c : tok) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    v = v * 10 + (c - '0');
  }
  return v >= lo && v <= hi;
}

bool valid_cron(std::string const& schedule) {
  // 按空白切分为字段
  std::vector<std::string> fields;
  std::size_t i = 0;
  while (i < schedule.size()) {
    while (i < schedule.size() && std::isspace(static_cast<unsigned char>(schedule[i]))) ++i;
    std::size_t start = i;
    while (i < schedule.size() && !std::isspace(static_cast<unsigned char>(schedule[i]))) ++i;
    if (i > start) fields.push_back(schedule.substr(start, i - start));
  }
  if (fields.size() != 5) return false;
  constexpr int kRange[5][2] = {{0, 59}, {0, 23}, {1, 31}, {1, 12}, {0, 7}};
  for (int f = 0; f < 5; ++f) {
    // 逗号列表；每项必须是合法 token
    std::size_t pos = 0;
    std::string_view sf = fields[f];
    while (true) {
      std::size_t comma = sf.find(',', pos);
      std::string_view tok = sf.substr(pos, comma == std::string_view::npos
                                                 ? sf.size() - pos
                                                 : comma - pos);
      if (!cron_token(tok, kRange[f][0], kRange[f][1])) return false;
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }
  }
  return true;
}

// 检查 trigger.raw 是否含某个 top-level 键
bool has_top_key(Json const& obj, std::string_view key) {
  return obj.is_object() && obj.contains(std::string(key));
}

// 递归找 trigger 里任何名为 key 的键（VT3 的 condition）
bool has_key_anywhere(Json const& j, std::string_view key) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == key) return true;
      if (has_key_anywhere(it.value(), key)) return true;
    }
  } else if (j.is_array()) {
    for (Json const& e : j)
      if (has_key_anywhere(e, key)) return true;
  }
  return false;
}

// ---- 内部上下文 ----

struct Ctx {
  WorkflowIR const& ir;
  // 前序扁平遍历（含 loop body）。第二项是该节点所在的循环作用域，nullptr = 顶层。
  std::vector<std::pair<NodeIR const*, LoopScope const*>> flat;
  std::vector<std::unique_ptr<LoopScope>> scope_store;  // flat 里指针的稳定落点
  std::unordered_map<std::string, std::size_t> index;   // id → 前序扁平序号
  std::unordered_map<std::string, NodeIR const*> by_id; // id → 节点
  std::unordered_map<std::string, NodeKind> kind;       // id → 节点类型
  std::unordered_map<std::string, std::string> owner;   // id → 所属 loop id（顶层不在表里）
  std::unordered_map<std::string, LoopScope const*> scope_of;  // id → 作用域
  ValidationResult out;

  explicit Ctx(WorkflowIR const& w) : ir(w) {}

  void err(std::string rule, std::string message) {
    out.errors.push_back({std::move(rule), std::move(message)});
  }
};

// 建索引：前序遍历一次填平 flat / index / by_id / kind / owner。
void build_index(Ctx& c) {
  c.index = flatten_index(c.ir);
  for_each_node(c.ir, [&c](NodeIR const& n, LoopScope const* scope) {
    LoopScope const* stable = nullptr;
    if (scope != nullptr) {
      c.scope_store.push_back(std::make_unique<LoopScope>(*scope));
      stable = c.scope_store.back().get();
    }
    c.flat.push_back({&n, stable});
    c.by_id[n.id] = &n;
    c.kind[n.id] = n.kind;
    if (scope != nullptr) {
      c.owner[n.id] = scope->loop_id;
      c.scope_of[n.id] = stable;
    }
    return true;
  });
}

// 指针目标替换：_turn_end → 终止 id；loop 保留名 → 节点头/出口；其余原样（id 全局唯一，
// 不再需要 label 那种"续接消解"）。
// scope 非空表示"这个节点在某个 loop 的 body 里"。
std::string resolve_target(Ctx const& c, std::string const& t, LoopScope const* scope = nullptr) {
  if (t == kTurnEnd) return kEndId;
  if (t == kContinueName) return scope != nullptr ? scope->loop_id : t;  // 回边 → loop 节点头
  if (t == kBreakName)
    return scope != nullptr ? resolve_target(c, scope->exit_target, nullptr) : t;
  return t;
}

void find_refs_in_node(std::vector<std::pair<std::string, Ref>>& out, NodeIR const& n) {
  // 引用收集与 VR1 的语法检查走**同一个钩子**（NodeType::ref_texts）——
  // 原先两处各自按 kind 走、都只覆盖 Tool/Expr，history 节点的 path/session 两边都漏。
  NodeType const* t = node_type_of_ir(n);
  if (t == nullptr || !t->ref_texts) return;
  for (std::string const& text : t->ref_texts(n))
    for (RefScan const& sc : scan_refs(text))
      if (sc.ok) out.emplace_back(n.id, sc.ref);
}

bool schema_declares(Json const& schema, std::string const& field) {
  if (!schema.is_object()) return false;
  if (schema.contains("*")) return true;
  return schema.contains(field);
}

}  // namespace

std::string ValidationResult::describe() const {
  std::string s;
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i) s += "；";
    s += "[" + errors[i].rule + "] " + errors[i].message;
  }
  return s;
}

ValidationResult validate(WorkflowIR const& ir, ValidatorOptions opts) {
  Ctx c{ir};

  // ---- V：id 与顶层的合法键 ----
  if (ir.id.empty()) c.err("V1", "workflow id 为空");
  if (ir.id == "result" || is_internal_key(ir.id))
    c.err("V3", "workflow id '" + ir.id + "' 是保留名/内部状态键");

  // ---- 建索引（前序扁平，含 loop body）----
  build_index(c);

  // ---- VT：trigger（可选；省略 = 宿主手工触发，wfr 直接拉起）----
  if (ir.trigger) {
    TriggerIR const& tr = *ir.trigger;
    if (tr.source != "time")
      c.err("VT1", "trigger.source 必须是 'time'（当前实现只放行 time 触发；实际是 '" + tr.source +
                       "'）");
    if (tr.kind != "schedule")
      c.err("VT2", "trigger 必须是 timing.kind == 'schedule'（当前实现只放行 schedule；实际是 '" +
                       tr.kind + "'）");
    if (has_key_anywhere(tr.raw, "condition"))
      c.err("VT3", "trigger 不允许带 condition —— 业务条件必须下沉到 expr 节点");
    if (has_top_key(tr.raw, "filter"))
      c.err("VT4", "trigger 不允许带 filter（过滤/STRICT_3V 层未实现）—— 匹配条件必须用 expr 节点");
    if (!valid_cron(tr.schedule))
      c.err("VT5", "schedule 必须是合法 5 字段 cron（分 时 日 月 周；拒绝秒字段/@宏/L/W/#/步进）/，收到: '" +
                       tr.schedule + "'");
    if (tr.timezone && tr.timezone->empty())
      c.err("VT6", "timezone 若给出必须非空");
    // VT7：trigger 未知键
    {
      Json const& t = tr.raw;
      const std::string known[] = {"source", "timing", "kind", "args", "enabled"};
      if (t.is_object())
        for (auto it = t.begin(); it != t.end(); ++it) {
          bool ok = false;
          for (std::string const& k : known)
            if (it.key() == k) {
              ok = true;
              break;
            }
          if (!ok)
            c.err("VT7", "trigger 含未知键 '" + it.key() + "'（当前实现只支持 source/timing 简写形）");
        }
      if (t.contains("enabled") && !t["enabled"].is_boolean())
        c.err("VT7", "trigger.enabled 必须是布尔");
    }
  }

  // ---- VN：节点（含 loop body 里的节点）----
  for (auto const& [np, scope] : c.flat) {
    NodeIR const& n = *np;
    (void)scope;

    if (!is_ident(n.id))
      c.err("VN1", "节点 id '" + n.id + "' 不符合 [A-Za-z][A-Za-z0-9_]*（无前导 _）");
    if (n.id == "result" || is_internal_key(n.id))
      c.err("VN2", "节点 id '" + n.id + "' 是保留名/内部状态键");
    if (n.id == "_turn_start" || n.id == "_turn_end")
      c.err("VN2", "节点 id 不得用保留名 '" + n.id + "'");

    // 类型专属校验走注册表（VN5–VN11）。通用图规则不在这里 —— 见下方 VG 段（② 会收敛到 edges()）。
    NodeType const* type = node_type_of_ir(n);
    if (type != nullptr && type->validate) {
      NodeType::ValidateContext const vctx{opts.registry, opts.components};
      NodeType::ErrorSink const sink = [&c](std::string rule, std::string message) {
        c.err(std::move(rule), std::move(message));
      };
      type->validate(n, vctx, sink);
    }
  }

  // ---- VI：imports 形状（⑤ 动态库导入）----
  // 真正的"库存在 / 符号取得到 / 版本匹配 / 枚举"由 load_workflow_imports 在 parse 前做
  // （fail-closed）；这里兜底程序化构造的 IR（不经 parse 直接 validate 的）。
  // nodes 可省略（A1 枚举整库，⑤b）；列了就必须是标识符。
  for (std::size_t i = 0; i < ir.imports.size(); ++i) {
    ImportIR const& im = ir.imports[i];
    std::string const where = "imports[" + std::to_string(i) + "]";
    if (im.lib.empty()) c.err("VI1", where + " 缺 lib（库是信任单元，库名必须显式）");
    for (std::string const& nm : im.nodes)
      if (!is_ident(nm))
        c.err("VI1", where + " 的节点名 '" + nm + "' 非法（应为 [A-Za-z][A-Za-z0-9_]*）");
  }

  // ---- 唯一性 ----
  {
    std::vector<std::string> ids;
    ids.reserve(c.flat.size());
    for (auto const& [np, _s] : c.flat) ids.push_back(np->id);
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 1; i < ids.size(); ++i)
      if (ids[i] == ids[i - 1])
        c.err("VN2", "节点 id '" + ids[i] + "' 重复（全图唯一，含 loop body 内的节点）");
  }

  // ---- VH：on_error ----
  for (auto const& [np, _s] : c.flat) {
    NodeIR const& n = *np;
    if (n.on_error) {
      // retry 必须在 [0, kMaxRetry] 内。判定回读 raw 的**原始数值**，而不是解析结果：
      // ir.cpp 用 get<int>() 取值，超出 int 范围的数字会被静默截断
      // （3000000000 → 负数；超 64 位 → 0），只看解析结果会漏判。
      bool retry_bad = n.on_error->retry < 0;
      Json const* oe = nullptr;
      if (auto it = n.raw.find("on_error"); it != n.raw.end() && it->is_object()) oe = &(*it);
      if (oe != nullptr) {
        if (auto it = oe->find("retry"); it != oe->end()) {
          Json const& rv = *it;
          // 必须是整数（浮点/字符串一律不收）；上限很小，用 double 判值域足够。
          retry_bad = !(rv.is_number_integer() || rv.is_number_unsigned()) ||
                      rv.get<double>() < 0 || rv.get<double>() > kMaxRetry;
        }
      }
      if (retry_bad)
        c.err("VH1", "tool 节点 '" + n.id + "' 的 on_error.retry 必须是 0–" +
                         std::to_string(kMaxRetry) + " 的整数");
      if (n.on_error->fallback) {
        std::string const& t = *n.on_error->fallback;
        bool const exists = t == kTurnEnd || c.index.find(t) != c.index.end();
        if (!exists)
          c.err("VH1", "tool 节点 '" + n.id + "' 的 on_error.fallback 指向不存在的节点 '" + t +
                           "'（应为节点 id 或 _turn_end）");
      }
    }
  }

  // ---- VG：指针、环、可达性 ----

  // 以下四条通用图规则**全部由 NodeType::edges() 驱动** —— 加一种节点不必再改这里。
  // （见 02-arch-design/12 §4.2：原先它们按 kind 各写一遍，是 28 处 if 链的来源。）

  // VG1：每条边的目标必须存在（或为保留名）
  for (auto const& [np, scope] : c.flat) {
    NodeIR const& n = *np;
    NodeType const* t = node_type_of_ir(n);
    if (t == nullptr || !t->edges) continue;
    for (NodeType::Edge const& e : t->edges(n)) {
      if (e.target.empty()) continue;  // 缺字段由各类型的 validate 报（VN*）
      // VG10：loop 保留名只能出现在循环体里；body 里的指针也不许指向 body 外的普通节点
      //（那多半是漏写 `_break` 造成的意外逃逸）。
      if (e.target == kContinueName || e.target == kBreakName) {
        if (scope == nullptr)
          c.err("VG10", "节点 '" + n.id + "' 的 " + e.label + " 指向 " + e.target +
                            "，但它不在任何 loop 的 body 里");
        continue;
      }
      if (scope != nullptr && c.by_id.count(e.target) != 0 && c.owner.count(e.target) == 0)
        c.err("VG10", "loop '" + scope->loop_id + "' 的 body 节点 '" + n.id + "' 的 " + e.label +
                          " 指向 body 外的节点 '" + e.target +
                          "'（要跳出循环请用 _break，要下一轮请用 _continue）");
      if (e.target == "_turn_start") {
        c.err("VG1", "节点 '" + n.id + "' 的 " + e.label + " 指向保留名 _turn_start（不允许）");
      } else if (e.target != kTurnEnd && e.target != kFailName &&
                 c.index.find(e.target) == c.index.end()) {
        c.err("VG1", "节点 '" + n.id + "' 的 " + e.label + " 指向不存在的节点 '" + e.target + "'");
      }
    }
  }

  // ---- VG9：load_history 必须在消费 messages 的节点之前 ----
  // 顺序用**数组下标**判定（与 VR2 的"上游/更早"同一套约定）。读了没人用就是编排错误，
  // 硬拒绝比告警好：告警会被忽略，而这条错会让"历史明明加载了却没进上下文"变成谜案。
  {
    // 按作用域判：顶层 + 每个 loop body 各算一处。作用域里"有消费者但没有 load_history"
    // **不报** —— 历史可以在祖先作用域加载（chat 就是 recall 在 loop 之前）。
    auto first_of_in = [&](LoopScope const* want, bool (*pred)(NodeIR const&))
        -> std::optional<std::size_t> {
      std::optional<std::size_t> best;
      for (auto const& [np, sc] : c.flat) {
        if (sc != want) continue;
        if (!pred(*np)) continue;
        auto const at = c.index.at(np->id);
        if (!best || at < *best) best = at;
      }
      return best;
    };
    std::vector<LoopScope const*> scopes{nullptr};
    for (auto const& [np, sc] : c.flat) {
      (void)np;
      if (sc != nullptr && std::find(scopes.begin(), scopes.end(), sc) == scopes.end())
        scopes.push_back(sc);
    }
    for (LoopScope const* sc : scopes) {
      auto const load_i =
          first_of_in(sc, [](NodeIR const& n) { return n.kind == NodeKind::LoadHistory; });
      auto const use_i = first_of_in(sc, [](NodeIR const& n) {
        return n.kind == NodeKind::Model || n.kind == NodeKind::RunTools;
      });
      if (load_i && use_i && *load_i > *use_i) {
        std::string const where = sc == nullptr ? std::string("顶层") : ("loop '" + sc->loop_id + "'");
        c.err("VG9", where + "：load_history 出现在消费 messages 的节点之后：历史加载了也没人用");
      }
    }
  }

  // ---- VG2：前向边无环（label 已替换） ----
  {
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (auto const& [np, scope] : c.flat) {
      NodeIR const& n = *np;
      NodeType const* t = node_type_of_ir(n);
      if (t == nullptr || !t->edges) continue;
      for (NodeType::Edge const& e : t->edges(n)) {
        if (!e.forward) continue;  // 回边（_continue / goto）不参与前向环判定
        std::string const to = resolve_target(c, e.target, scope);
        if (to != kEndId) adj[n.id].push_back(to);
      }
    }
    // DFS 三色：0 未访问 / 1 在栈 / 2 完成
    std::unordered_map<std::string, int> color;
    bool cycle = false;
    std::string cycle_at;
    std::function<void(std::string const&)> dfs = [&](std::string const& v) {
      color[v] = 1;
      for (std::string const& w : adj[v]) {
        auto it = color.find(w);
        if (it == color.end())
          dfs(w);
        else if (it->second == 1 && !cycle) {
          cycle = true;
          cycle_at = v + "→" + w;
        }
      }
      color[v] = 2;
    };
    for (auto const& [v, _] : adj)
      if (!color.count(v)) dfs(v);
    if (cycle)
      c.err("VG2", "前向边出现环（" + cycle_at + "）：循环只能用 loop 节点表达，"
                   "禁止 expr/tool 自环或绕路");
  }

  // ---- VG7：可达性（前向边） + 入口 ----
  {
    std::string const entry = ir.nodes.empty() ? std::string{} : ir.nodes[0].id;

    if (ir.nodes.empty()) c.err("V2", "nodes 不能为空");
    if (entry.empty() || c.index.find(entry) == c.index.end())
      c.err("VG6", "入口无法解析（nodes 为空或 nodes[0] 不在索引里）");

    // 前向可达集：从入口出发，沿 next/then/else **以及 on_error.fallback** 展开到不动点。
    //
    // 早先的实现分两段走：先只沿前向边做 DFS，再把"可达节点声明的 fallback 目标"补进来，
    // 但补进来的目标**不展开其后继** —— 于是"只能经 fallback 到达"的下游会被误判成死节点，
    // 合法 IR 被拒（逼生成器空转修复轮）。统一成一个 worklist 就没有这个缝。
    // 另外 fallback 目标同样要过 resolve_target：它可能是一个 label。
    std::unordered_map<std::string, bool> seen;
    std::vector<std::string> stack;
    auto push = [&](std::string const& t) {
      if (t != kEndId && !seen[t]) {
        seen[t] = true;
        stack.push_back(t);
      }
    };
    if (!entry.empty() && c.index.count(entry)) push(entry);

    while (!stack.empty()) {
      std::string const v = stack.back();
      stack.pop_back();
      auto it = c.kind.find(v);
      if (it == c.kind.end()) continue;
      NodeIR const& node = *c.by_id.at(v);
      NodeType const* t = node_type_of_ir(node);
      if (t == nullptr || !t->edges) continue;
      auto const sit = c.scope_of.find(v);
      LoopScope const* const sc = sit == c.scope_of.end() ? nullptr : sit->second;
      for (NodeType::Edge const& e : t->edges(node)) {
        if (!e.forward) continue;  // 回边不参与"从入口可达"的展开
        push(resolve_target(c, e.target, sc));
      }
    }
    // 死节点检查（含 loop body 内的节点）
    for (auto const& [np, _s] : c.flat)
      if (!seen[np->id])
        c.err("VG7", "节点 '" + np->id +
                         "' 从入口不可达且不是任何可达节点的 on_error.fallback 目标（死节点）");
  }

  // ---- VR：引用 ----
  // VR1：本节点所有"可能含引用"的文本片段里，$/@config 片段必须语法合法。
  // 走 NodeType::ref_texts —— 与下面的引用收集**同一个钩子**，不会再出现某类节点漏检。
  for (auto const& [np, _s] : c.flat) {
    NodeIR const& n = *np;
    NodeType const* t = node_type_of_ir(n);
    if (t == nullptr || !t->ref_texts) continue;
    for (std::string const& text : t->ref_texts(n))
      if (auto p = ref_syntax_problem(text))
        c.err("VR1", "节点 '" + n.id + "' 的引用语法错误：" + *p);
  }
  for (auto const& [np, _s] : c.flat) {
    NodeIR const& n = *np;
    std::vector<std::pair<std::string, Ref>> refs;
    find_refs_in_node(refs, n);
    for (auto const& [_, r] : refs) {
      switch (r.kind) {
        case RefKind::Config: {
          // VR4：parse 已保证键是标识符；这里再按**宿主声明的 config 契约**核对（B2）。
          // 未声明契约（nullopt）→ 跳过，保持旧行为。
          if (opts.config_keys) {
            auto const& keys = *opts.config_keys;
            if (std::find(keys.begin(), keys.end(), r.field) == keys.end()) {
              std::string avail;
              for (std::string const& k : keys) {
                if (!avail.empty()) avail += ", ";
                avail += k;
              }
              c.err("VR4", "节点 '" + n.id + "' 引用了 @config." + r.field +
                               "，但宿主未声明该配置键（可用：" +
                               (avail.empty() ? "（无）" : avail) + "）");
            }
          }
          break;
        }
        case RefKind::NamedResult: {
          auto it = c.index.find(r.node);
          if (it == c.index.end())
            c.err("VR2", "节点 '" + n.id + "' 引用了不存在的节点 '" + r.node + "'");
          else {
            NodeIR const& target = *c.by_id.at(r.node);
            // 可引用 result 的节点：tool（output_schema 声明字段）+ model / run_tools
            //（wfr-㉗：模型/工具阶段发布 thinking/文本/调用摘要，字段运行时定义）
            // + load_history（wfr-㉙：发布历史压缩统计，供 echo 打印"修改过程"）。
            bool const result_ok = target.kind == NodeKind::Tool ||
                                   target.kind == NodeKind::Model ||
                                   target.kind == NodeKind::RunTools ||
                                   target.kind == NodeKind::LoadHistory ||
                                   target.kind == NodeKind::Route;
            if (!result_ok)
              c.err("VR2", "节点 '" + n.id + "' 的 $'" + r.node +
                               "' 指向非 result 节点（仅 tool / model / run_tools / "
                               "load_history / route 发布 result）");
            else if (it->second >= c.index.at(n.id))
              c.err("VR2", "节点 '" + n.id + "' 引用了下游节点 '" + r.node +
                               "'（只能引用上游/更早的节点）");
            else if (target.kind == NodeKind::Tool &&
                     !schema_declares(target.output_schema, r.field))
              c.err("VR2", "节点 '" + n.id + "' 引用了 '" + r.node + ".result." + r.field +
                               "'，但该节点 output_schema 未声明此字段");
          }
          break;
        }
        case RefKind::LastResult: {
          // 前序扁平序：$_ 认"更早出现过的任意 tool"（含 loop body 里的）
          bool declared = false;
          std::size_t const my = c.index.at(n.id);
          for (auto const& [mp, _s] : c.flat) {
            if (c.index.at(mp->id) >= my) continue;
            if (mp->kind == NodeKind::Tool && schema_declares(mp->output_schema, r.field)) {
              declared = true;
              break;
            }
          }
          if (!declared)
            c.err("VR3", "节点 '" + n.id + "' 的 $_" + ".result." + r.field +
                             " 未被任何上游 tool 的 output_schema 声明");
          break;
        }
      }
    }
  }

  // ---- 返回 ----
  // 去重相邻的重复错误（同一规则同一节点可能重复报）
  {
    std::vector<ValidationError> uniq;
    std::vector<std::string> seen;
    for (ValidationError const& e : c.out.errors) {
      std::string const key = e.rule + "\x1f" + e.message;
      if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        uniq.push_back(e);
      }
    }
    c.out.errors = std::move(uniq);
  }
  return c.out;
}

ValidationResult validate_json(Json const& raw, ValidatorOptions opts) {
  auto ir = WorkflowIR::parse(raw);
  if (!ir)
    return ValidationResult{{{"V1", "IR 无法解析：" + ir.error()}}};
  return validate(*ir, opts);
}

}  // namespace cppchain::workflow