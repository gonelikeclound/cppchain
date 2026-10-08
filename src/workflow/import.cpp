// cppchain —— src/workflow/import.cpp
// workflow import 的「展开式」实现（见 import.hpp 头注释）。算法：
//   对被引 workflow 递归展开 → 收集「可引用名 → 最终 id」映射 → 前缀化其节点与全部
//   指针/`$<id>` 引用 → 拼入本层。引擎零改动（展开后是单 IR）。
#include "cppchain/workflow/import.hpp"
#include "cppchain/workflow/refs.hpp"  // scan_refs / RefKind

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace cppchain::workflow {

namespace {

// 可引用名 → 最终 id（跨层累积）。key = 某被引 workflow **文件里写的原始 id**；
// value = 展开后该节点在扁平图中的最终 id（含全部前缀）。
using RenameMap = std::map<std::string, std::string>;

// 内置 reader：name → <base>/.wfr/workflows/<name>.json；path（含 /、.json、绝对）→ 直接读。
bool default_reader(std::string const& base_dir, std::string const& spec, Json& out,
                    std::string& err) {
  std::filesystem::path maybe(spec);
  std::filesystem::path path;
  if (maybe.is_absolute()) {
    path = maybe;
  } else if (spec.find('/') != std::string::npos || spec.ends_with(".json")) {
    path = std::filesystem::path(base_dir) / maybe;
  } else {
    path = std::filesystem::path(base_dir) / ".wfr" / "workflows" / (spec + ".json");
  }
  std::ifstream in(path);
  if (!in) {
    err = "找不到 workflow '" + spec + "'（searched: " + path.string() + "）";
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    out = Json::parse(ss.str());
  } catch (std::exception const& e) {
    err = "workflow '" + spec + "' 不是合法 JSON: " + e.what();
    return false;
  }
  return true;
}

// 指针目标字段：值是字符串且语义是"节点引用"（改写只碰这些 key；散文文本不动）
bool is_pointer_key(std::string_view key) {
  return key == "next" || key == "then" || key == "else" || key == "on_max" ||
         key == "default" || key == "fallback";
}

// 把字符串里的整段 `$<id>.result.<field>` 引用按 rn 重写（仅 NamedResult 且 id 命中）。
std::string rebuild_named_refs(std::string const& s, RenameMap const& rn) {
  auto const scans = scan_refs(s);
  if (scans.empty()) return s;
  std::string out;
  std::size_t last = 0;
  bool changed = false;
  for (RefScan const& sc : scans) {
    out.append(s, last, sc.begin - last);
    if (sc.ok && sc.ref.kind == RefKind::NamedResult) {
      if (auto const it = rn.find(sc.ref.node); it != rn.end()) {
        out += "$" + it->second + ".result." + sc.ref.field;
        changed = true;
        last = sc.end;
        continue;
      }
    }
    out.append(s, sc.begin, sc.end - sc.begin);
    last = sc.end;
  }
  out.append(s, last, s.size() - last);
  return changed ? out : s;
}

// 改写一个节点 JSON：指针字段整串换；任意字符串叶做 `$<id>` 引用重写。
void rewrite_node(Json& node, RenameMap const& rn) {
  if (node.is_string()) {
    std::string const cur = node.get<std::string>();
    std::string const rebuilt = rebuild_named_refs(cur, rn);
    if (rebuilt != cur) node = std::move(rebuilt);
    return;
  }
  if (node.is_array()) {
    for (Json& e : node) rewrite_node(e, rn);
    return;
  }
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (is_pointer_key(it.key()) && it->is_string()) {
        if (auto const hit = rn.find(it->get<std::string>()); hit != rn.end()) {
          it.value() = hit->second;  // 裸 id 整串替换
          continue;
        }
      }
      if (it.key() == "cases" && it->is_object()) {
        for (auto cv = it->begin(); cv != it->end(); ++cv)
          if (cv->is_string())
            if (auto const h = rn.find(cv->get<std::string>()); h != rn.end()) cv.value() = h->second;
        continue;
      }
      rewrite_node(it.value(), rn);  // 含字符串叶的 $ 引用重写
    }
  }
}

// import `with` 传参（见 import.hpp）：把**整串恰为** `@config.<键>` 的字符串叶，
// 按 with 换成字面量值，或转写为 `@config.<父层键>`（值本身也是 @config 引用时）。
void bind_import_config(Json& node, Json const& with) {
  if (node.is_string()) {
    std::string const cur = node.get<std::string>();
    if (cur.size() > 8 && cur.rfind("@config.", 0) == 0) {
      if (auto const it = with.find(cur.substr(8)); it != with.end()) node = it.value();
    }
    return;
  }
  if (node.is_array()) {
    for (Json& e : node) bind_import_config(e, with);
    return;
  }
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) bind_import_config(it.value(), with);
  }
}

// 遍历一层 nodes 数组（并递归进 loop 的 body）。import 的前缀化/打标/传参都是
// "整棵子树"语义，wfr-㉚ 之后子树还包含循环体，不能只看一层。
template <typename F>
void for_each_node_json(Json& nodes, F&& fn) {
  for (Json& n : nodes) {
    if (!n.is_object()) continue;
    fn(n);
    if (n.contains("body") && n["body"].is_array()) for_each_node_json(n["body"], fn);
  }
}

// 给一棵子树整体加一层前缀：id 与子树内引用全部 `pfx__`，rename 值也跟随。
struct Prefixed {
  Json nodes = Json::array();
  RenameMap renames;
};
Prefixed prepend_chunk(Json const& sub_nodes, RenameMap const& sub_renames, std::string const& pfx) {
  Prefixed out;
  out.nodes = sub_nodes;
  RenameMap id_map;  // 子树当前最终 id → 加前缀后
  // 先整棵子树（含 loop body）建好 id_map，再统一改写指针/引用 —— 顺序不能反。
  for_each_node_json(out.nodes, [&](Json& node) {
    if (!node.contains("id") || !node["id"].is_string()) return;
    std::string const old = node["id"].get<std::string>();
    std::string const nw = pfx + "__" + old;
    node["id"] = nw;
    id_map[old] = nw;
  });
  // 顶层逐个 rewrite；rewrite_node 自己会递归进 body 的指针/引用
  for (Json& node : out.nodes) rewrite_node(node, id_map);
  for (auto const& [k, v] : sub_renames) out.renames[k] = pfx + "__" + v;
  return out;
}

// 递归展开一棵 workflow JSON。node 顺序 = 本层 own 在前、各 import 子树按声明序。
struct Expanded {
  Json nodes = Json::array();
  RenameMap renames;  // 本文件内全部可引用名 → 最终 id
};

std::expected<Expanded, std::string> expand(
    Json const& wf, std::string const& base_dir, WorkflowImportLoader const& loader,
    std::set<std::string>& active) {
  auto err = [](std::string m) { return std::unexpected(std::move(m)); };
  if (!wf.is_object() || !wf.contains("id") || !wf["id"].is_string())
    return err("被 import 的工作流缺顶层 id（string）");
  if (!wf.contains("nodes") || !wf["nodes"].is_array())
    return err("workflow '" + wf["id"].get<std::string>() + "' 缺 nodes 数组");
  // 被引 workflow 的动态库导入 v1 拒绝（fail-closed）
  if (wf.contains("imports"))
    return err("workflow '" + wf["id"].get<std::string>() +
               "' 含动态库导入 imports —— 被 import 的工作流暂不支持（需宿主先 --allow-import 仅顶层）");

  Expanded e;
  e.nodes = wf["nodes"];  // 拷一份（下面对它原地改写）
  RenameMap merged = e.renames;
  // 本层 own id → 自身（最终不变）；冲突检测靠 VN2 或这里。
  // 含 loop body 里的节点：父层要能用它们的原始 id 引用，前缀化也要跟着走。
  for_each_node_json(e.nodes, [&](Json& node) {
    if (node.contains("id") && node["id"].is_string())
      merged[node["id"].get<std::string>()] = node["id"].get<std::string>();
  });

  if (wf.contains("import")) {
    Json import_decl = wf["import"];
    if (!import_decl.is_object() && !import_decl.is_array())
      return err("import 必须是对象或数组");
    std::vector<Json> entries;
    if (import_decl.is_array())
      entries.insert(entries.end(), import_decl.begin(), import_decl.end());
    else
      entries.push_back(std::move(import_decl));

    std::vector<Json> chunks;
    for (Json const& entry : entries) {
      if (!entry.is_object() || !entry.contains("workflow") || !entry["workflow"].is_string())
        return err("import 条目必须是 {workflow: \"<name-or-path>\", as?: \"<前缀>\", with?: {...}}");
      std::string const spec = entry["workflow"].get<std::string>();
      Json with = Json::object();
      if (entry.contains("with")) {
        if (!entry["with"].is_object())
          return err("import 'with' 必须是对象 { <config键>: <字面量|@config.<父层键>> }");
        with = entry["with"];
      }

      Json raw;
      std::string rerr;
      if (!loader.read(base_dir, spec, raw, rerr)) return err(rerr);
      if (!raw.is_object()) return err("workflow '" + spec + "' 顶层不是对象");

      // 循环检测：用可解析的代号（path 化）作 key
      std::string key = spec;
      {
        std::filesystem::path maybe(spec);
        std::filesystem::path resolved;
        if (maybe.is_absolute()) resolved = maybe;
        else if (spec.find('/') != std::string::npos || spec.ends_with(".json"))
          resolved = std::filesystem::path(base_dir) / maybe;
        else
          resolved = std::filesystem::path(base_dir) / ".wfr" / "workflows" / (spec + ".json");
        key = std::filesystem::weakly_canonical(resolved).string();
      }
      if (active.count(key)) {
        std::string chain;
        for (std::string const& s : active) { if (!chain.empty()) chain += " → "; chain += s; }
        return err("循环 import 检测: " + chain + " → " + key);
      }
      active.insert(key);
      auto sub = expand(raw, base_dir, loader, active);  // 递归（子工作流同目录基线，v1 约定）
      active.erase(key);
      if (!sub) return err(std::move(sub.error()));

      // 前缀 = as 或 被引 workflow 的 id
      std::string pfx;
      if (entry.contains("as") && entry["as"].is_string()) pfx = entry["as"].get<std::string>();
      else if (raw.contains("id") && raw["id"].is_string()) pfx = raw["id"].get<std::string>();
      if (pfx.empty()) return err("import '" + spec + "' 缺前缀（as 或被引 id）");
      if (!(pfx.front() >= 'A' && pfx.front() <= 'Z') && !(pfx.front() >= 'a' && pfx.front() <= 'z'))
        return err("import 前缀 '" + pfx + "' 必须以字母开头（VN1 同规）");

      Prefixed pr = prepend_chunk(sub->nodes, sub->renames, pfx);
      // 子链徽标：给本层被拉入的节点标 origin_wf（其**直属**子工作流名）。
      // 仅标尚未有 origin_wf 的 —— 深层 import 已在递归里标过（保留最深处归属）。
      {
        std::string const origin =
            raw.contains("id") && raw["id"].is_string() ? raw["id"].get<std::string>() : pfx;
        for_each_node_json(pr.nodes, [&](Json& n) {
          if (!n.contains("origin_wf")) n["origin_wf"] = origin;
        });
      }
      // with 传参（展开期特化）：把整棵子链（含更深的 import 节点）的 @config.<键> 折叠/转发
      if (!with.empty()) for_each_node_json(pr.nodes, [&](Json& n) { bind_import_config(n, with); });
      for (auto const& [k, v] : pr.renames) {
        if (auto const dup = merged.find(k); dup != merged.end() && dup->second != v)
          return err("import 节点重名 '" + k + "'（两个来源都叫这个名字）");
        merged[k] = v;
      }
      chunks.push_back(std::move(pr.nodes));
    }

    // 本层 own 的指针/引用按（含子命名空间在内的）映射改写 —— 父层用子链原始 id 引用
    for (Json& node : e.nodes) rewrite_node(node, merged);
    for (Json& c : chunks)
      for (Json& node : c) e.nodes.push_back(std::move(node));
  }
  e.renames.swap(merged);
  return e;
}

}  // namespace

std::expected<void, std::string> expand_workflow_imports(Json& ir, std::string const& base_dir,
                                                         WorkflowImportLoader const& loader) {
  if (!ir.is_object()) return std::unexpected(std::string("workflow 顶层不是对象"));
  if (!ir.contains("import")) return {};  // 无 import：原样

  WorkflowImportLoader use = loader;
  if (!use.read) use.read = default_reader;

  std::set<std::string> active;
  auto e = expand(ir, base_dir, use, active);
  if (!e) return std::unexpected(std::move(e.error()));

  ir.erase("import");
  ir["nodes"] = std::move(e->nodes);
  return {};
}

}  // namespace cppchain::workflow