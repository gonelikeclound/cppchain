// M6 测试：极简 Graph 引擎（顺序 super-step / 条件路由 / 上限 / 快照恢复）
#include <gtest/gtest.h>

#include <string>

#include "cppchain/graph/graph.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

// 把每次节点的名字追加到 state.visited（用 add_messages 那套 reducer 的通用写法）
Json append_visited(Json const& state, std::string const& name) {
  Json out = state.is_object() ? state : Json::object();
  if (!out.contains("visited")) out["visited"] = Json::array();
  out["visited"].push_back(name);
  return out;
}

StateSchema trace_schema() {
  StateSchema s;  // 全部键 overwrite（visited 由节点自己累积后整体返回）
  return s;
}

}  // namespace

TEST(Graph, linear_execution_in_order) {
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) {
     return Command::with_update(append_visited(st, "a"));
   })
      .add_node("b", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "b"));
      })
      .add_node("c", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "c"));
      })
      .add_edge("a", "b")
      .add_edge("b", "c")
      .add_edge("c", kEnd)
      .set_entry_point("a");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

  auto out = compiled->invoke(Json::object());
  ASSERT_TRUE(out.has_value()) << out.error().message;
  EXPECT_EQ(out->at("visited"), Json::array({"a", "b", "c"}));
  EXPECT_EQ(compiled->last_steps(), 3u);
}

TEST(Graph, nodes_read_and_reduce_state) {
  auto schema = StateSchema::messages_state();
  StateGraph g(schema);
  g.add_node("greet", [](Json const&, NodeContext&) {
     return Command::with_update(Json{{"messages", Json::array({Json("你好")})}});
   })
      .add_node("reply", [](Json const& st, NodeContext&) {
        // 能读到上一步写的状态
        std::size_t const n = st.at("messages").size();
        return Command::with_update(Json{{"messages", Json::array({Json("第" + std::to_string(n) + "条")})}});
      })
      .add_edge("greet", "reply")
      .add_edge("reply", kEnd)
      .set_entry_point("greet");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object());
  ASSERT_TRUE(out.has_value()) << out.error().message;
  ASSERT_EQ(out->at("messages").size(), 2u);  // reducer 追加
}

TEST(Graph, conditional_edges_route_or_end) {
  StateGraph g(trace_schema());
  g.add_node("entry", [](Json const& st, NodeContext&) {
     return Command::with_update(append_visited(st, "entry"));
   })
      .add_node("yes", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "yes"));
      })
      .add_node("no", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "no"));
      })
      .add_conditional_edges("entry", [](Json const& st) -> std::vector<std::string> {
        bool const go = st.value("go", false);
        return {go ? "yes" : "no"};
      })
      .add_edge("yes", kEnd)
      .add_edge("no", kEnd)
      .set_entry_point("entry");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  auto a = compiled->invoke(Json{{"go", true}});
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->at("visited"), Json::array({"entry", "yes"}));

  auto b = compiled->invoke(Json{{"go", false}});
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->at("visited"), Json::array({"entry", "no"}));
}

TEST(Graph, loop_hits_recursion_limit) {
  StateGraph g(trace_schema());
  g.add_node("spin", [](Json const& st, NodeContext&) {
     Json out = append_visited(st, "spin");
     out["count"] = st.value("count", 0) + 1;
     return Command::with_update(out);
   })
      .add_conditional_edges("spin", [](Json const& st) -> std::vector<std::string> {
        return st.value("count", 0) < 1000 ? std::vector<std::string>{"spin"}
                                          : std::vector<std::string>{kEnd};
      })
      .set_entry_point("spin");

  CompileOptions opts;
  opts.max_steps = 5;
  auto compiled = g.compile(nullptr, opts);
  ASSERT_TRUE(compiled.has_value());

  auto out = compiled->invoke(Json::object());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::RecursionLimit);
}

TEST(Graph, command_goto_overrides_edges) {
  StateGraph g(trace_schema());
  g.add_node("start", [](Json const&, NodeContext&) {
     return Command::jump({"c"});  // 显式跳到 c（忽略静态边 start→b）
   })
      .add_node("b", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "b"));
      })
      .add_node("c", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "c"));
      })
      .add_edge("start", "b")
      .add_edge("b", kEnd)
      .add_edge("c", kEnd)
      .set_entry_point("start");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->at("visited"), Json::array({"c"}));  // b 未被执行
}

TEST(Graph, node_exception_becomes_node_error) {
  StateGraph g(trace_schema());
  g.add_node("boom", [](Json const&, NodeContext&) -> Command {
     throw std::runtime_error("kaboom");
   })
      .add_edge("boom", kEnd)
      .set_entry_point("boom");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::NodeError);
  EXPECT_NE(out.error().message.find("kaboom"), std::string::npos);
}

TEST(Graph, invalid_graphs_are_rejected_at_compile) {
  {
    StateGraph g(trace_schema());  // 没有节点
    EXPECT_FALSE(g.compile().has_value());
  }
  {
    StateGraph g(trace_schema());
    g.add_node("a", [](Json const&, NodeContext&) { return Command{}; });
    EXPECT_FALSE(g.compile().has_value());  // 未设入口
  }
  {
    StateGraph g(trace_schema());
    g.add_node("a", [](Json const&, NodeContext&) { return Command{}; })
        .add_edge("a", "ghost")  // 指向不存在的节点
        .set_entry_point("a");
    auto c = g.compile();
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().kind, GraphError::Kind::InvalidGraph);
  }
  {
    StateGraph g(trace_schema());  // 同节点既有静态边又有条件边
    g.add_node("a", [](Json const&, NodeContext&) { return Command{}; })
        .add_edge("a", kEnd)
        .add_conditional_edges("a", [](Json const&) { return std::vector<std::string>{kEnd}; })
        .set_entry_point("a");
    auto c = g.compile();
    ASSERT_FALSE(c.has_value());
    EXPECT_NE(c.error().message.find("both static and conditional"), std::string::npos);
  }
}

TEST(Graph, checkpoints_every_step_with_next) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) {
     return Command::with_update(append_visited(st, "a"));
   })
      .add_node("b", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "b"));
      })
      .add_edge("a", "b")
      .add_edge("b", kEnd)
      .set_entry_point("a");

  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object(), "thread-1");
  ASSERT_TRUE(out.has_value()) << out.error().message;

  auto const snaps = cp.list("thread-1", "", 0);
  ASSERT_EQ(snaps.size(), 2u);  // 每步一个快照
  EXPECT_EQ(snaps[1].metadata.at("node").get<std::string>(), "a");
  EXPECT_EQ(snaps[1].next, std::vector<state::Packet>{{"b"}});  // next 已记录
  EXPECT_EQ(snaps[0].metadata.at("node").get<std::string>(), "b");
  EXPECT_TRUE(snaps[0].next.empty());
}

TEST(Graph, resumes_from_latest_checkpoint) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) {
     return Command::with_update(append_visited(st, "a"));
   })
      .add_node("b", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "b"));
      })
      .add_node("c", [](Json const& st, NodeContext&) {
        return Command::with_update(append_visited(st, "c"));
      })
      .add_edge("a", "b")
      .add_edge("b", "c")
      .add_edge("c", kEnd)
      .set_entry_point("a");

  CompileOptions opts;
  opts.max_steps = 1;  // 只允许跑一步 → 触发上限，但留下快照
  auto partial = g.compile(&cp, opts);
  ASSERT_TRUE(partial.has_value());
  auto first = partial->invoke(Json::object(), "t-resume");
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().kind, GraphError::Kind::RecursionLimit);

  auto const snaps = cp.list("t-resume", "", 0);
  ASSERT_EQ(snaps.size(), 1u);
  EXPECT_EQ(snaps[0].next, std::vector<state::Packet>{{"b"}});

  // 换一个不限制步数的编译产物，从快照续跑（input 被忽略）
  auto full = g.compile(&cp);
  ASSERT_TRUE(full.has_value());
  auto resumed = full->invoke(Json{{"ignored", true}}, "t-resume");
  ASSERT_TRUE(resumed.has_value()) << resumed.error().message;
  EXPECT_EQ(resumed->at("visited"), Json::array({"a", "b", "c"}));
  EXPECT_FALSE(resumed->contains("ignored"));
}

TEST(Graph, state_stays_json_serializable) {
  MemoryCheckpointer cp;
  StateGraph g(trace_schema());
  g.add_node("a", [](Json const& st, NodeContext&) {
     Json out = append_visited(st, "a");
     out["nested"] = Json{{"k", Json::array({1, 2, 3})}};
     return Command::with_update(out);
   })
      .add_edge("a", kEnd)
      .set_entry_point("a");

  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object(), "t-json");
  ASSERT_TRUE(out.has_value());

  auto const snap = cp.get_tuple("t-json", "", std::nullopt);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(Json::parse(snap->values.dump()), snap->values);  // dump→parse 语义不变
}