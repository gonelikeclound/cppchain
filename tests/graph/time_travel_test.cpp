// M10 测试：时间旅行分叉（fork）——从历史 checkpoint 恢复并继续（新线程），
// 原线程轨迹不污染；分叉点即新链的 parent。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

Json append_visited(Json const& state, std::string const& name) {
  auto v = state.value("visited", Json::array());
  v.push_back(name);
  return Json{{"visited", std::move(v)}};
}

// 一个线程里 metadata.node 匹配的快照的 checkpoint_id（最新优先）
std::string ckpt_of(MemoryCheckpointer& cp, std::string const& thread,
                    std::string const& node_meta) {
  for (auto const& s : cp.list(thread, "", 0))
    if (s.metadata.value("node", "") == node_meta) return s.config.value("checkpoint_id", "");
  return "";
}

StateSchema trace_schema() { return StateSchema{}; }  // visited 默认 overwrite

}  // namespace

TEST(Fork, continuesFromNamedCheckpointOnNewThread) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "a")); })
      .add_node("b", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "b")); })
      .add_node("c", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "c")); })
      .add_edge("a", "b")
      .add_edge("b", "c")
      .add_edge("c", kEnd)
      .set_entry_point("a");
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());

  ASSERT_TRUE(full->invoke(Json::object(), "thread-A").has_value());
  ASSERT_EQ(cp.list("thread-A", "", 0).size(), 3u);
  std::string const snap_b = ckpt_of(cp, "thread-A", "b");  // a/b 已跑完、等待 c
  ASSERT_FALSE(snap_b.empty());

  // fork 到 thread-B：分叉点 state + next 恢复，input 被忽略（visited 不从 input 开始）
  auto out = full->invoke(Json{{"visited", Json::array({"x"})}}, "thread-B",
                          InvokeOptions{.fork = ForkSpec{"thread-A", snap_b}});
  ASSERT_TRUE(out.has_value()) << out.error().message;
  auto const& v = out->at("visited");
  ASSERT_EQ(v.size(), 3u) << "应从分叉点快照（a,b）继续补跑 c";
  EXPECT_EQ(v[0], "a");
  EXPECT_EQ(v[1], "b");
  EXPECT_EQ(v[2], "c");

  // 原线程轨迹不被污染（仍 3 个快照，最新 = c 跑完）
  auto const a_snaps = cp.list("thread-A", "", 0);
  ASSERT_EQ(a_snaps.size(), 3u);
  EXPECT_EQ(a_snaps[0].metadata.value("node", ""), "c");
  EXPECT_TRUE(a_snaps[0].next.empty());

  // 新线程只补跑了 c（1 个快照），其 parent = 分叉点（链跨线程可追溯）
  auto const b_snaps = cp.list("thread-B", "", 0);
  ASSERT_EQ(b_snaps.size(), 1u);
  ASSERT_TRUE(b_snaps[0].parent_checkpoint_id.has_value());
  EXPECT_EQ(*b_snaps[0].parent_checkpoint_id, snap_b);
}

TEST(Fork, unknownCheckpointIsFailClosed) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "a")); })
      .add_edge("a", kEnd)
      .set_entry_point("a");
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(full->invoke(Json::object(), "thread-A").has_value());

  // 分叉点不存在：fail-closed，不默默退回"最新/全新开跑"
  auto out = full->invoke(Json::object(), "thread-B",
                          InvokeOptions{.fork = ForkSpec{"thread-A", "ckpt-999"}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::CheckpointNotFound);
}

TEST(Fork, forkUnknownSourceThreadIsFailClosed) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "a")); })
      .add_edge("a", kEnd)
      .set_entry_point("a");
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(full->invoke(Json::object(), "thread-A").has_value());

  auto out = full->invoke(Json::object(), "thread-B",
                          InvokeOptions{.fork = ForkSpec{"ghost", "whatever"}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::CheckpointNotFound);
}

// 回归：fork 的空 checkpoint_id（文档语义 = 该线程最新快照）不得被当作字面 id 精确查
TEST(Fork, emptyCheckpointMeansLatest) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "a")); })
      .add_node("b", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "b")); })
      .add_node("c", [](Json const& st, NodeContext&) { return Command::with_update(append_visited(st, "c")); })
      .add_edge("a", "b")
      .add_edge("b", "c")
      .add_edge("c", kEnd)
      .set_entry_point("a");
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(full->invoke(Json::object(), "thread-A").has_value());

  // 最新快照 = c 跑完（next 空）→ 分叉到新线程立即返回该终态，不报 CheckpointNotFound
  auto out = full->invoke(Json::object(), "thread-B",
                          InvokeOptions{nullptr, EventMode::Updates, std::nullopt,
                                        std::optional<ForkSpec>{ForkSpec{"thread-A", ""}}});
  ASSERT_TRUE(out.has_value()) << out.error().message;
  auto const& v = out->at("visited");
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0], "a");
  EXPECT_EQ(v[2], "c");
}

// 分叉点若正站在 interrupt 上：语义与普通恢复一致（无 resume 保持挂着、有 resume 继续）
TEST(Fork, interruptedForkPointRequiresResume) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("approve", [](Json const&, NodeContext& ctx) {
     auto const d = interrupt(Json{{"txn", "T-9"}}, ctx);
     return Command::with_update(Json{{"decision", d}});
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());

  ASSERT_TRUE(full->invoke(Json::object(), "thread-A").has_value());
  EXPECT_TRUE(full->last_interrupt().interrupted);
  std::string const snap_a = ckpt_of(cp, "thread-A", "approve");
  ASSERT_FALSE(snap_a.empty());

  // 无 resume → 分叉后仍挂着
  auto out1 = full->invoke(Json::object(), "thread-B",
                           InvokeOptions{.fork = ForkSpec{"thread-A", snap_a}});
  ASSERT_TRUE(out1.has_value());
  EXPECT_TRUE(full->last_interrupt().interrupted);
  EXPECT_EQ(full->last_interrupt().node, "approve");
  EXPECT_EQ(full->last_interrupt().payload.at("txn"), "T-9");

  // 带 resume → 从分叉点继续完成（新线程）；designated 顺序须按声明（resume 在 fork 前）
  auto out2 = full->invoke(Json::object(), "thread-B",
                           InvokeOptions{.resume = Json{{"ok", true}},
                                         .fork = ForkSpec{"thread-A", snap_a}});
  ASSERT_TRUE(out2.has_value()) << out2.error().message;
  EXPECT_EQ(out2->at("decision").at("ok"), true);
  EXPECT_FALSE(full->last_interrupt().interrupted);
}