// M12 测试：history 两个节点（load_history / save_history）—— **走完整 workflow 的跨命令续跑**
//
// 与 examples/session_resume（验证 A）的区别：A 直接用 agent::Agent + 落盘 Checkpointer；
// 这里走的是 workflow —— history 是图上的**普通节点**，state 每次触发全新（M11 的 D1）。
//
// 输入进 state 的路径（重要，见 02-arch-design/11 §5.5）：
//   宿主（CLI）**先把本次用户输入写进会话文件**，再由 workflow 整体 load → 跑 → save。
//   这样顺序天然正确（历史在前、本轮输入在后），且不用给引擎加"初始状态种子"——
//   若改用种子，add_messages 会把种子放在 load 回来的历史**之前**，顺序就反了。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <string>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/history_file.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;

namespace {

class ScriptedModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::vector<Message> last_received;  // 记录"模型实际看到了什么"
  std::size_t invoked = 0;

  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    last_received.assign(msgs.begin(), msgs.end());
    if (replies.empty())
      return std::unexpected(model::ModelError{model::ModelError::Kind::Parse, 0, "no reply", {}});
    Message m = replies.front();
    replies.pop_front();
    ++invoked;
    return m;
  }
  std::generator<message::MessageChunk> stream(std::span<Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() /
          ("cppchain-hist-" + std::to_string(::getpid()) + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string str() const { return dir.string(); }
};

// D 阶段的完整形态：load_history → [label → model → route → run_tools → goto] → save_history
Json history_react_ir() {
  return Json{
      {"id", "wf_cli"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "recall"},
                          {"kind", "load_history"},
                          {"path", "@config.workspace"},
                          {"session", "@config.session"},
                          {"next", "L"}},
                     Json{{"id", "L"}, {"kind", "label"}},
                     Json{{"id", "think"},
                          {"kind", "model"},
                          {"model", "m"},
                          {"tools", "ts"},
                          {"next", "route"}},
                     Json{{"id", "route"},
                          {"kind", "route"},
                          {"on", "last_ai_outcome"},
                          {"cases", Json{{"tool_calls", "act"},
                                         {"invalid_tool_calls", "_fail"},
                                         {"text", "persist"}}},
                          {"default", "_fail"}},
                     Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "loop"}},
                     Json{{"id", "loop"}, {"goto", "L"}, {"max", 4}},
                     Json{{"id", "persist"},
                          {"kind", "save_history"},
                          {"path", "@config.workspace"},
                          {"session", "@config.session"},
                          {"next", "_turn_end"}}})}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry components;

  Fixture() {
    components.models["m"] = &model;
    components.toolsets["ts"] = {"fetch_price"};
  }

  Json config(std::string const& ws) const {
    return Json{{"workspace", ws}, {"session", "cli"}};
  }

  std::expected<CompiledWorkflow, WorkflowError> compile() {
    auto ir = WorkflowIR::parse(history_react_ir());
    if (!ir) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, ir.error()});
    CompileOptions o;
    o.components = &components;
    return compile_workflow(*ir, registry, nullptr, o);
  }
};

}  // namespace

// 跨"两次命令"续跑：第一次留下历史，第二次（全新编译 + 全新进程语义）把它读回来。
TEST(WfHistoryNodes, second_run_sees_first_run_history) {
  TempDir tmp;

  // ---- 第一次"命令" ----
  {
    Fixture f;
    f.model.replies.push_back(cppchain::message::ai_message("记住了，你叫东俊"));

    // 宿主先把本次输入写进会话文件（顺序：历史在前、本轮在后）
    std::vector<Message> session{cppchain::message::human_message("我叫东俊")};
    save_history_file(session_path(tmp.str(), "cli"), session, "cli");

    auto wf = f.compile();
    ASSERT_TRUE(wf) << wf.error().message;
    auto r = fire_once(*wf, f.config(tmp.str()));
    ASSERT_TRUE(r) << r.error().message;

    ASSERT_EQ(f.model.invoked, 1u);
    ASSERT_EQ(f.model.last_received.size(), 1u);  // 只看到本轮输入
    EXPECT_EQ(f.model.last_received[0].text(), "我叫东俊");

    // save_history 落盘：应含 [human, ai]
    auto saved = load_history_file(session_path(tmp.str(), "cli"));
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_EQ(saved[0].text(), "我叫东俊");
    EXPECT_EQ(saved[1].text(), "记住了，你叫东俊");
  }

  // ---- 第二次"命令"：全新 fixture（模拟新进程），只共享工作路径 ----
  {
    Fixture f;
    f.model.replies.push_back(cppchain::message::ai_message("你叫东俊"));

    // 宿主把本轮输入追加进同一个会话文件
    auto session = load_history_file(session_path(tmp.str(), "cli"));
    session.push_back(cppchain::message::human_message("我叫什么？"));
    save_history_file(session_path(tmp.str(), "cli"), session, "cli");

    auto wf = f.compile();
    ASSERT_TRUE(wf) << wf.error().message;
    auto r = fire_once(*wf, f.config(tmp.str()));
    ASSERT_TRUE(r) << r.error().message;

    // **核心断言**：模型这次看到的是 3 条 —— 上次的两条 + 本轮输入，且顺序正确
    ASSERT_EQ(f.model.last_received.size(), 3u) << "load_history 没把上次的对话带回来";
    EXPECT_EQ(f.model.last_received[0].text(), "我叫东俊");
    EXPECT_EQ(f.model.last_received[1].text(), "记住了，你叫东俊");
    EXPECT_EQ(f.model.last_received[2].text(), "我叫什么？");

    auto saved = load_history_file(session_path(tmp.str(), "cli"));
    EXPECT_EQ(saved.size(), 4u);  // 又追加了一条 AI 回复
  }
}

// 换工作路径 → 完全隔离（历史不串）
TEST(WfHistoryNodes, different_workspace_is_isolated) {
  TempDir a, b;

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  save_history_file(session_path(a.str(), "cli"),
                    std::vector<Message>{cppchain::message::human_message("A 的历史")}, "cli");

  auto wf = f.compile();
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, f.config(b.str()));  // 跑在 b
  ASSERT_TRUE(r) << r.error().message;

  EXPECT_EQ(f.model.last_received.size(), 0u) << "b 里不该看到 a 的历史";
}

// ---- 校验规则 ----

// VG9：load_history 出现在消费 messages 的节点之后 → 硬拒绝（读了没人用）
TEST(WfHistoryNodes, validator_rejects_load_history_after_consumer) {
  auto ir_json = history_react_ir();
  // 把 recall 挪到最后（数组下标即顺序）
  Json nodes = ir_json["nodes"];
  Json recall = nodes[0];
  nodes.erase(nodes.begin());
  nodes.push_back(recall);
  ir_json["nodes"] = nodes;

  FakeToolSinks sinks;
  tool::ToolRegistry const reg = make_fake_tools(sinks);
  auto r = validate_json(ir_json, ValidatorOptions{&reg});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VG9"), std::string::npos) << r.describe();
}

// VN11：缺 path → 拒绝
TEST(WfHistoryNodes, validator_requires_path) {
  auto ir_json = history_react_ir();
  ir_json["nodes"][0].erase("path");

  FakeToolSinks sinks;
  tool::ToolRegistry const reg = make_fake_tools(sinks);
  auto r = validate_json(ir_json, ValidatorOptions{&reg});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VN11"), std::string::npos) << r.describe();
}

// ---- history_file 单元 ----

TEST(WfHistoryFile, round_trip_and_fail_closed) {
  TempDir tmp;
  std::string const p = session_path(tmp.str(), "s1");
  EXPECT_EQ(p, (tmp.dir / ".cppchain" / "session-s1.json").string());

  // 文件不存在 → 空（首次使用是正常的）
  EXPECT_TRUE(load_history_file(p).empty());

  std::vector<Message> msgs{cppchain::message::human_message("你好"),
                            cppchain::message::ai_message("你也好")};
  save_history_file(p, msgs, "s1");
  auto back = load_history_file(p);
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[0].text(), "你好");
  EXPECT_EQ(back[1].text(), "你也好");

  // 原子写：不留临时文件
  EXPECT_FALSE(std::filesystem::exists(p + ".tmp"));

  // 损坏 → 默认抛（fail-closed，不静默丢历史）
  {
    std::ofstream out(p, std::ios::trunc);
    out << "{ 这不是合法 JSON";
  }
  EXPECT_THROW(load_history_file(p), std::runtime_error);
  EXPECT_TRUE(load_history_file(p, /*start_empty_on_corrupt=*/true).empty());
}
