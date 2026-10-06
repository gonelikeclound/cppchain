// demo: m10_streaming —— graph 层 Streaming 事件（M10，纯离线、无网络/模型依赖）
//
// 验证主线：引擎在包边界注入事件 sink 逐包吐 node_start / node_end（含状态增量），
// 节点内经 ctx.emit 自报流式事件（model delta 等，Deltas 模式才转发）。
// 事件信封 {seq, type, node, step, payload, ok, status} 与 workflow 层 session_log 对齐。
#include <cppchain/graph/graph.hpp>

#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::graph;

int main() {
  StateGraph g(state::StateSchema{});
  g.add_node("fetch", [](Json const&, NodeContext&) -> Command {
     return Command::with_update(Json{{"text", std::string("hello")}});
   })
      .add_node("stream_out", [](Json const& st, NodeContext& ctx) -> Command {
        // 模拟模型流式：把文本逐字符经 ctx.emit 吐成 delta 事件
        for (char c : st.at("text").get<std::string>())
          ctx.emit("delta", Json{{"char", std::string(1, c)}});
        return Command::with_update(Json{{"done", true}});
      })
      .add_node("tail", [](Json const&, NodeContext&) -> Command {
        return Command::with_update(Json::object());
      })
      .add_edge("fetch", "stream_out")
      .add_edge("stream_out", "tail")
      .add_edge("tail", kEnd)
      .set_entry_point("fetch");

  auto compiled = g.compile();
  // stream() = 全模式收集（Updates | Snapshot | Deltas；唯一执行通道仍是 invoke）
  auto const evs = compiled->stream(Json::object());

  std::println("=== m10_streaming: 事件时间线（共 {} 个事件）===", evs.size());
  for (auto const& e : evs) {
    std::string const status = e.status.value_or(std::string{});
    if (e.type == "delta") {
      std::println("  #{:>2}  {:>10}  node={:<10}  payload={}", e.seq, e.type, e.node,
                   e.payload.dump());
    } else {
      std::println("  #{:>2}  {:>10}  node={:<10}  step={}  ok={}  {}", e.seq, e.type, e.node,
                   e.step, e.ok ? "true" : "false", status);
    }
  }

  std::println();
  std::println("要点：引擎在包边界吐 node_start / node_end（node_end 的 payload 是状态增量），");
  std::println("     节点内经 ctx.emit 自报流式事件；seq 单调、不注入 sink 时零开销。");
}