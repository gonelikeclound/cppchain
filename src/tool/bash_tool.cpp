// cppchain —— src/tool/bash_tool.cpp
#include "cppchain/tool/bash_tool.hpp"

#include <string>

#include "cppchain/tool/command_classify.hpp"

namespace cppchain::tool {
namespace {

using Json = nlohmann::json;
using Res = std::expected<Json, ToolError>;

ToolSpec build(ToolExecutor const* executor, std::optional<std::chrono::milliseconds> timeout) {
  // 参数 schema 显式构造（嵌套花括号初始化器易被 nlohmann 折叠成错层）
  Json properties = Json::object();
  properties["command"] = Json{{"type", "string"},
                               {"description", "要执行的 shell 命令（由模型给出，长度不限）"}};
  Json required = Json::array();
  required.push_back("command");
  Json const params = Json{{"type", "object"}, {"properties", properties}, {"required", required}};

  ToolSpec spec = make_tool(
      "bash", "在宿主机 shell 中执行任意命令（副作用任意，需审批：仅词面可证纯只读才免审）",
      params,
      [timeout](Json const& args, ToolContext& ctx) -> Res {
        auto c = arg<std::string>(args, "command");
        if (!c) return std::unexpected(c.error());
        if (ctx.executor == nullptr)
          return std::unexpected(
              ToolError{"bash 工具未配置执行器（注入 ToolExecutor，批③ 起为 SandboxManager）"
                        "—— 拒绝执行（fail-closed）"});
        ExecResult const r = ctx.executor->run(*c, timeout);
        if (r.timed_out)
          return std::unexpected(ToolError{"bash 命令超时，进程组已强杀"
                                           + (r.note.empty() ? std::string{} : "：" + r.note)});
        if (r.exit_code == -1)
          return std::unexpected(ToolError{"bash 命令启动失败"
                                           + (r.note.empty() ? std::string{} : "：" + r.note)});

        // 正常路径：exit code + 输出合并文本（截断说明并入 note）
        std::string out = "exit_code: " + std::to_string(r.exit_code);
        if (!r.output.empty()) out += "\n" + r.output;
        if (!r.note.empty()) out += "\n" + r.note;
        return Json(out);
      },
      /*read_only=*/false);

  spec.category = "action";
  spec.approval = ToolApprovalLevel::NeedHuman;   // 越过静态策略，直接要求人审（批② interrupt 悬挂）
  spec.timeout_ms = timeout;
  spec.executor = executor;
  // L2 词面静态门禁：参数里的 command 交给分类器；缺参/坏参一律 Unknown（fail-closed）
  spec.command_classify = [](Json const& args) -> CommandClass {
    auto c = arg<std::string>(args, "command");
    if (!c) return CommandClass::Unknown;
    return classify_bash_command(*c);
  };
  return spec.with_effect_class(EffectClass::Irreversible);  // 治理(problems/33)：任意命令，不可安全重放
}

}  // namespace

ToolSpec make_bash_tool(ToolExecutor const* executor,
                        std::optional<std::chrono::milliseconds> timeout) {
  return build(executor, timeout);
}

}  // namespace cppchain::tool