// cppchain —— src/workflow/plugin.cpp
#include "cppchain/workflow/plugin_node.hpp"

#include <dlfcn.h>
#include <elf.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
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

std::expected<std::vector<std::string>, std::string> enumerate_plugin_nodes(
    std::string const& lib_path) {
  // A1（⑤b）：**读 ELF 文件扫本库自己的 `.dynsym`**。之所以不 dl_iterate_phdr ——
  // 它遍历整个进程，会把别的库的同名符号也扫进来（docs/12 §5.2 的告诫）。
  // 顺带性：不 dlopen 就不执行库代码，`--list-nodes` 无需 --allow-import。
  using Byte = std::uint8_t;
  std::ifstream in(lib_path, std::ios::binary);
  if (!in) return std::unexpected("无法打开库文件 '" + lib_path + "'");
  std::vector<Byte> const bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

  auto const have = [&](std::size_t off, std::size_t n) -> bool { return off + n <= bytes.size(); };
  if (!have(0, 16) || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
    return std::unexpected("不是 ELF 文件：'" + lib_path + "'");
  if (bytes[4] != ELFCLASS64)
    return std::unexpected("A1 暂只支持 ELF64（拿到 ELF32）：'" + lib_path + "'");
  if (bytes[5] != ELFDATA2LSB)
    return std::unexpected("A1 暂只支持 little-endian：'" + lib_path + "'");

  Elf64_Ehdr eh;
  if (!have(0, sizeof eh)) return std::unexpected("ELF 头不完整：'" + lib_path + "'");
  std::memcpy(&eh, bytes.data(), sizeof eh);

  auto const sh_table = [&](Elf64_Shdr& out, std::size_t i) -> bool {
    if (i >= eh.e_shnum || eh.e_shentsize < sizeof out) return false;
    if (!have(static_cast<std::size_t>(eh.e_shoff) + i * eh.e_shentsize, sizeof out)) return false;
    std::memcpy(&out, bytes.data() + static_cast<std::size_t>(eh.e_shoff) + i * eh.e_shentsize,
                sizeof out);
    return true;
  };

  std::vector<std::string> names;
  constexpr std::string_view kPrefix = "graph_node_";
  constexpr std::string_view kSuffix = "_creator";
  for (std::size_t i = 0; i < eh.e_shnum; ++i) {
    Elf64_Shdr sh;
    if (!sh_table(sh, i)) continue;
    if (sh.sh_type != SHT_DYNSYM) continue;
    Elf64_Shdr str;  // .dynsym 的字符串表（sh_link）
    if (!sh_table(str, sh.sh_link)) continue;
    if (sh.sh_entsize < sizeof(Elf64_Sym)) continue;
    std::size_t const syms_off = static_cast<std::size_t>(sh.sh_offset);
    std::size_t const strs_off = static_cast<std::size_t>(str.sh_offset);
    if (!have(syms_off, sizeof(Elf64_Sym)) || !have(strs_off, 1)) continue;
    std::size_t const n_syms = sh.sh_size / sh.sh_entsize;
    for (std::size_t k = 0; k < n_syms; ++k) {
      if (!have(syms_off + k * sh.sh_entsize, sizeof(Elf64_Sym))) break;
      Elf64_Sym sy{};
      std::memcpy(&sy, bytes.data() + syms_off + k * sh.sh_entsize, sizeof sy);
      if (sy.st_name == 0 || sy.st_shndx == SHN_UNDEF) continue;  // 只收本库**定义**的符号
      std::size_t const name_off = strs_off + static_cast<std::size_t>(sy.st_name);
      if (name_off >= bytes.size()) continue;
      std::size_t e = name_off;
      while (e < bytes.size() && bytes[e] != 0) ++e;
      std::string_view const fn(reinterpret_cast<char const*>(bytes.data() + name_off),
                                e - name_off);
      if (!fn.starts_with(kPrefix) || !fn.ends_with(kSuffix)) continue;
      std::string_view const nm =
          fn.substr(kPrefix.size(), fn.size() - kPrefix.size() - kSuffix.size());
      if (nm.empty() || !is_ident(nm)) continue;
      names.emplace_back(nm);
    }
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
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
    if (!imp.is_object()) return std::unexpected("imports 条目不是对象（应为 {lib, nodes[]?}）");
    std::string const lib = imp.value("lib", "");
    if (lib.empty()) return std::unexpected("imports 条目缺 lib（库是信任单元，必须显式）");

    // 节点名单：显式列出（A0）或省略走 A1 枚举。列了空数组 = 类型错误，拒。
    std::vector<std::string> names;
    if (imp.contains("nodes")) {
      if (!imp["nodes"].is_array()) return std::unexpected("imports['" + lib + "'] 的 nodes 必须是数组");
      for (Json const& nn : imp["nodes"]) {
        if (!nn.is_string()) return std::unexpected("imports['" + lib + "'] 的节点名必须是字符串");
        names.push_back(nn.get<std::string>());
      }
      if (names.empty())
        return std::unexpected("imports['" + lib +
                               "'] 的 nodes 不能是空数组（要整库请省略 nodes 走 A1 枚举）");
    } else {
      auto e = enumerate_plugin_nodes(lib);
      if (!e)
        return std::unexpected("imports['" + lib + "'] 省略 nodes 但无法枚举：" + e.error() +
                               "（可改为显式列出节点名）");
      names = std::move(*e);
      if (names.empty())
        return std::unexpected("imports['" + lib + "'] 枚举不到任何 graph_node_*_creator");
    }

    for (std::string& name : names) {
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