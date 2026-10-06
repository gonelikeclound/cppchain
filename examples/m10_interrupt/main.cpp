// demo: m10_interrupt —— 引擎级 interrupt 原语 + resume（M10，纯离线）
//
// 验证主线：审批节点 interrupt(payload) 暂停 → 落 checkpoint → invoke 返回"挂着"态 →
// 外部给答复（这里用确定性答复，真实场景可换成 stdin / 审批命令 / 另一个进程）→
// resume 再 invoke → 该节点**从头重跑**、interrupt() 返回答复。
//
// 幂等契约（LangGraph 同款）：interrupt **之前**的副作用在重跑时会重复（下方
// side_effect_before 出现两次）；之后只执行一次。副作用应写在 interrupt 之后、或幂等。
#include <cppchain/graph/graph.hpp>
#include <cppchain/state/checkpointer.hpp>

#include <print>

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

int main() {
  MemoryCheckpointer cp;
  int side_effect_before = 0;  // interrupt 之前（重跑会重复）
  int side_effect_after = 0;   // interrupt 之后（只执行一次）

  StateGraph g(StateSchema{});
  g.add_node("approve", [&](Json const&, NodeContext& ctx) -> Command {
     ++side_effect_before;  // ← 副作用在 interrupt 之前：resume 重跑会重复
     auto const decision = interrupt(Json{{"txn", "T-42"}, {"amount", 100}}, ctx);
     ++side_effect_after;   // ← 副作用在 interrupt 之后：只执行一次
     return Command::with_update(
         Json{{"decision", decision}, {"approved", decision.at("ok")}});
   })
      .add_node("ship", [](Json const& st, NodeContext&) -> Command {
        return Command::with_update(
            Json{{"shipped", st.at("approved").get<bool>()}, {"onwards", true}});
      })
      .add_edge("approve", "ship")
      .add_edge("ship", kEnd)
      .set_entry_point("approve");

  auto compiled = g.compile(&cp);
  if (!compiled) {
    std::println("编译失败: {}", compiled.error().message);
    return 1;
  }

  // 第一次触发：approve 调 interrupt → 引擎悬挂（完整 state 返回，无 update 生效）
  auto r1 = compiled->invoke(Json{{"order_id", "O-1"}}, "t-approve");
  if (!r1) {
    std::println("invoke 失败: {}", r1.error().message);
    return 1;
  }
  auto const& pend = compiled->last_interrupt();
  std::println("=== 第 1 次触发：悬挂 ===");
  std::println("  last_interrupt: node={} payload={}", pend.node, pend.payload.dump());
  std::println("  副作用计数: before={} after={}（before 已执行、after 未执行）",
               side_effect_before, side_effect_after);

  // —— 外部审批：这里拿一个确定性答复模拟（真实场景：等用户/审批服务回复）
  Json const decision = {{"ok", true}, {"reviewer", "alice"}};
  std::println();
  std::println("（模拟外部审批: ok=true, reviewer=alice）");

  // 第 2 次触发：带 resume 恢复 → approve 节点从头重跑 → interrupt() 返回该答复
  auto r2 = compiled->invoke(Json::object(), "t-approve",
                             InvokeOptions{.resume = std::optional<Json>{decision}});
  if (!r2) {
    std::println("resume 失败: {}", r2.error().message);
    return 1;
  }
  bool const approved = (*r2).at("approved").get<bool>();
  bool const shipped = (*r2).at("shipped").get<bool>();
  std::println();
  std::println("=== 第 2 次触发：resume 恢复 ===");
  std::println("  决策写入: approve 返回 {}", (*r2).at("decision").dump());
  std::println("  终态: approved={} shipped={}", approved, shipped);
  std::println("  副作用计数: before={} after={}（before 重复、after 恰好一次）",
               side_effect_before, side_effect_after);
  std::println("  挂在峰值: {}", compiled->last_interrupt().interrupted ? "true" : "false");

  std::println();
  std::println("要点：interrupt 之前的副作用重跑会重复 —— 副作用必须放 interrupt 之后或幂等；");
  std::println("     悬挂态可随 checkpoint 落盘，跨进程恢复（见 tests/graph/interrupt_test）。");
}