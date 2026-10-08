// M12 测试：history = 日志的投影（docs/02-arch-design/12 §7，③④ 落地）——
// **走完整 workflow 的跨命令续跑**。
//
// 与 examples/session_resume（验证 A）的区别：A 直接用 agent::Agent + 落盘 Checkpointer；
// 这里走的是 workflow —— history 是图上的**普通节点**（load_history），state 每次触发全新（D1）。
//
// 输入进 state 的路径（④ 起改进了 02-arch-design/11 §5.5 的 D 阶段方案）：
//   宿主把**本轮输入**经 FireOptions.input_messages 传给 fire_once，引擎在 run 开头写一条
//   source=engine 的 input 事件；load_history 对该会话日志做 replay 投影，按 seq 重建 messages。
//   顺序天然正确（历史在前、本轮在后），且没有 save_history —— 日志就是唯一事实来源。
//
// 附带验证（③）：引擎在节点边界写 enter/exit 结构事件；节点经注入的 NodeLog 写 source=node
// 诊断（不参与 replay）；load_history 的 update 是"回声"，不重复落盘。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <sstream>
#include <string>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/session_log.hpp"
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

// D 阶段的完整形态现在收敛为：load_history → [label → model → route → run_tools → goto]
// （save_history 已随 ④ 删除；历史是日志的投影，无需持久化节点）。
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
                                         {"text", "_turn_end"}}},
                          {"default", "_fail"}},
                     Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "loop"}},
                     Json{{"id", "loop"}, {"goto", "L"}, {"max", 4}}})}};
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

// 去日志里取 source=engine && phase=exit && status=ok 的记录（与 replay 同判据，但保留 update 原文，
// 用于断言引擎确实采集到了节点 update / load_history 回声不落）。
std::vector<Json> engine_ok_exits(std::vector<Json> const& recs) {
  std::vector<Json> out;
  for (Json const& r : recs)
    if (r.value("source", "") == "engine" && r.value("phase", "") == "exit" &&
        r.value("status", "") == "ok")
      out.push_back(r);
  return out;
}

}  // namespace

// 跨"两次命令"续跑：第一次在日志里留下结构事件（含 input 事件），第二次（全新编译 +
// 全新进程语义）经 load_history replay 把它读回来。这是 ④ 的核心命题。
TEST(WfHistoryNodes, second_run_sees_first_run_history) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");

  // ---- 第一次"命令" ----
  {
    Fixture f;
    f.model.replies.push_back(cppchain::message::ai_message("记住了，你叫东俊"));

    SessionLog slog(path);
    auto wf = f.compile();
    ASSERT_TRUE(wf) << wf.error().message;
    FireOptions fo;
    fo.log = &slog;
    fo.input_messages = Json::array({cppchain::message::human_message("我叫东俊")});
    auto r = fire_once(*wf, f.config(tmp.str()), std::move(fo));
    ASSERT_TRUE(r) << r.error().message;

    ASSERT_EQ(f.model.invoked, 1u);
    ASSERT_EQ(f.model.last_received.size(), 1u);  // 只看到本轮输入
    EXPECT_EQ(f.model.last_received[0].text(), "我叫东俊");

    // 日志即历史：replay 应含 [human, ai]
    auto const msgs = replay_session_messages(path);
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].text(), "我叫东俊");
    EXPECT_EQ(msgs[1].text(), "记住了，你叫东俊");

    // ---- ③ 的日志形状 ----
    auto const recs = slog.records();
    // 引擎写 input 事件（本轮输入进日志的机制，D 阶段"宿主先写文件"已废）
    auto input_exit = std::find_if(recs.begin(), recs.end(), [](Json const& r) {
      return r.value("node", "") == "__input__" && r.value("phase", "") == "exit";
    });
    ASSERT_NE(input_exit, recs.end());
    EXPECT_EQ((*input_exit)["source"], "engine");
    EXPECT_EQ((*input_exit)["update"]["messages"].size(), 1u);
    // load_history 的 update 是回声 → 不落 update 键（否则 replay 重复追加整段消息）
    for (Json const& e : engine_ok_exits(recs)) {
      if (e.value("node_kind", "") == "load_history") {
        EXPECT_FALSE(e.contains("update")) << "load_history 的 update 不应落盘";
      }
    }
    // 节点自报（source=node）：load_history 至少写了一条诊断
    auto node_log = std::find_if(recs.begin(), recs.end(),
                                 [](Json const& r) { return r.value("source", "") == "node"; });
    EXPECT_NE(node_log, recs.end());
  }

  // ---- 第二次"命令"：全新 fixture（模拟新进程），只共享工作路径 ----
  {
    Fixture f;
    f.model.replies.push_back(cppchain::message::ai_message("你叫东俊"));

    SessionLog slog(path);  // 跨进程续写：seq 应从上次继续，且不会丢上次的记录
    auto wf = f.compile();
    ASSERT_TRUE(wf) << wf.error().message;
    FireOptions fo;
    fo.log = &slog;
    fo.input_messages = Json::array({cppchain::message::human_message("我叫什么？")});
    auto r = fire_once(*wf, f.config(tmp.str()), std::move(fo));
    ASSERT_TRUE(r) << r.error().message;

    // **核心断言**：模型这次看到的是 3 条 —— 上次的两条 + 本轮输入，且顺序正确
    ASSERT_EQ(f.model.last_received.size(), 3u) << "load_history 没把上次的对话 replay 回来";
    EXPECT_EQ(f.model.last_received[0].text(), "我叫东俊");
    EXPECT_EQ(f.model.last_received[1].text(), "记住了，你叫东俊");
    EXPECT_EQ(f.model.last_received[2].text(), "我叫什么？");

    // 日志里又追加了一条 AI 回复
    EXPECT_EQ(replay_session_messages(path).size(), 4u);
  }
}

// 换工作路径 → 完全隔离（历史不串）
TEST(WfHistoryNodes, different_workspace_is_isolated) {
  TempDir a, b;

  // a 里已有历史（模拟上次在 a 跑过：日志里留下一轮 input 事件）
  {
    SessionLog log_a(session_path(a.str(), "cli"));
    log_a.engine_enter("r-1", "__input__", "input");
    log_a.engine_exit("r-1", "__input__", "input", true, "ok", 0,
                      Json{{"messages", Json::array({cppchain::message::human_message("A 的历史")})}});
  }

  // 这次跑在 b：log 也指向 b
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog log_b(session_path(b.str(), "cli"));
  auto wf = f.compile();
  ASSERT_TRUE(wf) << wf.error().message;
  FireOptions fo;
  fo.log = &log_b;
  fo.input_messages = Json::array({cppchain::message::human_message("跑在 b")});
  auto r = fire_once(*wf, f.config(b.str()), std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  ASSERT_EQ(f.model.last_received.size(), 1u);
  EXPECT_EQ(f.model.last_received[0].text(), "跑在 b") << "b 里不该看到 a 的历史";
}

// ---- 校验规则（与 D 阶段相同，仅随 save_history 删除收敛）----

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

// 回归：**load_history 的 path/session 也是引用字段**（④ 后仍成立）—— 必须被 VR 段收集，
// 否则 B2 的 config 契约（VR4）看不到它们 —— 写错键名会一路通过校验，
// 运行期 resolve 成 null、as_string 得到字符串 "null"，于是跑在一个叫 null 的目录上。
TEST(WfHistoryNodes, config_keys_inside_history_fields_are_validated) {
  auto ir_json = history_react_ir();
  ir_json["nodes"][0]["path"] = "@config.wrokspace";  // 故意拼错

  FakeToolSinks sinks;
  tool::ToolRegistry const reg = make_fake_tools(sinks);
  ValidatorOptions o{&reg};
  o.config_keys = std::vector<std::string>{"workspace", "session"};  // 契约里没有 wrokspace

  auto r = validate_json(ir_json, o);
  ASSERT_FALSE(r.ok()) << "history 节点里的引用没被收集 → 契约形同虚设";
  EXPECT_NE(r.describe().find("VR4"), std::string::npos) << r.describe();
}

// ---- session_log 单元（③）----

TEST(WfSessionLog, append_seq_continues_across_reopen) {
  TempDir tmp;
  std::string const p = session_path(tmp.str(), "s1");
  EXPECT_EQ(p, (tmp.dir / ".wfr" / "sessions" / "session-s1.log.jsonl").string());
  EXPECT_FALSE(std::filesystem::exists(p));  // 首次使用：sessions 文件还没写（.wfr 根已建）

  {
    SessionLog slog(p);
    slog.engine_enter("r-1", "think", "model");
    slog.engine_exit("r-1", "think", "model", true, "ok", 3, Json{{"messages", Json::array()}});
    slog.log("info", "诊断", Json{{"k", 1}});
    auto recs = slog.records();
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0]["seq"], 1);  // enter
    EXPECT_EQ(recs[1]["seq"], 2);  // exit
    EXPECT_EQ(recs[2]["seq"], 3);  // node 自报
    EXPECT_EQ(recs[2]["source"], "node");
    EXPECT_TRUE(recs[0]["ts"].is_string());
  }

  // 重新打开（模拟下一次命令）：seq 继续，不重复
  {
    SessionLog slog(p);
    slog.engine_exit("r-2", "recall", "load_history", true, "ok", 0, Json::object());
    auto recs = slog.records();
    ASSERT_EQ(recs.size(), 4u);
    EXPECT_EQ(recs[3]["seq"], 4);
    EXPECT_EQ(recs[3]["run"], "r-2");
  }
}

// 崩溃残迹：日志末尾是半行（非法 JSON）→ 打开时截断 + 计数，前面完好
TEST(WfSessionLog, trims_trailing_partial_line) {
  TempDir tmp;
  std::string const p = session_path(tmp.str(), "s1");

  {
    SessionLog slog(p);
    slog.engine_enter("r-1", "a", "model");
    slog.engine_exit("r-1", "a", "model", true, "ok", 1, Json{{"messages", Json::array()}});
  }
  {  // 模拟崩溃：文件被截断成一个非法半行
    std::ofstream out(p, std::ios::app);
    out << "{\"seq\":3,\"source\":\"eng";  // 写到一半
  }

  {
    SessionLog slog(p);
    EXPECT_EQ(slog.truncated_lines(), 1u);
    auto recs = slog.records();
    ASSERT_EQ(recs.size(), 2u) << "半行应被清除，前面的记录完好";
    EXPECT_EQ(recs[0]["seq"], 1);
    EXPECT_EQ(recs[1]["seq"], 2);
    // 续写 seq 从 3 继续（半行虽被丢弃，它声称的 seq 不占位）
    slog.engine_exit("r-1", "b", "expr", true, "ok", 0, Json::object());
    EXPECT_EQ(slog.records().back()["seq"], 3);
  }
}

// replay 铁律（docs/12 §7.1）：只读 source=engine && phase=exit && status=ok；
// 节点日志即使带了 update 字段也不进 replay；错误 exit 不携带 update。
TEST(WfSessionLog, replay_ignores_non_engine_records) {
  TempDir tmp;
  std::string const p = session_path(tmp.str(), "s1");

  {
    SessionLog slog(p);
    slog.engine_enter("r-1", "recall", "load_history");
    slog.engine_exit("r-1", "recall", "load_history", true, "ok", 0,
                     Json::object());  // load_history：回声，update 为空由调用方保证
    slog.engine_exit("r-1", "think", "model", true, "ok", 0,
                     Json{{"messages", Json::array({cppchain::message::ai_message("嗨")})}});
    // 节点的"诊断日志"里塞了一个 messages 字段 —— 铁律要求它不进 replay
    slog.log("info", "假的更新", Json{{"messages", Json::array({cppchain::message::ai_message("假")})}});
    // 一条 error 退出：无 update，也不该重建任何东西
    slog.engine_exit("r-1", "boom", "tool", false, "error", 5, Json::object());
  }

  auto const msgs = replay_session_messages(p);
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].text(), "嗨");  // 只有那条 status=ok 的 engine exit 进了 replay
}
// wfr 工具：会话日志统一落在 <ws>/.wfr/sessions/（从 .cppchain 迁移）；.wfr/.gitignore
// 忽略运行态（sessions/checkpoints/）、保留 workflows/ 源
TEST(WfHistoryNodes, session_log_path_migrated_to_wfr_root) {
  TempDir tmp;
  std::string const p = session_path(tmp.str(), "s1");
  EXPECT_TRUE(p.find("/.wfr/sessions/session-s1.log.jsonl") != std::string::npos);

  SessionLog slog(p);  // 构造即建目录
  EXPECT_TRUE(std::filesystem::exists(p));
  // .wfr/.gitignore 内容 = sessions/ + checkpoints/（workflows 源不被忽略，不再是全忽略 `*`）
  std::string gi;
  {
    std::ifstream in(tmp.str() + "/.wfr/.gitignore");
    std::ostringstream buf;
    buf << in.rdbuf();
    gi = buf.str();
  }
  EXPECT_NE(gi, "*\n") << "不能再是 .cppchain 时代的通配全忽略";
  EXPECT_TRUE(gi.find("sessions/") != std::string::npos);
  EXPECT_TRUE(gi.find("checkpoints/") != std::string::npos);
}

// ---- wfr-㉙：历史渲染三档（full / drop_round / keep_evidence）----
//
// 压缩是**投影的渲染期变换**：同一条日志、同一句话，只换 mode，断言"模型实际收到了什么"。
// 这也是 chat2 对照实验的离线底座（真机版见 docs/04-problems/14）。
namespace {

// 一段"工具轮"历史：human → ai(带 tool_calls + reasoning) → tool → ai(带 reasoning)
Json beijing_history() {
  using cppchain::message::Message;
  using cppchain::message::ReasoningBlock;
  using cppchain::message::Role;
  using cppchain::message::TextBlock;
  using cppchain::message::ToolCall;

  Message call;
  call.type = Role::Ai;
  call.content.push_back(ReasoningBlock{"用户问天气，需要调用 fetch_price", std::nullopt});
  call.content.push_back(TextBlock{"", std::nullopt});
  call.tool_calls.push_back(ToolCall{"call_1", "fetch_price", Json{{"symbol", "BJ"}}});

  Message answer;
  answer.type = Role::Ai;
  answer.content.push_back(ReasoningBlock{"拿到结果，可直接回答", std::nullopt});
  answer.content.push_back(TextBlock{"北京今天 12°C，晴。", std::nullopt});

  return Json::array({cppchain::message::human_message("北京天气怎么样？"), call,
                      cppchain::message::tool_message("price=12 晴", "call_1"), answer});
}

// 预置一条 engine 的 input 事件（= 上次"命令"留下的历史）
void seed_history(std::string const& log_path, Json const& messages) {
  SessionLog log(log_path);
  log.engine_enter("r-1", "__input__", "input");
  log.engine_exit("r-1", "__input__", "input", true, "ok", 0, Json{{"messages", messages}});
}

Json ir_with_mode(std::string const& mode_spec) {
  Json ir_json = history_react_ir();
  ir_json["nodes"][0]["mode"] = mode_spec;  // nodes[0] = recall
  return ir_json;
}

std::size_t count_reasoning_blocks(std::vector<Message> const& msgs) {
  std::size_t n = 0;
  for (Message const& m : msgs)
    for (auto const& b : m.content)
      if (std::holds_alternative<cppchain::message::ReasoningBlock>(b)) ++n;
  return n;
}

}  // namespace

TEST(WfHistoryNodes, history_mode_full_replays_everything_including_reasoning) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, beijing_history());

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog slog(path);
  auto ir = WorkflowIR::parse(ir_with_mode("full"));
  ASSERT_TRUE(ir) << ir.error();
  CompileOptions co;
  co.components = &f.components;
  auto wf = compile_workflow(*ir, f.registry, nullptr, co);
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.log = &slog;
  fo.input_messages = Json::array({cppchain::message::human_message("那上海呢？")});
  auto r = fire_once(*wf, f.config(tmp.str()), std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  ASSERT_EQ(f.model.last_received.size(), 5u);  // 4 条历史 + 本轮输入
  EXPECT_EQ(f.model.last_received[0].text(), "北京天气怎么样？");
  EXPECT_EQ(f.model.last_received[3].text(), "北京今天 12°C，晴。");
  EXPECT_EQ(f.model.last_received[4].text(), "那上海呢？");
  EXPECT_EQ(count_reasoning_blocks(f.model.last_received), 2u);
  EXPECT_EQ((*r)["results"]["recall"]["mode"], "full");
}

TEST(WfHistoryNodes, history_mode_drop_round_removes_the_tool_round) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, beijing_history());

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog slog(path);
  auto ir = WorkflowIR::parse(ir_with_mode("drop_round"));
  ASSERT_TRUE(ir) << ir.error();
  CompileOptions co;
  co.components = &f.components;
  auto wf = compile_workflow(*ir, f.registry, nullptr, co);
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.log = &slog;
  fo.input_messages = Json::array({cppchain::message::human_message("那上海呢？")});
  auto r = fire_once(*wf, f.config(tmp.str()), std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  ASSERT_EQ(f.model.last_received.size(), 3u);  // 工具轮整组没了
  EXPECT_EQ(f.model.last_received[0].text(), "北京天气怎么样？");
  EXPECT_EQ(f.model.last_received[1].text(), "北京今天 12°C，晴。");
  EXPECT_EQ(f.model.last_received[2].text(), "那上海呢？");
  for (Message const& m : f.model.last_received) EXPECT_TRUE(m.tool_calls.empty());
  EXPECT_EQ((*r)["results"]["recall"]["dropped_msgs"], 2);
}

TEST(WfHistoryNodes, history_mode_keep_evidence_keeps_the_call_but_drops_reasoning) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, beijing_history());

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog slog(path);
  auto ir = WorkflowIR::parse(ir_with_mode("keep_evidence"));
  ASSERT_TRUE(ir) << ir.error();
  CompileOptions co;
  co.components = &f.components;
  auto wf = compile_workflow(*ir, f.registry, nullptr, co);
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.log = &slog;
  fo.input_messages = Json::array({cppchain::message::human_message("那上海呢？")});
  auto r = fire_once(*wf, f.config(tmp.str()), std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  // 条数与 full 相同 —— 这是它和 drop_round 的本质区别：模型仍看得见"我调用过工具"
  ASSERT_EQ(f.model.last_received.size(), 5u);
  EXPECT_EQ(count_reasoning_blocks(f.model.last_received), 0u);
  ASSERT_EQ(f.model.last_received[1].tool_calls.size(), 1u);
  EXPECT_EQ(f.model.last_received[1].tool_calls[0].name, "fetch_price");
  EXPECT_EQ(f.model.last_received[2].text(), "price=12 晴");  // 短结果逐字保留
  EXPECT_EQ((*r)["results"]["recall"]["rewritten_msgs"], 0);
  EXPECT_LT((*r)["results"]["recall"]["after_tokens"].get<int>(),
            (*r)["results"]["recall"]["before_tokens"].get<int>());
}

TEST(WfHistoryNodes, compressed_history_is_never_written_back_to_the_log) {
  // 哨兵：压缩结果若落进日志，下次 replay 会把整段消息再追加一遍（add_messages 对无 id 消息是追加）。
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, beijing_history());

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog slog(path);
  auto ir = WorkflowIR::parse(ir_with_mode("@config.history_mode"));
  ASSERT_TRUE(ir) << ir.error();
  CompileOptions co;
  co.components = &f.components;
  auto wf = compile_workflow(*ir, f.registry, nullptr, co);
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.log = &slog;
  fo.input_messages = Json::array({cppchain::message::human_message("那上海呢？")});
  Json cfg = f.config(tmp.str());
  cfg["history_mode"] = "drop_round";
  auto r = fire_once(*wf, cfg, std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  for (Json const& e : engine_ok_exits(slog.records())) {
    if (e.value("node_kind", "") == "load_history") {
      EXPECT_FALSE(e.contains("update")) << "load_history 的 update 不应落盘";
    }
  }

  // 日志里仍是 4 条历史 + 1 条本轮输入 + 1 条本轮回复（未被压缩结果污染；
  // 若回声落了盘，drop_round 那 3 条会再追加一遍，这里就会明显大于 6）。
  EXPECT_EQ(replay_session_messages(path).size(), 6u);
}

TEST(WfHistoryNodes, unknown_history_mode_is_a_hard_error) {
  // fail-closed：拼错的 --set history_mode=keep_evidnce 必须报出来，而不是静默当 full 跑。
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, beijing_history());

  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  SessionLog slog(path);
  auto ir = WorkflowIR::parse(ir_with_mode("@config.history_mode"));
  ASSERT_TRUE(ir) << ir.error();
  CompileOptions co;
  co.components = &f.components;
  auto wf = compile_workflow(*ir, f.registry, nullptr, co);
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.log = &slog;
  fo.input_messages = Json::array({cppchain::message::human_message("那上海呢？")});
  Json cfg = f.config(tmp.str());
  cfg["history_mode"] = "keep_evidnce";
  auto r = fire_once(*wf, cfg, std::move(fo));
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("不在词表里"), std::string::npos) << r.error().message;
}
