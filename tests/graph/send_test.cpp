// M10 测试：Send —— 动态 fan-out：Send(node, partial_state) 展开为一个独立"包"，
// 每个包读自己的局部输入，结果经 per-key reducer 归约回主 state（本轮顺序执行）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

// results 的 append reducer（左累计数组 ∪ 右增量）
Json push_results(Json const& left, Json const& right) {
  Json out = left.is_array() ? left : Json::array();
  if (right.is_array())
    for (auto const& e : right) out.push_back(e);
  else
    out.push_back(right);
  return out;
}

StateSchema fanout_schema() {
  StateSchema s;
  s.set_reducer("results", push_results);
  return s;
}

// map-reduce 三件套：orchestrator（sends）→ worker（写 results）→（worker 无出边自然收）
// 注意：Send 扇出的 worker 跑完后如何"归拢"需要 barrier 语义，本轮不做 ——
// 归约发生在共享 state 上，由 reducer 累积；汇聚节点留待真并行/将来。
struct MapReduce {
  StateGraph g;
  explicit MapReduce() : g(fanout_schema()) {
    g.add_node("orchestrator", [](Json const& st, NodeContext&) {
       std::vector<Send> sends;
       for (auto const& item : st.at("items")) sends.push_back(Send{"worker", {{"item", item}}});
       return Command::with_sends(std::move(sends));
     });
    g.add_node("worker", [](Json const& input, NodeContext&) {
       // 输入视图 = 本包的局部 state
       return Command::with_update(
           {{"results", Json::array({Json{{"item", input.at("item")}}})}});
     });
    g.set_entry_point("orchestrator");
  }
};

}  // namespace

TEST(Send, fan_out_and_reducer_merge) {
  MapReduce mr;
  auto compiled = mr.g.compile();
  ASSERT_TRUE(compiled.has_value());

  auto const out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_TRUE(out.has_value());
  auto const& results = out->at("results");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].at("item"), "a");  // 顺序执行确定性：a 先 b 后
  EXPECT_EQ(results[1].at("item"), "b");
  // partial state 只是输入视图，不主动合并进主 state
  EXPECT_FALSE(out->contains("item"));
  // 包数 = 编排器 1 + 两个 worker = 3 步
  EXPECT_EQ(compiled->last_steps(), 3u);
}

TEST(Send, partial_state_is_node_input_only) {
  StateSchema schema = fanout_schema();
  std::string seen;
  StateGraph g(schema);
  g.add_node("orchestrator", [](Json const&, NodeContext&) {
     return Command::with_sends({Send{"worker", {{"item", "a"}}}});
   })
      .add_node("worker", [&](Json const& input, NodeContext&) {
     // 只看得到局部输入，看不到全局键
     seen = input.dump();
     return Command::with_update({{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("orchestrator");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  Json input;
  input["items"] = Json::array();  // 主 state 的全局通道（worker 不应看到）
  input["global"] = "bar";
  auto const out = compiled->invoke(std::move(input));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(seen, R"({"item":"a"})") << "worker 的输入视图应恰好是该包的局部 state";
  EXPECT_EQ(out->at("global"), "bar");
}

TEST(Send, target_node_unknown_is_error) {
  MapReduce mr;  // orchestrator/worker 已注册
  // 手工构造一个 sends 到未知节点的图
  StateGraph g(StateSchema{});
  g.add_node("o", [](Json const&, NodeContext&) {
     return Command::with_sends({Send{"ghost", {{"x", 1}}}});
   })
      .set_entry_point("o");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::UnknownNode);
}

TEST(Send, goto_takes_precedence_over_sends) {
  std::vector<std::string> ran;
  StateGraph g(StateSchema{});
  g.add_node("router", [](Json const&, NodeContext&) {
     // goto 与 sends 并存 → 契约：仅 goto 生效
     return Command{Json{{"n", 1}}, {"dest"}, std::vector<Send>{Send{"discarded", {}}}};
   })
      .add_node("dest", [&](Json const&, NodeContext&) {
     ran.push_back("dest");
     return Command::with_update(Json::object());
   })
      .add_node("discarded", [&](Json const&, NodeContext&) {
     ran.push_back("discarded");
     return Command::with_update(Json::object());
   })
      .set_entry_point("router");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  ASSERT_TRUE(compiled->invoke(Json::object()).has_value());
  EXPECT_EQ(ran, (std::vector<std::string>{"dest"}));
}

TEST(Send, send_overrides_static_edge) {
  std::vector<std::string> ran;
  StateGraph g(StateSchema{});
  g.add_node("fan", [](Json const&, NodeContext&) {
     return Command::with_sends({Send{"w", {{"k", 1}}}});
   })
      .add_node("w", [&](Json const&, NodeContext&) {
     ran.push_back("w");
     return Command::with_update(Json::object());
   })
      .add_node("via_static", [&](Json const&, NodeContext&) {
     ran.push_back("via_static");
     return Command::with_update(Json::object());
   })
      .add_edge("fan", "via_static")  // 静态边存在，但 sends 覆盖它
      .set_entry_point("fan");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  ASSERT_TRUE(compiled->invoke(Json::object()).has_value());
  EXPECT_EQ(ran, (std::vector<std::string>{"w"}));
}

TEST(Send, multi_level_fanout) {
  StateGraph g(fanout_schema());
  std::vector<std::string> trace;
  g.add_node("root", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"mgr", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("mgr", [&](Json const& input, NodeContext&) {
     std::string const tag = input.at("item").get<std::string>();
     trace.push_back("mgr:" + tag);
     // 二级扇出：每个 mgr 拆两个 worker
     return Command::with_sends(
         {Send{"worker", {{"item", tag + "L"}}}, Send{"worker", {{"item", tag + "R"}}}});
   })
      .add_node("worker", [](Json const& input, NodeContext&) {
     return Command::with_update(
         {{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("root");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());

  // 每包一个结果 → 包数 + 编排器 = 4 步（root+mgr×?…）：root(0) → mgr(1,2) → worker×4(3,4,5,6)
  auto const out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->at("results").size(), 4u);  // 2 mgr × 2 worker
  EXPECT_EQ(trace.size(), 2u);
  EXPECT_EQ(trace[0], "mgr:a");
  EXPECT_EQ(trace[1], "mgr:b");
}

TEST(Send, checkpoint_next_carries_packets) {
  MemoryCheckpointer cp;
  MapReduce mr;

  // 上限 1：orchestrator 扇出后只跑一个 worker 就停，快照要能承载"包"（含局部 state）
  CompileOptions tight;
  tight.max_steps = 1;
  auto partial = mr.g.compile(&cp, tight);
  ASSERT_TRUE(partial.has_value());
  auto first = partial->invoke({{"items", Json::array({"a", "b"})}}, "t-fan");
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().kind, GraphError::Kind::RecursionLimit);

  auto const snaps = cp.list("t-fan", "", 0);
  ASSERT_FALSE(snaps.empty());
  bool saw_packet = false;
  for (auto const& p : snaps[0].next)
    if (p.node == "worker" && p.state.has_value()) saw_packet = true;
  EXPECT_TRUE(saw_packet) << "快照的 next 应以包形式承载 Send 目标（node + 局部 state）";

  // 不限步恢复：从最新快照继续把剩余包跑完
  auto full = mr.g.compile(&cp);
  ASSERT_TRUE(full.has_value());
  auto resumed = full->invoke(Json::object(), "t-fan");
  ASSERT_TRUE(resumed.has_value()) << resumed.error().message;
  EXPECT_EQ(resumed->at("results").size(), 2u);
}

// 回归修复：Send 扇入**同一节点**时，resume 只注入「重跑包」（is_restart 标记）；
// 队列里同节点的其它包调 interrupt 时应**再次悬挂**，而不是被上一个 resume 值误穿过。
TEST(Send, resume_marker_targets_only_restart_packet) {
  MemoryCheckpointer cp;
  std::vector<std::string> done;                           // interrupt 返回后（resume 生效）
  std::vector<std::string> pending;                        // 首见即被悬挂
  StateGraph g(StateSchema{});
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", Json{{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [&](Json const& input, NodeContext& ctx) {
        std::string const tag = input.at("item").get<std::string>();
        if (!ctx.pending_resume) pending.push_back(tag);   // 首见：即将被悬挂
        auto const v = interrupt(Json{{"tag", tag}}, ctx);  // 首调抛、重跑返回注入值
        done.push_back(tag);
        return Command::with_update(Json{{"done", tag}, {"resume", v}});
      })
      .set_entry_point("o");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  // ① 首次：w(X) 悬挂，w(Y) 还在队列
  (void)compiled->invoke(Json{{"items", Json::array({"X", "Y"})}}, "t-fan");
  ASSERT_TRUE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(compiled->last_interrupt().payload.at("tag"), "X");
  EXPECT_EQ(pending, (std::vector<std::string>{"X"}));

  // ② resume：w(Y)【不是】重跑包 → 不注入 → 自己再次悬挂（旧实现会误注入让它穿过）
  (void)compiled->invoke(Json::object(), "t-fan",
                         InvokeOptions{nullptr, EventMode::Updates, Json{{"v", "r"}},
                                       std::nullopt});
  EXPECT_TRUE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(compiled->last_interrupt().payload.at("tag"), "Y") << "w(Y) 应自身悬挂，而非被误注入";
  EXPECT_EQ(pending, (std::vector<std::string>{"X", "Y"}));
  EXPECT_TRUE(done.empty());

  // ③ 再 resume：两个重跑包依次拿到注入值完成
  auto out = compiled->invoke(Json::object(), "t-fan",
                              InvokeOptions{nullptr, EventMode::Updates, Json{{"v", "r2"}},
                                            std::nullopt});
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(done, (std::vector<std::string>{"X", "Y"}));
  EXPECT_EQ((*out)["done"], "Y");
}

// super-step 内第二个包 interrupt：恢复后剩余包先跑、中断节点最后重跑
TEST(Send, resume_after_partial_super_step) {
  MemoryCheckpointer cp;
  std::vector<std::string> trace;
  StateGraph g(fanout_schema());
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [&](Json const& input, NodeContext& ctx) {
     std::string const tag = input.at("item").get<std::string>();
     if (tag == "b") (void)interrupt({{"ask", tag}}, ctx);  // 第二个包悬挂
     trace.push_back("w:" + tag);
     return Command::with_update({{"results", Json{{"item", tag}}}});
   })
      .set_entry_point("o");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  (void)compiled->invoke(Json{{"items", Json::array({"a", "b", "c"})}}, "t-mix");
  // 悬挂时：a 已跑，b 首跑悬挂（b 的尾部没执行）
  EXPECT_TRUE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(compiled->last_interrupt().node, "w");
  EXPECT_EQ(trace, (std::vector<std::string>{"w:a"}));

  auto resumed = compiled->invoke(
      Json::object(), "t-mix", InvokeOptions{.resume = Json{{"ok", true}}});
  ASSERT_TRUE(resumed.has_value()) << "resume 失败: "
                                   << graph_error_kind(resumed.error().kind) << " — "
                                   << resumed.error().message;
  // 剩余包（c）先跑，中断节点（b）最后重跑并拿到 resume
  EXPECT_EQ(resumed->at("results").size(), 3u);
  EXPECT_EQ(trace, (std::vector<std::string>{"w:a", "w:c", "w:b"})) << "剩余包先于重跑包";
  EXPECT_FALSE(compiled->last_interrupt().interrupted);
  auto const& results = resumed->at("results");
  EXPECT_EQ(results[0].at("item"), "a");
  EXPECT_EQ(results[1].at("item"), "c");
  EXPECT_EQ(results[2].at("item"), "b");
}