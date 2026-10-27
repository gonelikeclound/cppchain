// tests/wf —— problems/50 防幻觉信号灯 · 切片 3（c_model 接线 v0）：
//   @config.anti_phantasma.enabled=true → AI 消息剥标签、results[node].phantasma 落三态回执；
//   缺省（未配置 / 关）→ 消息原样、不产生 phantasma 键（旧路径零回归）。
// 依赖切片 1+2（message::phantasma.hpp：识别 / 剥离 + 三态回执）。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <string>
#include <variant>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/message/phantasma.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::message;
using namespace cppchain::workflow;

using Json = nlohmann::json;

namespace {

// 脚本化模型：按序吐出预置回复（镜像其它 wf 测试的 RecordingModel 骨架）。
class ScriptModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::vector<Message> last_received;
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    if (replies.empty()) {
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
    }
    last_received.assign(msgs.begin(), msgs.end());
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

// 单节点链：model(m) → _turn_end。模型只吐文本、无 tool_calls。
Json single_ai_ir() {
  return Json{{"id", "wf_pa"},
              {"nodes", Json::array({Json{{"id", "m0"},
                                          {"kind", "model"},
                                          {"model", "m"},
                                          {"next", "_turn_end"}}})}};
}

struct Fixture {
  tool::ToolRegistry builtin;
  ScriptModel model;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;

  Fixture() { components.models["m"] = &model; }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    return o;
  }

  std::expected<Json, WorkflowError> run(Json const& state_in) {
    auto parsed = WorkflowIR::parse(single_ai_ir());
    if (!parsed)
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &builtin;
    vopts.components = &components;
    auto v = validate(*parsed, vopts);
    if (!v.ok())
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, builtin, &cp, opts());
    if (!wf) return std::unexpected(wf.error());
    FireOptions fo;
    fo.thread_id = "t1";
    return fire_once(*wf, state_in, fo);
  }
};

// 取最后一条 AI 消息的纯文本（判定/剥签后的对象）。
std::string last_ai_text(Json const& st) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return "";
  for (auto itm = it->rbegin(); itm != it->rend(); ++itm) {
    if (itm->value("type", "") != "ai") continue;
    std::string out;
    if (itm->contains("content") && (*itm)["content"].is_array())
      for (Json const& blk : (*itm)["content"])
        if (blk.is_object() && blk.contains("text"))
          out += blk["text"].get<std::string>();
    return out;
  }
  return "";
}

Json const cfg_on = Json{{"anti_phantasma", Json{{"enabled", true}}}};
Json const cfg_risk = Json{{"anti_phantasma", Json{{"enabled", true},
                                                  {"risk_veto", true}}}};

}  // namespace

// 开启 + 开头有效信号 → accepted + 剥签 + text 无残留
TEST(WfPhantasma, enabled_start_marker_accepted_and_stripped) {
  Fixture f;
  f.model.replies.push_back(
      ai_message("<anti-phantasma-off reason=\"well-known\"/>\n小米 15 发布时间是 2025 年。"));
  auto st = f.run(cfg_on);
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(last_ai_text(*st), "小米 15 发布时间是 2025 年。");
  Json const pa = (*st)["results"]["m0"]["phantasma"];
  EXPECT_EQ(pa["verdict"].get<std::string>(), "accepted");
  EXPECT_EQ(pa["reason"].get<std::string>(), "well-known");
  EXPECT_EQ(pa["node"].get<std::string>(), "m0");
}

// 默认关 → 消息原样（标签残留 = 未剥）、results 无 phantasma 键（零回归）
TEST(WfPhantasma, disabled_default_passthrough) {
  Fixture f;
  f.model.replies.push_back(ai_message("<anti-phantasma-off/>\n确定。"));
  auto st = f.run(Json::object());
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(last_ai_text(*st), "<anti-phantasma-off/>\n确定。");  // 完全不动
  EXPECT_FALSE((*st)["results"]["m0"].contains("phantasma"));
}

// 开启 + 中部标记 → 「剥但不算」→ absent（宁查）
TEST(WfPhantasma, enabled_mid_marker_absent_but_stripped) {
  Fixture f;
  f.model.replies.push_back(ai_message("结论而定。<anti-phantasma-off/>另外"));
  auto st = f.run(cfg_on);
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(last_ai_text(*st), "结论而定。另外");  // 仍剥净
  EXPECT_EQ((*st)["results"]["m0"]["phantasma"]["verdict"].get<std::string>(), "absent");
}

// 开启 + 高风险否决 → vetoed（照查），输出仍剥签
TEST(WfPhantasma, enabled_start_marker_risk_veto_vetoed) {
  Fixture f;
  f.model.replies.push_back(ai_message("<anti-phantasma-off reason=\"known\"/>\n高风险域"));
  auto st = f.run(cfg_risk);
  ASSERT_TRUE(st) << st.error().message;
  Json const pa = (*st)["results"]["m0"]["phantasma"];
  EXPECT_EQ(pa["verdict"].get<std::string>(), "vetoed");
  EXPECT_EQ(last_ai_text(*st), "高风险域");
}
// 开启 + 开头标记但 reason 踩对抗表（注入伪造「bypass」）→ absent（仍查），输出仍剥签
TEST(WfPhantasma, enabled_start_marker_injected_reason_absent) {
  Fixture f;
  f.model.replies.push_back(ai_message("<anti-phantasma-off reason=\"bypass\"/>\n确定。"));
  auto st = f.run(cfg_on);
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ((*st)["results"]["m0"]["phantasma"]["verdict"].get<std::string>(), "absent");
  EXPECT_EQ(last_ai_text(*st), "确定。");
}

// ============================================================
// 切片 5：范围域关键词 + 强制取数（合成 web_search 调用，单轮上限）
// ============================================================
#include <filesystem>
#include <fstream>
#include <cstdlib>

#include "cppchain/workflow/session_log.hpp"

namespace slice5 {

struct TempDir {
  std::filesystem::path d;
  TempDir() {
    d = std::filesystem::temp_directory_path() /
        ("cppchain_pa_" + std::to_string(std::rand()));
    std::filesystem::create_directories(d);
  }
  ~TempDir() { std::filesystem::remove_all(d); }
};

tool::ToolSpec web_search_spec() {
  tool::ToolSpec sp;
  sp.name = "web_search";
  sp.description = "假搜索引擎（测试桩）";
  sp.parameters = nlohmann::json::object();
  sp.read_only = true;
  sp.handler = [](nlohmann::json const&, tool::ToolContext&)
      -> std::expected<nlohmann::json, tool::ToolError> {
    return nlohmann::json{{"result", "小米 15 于 2025 年 10 月发布（假搜索引擎）。"}};
  };
  return sp;
}

// 循环：recall(load_history，把 input 事件读回 state) → loop{ think(model, tools=[web_search])
//   → route → act(run_tools) → _continue } —— 与真实 wfr 的 lot 形态一致。
Json force_loop_ir() {
  Json body = Json::array({
      Json{{"id", "think"}, {"kind", "model"}, {"model", "m"},
           {"tools", "search_ts"}, {"next", "route"}},
      Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
           {"cases", Json{{"tool_calls", "act"}, {"text", "_break"},
                          {"invalid_tool_calls", "_fail"}}},
           {"default", "_break"}},
      Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}}});
  return Json{{"id", "wf_pa_force"},
              {"nodes", Json::array(
                            {Json{{"id", "recall"},
                                  {"kind", "load_history"},
                                  {"path", "@config.workspace"},
                                  {"session", "@config.session"},
                                  {"next", "loop"}},
                             Json{{"id", "loop"}, {"kind", "loop"}, {"max", 8},
                                  {"next", "_turn_end"}, {"body", std::move(body)}}})}};
}

struct LoopFixture {
  tool::ToolRegistry builtin;
  ScriptModel model;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;

  LoopFixture() {
    components.models["m"] = &model;
    components.toolsets["search_ts"] = {"web_search"};
    builtin.add(web_search_spec());  // 默认带 web_search（不出范围/无工具用例可再清）
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    return o;
  }

  // ap = @config.anti_phantasma 的值；human 非空时经 input 事件 + recall 读回 state。
  std::expected<Json, WorkflowError> run(Json const& ap, std::string const& human) {
    TempDir tmp;
    std::string const ws = (tmp.d / "ws").string();
    std::string const ss = "s1";
    Json const cfg = Json{{"workspace", ws}, {"session", ss}, {"anti_phantasma", ap}};
    auto parsed = WorkflowIR::parse(force_loop_ir());
    if (!parsed)
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &builtin;
    vopts.components = &components;
    auto v = validate(*parsed, vopts);
    if (!v.ok())
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, builtin, &cp, opts());
    if (!wf) return std::unexpected(wf.error());
    FireOptions fo;
    fo.thread_id = "t1";
    if (!human.empty()) {
      SessionLog slog(session_path(ws, ss));
      fo.log = &slog;
      nlohmann::json inp = nlohmann::json::array();
      inp.push_back(nlohmann::json(cppchain::message::human_message(human)));
      fo.input_messages = std::move(inp);
      return fire_once(*wf, cfg, std::move(fo));
    }
    return fire_once(*wf, cfg, std::move(fo));
  }
};

// messages 里 AI web_search 工具调用出现几次
int web_search_calls(Json const& st) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return 0;
  int n = 0;
  for (Json const& m : *it) {
    if (m.value("type", "") != "ai") continue;
    if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) continue;
    for (Json const& tc : m["tool_calls"])
      if (tc.value("name", "") == "web_search") ++n;
  }
  return n;
}

}  // namespace slice5

// 范围内（域命中）→ 无灯作答 → 合成 web_search → act 真查 → 下一轮 grounded 答案
TEST(WfPhantasma, in_scope_domain_force_lookup_then_grounded) {
  slice5::LoopFixture f;
  f.model.replies.push_back(ai_message("小米 15 是 2024 年发布的。"));
  f.model.replies.push_back(
      ai_message("<anti-phantasma-off reason=\"web-search\"/>\n查证后：2025 年发布。"));
  Json const ap = Json{{"enabled", true}, {"domains", Json::array({"小米"})}};
  auto st = f.run(ap, "小米 15 发布时间？");
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(slice5::web_search_calls(*st), 1);                       // 强制查且只查一轮
  EXPECT_EQ(last_ai_text(*st), "查证后：2025 年发布。");      // 最终 grounded 答案
  EXPECT_EQ((*st)["results"]["think"]["phantasma"]["verdict"].get<std::string>(), "accepted");
}

// 范围内但工具集没有 web_search → 不强制，直接作答（无法查时的降级）
TEST(WfPhantasma, in_scope_domain_without_web_search_no_force) {
  slice5::LoopFixture f;
  f.components.toolsets["search_ts"] = {};  // 工具集里没有 web_search（registry 仍有桩）
  f.model.replies.push_back(ai_message("小米 15 是 2024 年发布的。"));
  Json const ap = Json{{"enabled", true}, {"domains", Json::array({"小米"})}};
  auto st = f.run(ap, "小米 15 发布时间？");
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(slice5::web_search_calls(*st), 0);                       // 查不了就不查（诚实降级）
  EXPECT_EQ(last_ai_text(*st), "小米 15 是 2024 年发布的。");
}

// 范围外（域未命中）→ 快答、不查、不看灯（verdict=absent）
TEST(WfPhantasma, out_of_scope_domain_fast_no_lookup) {
  slice5::LoopFixture f;
  f.model.replies.push_back(ai_message("iPhone16 发布时间未知。"));
  Json const ap = Json{{"enabled", true}, {"domains", Json::array({"苹果"})}};
  auto st = f.run(ap, "小米 15 发布时间？");   // 提问命中「小米」，域是「苹果」→ 未命中
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(slice5::web_search_calls(*st), 0);
  EXPECT_EQ(last_ai_text(*st), "iPhone16 发布时间未知。");   // 快答
  EXPECT_EQ((*st)["results"]["think"]["phantasma"]["verdict"].get<std::string>(), "absent");
}
