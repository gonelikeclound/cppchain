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
//   VH1 on_error：retry ≥ 0；fallback 为存在节点 id 或 _turn_end
//   VG1 指针目标：next/then/else 为存在节点或 _turn_end；goto 必须指向 label
//   VG2 前向边（tool.next / expr.then/else 替换 label 后）无环（循环只能经 goto）
//   VG3 每个 label 有续接节点（其"后第一个非 label 节点"）
//   VG6 入口 = nodes[0]；为 label 时取它的续接
//   VG7 除入口外每个节点可达（前向边），或作为某可达节点 on_error.fallback 的目标
//   VG8 每个 label 至少被一个 goto / next / then / else 指向
#include "cppchain/workflow/validator.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cppchain/workflow/expr.hpp"

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

bool is_ident_chars(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}

bool is_internal_key(std::string_view id) {
  return id == kResultsKey || id == kLastExecKey || id == kLoopCountsKey || id == kConfigKey;
}

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
  std::unordered_map<std::string, std::size_t> index;  // id → 数组下标
  std::unordered_map<std::string, NodeKind> kind;       // id → 节点类型
  ValidationResult out;

  void err(std::string rule, std::string message) {
    out.errors.push_back({std::move(rule), std::move(message)});
  }
};

// 计算每个 label 的续接节点（D2：整体求解；不要求 label 先于 goto）。
// 规则：label L 的续接 = 数组里 L 之后第一个非 label 节点。
void resolve_labels(Ctx& c) {
  auto const& nodes = c.ir.nodes;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    NodeIR const& n = nodes[i];
    if (n.kind != NodeKind::Label) continue;
    std::optional<std::size_t> j;
    for (std::size_t k = i + 1; k < nodes.size(); ++k)
      if (nodes[k].kind != NodeKind::Label) {
        j = k;
        break;
      }
    if (!j)
      c.err("VG3", "label '" + n.id + "' 之后没有续接节点（label 必须位于其将要续接的链之前）");
  }
}

// 指针目标替换：_turn_end → 终止 id；label → 续接。
std::string resolve_target(Ctx const& c, std::string const& t) {
  if (t == kTurnEnd) return kEndId;
  auto it = c.kind.find(t);
  if (it != c.kind.end() && it->second == NodeKind::Label) {
    // 续接由 resolve_labels 保证存在（label 之后必有非 label）
    auto const& nodes = c.ir.nodes;
    auto idx = c.index.at(t);
    for (std::size_t k = idx + 1; k < nodes.size(); ++k)
      if (nodes[k].kind != NodeKind::Label) return nodes[k].id;
    return t;  // 不会走到（VG3 已报错）
  }
  return t;
}

void find_refs_in_node(std::vector<std::pair<std::string, Ref>>& out, NodeIR const& n) {
  if (n.kind == NodeKind::Tool) {
    // 参数值里的引用（整值或模板）
    std::function<void(Json const&)> walk = [&](Json const& v) {
      if (v.is_string()) {
        for (RefScan const& sc : scan_refs(v.get_ref<std::string const&>()))
          if (sc.ok) out.emplace_back(n.id, sc.ref);
      } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) walk(it.value());
      } else if (v.is_array()) {
        for (Json const& e : v) walk(e);
      }
    };
    walk(n.parameters);
  } else if (n.kind == NodeKind::Expr) {
    auto parsed = Expr::parse(n.expr);
    if (parsed)
      for (Ref const& r : parsed->refs()) out.emplace_back(n.id, r);
  }
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
  Ctx c{ir, {}, {}, {}};

  // ---- V：id 与顶层的合法键 ----
  if (ir.id.empty()) c.err("V1", "workflow id 为空");
  if (ir.id == "result" || is_internal_key(ir.id))
    c.err("V3", "workflow id '" + ir.id + "' 是保留名/内部状态键");

  // ---- 建索引 ----
  for (std::size_t i = 0; i < ir.nodes.size(); ++i) {
    NodeIR const& n = ir.nodes[i];
    c.index[n.id] = i;
    c.kind[n.id] = n.kind;
  }

  // ---- VT：trigger ----
  if (ir.trigger.source != "time")
    c.err("VT1", "trigger.source 必须是 'time'（当前实现只放行 time 触发；实际是 '" +
                     ir.trigger.source + "'）");
  if (ir.trigger.kind != "schedule")
    c.err("VT2", "trigger 必须是 timing.kind == 'schedule'（当前实现只放行 schedule；实际是 '" +
                     ir.trigger.kind + "'）");
  if (has_key_anywhere(ir.trigger.raw, "condition"))
    c.err("VT3", "trigger 不允许带 condition —— 业务条件必须下沉到 expr 节点");
  if (has_top_key(ir.trigger.raw, "filter"))
    c.err("VT4", "trigger 不允许带 filter（过滤/STRICT_3V 层未实现）—— 匹配条件必须用 expr 节点");
  if (!valid_cron(ir.trigger.schedule))
    c.err("VT5", "schedule 必须是合法 5 字段 cron（分 时 日 月 周；拒绝秒字段/@宏/L/W/#/步进）/，收到: '" +
                     ir.trigger.schedule + "'");
  if (ir.trigger.timezone && ir.trigger.timezone->empty())
    c.err("VT6", "timezone 若给出必须非空");
  // VT7：trigger 未知键
  {
    Json const& t = ir.trigger.raw;
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

  // ---- VN：节点 ----
  for (std::size_t i = 0; i < ir.nodes.size(); ++i) {
    NodeIR const& n = ir.nodes[i];

    if (!is_ident(n.id))
      c.err("VN1", "节点 id '" + n.id + "' 不符合 [A-Za-z][A-Za-z0-9_]*（无前导 _）");
    if (n.id == "result" || is_internal_key(n.id))
      c.err("VN2", "节点 id '" + n.id + "' 是保留名/内部状态键");
    if (n.id == "_turn_start" || n.id == "_turn_end")
      c.err("VN2", "节点 id 不得用保留名 '" + n.id + "'");

    switch (n.kind) {
      case NodeKind::Label:
        // VN5：label 只允许 id/kind
        {
          Json const& raw = n.raw;
          if (raw.is_object())
            for (auto it = raw.begin(); it != raw.end(); ++it)
              if (it.key() != "id" && it.key() != "kind")
                c.err("VN5", "label '" + n.id + "' 不得带字段 '" + it.key() + "'");
        }
        break;

      case NodeKind::Tool: {
        if (n.tool.empty()) c.err("VN6", "tool 节点 '" + n.id + "' 缺少 tool 名");
        if (!n.parameters.is_object())
          c.err("VN6", "tool 节点 '" + n.id + "' 的 parameters 必须是对象");
        if (n.next.empty()) c.err("VN6", "tool 节点 '" + n.id + "' 缺少 next（成功出口）");
        if (opts.registry != nullptr) {
          tool::ToolSpec const* spec = opts.registry->find(n.tool);
          if (spec == nullptr)
            c.err("VN6", "tool 节点 '" + n.id + "' 引用了目录里不存在的工具 '" + n.tool +
                             "'（可用工具见目录注册表）");
          else if (n.parameters.is_object()) {
            // VN7：参数键 ⊆ 规格 properties；必填项必须出现
            Json const& props = spec->parameters.value("properties", Json::object());
            for (auto it = n.parameters.begin(); it != n.parameters.end(); ++it)
              if (!props.contains(it.key()))
                c.err("VN7", "tool 节点 '" + n.id + "' 的参数 '" + it.key() +
                                 "' 不在工具 '" + n.tool + "' 的 input schema 里");
            if (spec->parameters.contains("required") &&
                spec->parameters["required"].is_array())
              for (Json const& r : spec->parameters["required"]) {
                std::string const key = r.get<std::string>();
                if (!n.parameters.contains(key))
                  c.err("VN7", "tool 节点 '" + n.id + "' 缺少工具 '" + n.tool +
                                   "' 的必填参数 '" + key + "'");
              }
          }
        }
        // VN8：output_schema
        if (!n.output_schema.is_object())
          c.err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 必须是对象");
        else
          for (auto it = n.output_schema.begin(); it != n.output_schema.end(); ++it) {
            bool const ok_type =
                it.value().is_string() &&
                (it.value() == "number" || it.value() == "string" || it.value() == "boolean" ||
                 it.value() == "*");
            if (!is_ident_chars(it.key()))
              c.err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 字段名 '" + it.key() +
                               "' 非法");
            if (!ok_type)
              c.err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 字段 '" + it.key() +
                               "' 的类型必须是 number/string/boolean/*");
          }
        break;
      }

      case NodeKind::Expr: {
        auto parsed = Expr::parse(n.expr);
        if (!parsed)
          c.err("VN9", "expr 节点 '" + n.id + "' 的表达式不合法：" + parsed.error());
        if (n.then.empty()) c.err("VN9", "expr 节点 '" + n.id + "' 缺少 then");
        if (n.else_.empty()) c.err("VN9", "expr 节点 '" + n.id + "' 缺少 else");
        break;
      }

      case NodeKind::Goto:
        if (n.max <= 0)
          c.err("VN9", "goto 节点 '" + n.id + "' 的 max 必须是正整数（循环有界），收到 " +
                           std::to_string(n.max));
        break;
    }
  }

  // ---- 唯一性 ----
  {
    std::vector<std::string> ids;
    ids.reserve(ir.nodes.size());
    for (NodeIR const& n : ir.nodes) ids.push_back(n.id);
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 1; i < ids.size(); ++i)
      if (ids[i] == ids[i - 1])
        c.err("VN2", "节点 id '" + ids[i] + "' 重复（全图唯一，含 label/goto）");
  }

  // ---- VH：on_error ----
  for (NodeIR const& n : ir.nodes)
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
        bool const exists =
            t == kTurnEnd || (c.index.find(t) != c.index.end() && c.kind[t] != NodeKind::Goto);
        if (!exists)
          c.err("VH1", "tool 节点 '" + n.id + "' 的 on_error.fallback 指向不存在的节点 '" + t +
                           "'（应为节点 id 或 _turn_end）");
      }
    }

  // ---- VG：指针、label 消解、环、可达性 ----
  resolve_labels(c);

  for (NodeIR const& n : ir.nodes) {
    auto target_ok = [&](std::string const& key, std::string const& t) {
      if (t == "_turn_start")
        c.err("VG1", "节点 '" + n.id + "' 的 " + key + " 指向保留名 _turn_start（不允许）");
      else if (t != kTurnEnd && c.index.find(t) == c.index.end())
        c.err("VG1", "节点 '" + n.id + "' 的 " + key + " 指向不存在的节点 '" + t + "'");
    };
    switch (n.kind) {
      case NodeKind::Tool:
        target_ok("next", n.next);
        break;
      case NodeKind::Expr:
        target_ok("then", n.then);
        target_ok("else", n.else_);
        break;
      case NodeKind::Goto: {
        auto it = c.kind.find(n.goto_target);
        if (it == c.kind.end())
          c.err("VG1", "goto 节点 '" + n.id + "' 指向不存在的 label '" + n.goto_target + "'");
        else if (it->second != NodeKind::Label)
          c.err("VG1", "goto 节点 '" + n.id + "' 必须指向 label 节点，但 '" + n.goto_target +
                           "' 不是 label");
        break;
      }
      case NodeKind::Label:
        break;
    }
  }

  // VG8：label 必须被引用
  for (NodeIR const& n : ir.nodes)
    if (n.kind == NodeKind::Label) {
      bool referenced = false;
      for (NodeIR const& m : ir.nodes) {
        if (m.kind == NodeKind::Tool && m.next == n.id)
          referenced = true;
        else if (m.kind == NodeKind::Expr && (m.then == n.id || m.else_ == n.id))
          referenced = true;
        else if (m.kind == NodeKind::Goto && m.goto_target == n.id)
          referenced = true;
      }
      if (!referenced)
        c.err("VG8", "label '" + n.id + "' 没有被任何 goto / next / then / else 指向（死 label）");
    }

  // ---- VG2：前向边无环（label 已替换） ----
  {
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (NodeIR const& n : ir.nodes) {
      if (n.kind == NodeKind::Tool) {
        std::string t = resolve_target(c, n.next);
        if (t != kEndId) adj[n.id].push_back(t);
      } else if (n.kind == NodeKind::Expr) {
        for (std::string const& t_ : {resolve_target(c, n.then), resolve_target(c, n.else_)})
          if (t_ != kEndId) adj[n.id].push_back(t_);
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
      c.err("VG2", "前向边出现环（" + cycle_at + "）：循环只能通过 label + goto 表达，"
                   "禁止 expr/tool 自环或绕路");
  }

  // ---- VG7：可达性（前向边） + 入口 ----
  {
    std::string const entry = [&] {
      if (ir.nodes.empty()) return std::string{};
      NodeIR const& first = ir.nodes[0];
      if (first.kind == NodeKind::Label) return resolve_target(c, first.id);
      return first.id;
    }();

    if (ir.nodes.empty()) c.err("V2", "nodes 不能为空");
    if (entry.empty() || c.index.find(entry) == c.index.end())
      c.err("VG6", "入口无法解析（nodes[0] 是 label 但无续接）");

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
      if (it == c.kind.end() || it->second == NodeKind::Label) continue;
      NodeIR const& node = ir.nodes[c.index.at(v)];
      if (it->second == NodeKind::Tool) {
        push(resolve_target(c, node.next));
        // 失败路径也是一条可达路径：目标要解析，且它自己的后继要接着展开
        if (node.on_error && node.on_error->fallback)
          push(resolve_target(c, *node.on_error->fallback));
      } else if (it->second == NodeKind::Expr) {
        push(resolve_target(c, node.then));
        push(resolve_target(c, node.else_));
      }
    }
    // 死节点检查
    for (NodeIR const& n : ir.nodes)
      if (n.kind != NodeKind::Label && !seen[n.id])
        c.err("VG7", "节点 '" + n.id + "' 从入口不可达且不是任何可达节点的 on_error.fallback 目标（死节点）");
  }

  // ---- VR：引用 ----
  for (NodeIR const& n : ir.nodes) {
    // VR1：所有字符串参数 / expr 源里的 $/@config 片段必须语法合法
    if (n.kind == NodeKind::Tool) {
      std::function<void(Json const&)> walk = [&](Json const& v) {
        if (v.is_string()) {
          if (auto p = ref_syntax_problem(v.get_ref<std::string const&>()))
            c.err("VR1", "节点 '" + n.id + "' 的参数有引用语法错误：" + *p);
        } else if (v.is_object()) {
          for (auto it = v.begin(); it != v.end(); ++it) walk(it.value());
        } else if (v.is_array()) {
          for (Json const& e : v) walk(e);
        }
      };
      walk(n.parameters);
    } else if (n.kind == NodeKind::Expr) {
      if (auto p = ref_syntax_problem(n.expr))
        c.err("VR1", "节点 '" + n.id + "' 的表达式有引用语法错误：" + *p);
    }
  }
  for (NodeIR const& n : ir.nodes) {
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
            NodeIR const& target = ir.nodes[it->second];
            if (target.kind != NodeKind::Tool)
              c.err("VR2", "节点 '" + n.id + "' 的 $'" + r.node + "' 指向非 tool 节点（只有 tool 有 result）");
            else if (it->second >= c.index.at(n.id))
              c.err("VR2", "节点 '" + n.id + "' 引用了下游节点 '" + r.node +
                               "'（只能引用上游/更早的节点）");
            else if (!schema_declares(target.output_schema, r.field))
              c.err("VR2", "节点 '" + n.id + "' 引用了 '" + r.node + ".result." + r.field +
                               "'，但该节点 output_schema 未声明此字段");
          }
          break;
        }
        case RefKind::LastResult: {
          bool declared = false;
          std::size_t const my = c.index.at(n.id);
          for (std::size_t k = 0; k < my; ++k)
            if (ir.nodes[k].kind == NodeKind::Tool &&
                schema_declares(ir.nodes[k].output_schema, r.field)) {
              declared = true;
              break;
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