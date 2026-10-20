// cppchain —— tool/tool.hpp
// 工具规格：显式 JSON Schema + handler（反射实测不可用，见 02-arch-design/03 §3）。
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cppchain/core/cancel.hpp"
#include "cppchain/tool/command_classify.hpp"
#include "cppchain/tool/executor.hpp"
#include "cppchain/tool/probe.hpp"

namespace cppchain::tool {

struct ToolError {
  std::string message;
};

struct ToolException : std::runtime_error {
  explicit ToolException(std::string const& msg) : std::runtime_error(msg) {}
};

// 执行上下文：注入给工具（后续扩展 state / store / 审批）
struct ToolContext {
  std::string tool_call_id;
  core::CancelToken cancel;
  // 命令类工具（bash 等）的执行后端：由 ExecutionGuard::run 从 ToolSpec::executor 注入。
  // 无执行器 → 命令工具的 handler 必须 fail-closed 拒跑（wfr-㊸ @① 铁律：不直接 system()）。
  ToolExecutor const* executor = nullptr;
  // 预留：StateRef state; Store* store; StreamWriter* writer;
};

// 审批级别（wfr-㊸ @① L3）：工具声明"谁来放行它的副作用"。
//   消费方链：approve_tools 节点（静态策略）+ 批② 的 interrupt 悬挂；read_only 工具天然免审。
//   命名避开 agent::ToolApproval（审批中间件类）—— 那是通道层，这里是规格契约层。
enum class ToolApprovalLevel : unsigned char {
  Never,     // 免审批（只读工具默认）
  Policy,    // 静态策略决定（现在 approve_tools 的行为，多数工具默认）
  NeedHuman, // 越过静态策略，直接要求人审（bash 出厂值；批② 起 approve_tools 对该类悬挂）
};

// ---- 效果域副作用分类（治理，problems/33，对齐 总纲 EffectContract.effectClass）----
// 工具作者对副作用的**静态声明契约**（区别于 command_classify 的 bash 命令**词面动态**分类）：
//   Read         只读，无副作用（read_only==true 隐含本类，无需显式声明）
//   Idempotent   有副作用但可安全重放（同参同果，如确定性 file_write / submit_result）
//   Irreversible 有副作用且不可安全重放（如任意 bash —— 词面挡不住、需人审/沙箱兜底）
enum class EffectClass : unsigned char { Read, Idempotent, Irreversible };

inline constexpr std::string_view kEffectClassNames[] = {"read", "idempotent", "irreversible"};

[[nodiscard]] inline bool is_known_effect_class(std::string_view s) {
  for (std::string_view const k : kEffectClassNames)
    if (s == k) return true;
  return false;
}

struct ToolSpec {
  // handler 契约：可能被并行调用 → 必须线程安全；args 为已解析的 JSON 参数
  using Handler = std::function<std::expected<nlohmann::json, ToolError>(
      nlohmann::json const& args, ToolContext& ctx)>;

  std::string name;
  std::string description;
  nlohmann::json parameters = nlohmann::json::object();  // JSON Schema
  Handler handler;
  bool read_only = true;  // 副作用标记（M7 审批中间件用）

  // 治理（problems/33）：效果域副作用声明（缺省 Read = 只读，read_only==false 必须显式
  // 声明 Idempotent/Irreversible，否则启动自检 effect_class_violations 报违规 fail-fast）。
  EffectClass effect_class = EffectClass::Read;

  // 结果时效性：超过该时长即视为"可能已过期"（余额/库存/价格/文件内容这类易变状态）。
  // 留空 = 不过期（静态知识类工具）。由 agent::StaleStateGuard 消费，见 04-problems/06。
  std::optional<std::chrono::seconds> ttl;

  // 副作用产物探测（wfr-㊷）：崩溃缺环（Intent 残留）时，工具自查"外部世界现状"以判定
  // 副作用是否已发生 / 是否完整。判据闭合四格见 probe.hpp；铁律：**probe 必须只读**。
  // 缺省 nullptr = 无探测能力 → ExecutionGuard 对 Intent 一律 fail-closed（宁可请人，不可重跑）。
  // 由 ExecutionGuard 消费（执行点防护），详见 02-arch-design/18 §6 D3。
  std::function<ProbeResult(nlohmann::json const& args)> probe = nullptr;

  // 工具类别（wfr-㊵）：供"历史精简"按类别过滤（如丢掉联网取数的原始长文）。
  // 词表见下面的 kToolCategories / is_known_category。**不进 to_openai()** ——
  // 类别是库内分类，不是 function schema 的一部分；真端点对未知字段敏感（见 to_openai 的教训）。
  std::string category = "general";

  // wfr-㊸ @① 副作用契约：审批级别（L3 消费；bash 出厂 NeedHuman，其余默认 Policy）
  ToolApprovalLevel approval = ToolApprovalLevel::Policy;

  // wfr-㊸ @① 命令执行超时（L5 消费；bash 出厂 120s，执行器侧以它为强杀上限）
  std::optional<std::chrono::milliseconds> timeout_ms;

  // wfr-㊸ @① 命令类工具的执行后端（L4 抽象）：无执行器时命令工具 fail-closed 拒跑。
  // 批① 注入 make_plain_spawn_executor()（进程级隔离）；批③ 由 SandboxManager provider 替换。
  ToolExecutor const* executor = nullptr;

  // wfr-㊸ @① 命令词面分类器（L2 静态门禁）：在参数上做保守分类。
  // 消费方：ExecutionGuard::prepare 的"未开始"分支 —— ReadOnly 放行、其余 fail-closed 阻断
  // （批② 起升级为 interrupt 悬挂）。read_only 工具不经过它（只读早退在前）。
  std::function<CommandClass(nlohmann::json const& args)> command_classify = nullptr;

  // 链式设置类别：`reg.add(make_tool(...).with_category("retrieval"))`
  ToolSpec& with_category(std::string c) {
    category = std::move(c);
    return *this;
  }

  // 治理（problems/33）：链式声明效果域副作用分类：
  // `reg.add(make_file_write_tool(...).with_effect_class(EffectClass::Idempotent))`
  ToolSpec& with_effect_class(EffectClass e) {
    effect_class = e;
    return *this;
  }

  // → {"type":"function","function":{name,description,parameters}}，直接填入 CallOptions::tools
  [[nodiscard]] nlohmann::json to_openai() const;
};

// 类别词表（封闭）。语义粗略但够用 —— 目的是"按用途筛选历史"，不是权限模型：
//   retrieval 联网/外部取数（易变、返回往往很长）  compute 纯本地确定性计算
//   action    有副作用（通知/写文件/下单）        model  反过来调模型（llm / 结构化抽取）
//   time      时间基准                             general 未归类（不匹配任何显式类别）
inline constexpr std::string_view kToolCategories[] = {"retrieval", "compute", "action",
                                                       "model", "time", "general"};

[[nodiscard]] inline bool is_known_category(std::string_view c) {
  for (std::string_view const k : kToolCategories)
    if (c == k) return true;
  return false;
}

// ---- 参数取值助手（无反射，显式取值 + 明确报错）----

template <typename T>
std::expected<T, ToolError> arg(nlohmann::json const& args, std::string_view key,
                               bool required = true) {
  auto it = args.find(std::string(key));
  if (it == args.end() || it->is_null()) {
    if (!required) return T{};
    return std::unexpected(ToolError{"missing required argument: '" + std::string(key) + "'"});
  }
  try {
    return it->get<T>();
  } catch (nlohmann::json::exception const& e) {
    return std::unexpected(ToolError{"argument '" + std::string(key) + "' invalid: " + e.what()});
  }
}

// ---- Schema builder（写入参数即成 schema）----

class SchemaBuilder {
 public:
  SchemaBuilder& prop(std::string name, std::string type, std::string description,
                      bool required = true) {
    std::string const key = name;
    properties_[key] = {{"type", std::move(type)}, {"description", std::move(description)}};
    if (required) required_.push_back(key);
    return *this;
  }

  [[nodiscard]] nlohmann::json build() const {
    nlohmann::json out = {{"type", "object"}, {"properties", properties_}};
    if (!required_.empty()) out["required"] = required_;
    return out;
  }

 private:
  nlohmann::json properties_ = nlohmann::json::object();
  nlohmann::json required_ = nlohmann::json::array();
};

// 便捷构造
inline ToolSpec make_tool(std::string name, std::string description, nlohmann::json parameters,
                          ToolSpec::Handler handler, bool read_only = true) {
  ToolSpec spec;
  spec.name = std::move(name);
  spec.description = std::move(description);
  spec.parameters = std::move(parameters);
  spec.handler = std::move(handler);
  spec.read_only = read_only;
  return spec;
}

}  // namespace cppchain::tool