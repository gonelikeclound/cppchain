// M10 测试：workflow 层的引擎事件接线（P4）——
//   · FireOptions.stream：引擎事件通道注入 + wrapper 结构日志**让位**（双源互斥，防重复记）；
//   · project_stream_to_log：把 StreamEvent 投影成会话日志信封（与 wrapper 相同形状）；
//   · engine_interrupt：interrupt 记录格式 + replay 铁律不变（天然不参与重建）。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/serializer.hpp"  // Message ↔ JSON（input 事件的形状）
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() /
          ("cppchain-wf-m10-" + std::to_string(::getpid()) + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string file(std::string const& name = "session.log.jsonl") const {
    return (dir / name).string();
  }
};

// 线性价格检查（3 个图节点：fetch_p → check_p → notify），与 compiler_test 同款
Json price_ir() {
  return Json{
      {"id", "wf_price"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8,20 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "fetch_p"},
                          {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"price", "number"}, {"in_stock", "boolean"}}},
                          {"next", "check_p"}},
                     Json{{"id", "check_p"},
                          {"expr", "$_.result.price != null && $_.result.price < 800"},
                          {"then", "notify"}, {"else", "_turn_end"}},
                     Json{{"id", "notify"},
                          {"tool", "send_notification"},
                          {"parameters", Json{{"title", "降价了"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
}

}  // namespace

// 注入引擎事件通道：收到全部 node_start/node_end（3 节点 → 6 个结构事件）
TEST(WfM10, fire_once_stream_collects_events) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());
  ASSERT_TRUE(ir);
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  std::vector<graph::StreamEvent> evs;
  auto r = fire_once(*wf, Json{{"product_id", "P1"}}, FireOptions{.stream = [&evs](auto const& e) {
                                                                evs.push_back(e);
                                                              }});
  ASSERT_TRUE(r) << r.error().message;
  // EventMode::All = updates + snapshot + deltas → 每节点 (node_start, node_end, values)
  ASSERT_EQ(evs.size(), 9u) << "3 个节点 × (node_start+node_end+values)";
  for (std::size_t i = 0; i < evs.size(); i += 3) {
    EXPECT_EQ(evs[i].type, "node_start");
    EXPECT_EQ(evs[i + 1].type, "node_end");
    EXPECT_EQ(evs[i + 2].type, "values");
    EXPECT_TRUE(evs[i + 1].ok);
  }
  EXPECT_EQ(evs[0].node, "fetch_p");
  EXPECT_EQ(evs[3].node, "check_p");
  EXPECT_EQ(evs[6].node, "notify");
  EXPECT_EQ(sinks.notifications.size(), 1u);
}

// 双源互斥：stream 启用时 wrapper 让位，会话日志里没有节点的结构事件（只剩火火入 input 事件）
TEST(WfM10, stream_mode_skips_wrapper_structure_log) {
  TempDir tmp;
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());
  ASSERT_TRUE(ir);
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  SessionLog slog(tmp.file());
  (void)fire_once(*wf, Json{{"product_id", "P1"}},
                  FireOptions{.log = &slog, .stream = [](graph::StreamEvent const&) {}});

  for (auto const& rec : slog.records()) {
    if (rec.value("source", "") == "engine") {
      std::string const node = rec.value("node", "");
      EXPECT_NE(node, "fetch_p") << "wrapper 应让位（双源互斥），不写结构事件";
      EXPECT_NE(node, "check_p");
      EXPECT_NE(node, "notify");
    }
  }
}

// 投影适配器：StreamEvent → 会话日志信封（与 wrapper 相同形状），replay 铁律不受影响
TEST(WfM10, project_stream_to_log_writes_same_envelope) {
  TempDir tmp;
  SessionLog slog(tmp.file());

  project_stream_to_log(slog, "r-1", graph::StreamEvent{.type = "node_start", .node = "a"},
                        "tool");
  project_stream_to_log(slog, "r-1",
                        graph::StreamEvent{.type = "node_end", .node = "a",
                                           .payload = Json{{"x", 1}}},
                        "tool");
  project_stream_to_log(slog, "r-1",
                        graph::StreamEvent{.type = "node_end", .node = "b", .ok = false,
                                           .status = "error"},
                        "tool");
  project_stream_to_log(slog, "r-1",
                        graph::StreamEvent{.type = "interrupt", .node = "a"}, "tool");

  auto const recs = slog.records();
  ASSERT_EQ(recs.size(), 4u);
  // node_start → enter
  EXPECT_EQ(recs[0].value("phase", ""), "enter");
  EXPECT_EQ(recs[0].value("node_kind", ""), "tool");
  // node_end ok → exit + update
  EXPECT_EQ(recs[1].value("phase", ""), "exit");
  EXPECT_TRUE(recs[1].value("status", "") == "ok");
  EXPECT_EQ(recs[1].at("update").at("x"), 1);
  // node_end !ok → error（无 update）
  EXPECT_EQ(recs[2].value("phase", ""), "error");
  EXPECT_FALSE(recs[2].contains("update"));
  // interrupt → interrupt 记录
  EXPECT_EQ(recs[3].value("phase", ""), "interrupt");
  // seq 单调（投影后仍满足日志契约）
  for (std::size_t i = 1; i < recs.size(); ++i)
    EXPECT_GT(recs[i].value("seq", 0ull), recs[i - 1].value("seq", 0ull));
}

// interrupt 记录不参与 replay：日志里有 interrupt 记录时，replay 仍只按 exit+ok 重建
TEST(WfM10, engine_interrupt_record_ignored_by_replay) {
  TempDir tmp;
  SessionLog slog(tmp.file());
  // 惯例：input 事件（exit+ok） + 一次 interrupt 悬挂
  slog.engine_enter("r-1", "__input__", "input");
  Json input_update = {{"messages", Json::array({message::human_message("你好")})}};
  slog.engine_exit("r-1", "__input__", "input", true, "ok", 0, input_update);
  slog.engine_enter("r-1", "approve", "node");
  slog.engine_interrupt("r-1", "approve", "node", 5);

  auto const msgs = replay_session_messages(tmp.file());
  ASSERT_EQ(msgs.size(), 1u);  // interrupt 记录被跳过，消息仍只来自 input
  EXPECT_EQ(msgs[0].type, message::Role::Human);

  // 信封形状单独校验
  auto const recs = slog.records();
  ASSERT_EQ(recs.size(), 4u);
  EXPECT_EQ(recs[3].value("phase", ""), "interrupt");
  EXPECT_EQ(recs[3].value("status", ""), "interrupt");
  EXPECT_EQ(recs[3].value("source", ""), "engine");
}