// demo: m10_fork —— 时间旅行分叉（time travel / fork，M10，纯离线）
//
// 验证主线：先在 thread-A 跑完一整条轨迹（落一串 checkpoint）→ 打印快照链 →
// 重新打开（simulate"需求/环境变了"，这里改一个闭包捕获的 theme）→ 从 A 的**中间
// checkpoint**（tool 节点之前）分叉到 thread-B 继续 → 拿到"不同未来"，且 A 轨迹不被污染。
//
// 这是 LangGraph 时间旅行的核心：任意 checkpoint 上分叉探索不同轨迹。
#include <cppchain/graph/graph.hpp>
#include <cppchain/state/checkpointer.hpp>

#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

int main() {
  MemoryCheckpointer cp;
  std::string theme = "default";  // 外部"需求/环境"（分叉后改变 → 得到不同未来）

  StateGraph g(StateSchema{});
  g.add_node("setup", [](Json const&, NodeContext&) -> Command {
     return Command::with_update(Json{{"phase", "setup_done"}});
   })
      .add_node("plan", [&](Json const&, NodeContext&) -> Command {
        // 依赖外部环境：thread-A 用旧 theme 跑、分叉后 thread-B 用新 theme 重算
        return Command::with_update(Json{{"plan", "plan/" + theme}});
      })
      .add_node("finish", [](Json const&, NodeContext&) -> Command {
        return Command::with_update(Json{{"done", true}});
      })
      .add_edge("setup", "plan")
      .add_edge("plan", "finish")
      .add_edge("finish", kEnd)
      .set_entry_point("setup");

  auto compiled = g.compile(&cp);
  if (!compiled) {
    std::println("编译失败: {}", compiled.error().message);
    return 1;
  }

  // 1) thread-A 完整跑一遍
  auto r1 = compiled->invoke(Json{{"order", "O-77"}}, "thread-A");
  if (!r1) {
    std::println("invoke 失败: {}", r1.error().message);
    return 1;
  }
  std::println("=== thread-A: 完整轨迹（theme={}）===", theme);
  std::println("  终态 plan = {}", (*r1).at("plan").get<std::string>());
  std::println("  快照链（新在前）:");
  int i = 0;
  std::string fork_point;
  for (auto const& s : cp.list("thread-A", "", 0)) {
    std::string const node = s.metadata.value("node", "");
    auto const id = s.config.value("checkpoint_id", "");
    std::println("    #{} ckpt={} node={}", ++i, id, node);
    if (node == "setup") fork_point = id;  // 记住"plan 之前"的分叉点
  }

  // 2) 环境变了（模拟：需求/数据变化）
  theme = "expanded";
  std::println();
  std::println("=== 环境变化: theme={}（从 setup 之后、plan 之前分叉）===", theme);

  // 3) 时间旅行分叉：从 thread-A 的 setup 快照继续，落到 thread-B（原轨迹不动）
  if (fork_point.empty()) {
    std::println("未找到分叉点");
    return 1;
  }
  auto r2 = compiled->invoke(Json::object(), "thread-B",
                             InvokeOptions{.fork = ForkSpec{"thread-A", fork_point}});
  if (!r2) {
    std::println("fork 失败: {}", r2.error().message);
    return 1;
  }
  std::println("  thread-B 终态 plan = {}", (*r2).at("plan").get<std::string>());
  auto const bs = cp.list("thread-B", "", 0);
  std::println("  thread-B 快照 {} 个；链根（最老快照）的 parent = {}（= 分叉点）", bs.size(),
               bs.empty() ? std::string{"?"}
                          : (bs.back().parent_checkpoint_id
                                 ? *bs.back().parent_checkpoint_id
                                 : std::string{"(无)"}));
  std::println("  thread-A 快照 {} 个（未被污染）", cp.list("thread-A", "", 0).size());

  std::println();
  std::println("要点：fork = 从任意历史 checkpoint 恢复（state+next）并继续，新轨迹落新线程、");
  std::println("     原轨迹保留 —— 同一张图可以背对背探索多条未来。");
}