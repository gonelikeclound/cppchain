// wfr-㊵ 测试：两个历史处理节点（`summarize` / `refine_history`）。
//
// 覆盖三层：
//   · 校验（VN15/VN16 词表与正整数、VG10"必须在 load_history 之后"、VR2 白名单）
//   · 运行期（折叠/删改真的作用到 state.messages；op 落日志；stats 发布）
//   · 关键行为（**不重复摘要**、LLM 失败降级、按工具类别过滤）
#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <expected>
#include <filesystem>
#include <generator>
#include <span>
#include <string>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/validator.hpp"
#include "cppchain/workflow/wfr.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;
using cppchain::message::Role;
using demo::fakes::FakeToolSinks;
using demo::fakes::make_fake_tools;

namespace {

// 记录"模型实际收到什么"的桩（沿用 history_nodes_test.cpp 的手法）
class ScriptedModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::vector<Message> last_received;
  std::size_t invoked = 0;
  bool fail_next = false;

  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    last_received.assign(msgs.begin(), msgs.end());
    ++invoked;
    if (fail_next) {
      fail_next = false;   // 只失败一次（否则后面的 think 也跟着失败，测不出降级）
      return std::unexpected(model::ModelError{model::ModelError::Kind::Parse, 0, "boom", {}});
    }
    if (replies.empty()) return std::unexpected(model::ModelError{model::ModelError::Kind::Parse, 0, "no reply", {}});
    Message m = replies.front();
    replies.pop_front();
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
  static std::atomic<unsigned> counter;
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_histproc_" + std::to_string(counter.fetch_add(1)));
  TempDir() { std::filesystem::create_directories(dir); }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string str() const { return dir.string(); }
};
std::atomic<unsigned> TempDir::counter{0};

// recall(load_history) → <节点> → think(model) → _turn_end
Json ir_with(Json node, std::string const& node_id = "proc") {
  node["id"] = node_id;
  if (!node.contains("next")) node["next"] = "think";
  return Json{{"id", "wf_hist"},
              {"nodes", Json::array({Json{{"id", "recall"}, {"kind", "load_history"},
                                          {"path", "@config.workspace"},
                                          {"session", "@config.session"},
                                          {"next", node_id}},
                                     node,
                                     Json{{"id", "think"}, {"kind", "model"}, {"model", "m"},
                                          {"next", "_turn_end"}}})}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry components;
  Fixture() { components.models["m"] = &model; }

  std::expected<CompiledWorkflow, WorkflowError> compile(Json const& ir_json) {
    auto ir = WorkflowIR::parse(ir_json);
    if (!ir) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, ir.error()});
    CompileOptions o;
    o.components = &components;
    return compile_workflow(*ir, registry, nullptr, o);
  }
  Json config(std::string const& ws, std::string const& sess = "cli") const {
    return Json{{"workspace", ws}, {"session", sess}};
  }
};

// 造一段"有工具轮"的历史写进日志（第二轮才能被 load_history 读到）
void seed_history(std::string const& path, std::size_t rounds,
                  std::string const& tool_name = "fetch_price") {
  SessionLog log(path);
  Json msgs = Json::array({Json(message::human_message("北京天气怎么样？"))});
  for (std::size_t i = 0; i < rounds; ++i) {
    Message call = message::ai_message("");
    call.tool_calls.push_back(
        message::ToolCall{"c" + std::to_string(i), tool_name, Json::object()});
    msgs.push_back(Json(call));
    msgs.push_back(Json(message::tool_message("price=799 in_stock=true", "c" + std::to_string(i))));
    msgs.push_back(Json(message::ai_message("北京 799，有货。")));
    msgs.push_back(Json(message::human_message("那上海呢？")));
  }
  log.engine_enter("r-1", "__input__", "input");
  log.engine_exit("r-1", "__input__", "input", true, "ok", 0, Json{{"messages", msgs}});
}

Json run_once(Fixture& f, Json const& ir_json, std::string const& ws, std::string const& sess = "cli",
              Json const& extra_config = Json::object()) {
  auto wf = f.compile(ir_json);
  EXPECT_TRUE(wf) << (wf ? "" : wf.error().message);
  SessionLog log(session_path(ws, sess));
  FireOptions fo;
  fo.log = &log;
  Json cfg = f.config(ws, sess);
  for (auto it = extra_config.begin(); it != extra_config.end(); ++it) cfg[it.key()] = it.value();
  auto r = fire_once(*wf, cfg, std::move(fo));
  EXPECT_TRUE(r) << (r ? "" : r.error().message);
  return r ? *r : Json::object();
}

}  // namespace

// ---- 校验 ----

TEST(WfHistoryProcess, summarize_requires_a_registered_model) {
  Fixture f;
  auto wf = f.compile(ir_with(Json{{"kind", "summarize"}, {"model", "不存在的组件"}}));
  ASSERT_FALSE(wf);
  EXPECT_NE(wf.error().message.find("VN15"), std::string::npos) << wf.error().message;
}

TEST(WfHistoryProcess, summarize_rejects_bad_literals_with_a_helper_message) {
  Fixture f;
  for (auto const& [key, bad] : std::vector<std::pair<char const*, char const*>>{
           {"when", "sometimes"}, {"style", "verbose"}, {"summary_role", "robot"},
           {"on_fail", "explode"}, {"keep_last", "0"}, {"max_msgs", "abc"}}) {
    auto wf = f.compile(ir_with(Json{{"kind", "summarize"}, {"model", "m"}, {key, bad}}));
    ASSERT_FALSE(wf) << key << " 竟然通过了";
    EXPECT_NE(wf.error().message.find("VN15"), std::string::npos) << wf.error().message;
  }
}

TEST(WfHistoryProcess, refine_rejects_bad_literals_and_unknown_rules_and_categories) {
  Fixture f;
  for (Json const& bad : {Json{{"kind", "refine_history"}, {"preset", "tiny"}},
                          Json{{"kind", "refine_history"}, {"clip_scope", "everything"}},
                          Json{{"kind", "refine_history"}, {"clip_mode", "summarize"}},
                          Json{{"kind", "refine_history"}, {"rules", Json::array({"drop_everything"})}},
                          Json{{"kind", "refine_history"}, {"rules", Json::array({"drop_category:bogus"})}},
                          Json{{"kind", "refine_history"}, {"tool_budget", "-1"}}}) {
    auto wf = f.compile(ir_with(bad));
    ASSERT_FALSE(wf) << bad.dump();
    EXPECT_NE(wf.error().message.find("VN16"), std::string::npos) << wf.error().message;
  }
}

TEST(WfHistoryProcess, history_nodes_must_come_after_load_history) {
  // VG10：历史还没投影出来，处理节点无事可做
  Fixture f;
  for (char const* kind : {"summarize", "refine_history"}) {
    Json node{{"id", "proc"}, {"kind", kind}, {"next", "think"}};
    if (std::string(kind) == "summarize") node["model"] = "m";
    Json ir_json{{"id", "wf_before"},
                 {"nodes", Json::array({node,
                                        Json{{"id", "recall"}, {"kind", "load_history"},
                                             {"path", "@config.workspace"},
                                             {"session", "@config.session"}, {"next", "think"}}})}};
    auto wf = f.compile(ir_json);
    ASSERT_FALSE(wf) << kind;
    EXPECT_NE(wf.error().message.find("VG10"), std::string::npos) << wf.error().message;
  }
}

TEST(WfHistoryProcess, results_are_referenceable_but_expr_still_is_not) {
  // VR2 白名单只**加项**，不是放开：指向 expr 的 $x.result.f 仍被拒
  auto ir = WorkflowIR::parse(ir_with(Json{{"kind", "summarize"}, {"model", "m"}}, "sum"));
  ASSERT_TRUE(ir) << ir.error();
  ValidatorOptions o;
  ComponentRegistry comps;
  ScriptedModel m;
  comps.models["m"] = &m;
  o.components = &comps;
  EXPECT_TRUE(validate(*ir, o).ok());

  auto bad = WorkflowIR::parse(Json{
      {"id", "wf_bad"},
      {"nodes", Json::array(
                    {Json{{"id", "recall"}, {"kind", "load_history"},
                          {"path", "@config.workspace"}, {"session", "@config.session"},
                          {"next", "e0"}},
                     Json{{"id", "e0"}, {"kind", "expr"}, {"expr", "true"}, {"then", "out"},
                          {"else", "_turn_end"}},
                     Json{{"id", "out"}, {"kind", "echo"}, {"text", "$e0.result.x"},
                          {"next", "_turn_end"}}})}});
  ASSERT_TRUE(bad) << bad.error();
  auto const vr = validate(*bad, o);
  EXPECT_FALSE(vr.ok());
  EXPECT_NE(vr.describe().find("VR2"), std::string::npos) << vr.describe();
}

// ---- summarize 运行期 ----

TEST(WfHistoryProcess, summarize_folds_and_writes_the_op_to_the_log) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 5);   // 5 轮 → 21 条

  Fixture f;
  f.model.replies.push_back(message::ai_message("用户问过北京天气，结论 799 有货。"));
  f.model.replies.push_back(message::ai_message("好的"));

  auto const r = run_once(f, ir_with(Json{{"kind", "summarize"}, {"model", "m"},
                                          {"when", "always"}, {"keep_last", "3"},
                                          {"style", "evidence"}},
                                    "sum"),
                          tmp.str());
  ASSERT_TRUE(r.contains("results"));
  auto const& s = r["results"]["sum"];
  EXPECT_EQ(s["action"], "folded");
  EXPECT_GT(s["summarized_msgs"].get<int>(), 0);
  // keep_last=3 是**下限**：safe_boundary 为避免"保留区以孤儿 ToolMessage 开头"可能多留
  // 一两条（真实端点会因孤儿 ToolMessage 直接 400）。所以断言 ≥ 3 而不是 == 3，
  // 并单独验证"保留区不以工具消息开头"这条硬性质。
  EXPECT_GE(s["kept_msgs"].get<int>(), 3);
  EXPECT_LT(s["after_tokens"].get<int>(), s["before_tokens"].get<int>());

  // op 落进了日志（重跑不再重算的关键）
  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(all.find("\"op\":\"fold\""), std::string::npos) << "摘要 op 必须落日志";
  EXPECT_NE(all.find("用户问过北京天气"), std::string::npos) << "摘要文本应在 op 里";

  // 折叠后：摘要在前，保尾 3 条在后
  auto const replayed = replay_session_messages(path);
  ASSERT_GE(replayed.size(), 4u);
  EXPECT_TRUE(message::is_summary_message(replayed.front()));
  ASSERT_GE(replayed.size(), 2u);
  EXPECT_NE(replayed[1].type, Role::Tool) << "保留区不得以孤儿 ToolMessage 开头";
  EXPECT_FALSE(message::validate_sequence(replayed).has_value());
}

TEST(WfHistoryProcess, summarize_is_not_redone_on_the_next_run) {
  // "摘要写进日志"的回报：第二轮 replay 会 apply 上一轮的 fold op → 已折叠且未超阈值 → skip，
  // 模型调用次数不再增加（不重算、不重复花钱）
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 5);

  Json const ir = ir_with(Json{{"kind", "summarize"}, {"model", "m"},
                               {"when", "messages"}, {"max_msgs", "10"},
                               {"keep_last", "3"}, {"style", "concise"}},
                          "sum");

  Fixture f1;
  f1.model.replies.push_back(message::ai_message("摘要一"));
  f1.model.replies.push_back(message::ai_message("回答一"));
  auto const r1 = run_once(f1, ir, tmp.str(), "cli");
  EXPECT_EQ(r1["results"]["sum"]["action"], "folded");
  std::size_t const invoked_1 = f1.model.invoked;

  Fixture f2;   // 新一轮（新进程语义）
  f2.model.replies.push_back(message::ai_message("回答二"));
  auto const r2 = run_once(f2, ir, tmp.str(), "cli");
  EXPECT_EQ(r2["results"]["sum"]["action"], "skipped") << "第二轮不该重新摘要";
  EXPECT_EQ(f2.model.invoked, 1u) << "第二轮只该有一次（回答用的）模型调用";
  EXPECT_GE(invoked_1, 2u);
}

TEST(WfHistoryProcess, summarize_skips_when_off_or_below_threshold) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 3);

  Fixture f;
  f.model.replies.push_back(message::ai_message("答"));
  auto const r = run_once(f, ir_with(Json{{"kind", "summarize"}, {"model", "m"},
                                          {"when", "off"}, {"keep_last", "2"}},
                                    "sum"),
                          tmp.str(), "cli");
  EXPECT_EQ(r["results"]["sum"]["action"], "skipped");
  EXPECT_EQ(r["results"]["sum"]["reason"], "when_off");
  EXPECT_EQ(f.model.invoked, 1u) << "when=off 不该调摘要模型";
}

TEST(WfHistoryProcess, summarize_failure_keeps_the_full_history_by_default) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 4);

  Fixture f;
  f.model.fail_next = true;                       // 摘要调用失败
  f.model.replies.push_back(message::ai_message("答"));
  auto const r = run_once(f, ir_with(Json{{"kind", "summarize"}, {"model", "m"},
                                          {"when", "always"}, {"keep_last", "2"},
                                          {"on_fail", "skip"}},
                                    "sum"),
                          tmp.str());
  EXPECT_EQ(r["results"]["sum"]["action"], "failed");
  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(all.find("\"op\":\"fold\""), std::string::npos) << "失败不该发 op（保留全量历史）";
}

TEST(WfHistoryProcess, summarize_placeholder_still_folds) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 4);

  Fixture f;
  f.model.fail_next = true;
  f.model.replies.push_back(message::ai_message("答"));
  auto const r = run_once(f, ir_with(Json{{"kind", "summarize"}, {"model", "m"},
                                          {"when", "always"}, {"keep_last", "2"},
                                          {"on_fail", "placeholder"}},
                                    "sum"),
                          tmp.str());
  EXPECT_EQ(r["results"]["sum"]["action"], "folded");
  auto const replayed = replay_session_messages(path);
  ASSERT_FALSE(replayed.empty());
  EXPECT_TRUE(message::is_summary_message(replayed.front()));
  EXPECT_NE(replayed.front().text().find("摘要生成失败"), std::string::npos);
}

// ---- refine_history 运行期 ----

TEST(WfHistoryProcess, refine_drops_by_category_and_publishes_stats) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 3, "fetch_web");   // 3 轮 retrieval 类的工具调用

  Fixture f;
  // 注册一个**带类别**的工具并让历史用它 —— 类别过滤的依据就是这张"名字→类别"表
  //（节点在编译期从注册表快照，交给 agent 层的规则引擎当纯数据）
  f.registry.add(tool::make_tool("fetch_web", "联网取数", Json::object(),
                                 [](Json const&, tool::ToolContext&) {
                                   return std::expected<Json, tool::ToolError>{
                                       Json{{"doc", "很长的网页正文"}}};
                                 })
                     .with_category("retrieval"));
  f.model.replies.push_back(message::ai_message("答"));

  Json const ir = ir_with(Json{{"kind", "refine_history"},
                               {"preset", "aggressive"},
                               {"categories", Json::array({"retrieval"})},
                               {"clip_scope", "both"}},
                          "refine");
  auto const r = run_once(f, ir, tmp.str());

  auto const& s = r["results"]["refine"];
  EXPECT_EQ(s["action"], "transformed");
  EXPECT_GE(s["dropped_groups"].get<int>(), 1);        // retrieval 的工具轮被整组删
  EXPECT_GE(s["stripped_reasoning"].get<int>(), 0);
  EXPECT_LT(s["after_msgs"].get<int>(), s["before_msgs"].get<int>());
  EXPECT_NE(s["rules"].get<std::string>().find("drop_category:retrieval"), std::string::npos);

  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(all.find("\"op\":\"transform\""), std::string::npos) << "transform op 必须落日志";

  // 删改之后协议仍然合法（不能留下 dangling tool_calls）
  auto const replayed = replay_session_messages(path);
  EXPECT_FALSE(message::validate_sequence(replayed).has_value());
}

TEST(WfHistoryProcess, refine_is_a_noop_when_nothing_matches) {
  TempDir tmp;
  std::string const path = session_path(tmp.str(), "cli");
  seed_history(path, 2);

  Fixture f;
  f.model.replies.push_back(message::ai_message("答"));
  auto const r = run_once(f, ir_with(Json{{"kind", "refine_history"},
                                          {"preset", "minimal"}},   // 只去思考过程；历史里没有
                                    "refine"),
                          tmp.str());
  EXPECT_EQ(r["results"]["refine"]["action"], "noop");
  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(all.find("\"op\":\"transform\""), std::string::npos) << "无事可做就不该发 op";
}
