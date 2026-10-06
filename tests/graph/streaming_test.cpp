// M10 测试：Streaming —— 事件 sink 逐包吐出进展（node_start/node_end/values/delta）。
// graph 层零依赖：只验证 graph.hpp 的事件面，不联动 workflow/session_log。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

Json push(Json const& state, std::string const& name) {
  auto v = state.value("visited", Json::array());
  v.push_back(name);
  return Json{{"visited", std::move(v)}};
}

// 把事件投影成易断言的元组（type,node,payload,step,ok）
struct Captured {
  std::string type, node;
  Json payload;
  int step;
  bool ok;
};
std::vector<Captured> capture(std::vector<StreamEvent> const& evs) {
  std::vector<Captured> out;
  for (auto const& e : evs) out.push_back({e.type, e.node, e.payload, e.step, e.ok});
  return out;
}

std::vector<std::string> types_of(std::vector<StreamEvent> const& evs) {
  std::vector<std::string> out;
  for (auto const& e : evs) out.push_back(e.type);
  return out;
}

}  // namespace

// 线性图全模式（Updates|Snapshot|Deltas）下的事件序列与 seq 单调
TEST(Streaming, per_step_node_boundary_events) {
  StateGraph g(StateSchema{});
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(push(st, "a")); })
      .add_node("b", [](Json const& st, NodeContext&) { return Command::with_update(push(st, "b")); })
      .add_edge("a", "b")
      .add_edge("b", kEnd)
      .set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  auto const evs = compiled->stream(Json::object());
  auto const caps = capture(evs);

  ASSERT_EQ(caps.size(), 6u);  // start/end × 2 + values × 2
  EXPECT_EQ(caps[0].type, "node_start");
  EXPECT_EQ(caps[0].node, "a");
  EXPECT_EQ(caps[1].type, "node_end");
  EXPECT_EQ(caps[1].node, "a");
  EXPECT_EQ(caps[1].payload.at("visited")[0], "a");
  EXPECT_EQ(caps[2].type, "values");
  EXPECT_EQ(caps[2].payload.at("visited").size(), 1u);
  EXPECT_EQ(caps[3].type, "node_start");
  EXPECT_EQ(caps[3].node, "b");
  EXPECT_EQ(caps[4].type, "node_end");
  EXPECT_EQ(caps[4].node, "b");
  EXPECT_EQ(caps[4].payload.at("visited").size(), 2u);
  EXPECT_EQ(caps[5].type, "values");
  EXPECT_EQ(caps[5].payload.at("visited").size(), 2u);
  for (std::size_t i = 1; i < evs.size(); ++i) EXPECT_GT(evs[i].seq, evs[i - 1].seq);
}

// Snapshot 开关：开时每 node_end 后跟完整 state 事件，关时没有
TEST(Streaming, snapshot_mode_emits_full_state) {
  StateGraph g(StateSchema{});
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(push(st, "a")); })
      .add_edge("a", kEnd)
      .set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  std::vector<StreamEvent> snap_evs;
  auto out = compiled->invoke(Json::object(), "",
                              InvokeOptions{[&snap_evs](StreamEvent const& e) { snap_evs.push_back(e); },
                                            EventMode::Snapshot, std::nullopt});
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(types_of(snap_evs),
            (std::vector<std::string>{"node_start", "node_end", "values"}));

  std::vector<StreamEvent> no_evs;
  (void)compiled->invoke(Json::object(), "",
                         InvokeOptions{[&no_evs](StreamEvent const& e) { no_evs.push_back(e); },
                                       EventMode::Updates, std::nullopt});
  EXPECT_EQ(types_of(no_evs), (std::vector<std::string>{"node_start", "node_end"}));
}

// 节点内 ctx.emit：Deltas 模式转发、Updates 模式静默
TEST(Streaming, node_emit_delta_forwarding) {
  StateGraph g(StateSchema{});
  g.add_node("a", [](Json const&, NodeContext& ctx) {
     ctx.emit("delta", Json{{"text", "你好"}});
     return Command::with_update({{"done", true}});
   })
      .add_edge("a", kEnd)
      .set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  std::vector<StreamEvent> with_delta;
  (void)compiled->invoke(Json::object(), "",
                         InvokeOptions{[&with_delta](StreamEvent const& e) { with_delta.push_back(e); },
                                       EventMode::Deltas, std::nullopt});
  bool saw_delta = false;
  for (auto const& e : with_delta) {
    if (e.type == "delta") {
      saw_delta = true;
      EXPECT_EQ(e.node, "a");
      EXPECT_EQ(e.payload.at("text"), "你好");
      EXPECT_TRUE(e.ok);
    }
  }
  EXPECT_TRUE(saw_delta) << "Deltas 模式下节点 emit 应被转发";

  std::vector<StreamEvent> no_delta;
  (void)compiled->invoke(Json::object(), "",
                         InvokeOptions{[&no_delta](StreamEvent const& e) { no_delta.push_back(e); },
                                       EventMode::Updates, std::nullopt});
  for (auto const& e : no_delta) EXPECT_NE(e.type, "delta");
}

// 不注入 sink = 完全零开销，行为与旧 invoke 一致（不吐事件、状态/步数不变）
TEST(Streaming, default_sink_null_is_noop) {
  StateGraph g(StateSchema{});
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(push(st, "a")); })
      .add_node("b", [](Json const& st, NodeContext&) { return Command::with_update(push(st, "b")); })
      .add_edge("a", "b")
      .add_edge("b", kEnd)
      .set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  auto const out = compiled->invoke(Json::object());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->at("visited").size(), 2u);
  EXPECT_EQ(compiled->last_steps(), 2u);
}

// 节点抛异常：node_end{ok:false, status:error} 后 invoke 返回 NodeError
TEST(Streaming, error_emits_node_end_not_ok) {
  StateGraph g(StateSchema{});
  g.add_node("a", [](Json const&, NodeContext&) -> Command {
     throw std::runtime_error("boom");
   })
      .add_edge("a", kEnd)
      .set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  std::vector<StreamEvent> evs;
  auto const out = compiled->invoke(Json::object(), "",
                                    InvokeOptions{[&evs](StreamEvent const& e) { evs.push_back(e); },
                                                  EventMode::Updates, std::nullopt});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::NodeError);
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].type, "node_start");
  EXPECT_TRUE(evs[0].ok);
  EXPECT_EQ(evs[1].type, "node_end");
  EXPECT_FALSE(evs[1].ok);
  ASSERT_TRUE(evs[1].status.has_value());
  EXPECT_EQ(*evs[1].status, "error");
}