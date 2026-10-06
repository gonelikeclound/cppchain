// M10 测试：Interrupt —— 引擎级 interrupt(payload) 原语。
// 语义：节点内幂等式暂停 → 落 checkpoint → 外部 resume → 该节点从头重跑、
// interrupt() 返回 resume 值。约束：interrupt 之前的副作用重跑时会重复。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "cppchain/graph/graph.hpp"
#include "cppchain/state/checkpointer.hpp"

using namespace cppchain;
using namespace cppchain::graph;
using cppchain::state::FileCheckpointer;
using cppchain::state::MemoryCheckpointer;
using cppchain::state::StateSchema;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() /
          ("cppchain-interrupt-" + std::to_string(::getpid()) + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string file(std::string const& name = "ckpt.json") const {
    return (dir / name).string();
  }
};

// 通用审批图：approve 节点 interrupt 等外部决策 → 写入 decision → END
StateGraph make_approval_graph() {
  StateGraph g(StateSchema{});
  g.add_node("approve", [](Json const&, NodeContext& ctx) {
     auto const decision = interrupt(Json{{"txn", "T-1"}, {"amount", 100}}, ctx);
     return Command::with_update({{"decision", decision}});
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  return g;
}

}  // namespace

TEST(Interrupt, suspends_and_returns_full_state) {
  MemoryCheckpointer cp;
  auto compiled = make_approval_graph().compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  auto out = compiled->invoke({{"items", Json::array({1, 2})}}, "t-1");
  ASSERT_TRUE(out.has_value());
  // 挂着 = 返回成功 + 完整 state；调用方用 last_interrupt() 区分
  EXPECT_TRUE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(compiled->last_interrupt().node, "approve");
  EXPECT_EQ(compiled->last_interrupt().payload.at("txn"), "T-1");
  EXPECT_EQ(out->at("items").size(), 2u);
  ASSERT_FALSE(out->contains("decision")) << "悬挂时节点的 update 不应生效（返回入口 state）";

  // 快照带悬挂标记，writes 存 interrupt payload
  auto const snaps = cp.list("t-1", "", 0);
  ASSERT_FALSE(snaps.empty());
  EXPECT_EQ(snaps[0].metadata.value("interrupted", false), true);
  auto const writes = cp.get_writes("t-1", "", snaps[0].config.value("checkpoint_id", ""));
  bool found = false;
  for (auto const& w : writes)
    if (w.channel == "interrupt") found = (w.value.at("amount") == 100);
  EXPECT_TRUE(found);
}

TEST(Interrupt, resume_returns_value_and_reruns_node) {
  MemoryCheckpointer cp;
  int side_effect_before = 0, side_effect_after = 0;
  StateGraph g(StateSchema{});
  g.add_node("approve", [&](Json const&, NodeContext& ctx) {
     ++side_effect_before;  // interrupt 之前：重跑会重复
     auto const decision = interrupt({{"approved", true}}, ctx);
     ++side_effect_after;  // interrupt 之后：仅当次成功执行一次
     return Command::with_update({{"decision", decision}});
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  (void)compiled->invoke(Json::object(), "t-1");
  EXPECT_EQ(side_effect_before, 1);
  EXPECT_EQ(side_effect_after, 0);

  auto resumed = compiled->invoke(Json::object(), "t-1",
                                  InvokeOptions{.resume = Json{{"approved", true}}});
  ASSERT_TRUE(resumed.has_value());
  // 重跑语义：节点从头执行 → interrupt() 返回注入值
  EXPECT_EQ((*resumed).at("decision").at("approved"), true);
  EXPECT_EQ(side_effect_before, 2) << "重跑使 interrupt 之前的副作用重复";
  EXPECT_EQ(side_effect_after, 1) << "interrupt 之后的副作用只执行一次";
  EXPECT_FALSE(compiled->last_interrupt().interrupted) << "恢复后不再挂着";
}

// 契约文档化：interrupt 之前的副作用必然重复（幂等不是默认，是约定）
TEST(Interrupt, side_effects_before_interrupt_are_not_idempotent) {
  MemoryCheckpointer cp;
  int sends = 0;
  StateGraph g(StateSchema{});
  g.add_node("approve", [&](Json const&, NodeContext& ctx) {
     ++sends;  // 假设这是一封已发出的通知：重跑会重复发
     (void)interrupt({{"ask", "ok?"}}, ctx);
     return Command::with_update({{"decision", true}});
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  (void)compiled->invoke(Json::object(), "t-1");
  EXPECT_EQ(sends, 1);
  (void)compiled->invoke(Json::object(), "t-1",
                         InvokeOptions{.resume = Json{{"ok", true}}});
  EXPECT_EQ(sends, 2) << "同一通知发了两次 —— interrupt 应放在副作用之后，或副作用须幂等";
}

TEST(Interrupt, plain_second_invoke_stays_suspended) {
  MemoryCheckpointer cp;
  int after = 0;
  StateGraph g(StateSchema{});
  g.add_node("approve", [&](Json const&, NodeContext& ctx) {
     (void)interrupt({{"ask", "ok?"}}, ctx);
     ++after;
     return Command::with_update(Json::object());
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  (void)compiled->invoke(Json::object(), "t-1");
  // 无 resume 再 invoke：保持挂着，不执行
  auto out2 = compiled->invoke(Json::object(), "t-1");
  ASSERT_TRUE(out2.has_value());
  EXPECT_TRUE(compiled->last_interrupt().interrupted);
  EXPECT_EQ(after, 0);
}

TEST(Interrupt, resume_on_finished_thread_errors) {
  MemoryCheckpointer cp;
  StateGraph g(StateSchema{});
  g.add_node("noop", [](Json const&, NodeContext&) { return Command::with_update(Json::object()); })
      .add_edge("noop", kEnd)
      .set_entry_point("noop");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  ASSERT_TRUE(compiled->invoke(Json::object(), "t-done").has_value());
  auto out = compiled->invoke(Json::object(), "t-done",
                              InvokeOptions{.resume = Json{{"x", 1}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::ResumeNotInterrupted);
}

TEST(Interrupt, requires_checkpointer) {
  // 无 cp / 空 thread_id 时 interrupt 不可用（挂起来没地方存、没法恢复）
  auto compiled = make_approval_graph().compile();  // cp = nullptr
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, GraphError::Kind::InterruptUnavailable);
}

// InterruptSignal 不继承 std::exception：节点的 catch(std::exception) 不得吞掉它
TEST(Interrupt, does_not_leak_into_generic_catch) {
  MemoryCheckpointer cp;
  bool caught_as_generic = false;
  StateGraph g(StateSchema{});
  g.add_node("approve", [&](Json const&, NodeContext& ctx) {
     try {
       (void)interrupt({{"ask", "ok?"}}, ctx);
     } catch (std::exception const&) {
       caught_as_generic = true;
     }
     return Command::with_update(Json::object());
   })
      .add_edge("approve", kEnd)
      .set_entry_point("approve");
  auto compiled = g.compile(&cp);
  ASSERT_TRUE(compiled.has_value());

  (void)compiled->invoke(Json::object(), "t-1");
  EXPECT_FALSE(caught_as_generic) << "InterruptSignal 不该被 catch(std::exception) 拦截";
  EXPECT_TRUE(compiled->last_interrupt().interrupted);
}

// 回归：带 resume 续跑时**不得重放**旧的 interrupt 事件（否则 stream 投影会落幽灵记录）
TEST(Interrupt, resume_does_not_reemit_stale_interrupt) {
  MemoryCheckpointer cp;
  auto compiled = make_approval_graph().compile(&cp);
  ASSERT_TRUE(compiled.has_value());
  (void)compiled->invoke(Json::object(), "t-1");  // 先挂起

  std::vector<StreamEvent> evs;
  auto const resumed = compiled->invoke(
      Json::object(), "t-1",
      InvokeOptions{[&evs](StreamEvent const& e) { evs.push_back(e); },
                    EventMode::Updates, Json{{"ok", true}}, std::nullopt});
  ASSERT_TRUE(resumed.has_value());
  ASSERT_EQ(evs.size(), 2u);  // 只有 node_start + node_end（重跑成功）
  EXPECT_EQ(evs[0].type, "node_start");
  EXPECT_EQ(evs[1].type, "node_end");
  for (auto const& e : evs) EXPECT_NE(e.type, "interrupt");
}

// 新进程/新 FileCheckpointer 实例能从落盘快照重建 last_interrupt（挂起状态持久化）
TEST(Interrupt, recover_interrupt_status_from_checkpoint) {
  TempDir tmp;
  {
    FileCheckpointer cp1(tmp.file());
    auto compiled = make_approval_graph().compile(&cp1);
    ASSERT_TRUE(compiled.has_value());
    (void)compiled->invoke(Json::object(), "t-1");  // 悬挂 → 落盘
  }
  // "另一个进程"：全新 FileCheckpointer 读同一文件
  FileCheckpointer cp2(tmp.file());
  auto compiled = make_approval_graph().compile(&cp2);
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json::object(), "t-1");  // 无 resume → 保持挂着
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(compiled->last_interrupt().interrupted) << "跨实例应重建挂起状态";
  EXPECT_EQ(compiled->last_interrupt().node, "approve");
  EXPECT_EQ(compiled->last_interrupt().payload.at("txn"), "T-1");
}