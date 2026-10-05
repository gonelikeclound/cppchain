// cppchain —— src/workflow/plugin.cpp
#include "cppchain/workflow/plugin_node.hpp"

#include <dlfcn.h>

#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

namespace {

bool is_ident(std::string_view s) {
  if (s.empty() || !std::isalpha(static_cast<unsigned char>(s.front()))) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}

std::string dlerror_msg() {
  char const* e = dlerror();
  return e != nullptr ? e : "";
}

// 探测一个动态节点（dlopen → dlsym → creator → 校验），**不注册**。
// 校验项（§5.1，全部 fail-closed）：名字合法且未被占用、库能打开、符号取得到、
// version == kNodeApiVersion、descriptor.name == <name>、create 非空、create 返回非空。
struct Pending {
  std::string name;
  std::string lib;
  cppchain_node_descriptor const* desc = nullptr;
  DynamicNode* node = nullptr;
};

std::expected<Pending, std::string> probe_dynamic_node(std::string const& lib,
                                                       std::string const& name) {
  if (!is_ident(name)) return std::unexpected("节点名 '" + name + "' 非法（应为 [A-Za-z][A-Za-z0-9_]*）");

  void* const handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr)
    return std::unexpected("dlopen 失败 '" + lib + "'：" + dlerror_msg());

  std::string const sym = "graph_node_" + name + "_creator";
  dlerror();  // 清掉上次错误，确保下面的错误是本次 dlsym 的
  void* const addr = dlsym(handle, sym.c_str());
  if (addr == nullptr)
    return std::unexpected("取不到符号 '" + sym + "'（库 " + lib + "）：" + dlerror_msg());

  auto const creator = reinterpret_cast<NodeCreatorFn>(addr);
  cppchain_node_descriptor const* const desc = creator(nullptr);
  if (desc == nullptr)
    return std::unexpected("creator 返回空 descriptor（node '" + name + "'）");
  if (desc->version != kNodeApiVersion)
    return std::unexpected("API 版本不匹配：node '" + name + "'（库 " + lib +
                           "）编译于 v" + std::to_string(desc->version) + "，宿主是 v" +
                           std::to_string(kNodeApiVersion) + "（拒绝加载）");
  if (desc->name == nullptr || name != desc->name)
    return std::unexpected("符号名与 descriptor.name 不一致：加载为 '" + name + "'，库声明 '" +
                           (desc->name != nullptr ? desc->name : "<null>") +
                           "'（改名漏改会立刻被发现）");
  if (desc->create == nullptr)
    return std::unexpected("descriptor 缺 create（node '" + name + "'）");
  void* const node = desc->create(desc->context);
  if (node == nullptr) return std::unexpected("create 返回空节点（node '" + name + "'）");

  return Pending{name, lib, desc, static_cast<DynamicNode*>(node)};
}

// 把库的抽象接口包成 NodeType 函数表（std::function 全部在**宿主侧**构造；
// dyn 由库持有、进程期生命周期，宿主不释放不 dlclose）。
std::unique_ptr<NodeType> make_adapter(DynamicNode const* dyn) {
  auto t = std::make_unique<NodeType>();
  t->kind = NodeKind::Dynamic;
  t->matches = [dyn](Json const& j) { return dyn->matches(j); };
  t->parse = [dyn](NodeIR& ir, Json const& j) { dyn->parse(ir, j); };
  t->write = [dyn](Json& j, NodeIR const& ir) { dyn->write(j, ir); };
  t->describe = [dyn](NodeIR const& ir) { return dyn->describe(ir); };
  t->edges = [dyn](NodeIR const& ir) { return dyn->edges(ir); };
  t->ref_texts = [dyn](NodeIR const& ir) { return dyn->ref_texts(ir); };
  t->validate = [dyn](NodeIR const& ir, NodeType::ValidateContext const& vc,
                      NodeType::ErrorSink const& es) { dyn->validate(ir, vc, es); };
  t->compile = [dyn](NodeIR const& ir, NodeType::CompileContext& cc) { dyn->compile(ir, cc); };
  return t;
}

}  // namespace

std::expected<void, std::string> load_dynamic_node(std::string const& lib_path,
                                                   std::string const& name) {
  // 幂等：同一个宿主反复 import 同一库（多 workflow 共享进程）不该炸；
  // 名字与既有绑定相同即视为已加载。register_dynamic_node_type 对真冲突仍守住。
  if (node_type_of_name(name) != nullptr) return {};
  auto probe = probe_dynamic_node(lib_path, name);
  if (!probe) return std::unexpected(probe.error());
  if (!register_dynamic_node_type(probe->name, make_adapter(probe->node)))
    return std::unexpected("注册失败：节点 '" + name + "' 的名字已被占用");
  return {};
}

std::expected<std::size_t, std::string> load_workflow_imports(Json const& ir_json,
                                                              bool allow_import,
                                                              SessionLog* host_log) {
  if (!ir_json.contains("imports")) return std::size_t{0};
  Json const& imps = ir_json["imports"];
  if (!imps.is_array()) return std::unexpected("imports 必须是数组");

  if (!allow_import)
    return std::unexpected(
        "IR 声明了 imports 但未放行：加载 .so = 任意代码执行（要使用必须显式 --allow-import）");

  // 先全部探测、再统一提交：任一失败 → 什么都不注册（fail-closed，不给 IR 判别留半吊子状态）
  std::vector<Pending> pending;
  for (Json const& imp : imps) {
    if (!imp.is_object()) return std::unexpected("imports 条目不是对象（应为 {lib, nodes[]}）");
    std::string const lib = imp.value("lib", "");
    if (lib.empty()) return std::unexpected("imports 条目缺 lib（库是信任单元，必须显式）");
    if (!imp.contains("nodes") || !imp["nodes"].is_array() || imp["nodes"].empty())
      return std::unexpected("imports['" + lib +
                             "'] 必须显式列出节点名（A1 枚举落地前省略即报错）");
    for (Json const& nn : imp["nodes"]) {
      if (!nn.is_string()) return std::unexpected("imports['" + lib + "'] 的节点名必须是字符串");
      std::string const name = nn.get<std::string>();
      if (node_type_of_name(name) != nullptr) continue;  // 幂等：已加载的节点跳过
      auto p = probe_dynamic_node(lib, name);
      if (!p) return std::unexpected(p.error());
      pending.push_back(std::move(*p));
    }
  }

  std::size_t registered = 0;
  for (Pending& p : pending) {
    if (!register_dynamic_node_type(p.name, make_adapter(p.node)))
      return std::unexpected("注册失败：节点 '" + p.name + "' 的名字已被占用");
    if (host_log != nullptr)
      host_log->host_log("import 已加载节点 '" + p.name + "'（库 " + p.lib + "）",
                         Json{{"node", p.name}, {"lib", p.lib}, {"api_version", kNodeApiVersion}});
    ++registered;
  }
  return registered;
}

}  // namespace cppchain::workflow