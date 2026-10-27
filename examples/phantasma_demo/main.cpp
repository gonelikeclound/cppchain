// demo: phantasma_demo —— 防幻觉信号灯最小演示（problems/50，纯离线，可复现）
//
// 演示「工具 + 信号灯」在 wfr loop 里的落地闭环：
//   ① 范围内 · 无灯 → 强制合成 web_search 真查一轮 → 下一轮模型拿 grounding 作答；
//   ② 范围外 → 快答、不查、不看灯（verdict=absent）；
//   ③ 范围内 · 直接给 <anti-phantasma-off/>（reason 过对抗表）→ 豁免快路，不查（accepted）。
// 每场景断言"路径真被走到"（验证陷阱纪律：看 messages/回执，不看嘴上）：
//   标签零残留、web_search 恰查 0/1 次、最终文本、results.think.phantasma 三态。
// 会话日志留痕：<workspace>/.wfr/sessions/s1.log.jsonl（input 事件 + 全程节点日志）。
//
// 运行：build/dev/examples/phantasma_demo   （全离线，模型为脚本化桩 + 网搜为假引擎）
#include <cppchain/graph/graph.hpp>
#include <cppchain/message/message.hpp>
#include <cppchain/message/phantasma.hpp>
#include <cppchain/message/serializer.hpp>
#include <cppchain/model/chat_model.hpp>
#include <cppchain/model/task.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/tool/tool.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/refs.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/validator.hpp>

#include <deque>
#include <filesystem>
#include <generator>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::message;
using namespace cppchain::workflow;

using Json = nlohmann::json;

namespace {

// 脚本化模型：按序吐出预置回复（无真 LLM，纯离线）。
class ScriptModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::size_t invoked = 0;

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
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

// 假搜索引擎（只读工具）：按 query 查"知识表"返回 grounding。
tool::ToolSpec web_search_spec() {
  tool::ToolSpec sp;
  sp.name = "web_search";
  sp.description = "假搜索引擎（demo 桩）：小米产品线有确切发布时间数据。";
  sp.parameters = Json{{"type", "object"},
                       {"properties", Json{{"query", Json{{"type", "string"}}}}},
                       {"required", Json::array({"query"})}};
  sp.read_only = true;
  sp.handler = [](Json const&, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
    return Json{{"result", "小米 15 于 2025 年 10 月发布（假搜索引擎证据）。"}};
  };
  return sp;
}

// 工作目录（每次 run 独立，避免 SessionLog 残留干扰）。
struct TempDir {
  std::filesystem::path d;
  TempDir() {
    d = std::filesystem::temp_directory_path() /
        ("cppchain_pa_demo_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
  }
  ~TempDir() { std::filesystem::remove_all(d); }
};

// 一个用例的输入/期望。
struct Case {
  std::string name;
  Json ap;            // @config.anti_phantasma 的值
  std::string human;  // 用户提问（经 input 事件 + recall 读回）
  std::deque<Message> replies;
  int expect_searches;   // 期望 web_search 工具调用次数
  std::string expect_final;
  std::string expect_verdict;
};

// 循环：recall(load_history) → loop{ think(model, tools=search_ts) → route → act(run_tools) }
Json demo_ir() {
  Json body = Json::array({
      Json{{"id", "think"}, {"kind", "model"}, {"model", "m"},
           {"tools", "search_ts"}, {"next", "route"}},
      Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
           {"cases", Json{{"tool_calls", "act"}, {"text", "_break"},
                          {"invalid_tool_calls", "_fail"}}},
           {"default", "_break"}},
      Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}}});
  return Json{{"id", "wf_pa_demo"},
              {"nodes", Json::array(
                            {Json{{"id", "recall"},
                                  {"kind", "load_history"},
                                  {"path", "@config.workspace"},
                                  {"session", "@config.session"},
                                  {"next", "loop"}},
                             Json{{"id", "loop"}, {"kind", "loop"}, {"max", 8},
                                  {"next", "_turn_end"}, {"body", std::move(body)}}})}};
}

struct Result {
  Json state;
  std::size_t model_invoked = 0;
  std::string log_path;
};

// 跑一个用例：编译 → fire（input 经 SessionLog + recall 读回）→ 返回终态与观测。
std::optional<Result> run_case(Case const& c) {
  tool::ToolRegistry builtin;
  builtin.add(web_search_spec());
  ScriptModel model;
  model.replies = c.replies;  // 把用例预置回复灌进脚本化模型
  ComponentRegistry components;
  components.models["m"] = &model;
  components.toolsets["search_ts"] = {"web_search"};
  state::MemoryCheckpointer cp;

  TempDir tmp;
  std::string const ws = (tmp.d / "ws").string();
  std::string const ss = "s1";
  Json const cfg = Json{{"workspace", ws}, {"session", ss}, {"anti_phantasma", c.ap}};

  auto parsed = WorkflowIR::parse(demo_ir());
  if (!parsed) {
    std::println("  [err] IR parse: {}", parsed.error());
    return std::nullopt;
  }
  ValidatorOptions vopts;
  vopts.registry = &builtin;
  vopts.components = &components;
  auto v = validate(*parsed, vopts);
  if (!v.ok()) {
    std::println("  [err] validate: {}", v.describe());
    return std::nullopt;
  }
  CompileOptions copts;
  copts.components = &components;
  auto wf = compile_workflow(*parsed, builtin, &cp, copts);
  if (!wf) {
    std::println("  [err] compile: {}", wf.error().message);
    return std::nullopt;
  }

  FireOptions fo;
  fo.thread_id = "t1";
  std::string const log_path = session_path(ws, ss);
  SessionLog slog(log_path);
  fo.log = &slog;
  Json inp = Json::array();
  inp.push_back(Json(cppchain::message::human_message(c.human)));
  fo.input_messages = std::move(inp);
  auto st = fire_once(*wf, cfg, std::move(fo));
  if (!st) {
    std::println("  [err] fire: {}（model invoked = {}）", st.error().message, model.invoked);
    return std::nullopt;
  }
  return Result{*st, model.invoked, log_path};
}

// —— 断言帮手（"看证据，不看嘴上"）——
std::string last_ai_text(Json const& st) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return "";
  for (auto itm = it->rbegin(); itm != it->rend(); ++itm) {
    if (itm->value("type", "") != "ai") continue;
    std::string out;
    if (itm->contains("content") && (*itm)["content"].is_array())
      for (Json const& blk : (*itm)["content"])
        if (blk.is_object() && blk.contains("text")) out += blk["text"].get<std::string>();
    return out;
  }
  return "";
}
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
bool any_tag_left(Json const& st) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return false;
  for (Json const& m : *it) {
    if (!m.contains("content") || !m["content"].is_array()) continue;
    for (Json const& blk : m["content"])
      if (blk.is_object() && blk.contains("text") &&
          blk["text"].get<std::string>().find("<anti-phantasma-off") != std::string::npos)
        return true;
  }
  return false;
}

bool verify(Case const& c, Result const& r, std::vector<std::string>& out) {
  bool ok = true;
  auto chk = [&](bool b, std::string const& what) {
    out.push_back(std::string("  ") + (b ? "[✓] " : "[✗] ") + what);
    ok = ok && b;
  };
  std::string const final = last_ai_text(r.state);
  int const searches = web_search_calls(r.state);
  std::string const verdict = r.state["results"]["think"]["phantasma"].value("verdict", "");
  chk(!any_tag_left(r.state), "消息里无 <anti-phantasma-off> 标签残留");
  chk(searches == c.expect_searches, "web_search 调用恰为 " + std::to_string(c.expect_searches) +
                                          " 次（实际 " + std::to_string(searches) + "）");
  chk(final == c.expect_final, "最终答案 ==「" + c.expect_final + "」");
  chk(verdict == c.expect_verdict, "results.think.phantasma.verdict == " + c.expect_verdict +
                                       "（实际 " + verdict + "）");
  return ok;
}

}  // namespace

int main() {
  std::println("防幻觉信号灯最小 demo（problems/50 · 全离线 · 可复现）\n");

  std::vector<Case> cases = {
      // ① 范围内（域命中「小米」）+ 无灯 → 强制查一轮 → grounded 作答
      {"范围内·无灯→强制查证→grounded", Json{{"enabled", true}, {"domains", Json::array({"小米"})}},
       "小米 15 发布时间？",
       {ai_message("小米 15 是 2024 年发布的。"),
        ai_message("<anti-phantasma-off reason=\"web-search\"/>\n查证后：2025 年发布。")},
       1, "查证后：2025 年发布。", "accepted"},
      // ② 范围外（域「苹果」，提问「小米」未命中）→ 快答、不查、不看灯
      {"范围外·域未命中→快答", Json{{"enabled", true}, {"domains", Json::array({"苹果"})}},
       "小米 15 发布时间？",
       {ai_message("iPhone16 发布时间未知。")},
       0, "iPhone16 发布时间未知。", "absent"},
      // ③ 范围内 + 直接给有效豁免信号（reason 过对抗表）→ 免查快路
      {"范围内·直接有效信号→免查", Json{{"enabled", true}, {"domains", Json::array({"小米"})}},
       "小米 15 发布时间？",
       {ai_message("<anti-phantasma-off reason=\"well-known\"/>\n小米 15 于 2025 年发布。")},
       0, "小米 15 于 2025 年发布。", "accepted"},
  };

  bool all = true;
  for (Case const& c : cases) {
    std::println("场景：{}", c.name);
    auto r = run_case(c);
    if (!r) {
      std::println("  [✗] run_case 失败（校验/编译/fire）");
      all = false;
      continue;
    }
    std::vector<std::string> out;
    bool const ok = verify(c, *r, out);
    for (std::string const& line : out) std::println("{}", line);
    std::println("  模型调用次数：{}（{}），会话日志：{}", r->model_invoked,
                 ok ? "断言路径真被走到" : "注意：断言失败", r->log_path);
    all = all && ok;
  }

  std::println("");
  if (all) {
    std::println("验收结论：三场景全过（标签零残留 · 强制查证恰 1 次 · 三态回执正确）。");
    return 0;
  }
  std::println("验收结论：存在失败项，见上 [✗]。");
  return 1;
}