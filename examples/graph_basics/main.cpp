// demo: graph_basics —— 极简 Graph 引擎（M6，纯离线）
//
// langchain 参照（LangGraph）：StateGraph + Node/Edge/ConditionalEdge + compile + checkpoint。
// 演示四件事：线性 workflow、条件路由、循环上限、**从中途快照恢复续跑**。
#include <cppchain/graph/graph.hpp>

#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

Json push(Json const& state, std::string const& item) {
  Json out = state.is_object() ? state : Json::object();
  if (!out.contains("trace")) out["trace"] = Json::array();
  out["trace"].push_back(item);
  return out;
}

void show_trace(char const* label, Json const& state) {
  std::print("{}: ", label);
  for (auto const& x : state.at("trace")) std::print("{} → ", x.get<std::string>());
  std::println("END");
}

}  // namespace

int main() {
  // ---- 1. 线性 workflow：确定性步骤串起来 ----
  std::println("=== 1. 线性 workflow（确定性步骤）===");
  {
    StateGraph g(StateSchema{});
    g.add_node("validate", [](Json const& st, NodeContext&) {
       return Command::with_update(push(st, "validate"));
     })
        .add_node("enrich", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "enrich"));
        })
        .add_node("store", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "store"));
        })
        .add_edge("validate", "enrich")
        .add_edge("enrich", "store")
        .add_edge("store", kEnd)
        .set_entry_point("validate");

    auto compiled = g.compile();
    auto out = compiled->invoke(Json::object());
    show_trace("  ", *out);
  }

  // ---- 2. 条件路由：router 读 state 决定走向（M4 的 classify 就是这个位置） ----
  std::println("\n=== 2. 条件路由 ===");
  {
    StateGraph g(StateSchema{});
    g.add_node("triage", [](Json const& st, NodeContext&) {
       return Command::with_update(push(st, "triage"));
     })
        .add_node("handle_weather", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "handle_weather"));
        })
        .add_node("handle_other", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "handle_other"));
        })
        // 真实场景里这里会调用 StructuredOutput::classify（M4）
        .add_conditional_edges("triage", [](Json const& st) -> std::vector<std::string> {
          return st.value("intent", "") == "weather" ? std::vector<std::string>{"handle_weather"}
                                                     : std::vector<std::string>{"handle_other"};
        })
        .add_edge("handle_weather", kEnd)
        .add_edge("handle_other", kEnd)
        .set_entry_point("triage");

    auto compiled = g.compile();
    show_trace("  intent=weather", *compiled->invoke(Json{{"intent", "weather"}}));
    show_trace("  intent=stock  ", *compiled->invoke(Json{{"intent", "stock"}}));
  }

  // ---- 3. 循环 + 步数上限（防失控）----
  std::println("\n=== 3. 循环与步数上限 ===");
  {
    StateGraph g(StateSchema{});
    g.add_node("refine", [](Json const& st, NodeContext&) {
       Json out = push(st, "refine");
       out["round"] = st.value("round", 0) + 1;
       return Command::with_update(out);
     })
        .add_conditional_edges("refine", [](Json const& st) -> std::vector<std::string> {
          return st.value("round", 0) < 3 ? std::vector<std::string>{"refine"}
                                          : std::vector<std::string>{kEnd};
        })
        .set_entry_point("refine");

    CompileOptions opts;
    opts.max_steps = 10;
    auto compiled = g.compile(nullptr, opts);
    show_trace("  自我精炼 3 轮", *compiled->invoke(Json::object()));

    // 上限守卫：把上限压到 2 → 报 RecursionLimit（而不是死循环）
    CompileOptions tight;
    tight.max_steps = 2;
    auto guarded = g.compile(nullptr, tight);
    auto bad = guarded->invoke(Json::object());
    std::println("  上限=2 时: {} ({})", bad ? "通过" : bad.error().message,
                 bad ? "-" : graph_error_kind(bad.error().kind));
  }

  // ---- 4. checkpoint：中途恢复续跑 ----
  std::println("\n=== 4. checkpoint 与中途恢复 ===");
  {
    MemoryCheckpointer cp;
    StateGraph g(StateSchema{});
    g.add_node("step1", [](Json const& st, NodeContext&) {
       return Command::with_update(push(st, "step1"));
     })
        .add_node("step2", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "step2"));
        })
        .add_node("step3", [](Json const& st, NodeContext&) {
          return Command::with_update(push(st, "step3"));
        })
        .add_edge("step1", "step2")
        .add_edge("step2", "step3")
        .add_edge("step3", kEnd)
        .set_entry_point("step1");

    // 第一次：把上限压到 1 → 只跑 step1 就停（模拟"中断"），快照留下
    CompileOptions tight;
    tight.max_steps = 1;
    auto partial = g.compile(&cp, tight);
    auto stopped = partial->invoke(Json::object(), "thread-42");
    std::println("  跑到上限: {} ✓（已落 {} 个快照）",
                 stopped ? "未触发" : graph_error_kind(stopped.error().kind),
                 cp.list("thread-42", "", 0).size());

    auto const snap = cp.get_tuple("thread-42", "", std::nullopt);
    std::println("  最新快照: step={} next={}", snap->metadata.at("step").get<int>(),
                 Json(snap->next).dump());

    // 第二次：同一 thread 续跑（input 被忽略，从快照的 next 继续）
    auto full = g.compile(&cp);
    auto resumed = full->invoke(Json::object(), "thread-42");
    show_trace("  续跑完成", *resumed);
    std::println("  该线程快照总数: {}", cp.list("thread-42", "", 0).size());
  }

  std::println("\n要点：每步一个快照（含 next）→ 恢复语义 = 回到该步边界继续；"
               "状态全程 JSON → 快照可直接落盘/跨进程。");
  return 0;
}