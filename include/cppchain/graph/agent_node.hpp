// cppchain —— graph/agent_node.hpp
// M8：把 M7 的 Agent 暴露为图节点（04-problems/05 的前置补齐）。
// as_agent_node 是编译期简单适配器：节点读 state 里**本节点**的输入切片 → Agent.invoke →
// 结果写回（append 到 messages 或落 output_key）。主上下文干净 = 各节点从共享 state 取自己的切片，
// worker 用 output_key 落结果、不开 append_to_messages，主线 messages 就不被内部膨胀。
#pragma once

#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/graph/graph.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/state/state.hpp"

namespace cppchain::graph {

struct AgentNodeConfig {
  // 从共享 state 提取本节点的输入消息。各节点取不同切片 → 主上下文分槽。
  using InputResolver = std::function<std::vector<message::Message>(Json const& state)>;
  InputResolver input;               // 必填

  std::string output_key;            // 可选：最终答案**文本**写 state[output_key]
                                     //   该键的 reducer 决定语义（overwrite / push_reducer）
  bool append_to_messages = true;    // 可选：最终 AI 消息追加 state.messages（add_messages reducer）
};

// 把 Agent 包成 NodeFn。Agent 必须活得比图久（非拥有）。
// Agent.invoke 失败 → 抛 runtime_error（引擎捕获 → GraphError::NodeError，沿"不静默"纪律）。
NodeFn as_agent_node(agent::Agent& agent, AgentNodeConfig cfg);

// "累计归一"键的 push reducer：left 数组 + right 单条 → 追加。
// 串行 orchestrator 用它让每个 worker 只写自己的一条结果（下标对齐 = 执行序）。
state::Reducer push_reducer();

}  // namespace cppchain::graph