// wfr-㊾ 批② 测试：Send 并行波。
// 一个 Command 的一批 sends（size>1）在 max_parallel>1 时由 worker 线程并发执行：
//   · 波 = 屏障（全体 join 后才继续），join 后**按 send 声明序**归约回主 state →
//     并行与串行的最终 state 一致（确定性契约）；
//   · 冲突护栏：wave 内 ≥2 成员写同一键、且该键是默认 overwrite → ConcurrentWriteConflict
//     （fail-closed）；累积类自定义 reducer（results 等）按声明序折叠合法；
//   · 并行波成员 interrupt 不支持 → InterruptUnavailable（不默默串行化）；
//   · 结构事件（node_start/node_end）由主线程按声明序发射 → 日志重放不分叉；
//   · max_parallel=1 时完全走既有串行路径（一字不改）。
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/state/state.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

// results 的 append reducer（左累计数组 ∪ 右增量）——累积类通道（send_test 同款）
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

// 标准 fan-out：orchestrator 按 items 扇出 N 个 worker，每个写 results 累积 + 尝试写 shared
struct WaveMR {
  StateGraph g;
  explicit WaveMR(StateSchema schema = fanout_schema()) : g(std::move(schema)) {
    g.add_node("orchestrator", [](Json const& st, NodeContext&) {
       std::vector<Send> sends;
       for (auto const& item : st.at("items")) sends.push_back(Send{"worker", {{"item", item}}});
       return Command::with_sends(std::move(sends));
     });
    g.add_node("worker", [](Json const& input, NodeContext&) {
       return Command::with_update(
           {{"results", Json::array({Json{{"item", input.at("item")}}})}});
     });
    g.set_entry_point("orchestrator");
  }
};

// 一次跑选的并行度
std::optional<Json> run_with(StateGraph& g, Json const& input,
                             std::size_t max_parallel = 1) {
  CompileOptions opts;
  opts.max_parallel = max_parallel;
  auto compiled = g.compile(nullptr, opts);
  if (!compiled) return std::nullopt;
  auto out = compiled->invoke(input);
  if (!out) return std::nullopt;
  return *out;
}

}  // namespace

TEST(SendParallel, wave_result_equals_serial) {
  WaveMR mr;
  Json input{{"items", Json::array({"a", "b", "c", "d"})}};

  auto serial = run_with(mr.g, input, 1);
  auto par = run_with(mr.g, input, 4);
  ASSERT_TRUE(serial.has_value());
  ASSERT_TRUE(par.has_value());

  // 确定性契约：并行按声明序归约 → 最终 state 与串行逐包归约完全相等
  EXPECT_EQ(*par, *serial);
  ASSERT_EQ(par->at("results").size(), 4u);
  EXPECT_EQ(par->at("results")[0].at("item"), "a");  // 声明序保持
  EXPECT_EQ(par->at("results")[3].at("item"), "d");
}

TEST(SendParallel, actually_runs_in_parallel) {
  // 用「同时活跃 ≥2」的并发窗口证明真的并行（不靠计时，避免 CI 抖动）：
  // 每个 worker 进入即 active++，sleep 期间保持活跃，max_seen 记录峰值。
  std::atomic<int> active(0), max_seen(0);
  StateSchema s = fanout_schema();
  StateGraph g(s);
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [&](Json const& input, NodeContext&) {
     int cur = active.fetch_add(1) + 1;
     int seen = max_seen.load();
     while (cur > seen && !max_seen.compare_exchange_weak(seen, cur)) {
       // CAS 更新峰值
     }
     std::this_thread::sleep_for(std::chrono::milliseconds(15));  // 保持活跃窗口
     active.fetch_sub(1);
     return Command::with_update(
         {{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("o");
  auto out = run_with(g, {{"items", Json::array({"a", "b"})}}, 2);
  ASSERT_TRUE(out.has_value());
  EXPECT_GE(max_seen.load(), 2) << "两个 worker 应同时活跃 —— 波确实并发执行";
  EXPECT_EQ(out->at("results").size(), 2u);
}

TEST(SendParallel, width_one_is_serial_fifo) {
  // max_parallel=1（默认）：串行语义 —— 每个 worker 的 step 递增、无并发窗口
  std::atomic<int> active(0), max_seen(0);
  StateSchema s = fanout_schema();
  StateGraph g(s);
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [&](Json const& input, NodeContext&) {
     int cur = active.fetch_add(1) + 1;
     int seen = max_seen.load();
     while (cur > seen && !max_seen.compare_exchange_weak(seen, cur)) {
     }
     std::this_thread::sleep_for(std::chrono::milliseconds(2));
     active.fetch_sub(1);
     return Command::with_update({{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("o");
  auto out = run_with(g, {{"items", Json::array({"a", "b", "c"})}});  // 默认 1
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(max_seen.load(), 1) << "串行：任何时刻最多一个 worker 活跃";
  EXPECT_EQ(out->at("results").size(), 3u);
}

TEST(SendParallel, same_overwrite_key_conflicts) {
  // 两个 worker 写同一 overwrite 键 → fail-closed：ConcurrentWriteConflict，点名两节点
  StateGraph g(fanout_schema());
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext&) {
     return Command::with_update({{"shared", input.at("item")}});  // 默认 overwrite 键
   })
      .set_entry_point("o");
  CompileOptions opts;
  opts.max_parallel = 2;
  auto compiled = g.compile(nullptr, opts);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::ConcurrentWriteConflict);
  EXPECT_NE(out.error().message.find("'shared'"), std::string::npos);
  EXPECT_NE(out.error().message.find("w"), std::string::npos);
}

TEST(SendParallel, accumulate_reducer_shared_key_is_folded) {
  // 同键但自定义累积 reducer → 合法，按声明序折叠（与串行一致）
  StateGraph g(fanout_schema());  // results 用 push_results
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext&) {
     return Command::with_update({{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("o");
  CompileOptions opts;
  opts.max_parallel = 2;
  auto compiled = g.compile(nullptr, opts);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->at("results").size(), 2u);
  EXPECT_EQ(out->at("results")[0].at("item"), "a");  // 声明序
  EXPECT_EQ(out->at("results")[1].at("item"), "b");
}

TEST(SendParallel, distinct_keys_all_land) {
  // 各成员写独立 overwrite 键 → 全部落
  StateGraph g(fanout_schema());
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext&) {
     std::string const key = "out" + input.at("item").get<std::string>();
     return Command::with_update({{key, input.at("item")}});
   })
      .set_entry_point("o");
  auto out = run_with(g, {{"items", Json::array({"a", "b", "c"})}}, 3);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->at("outa"), "a");
  EXPECT_EQ(out->at("outb"), "b");
  EXPECT_EQ(out->at("outc"), "c");
}

TEST(SendParallel, event_order_follows_send_order) {
  // 结构事件按声明序发射（join 后主线程按序发射 node_end）→ 会话日志重放不分叉；
  // seq 单调且在最后一轮收齐 values。
  StateGraph g(fanout_schema());
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext&) {
     return Command::with_update({{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("o");
  CompileOptions opts;
  opts.max_parallel = 3;
  auto compiled = g.compile(nullptr, opts);
  ASSERT_TRUE(compiled.has_value());

  auto events = compiled->stream({{"items", Json::array({"x", "y", "z"})}});

  // node_start / node_end 的节点名序列都必须 = orchestrator + sends 声明序（波内按序）
  std::vector<std::string> start_nodes, end_nodes;
  std::uint64_t prev_seq = 0;
  bool monotonic = true;
  for (auto const& e : events) {
    if (e.type == "node_start") start_nodes.push_back(e.node);
    if (e.type == "node_end") end_nodes.push_back(e.node);
    if (e.seq <= prev_seq) monotonic = false;
    prev_seq = e.seq;
  }
  EXPECT_EQ(start_nodes, (std::vector<std::string>{"o", "w", "w", "w"}));
  EXPECT_EQ(end_nodes, (std::vector<std::string>{"o", "w", "w", "w"}))
      << "并行波 node_end 也按声明序发射（join 后主线程按序）";
  // 每个 node_end 的 step = 该成员在波内的步号（orchestrator 0 → 波 1,2,3）
  std::vector<int> end_steps;
  for (auto const& e : events)
    if (e.type == "node_end") end_steps.push_back(e.step);
  EXPECT_EQ(end_steps, (std::vector<int>{0, 1, 2, 3}));
  EXPECT_TRUE(monotonic) << "stream 事件 seq 必须严格单调";
}

TEST(SendParallel, member_interrupt_rejected) {
  // 波成员 interrupt → fail-closed InterruptUnavailable（不默默串行化）
  MemoryCheckpointer cp;
  StateGraph g(StateSchema{});
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext& ctx) {
     (void)interrupt(Json{{"ask", input}}, ctx);  // 并行下应被拒
     return Command::with_update(Json::object());
   })
      .set_entry_point("o");
  CompileOptions opts;
  opts.max_parallel = 2;
  auto compiled = g.compile(&cp, opts);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke({{"items", Json::array({"a", "b"})}}, "t-par");
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::InterruptUnavailable);
}

TEST(SendParallel, member_exception_reports_first_in_order) {
  // 两个成员都抛 → 按声明序报首个；消息带节点名
  StateGraph g(fanout_schema());
  g.add_node("o", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"w", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("w", [](Json const& input, NodeContext&) {
     if (input.at("item") == "a") throw std::runtime_error("boom-a");
     if (input.at("item") == "b") throw std::runtime_error("boom-b");
     return Command::with_update(Json::object());
   })
      .set_entry_point("o");
  CompileOptions opts;
  opts.max_parallel = 2;
  auto compiled = g.compile(nullptr, opts);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::NodeError);
  EXPECT_NE(out.error().message.find("boom-a"), std::string::npos);
  EXPECT_NE(out.error().message.find("'w'"), std::string::npos);
}

TEST(SendParallel, nested_fanout_deterministic) {
  // root → sends[mgr a, mgr b] 并行；mgr 各自再 sends[worker×2]。
  // 最终 results 顺序确定：a 的子结果先于 b 的（成员序 + 内序）。
  StateSchema s = fanout_schema();
  StateGraph g(s);
  g.add_node("root", [](Json const& st, NodeContext&) {
     std::vector<Send> sends;
     for (auto const& item : st.at("items")) sends.push_back(Send{"mgr", {{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("mgr", [](Json const& input, NodeContext&) {
     std::string const tag = input.at("item").get<std::string>();
     return Command::with_sends(
         {Send{"worker", {{"item", tag + "L"}}}, Send{"worker", {{"item", tag + "R"}}}});
   })
      .add_node("worker", [](Json const& input, NodeContext&) {
     return Command::with_update(
         {{"results", Json{{"item", input.at("item")}}}});
   })
      .set_entry_point("root");
  auto out = run_with(g, {{"items", Json::array({"a", "b"})}}, 2);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->at("results").size(), 4u);
  EXPECT_EQ(out->at("results")[0].at("item"), "aL");
  EXPECT_EQ(out->at("results")[1].at("item"), "aR");
  EXPECT_EQ(out->at("results")[2].at("item"), "bL");
  EXPECT_EQ(out->at("results")[3].at("item"), "bR");
}

TEST(SendParallel, wave_respects_max_steps) {
  // 波内成员各计一步：本波会超界 → 提前 RecursionLimit（与串行「跑完超界」语义对齐）
  WaveMR mr;
  CompileOptions tight;
  tight.max_parallel = 2;
  tight.max_steps = 1;  // 波 2 成员 → 0+2 > 1 → RecursionLimit
  auto compiled = mr.g.compile(nullptr, tight);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke({{"items", Json::array({"a", "b"})}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::RecursionLimit);
}

TEST(SendParallel, checkpoint_after_wave_and_resume) {
  // 带 checkpointer 的并行 run：波作为一个整体落一个快照，恢复后继续执行剩余包。
  MemoryCheckpointer cp;
  WaveMR mr;

  CompileOptions tight;
  tight.max_parallel = 2;
  tight.max_steps = 1;  // orchestrator(0) 后本波要超界 → 提前报错并留下 checkpoint（next 含 worker 包）
  auto partial = mr.g.compile(&cp, tight);
  ASSERT_TRUE(partial.has_value());
  auto first = partial->invoke(Json{{"items", Json::array({"a", "b"})}}, "t-par");
  (void)first;
  // orchestrator 已落快照：next 里应有 worker 包（并行选项不影响快照承载）
  auto const snaps = cp.list("t-par", "", 0);
  ASSERT_FALSE(snaps.empty());
  bool saw_packet = false;
  for (auto const& p : snaps[0].next)
    if (p.node == "worker") saw_packet = true;
  EXPECT_TRUE(saw_packet) << "快照 next 应以包形式承载 Send 目标";

  // 放宽限制恢复：完成剩余包（并行波）
  CompileOptions full;
  full.max_parallel = 2;
  auto resumed = mr.g.compile(&cp, full);
  ASSERT_TRUE(resumed.has_value());
  auto done = resumed->invoke(Json::object(), "t-par");
  ASSERT_TRUE(done.has_value()) << done.error().message;
  EXPECT_EQ(done->at("results").size(), 2u);
}