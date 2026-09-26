// cppchain —— agent/subagent.hpp
// M8：subagent-as-tool（ask_expert）。把一个"领域专家子代理"包成普通 ToolSpec，
// 父模型把子任务委托给它。返回**最终答案 + 压缩 stats**，子转录永不进父上下文。
//
// 三条硬约束的实现落点（见 03-detail-design/05 §3.1）：
//   ① 每次调用现构全新子代理 → 上下文隔离 + 并发安全 + 无快照污染
//   ② 共享 ChatModel 的并发调用按实例串行化（M2 transport 并发安全未验证，先换安全）
//   ③ 返回体只含 answer/stats，stats 全部由 child.transcript() 计算（无魔法）
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::agent {

// 子代理规格。指针成员一律非拥有 → 生命周期必须长于使用它的 ToolRegistry。
struct SubAgentSpec {
  std::string name;                  // 工具名（父视角），如 "ask_math"
  std::string description;           // 擅长什么/怎么用/返回什么（父模型据此决策）
  model::ChatModel* model = nullptr; // 与父共享同一实例（非拥有）
  tool::ToolRegistry const* tools = nullptr;  // 子代理自己的工具集（非拥有，可含其他 subagent → 嵌套）
  AgentOptions options;              // 子循环参数；options.system_prompt 即人设；
                                     //   thread_id 必须为空（默认）→ invoke 不碰 checkpointer，干净
  std::vector<std::shared_ptr<Middleware>> middlewares;  // 子代理自己的中间件（审批/压缩等），可选
};

struct SubAgentToolOptions {
  std::string arg_name = "query";    // 唯一入参名
  bool include_trace = true;         // 返回里附 stats 行（iterations/tool_turns/usage）
  bool read_only = false;            // 委派是否视作只读（默认否：子代理内部可能有副作用）
};

// 规格 → 普通 ToolSpec（可放进父 ToolRegistry）。返回 JSON：
//   {"answer": <最终答复>, "stats": {"iterations","tool_turns","usage"}}（opts.include_trace 时）
tool::ToolSpec make_subagent_tool(SubAgentSpec const& spec, SubAgentToolOptions opts = {});

}  // namespace cppchain::agent