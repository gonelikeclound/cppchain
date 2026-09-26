// cppchain —— src/graph/agent_node.cpp
#include "cppchain/graph/agent_node.hpp"

#include <stdexcept>
#include <utility>

#include "cppchain/message/serializer.hpp"  // Message ↔ JSON（Json(*result) 需要）

namespace cppchain::graph {

using cppchain::message::Message;

NodeFn as_agent_node(agent::Agent& agent, AgentNodeConfig cfg) {
  return [&agent, cfg = std::move(cfg)](Json const& state, NodeContext&) -> Command {
    Command cmd;
    auto result = agent.invoke(cfg.input(state));
    if (!result)  // 不静默：模型/循环失败直接上抛，引擎包成 GraphError::NodeError
      throw std::runtime_error("agent node failed: " + result.error().message);

    if (cfg.append_to_messages) cmd.update["messages"] = Json::array({Json(*result)});
    if (!cfg.output_key.empty()) cmd.update[cfg.output_key] = result->text();
    return cmd;
  };
}

state::Reducer push_reducer() {
  return [](Json const& left, Json const& right) {
    Json out = left.is_array() ? left : Json::array();
    out.push_back(right);
    return out;
  };
}

}  // namespace cppchain::graph