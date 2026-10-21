// cppchain —— src/tool/tool_plugin.cpp
// 动态工具库加载（wfr-㊹ 治理，problems/34）：dlopen → dlsym creator → 校验 → 宿主侧组装 ToolSpec。
//
// 与 workflow/plugin.cpp 同纪律：fail-closed（任一失败整体失败，不部分注册）；
// 跨 DSO 用裸 C 函数指针 + C 字符串 JSON，std::function 全部宿主侧构造；
// 库进程期持有（不 dlclose），宿主不释放库的 context。
#include "cppchain/tool/tool_plugin.hpp"

#include <dlfcn.h>

#include <cctype>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

using Json = nlohmann::json;

namespace {

std::string dlerror_msg() {
  char const* e = dlerror();
  return e != nullptr ? e : "";
}

bool is_ident(std::string_view s) {
  if (s.empty() || !std::isalpha(static_cast<unsigned char>(s.front()))) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}

// 宿主侧把裸 C 函数指针包成 ToolSpec::Handler（std::function 宿主侧构造）。
// args 对象 dump 成 C 字符串 → 调库裸 fn → 解析结果/错误。
ToolSpec::Handler wrap_handler(ToolRunFn fn) {
  return [fn](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
    std::string const args_s = args.dump();
    char* out = nullptr;
    char* err = nullptr;
    int const rc = fn(args_s.c_str(), &out, &err);
    if (rc != 0 || out == nullptr) {
      std::string const why = (err != nullptr) ? err : "工具执行失败";
      if (err != nullptr) std::free(err);
      return std::unexpected(ToolError{why});
    }
    Json result;
    try {
      result = Json::parse(out, nullptr, /*allow_exceptions=*/false);
    } catch (...) {
      // 解析失败也当错误
    }
    std::free(out);
    if (result.is_discarded())
      return std::unexpected(ToolError{"工具返回非法 JSON"});
    return result;
  };
}

// 宿主侧把裸 C 函数指针包成 ToolSpec::probe（可为 null → 不包）。
std::function<ProbeResult(Json const&)> wrap_probe(ToolProbeFn fn) {
  return [fn](Json const& args) -> ProbeResult {
    std::string const args_s = args.dump();
    int const r = fn(args_s.c_str());
    switch (r) {
      case 0: return ProbeResult::Complete;
      case 1: return ProbeResult::Partial;
      case 2: return ProbeResult::Absent;
      default: return ProbeResult::Unknown;
    }
  };
}

}  // namespace

std::expected<std::vector<ToolSpec>, std::string>
load_dynamic_tools(std::string const& lib_path, std::vector<std::string> const& names) {
  // 探测阶段：dlopen 一次，逐 name 校验 creator/descriptor（fail-closed：任一失败整体失败）。
  void* const handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr)
    return std::unexpected("dlopen 失败 '" + lib_path + "'：" + dlerror_msg());

  std::vector<std::pair<cppchain_tool_descriptor const*, std::string>> pending;
  pending.reserve(names.size());
  for (std::string const& name : names) {
    if (!is_ident(name))
      return std::unexpected("工具名 '" + name + "' 非法（应为 [A-Za-z][A-Za-z0-9_]*）");
    std::string const sym = "tool_" + name + "_creator";
    dlerror();  // 清上次错误
    void* const addr = dlsym(handle, sym.c_str());
    if (addr == nullptr)
      return std::unexpected("取不到符号 '" + sym + "'（库 " + lib_path + "）：" + dlerror_msg());
    auto const creator = reinterpret_cast<ToolCreatorFn>(addr);
    cppchain_tool_descriptor const* const desc = creator(nullptr);
    if (desc == nullptr)
      return std::unexpected("creator 返回空 descriptor（tool '" + name + "'）");
    if (desc->version != kToolApiVersion)
      return std::unexpected("API 版本不匹配：tool '" + name + "'（库 " + lib_path +
                             "）编译于 v" + std::to_string(desc->version) + "，宿主是 v" +
                             std::to_string(kToolApiVersion) + "（拒绝加载）");
    if (desc->name == nullptr || name != desc->name)
      return std::unexpected("符号名与 descriptor.name 不一致：加载为 '" + name + "'，库声明 '" +
                             (desc->name != nullptr ? desc->name : "<null>") + "'");
    if (desc->handler == nullptr)
      return std::unexpected("descriptor 缺 handler（tool '" + name + "'）");
    pending.emplace_back(desc, name);
  }

  // 提交阶段：全部校验通过 → 宿主侧组装 ToolSpec（std::function 宿主侧构造；库进程期持有）。
  std::vector<ToolSpec> out;
  out.reserve(pending.size());
  for (auto const& [desc, name] : pending) {
    ToolSpec spec = make_tool(name, desc->description != nullptr ? desc->description : "",
                              Json::parse(desc->parameters_json != nullptr ? desc->parameters_json
                                                                           : "{}"),
                              wrap_handler(desc->handler),
                              desc->read_only != 0);
    spec.effect_class = desc->effect_class == kToolEffectIrreversible
                            ? EffectClass::Irreversible
                            : desc->effect_class == kToolEffectIdempotent ? EffectClass::Idempotent
                                                                           : EffectClass::Read;
    spec.approval = desc->approval == kToolApprovalNever
                        ? ToolApprovalLevel::Never
                        : desc->approval == kToolApprovalNeedHuman ? ToolApprovalLevel::NeedHuman
                                                                   : ToolApprovalLevel::Policy;
    if (desc->ttl_ms > 0)
      spec.ttl = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::milliseconds(desc->ttl_ms));
    if (desc->probe != nullptr)
      spec.probe = wrap_probe(desc->probe);
    out.push_back(std::move(spec));
  }
  return out;
}

}  // namespace cppchain::tool
