// demo: m10_send —— Send 动态 fan-out（M10，纯离线）
//
// 验证主线：orchestrator 返回 Send(node, partial_state) 列表 → 每个目标展开为一个
// 独立"包"（读自己的局部输入），执行结果经 push reducer 归约回主 state 的 results。
// 本轮顺序执行（接口/数据结构按可并行设计，真线程池押后）。
//
// 注意：Send 扇出的 worker 各自跑完后如何"归拢"到汇聚节点需要 barrier 语义，
// 本轮不做（归约在共享 state 上由 reducer 累积）。
#include <cppchain/graph/graph.hpp>
#include <cppchain/state/state.hpp>

#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::graph;
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

}  // namespace

int main() {
  StateSchema schema;
  schema.set_reducer("results", push_results);
  StateGraph g(schema);

  g.add_node("orchestrator", [](Json const& st, NodeContext&) -> Command {
     // 动态 fan-out：items 的每一项成为一个独立包（局部输入 = {item}）
     std::vector<Send> sends;
     for (auto const& item : st.at("items"))
       sends.push_back(Send{"worker", Json{{"item", item}}});
     return Command::with_sends(std::move(sends));
   })
      .add_node("worker", [](Json const& input, NodeContext&) -> Command {
        // 输入视图 = 本包的局部 state（看不到全局通道）
        return Command::with_update(
            Json{{"results",
                  Json::array({Json{{"item", input.at("item")},
                                    {"length", std::string(input.at("item").get<std::string>())
                                                   .size()}}})}});
      })
      .set_entry_point("orchestrator");

  auto compiled = g.compile();
  if (!compiled) {
    std::println("编译失败: {}", compiled.error().message);
    return 1;
  }

  auto out = compiled->invoke(Json{{"items", Json::array({"苹果", "banana", "c++"})}});
  if (!out) {
    std::println("invoke 失败: {}", out.error().message);
    return 1;
  }

  std::println("=== m10_send: 动态 fan-out（3 个 worker 包，顺序执行 / {} 步）===",
               compiled->last_steps());
  for (auto const& item : (*out).at("results")) {
    std::println("  - item={:<8} length={}", item.at("item").get<std::string>(),
                 item.at("length").get<std::size_t>());
  }
  std::println();
  std::println("要点：Send 目标带独立局部输入（worker 读不到全局通道）；");
  std::println("     结果经 reducer 归约回主 state —— 接口按可并行设计。");
}