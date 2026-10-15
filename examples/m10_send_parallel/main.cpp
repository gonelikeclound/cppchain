// demo: m10_send_parallel —— Send 并行波（wfr-㊾ 批②，纯离线）
//
// 验证主线：同一批 sends（size>1）在 max_parallel>1 时由 worker 线程并发执行：
//   ① 波 = 屏障：全体成员 join 后才继续，join 后按 send 声明序归约回主 state
//      → 与串行逐包归约的最终结果一致（确定性契约）；
//   ② 冲突护栏：两个成员写同一默认 overwrite 键 → 报 ConcurrentWriteConflict
//      （fail-closed，不默默后写覆盖前写）；累积类 reducer 同键则合法按序折叠；
//   ③ 并行波成员 interrupt 不支持 → InterruptUnavailable（不默默串行化）。
#include <cppchain/graph/graph.hpp>
#include <cppchain/state/state.hpp>

#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::StateSchema;

namespace {

// results 的 append reducer（左累计数组 ∪ 右增量）—— 累积类通道
Json push_results(Json const& left, Json const& right) {
  Json out = left.is_array() ? left : Json::array();
  if (right.is_array())
    for (auto const& e : right) out.push_back(e);
  else
    out.push_back(right);
  return out;
}

}  // namespace

int main() {
  // ---- ① 并行 map-reduce：orchestrator 扇出 4 个 worker，各写累积 results + 独立 outN ----
  StateSchema schema;
  schema.set_reducer("results", push_results);
  StateGraph g(schema);
  g.add_node("orchestrator", [](Json const& st, NodeContext&) -> Command {
     std::vector<Send> sends;
     for (auto const& item : st.at("items"))
       sends.push_back(Send{"worker", Json{{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("worker", [](Json const& input, NodeContext&) -> Command {
        std::string const name = input.at("item").get<std::string>();
        // 各自独立输出键（outN）+ 累积通道 results —— 不触发冲突护栏
        return Command::with_update(Json{{"out" + name, name},
                                         {"results", Json::array({Json{{"item", name}}})}});
      })
      .set_entry_point("orchestrator");

  CompileOptions opts;
  opts.max_parallel = 4;
  auto compiled = g.compile(nullptr, opts);
  if (!compiled) {
    std::println("编译失败: {}", compiled.error().message);
    return 1;
  }
  auto out = compiled->invoke(Json{{"items", Json::array({"a", "b", "c", "d"})}});
  if (!out) {
    std::println("invoke 失败: {}", out.error().message);
    return 1;
  }

  std::println("=== m10_send_parallel: 并行波（4 个 worker 并发 / {} 步）===",
               compiled->last_steps());
  std::string seq;
  for (auto const& item : (*out).at("results")) seq += item.at("item").get<std::string>();
  std::println("  results 按声明序: {}   max_parallel=4", seq);
  std::println("  outa..outd: {},{},{},{}（独立键全落）",
               (*out).at("outa").get<std::string>(), (*out).at("outb").get<std::string>(),
               (*out).at("outc").get<std::string>(), (*out).at("outd").get<std::string>());

  // ---- ② 冲突护栏演示：两个 worker 写同一默认 overwrite 键 → ConcurrentWriteConflict ----
  StateGraph gc(StateSchema{});
  gc.add_node("o", [](Json const& st, NodeContext&) -> Command {
      std::vector<Send> sends;
      for (auto const& item : st.at("items")) sends.push_back(Send{"w", Json{{"item", item}}});
      return Command::with_sends(std::move(sends));
    })
      .add_node("w", [](Json const& input, NodeContext&) -> Command {
        return Command::with_update(Json{{"shared", input.at("item")}});  // 默认 overwrite 键
      })
      .set_entry_point("o");
  CompileOptions oc;
  oc.max_parallel = 2;
  auto conflict = gc.compile(nullptr, oc);
  std::println();
  std::println("=== 冲突护栏：两个成员写同一默认键 'shared' ===");
  if (conflict) {
    auto bad = conflict->invoke(Json{{"items", Json::array({"x", "y"})}});
    if (!bad)
      std::println("  fail-closed → {}: {}", graph_error_kind(bad.error().kind),
                   bad.error().message);
  }

  std::println();
  std::println("要点：并行波 = 不可分割的屏障，join 后按声明序归约（确定性）；");
  std::println("     同键并发写 overwrite 通道直接报错 —— 逼出「并行步骤各写各的键/文件」；");
  std::println("     累积类 reducer（results/messages）同键合法。串行 max_parallel=1 行为不变。");
  return 0;
}