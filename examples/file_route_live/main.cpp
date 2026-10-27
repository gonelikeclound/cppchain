// demo: file_route_live —— problems/02 S6 真机评测（需要 .env 的 LLM_API_KEY）
//
// 对同一批"超长工具返回"任务跑两条臂：
//   A route 臂：recall → files(file_context 预取) → model（单次作答，模型不挑工具）
//   B base  臂：recall → loop{model(带文件工具) → route → act}（模型自己决定调什么）
// 指标：正确（答案含期望事实）、模型工具调用轮数、usage token、路由证据（route/attempts）。
// 语料与 questions 是确定性的（答案位置预埋），所以"正确率"可直接断言，不依赖 LLM 判分。
//
// 运行：./build/dev/examples/file_route_live
//   .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（可选 LLM_CA_BUNDLE）
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/message.hpp>
#include <cppchain/message/serializer.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/tool/file_tools.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/refs.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/validator.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace fs = std::filesystem;

namespace {

using Json = nlohmann::json;

std::string ca_bundle_from_env() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (!ca.empty()) return ca;
  if (char const* home = std::getenv("HOME")) {
    auto const geo = fs::path(home) / ".geo" / "corp-ca.pem";
    if (fs::exists(geo)) return geo.string();
  }
  return {};
}

void write_text(fs::path const& p, std::string const& s) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

fs::path ws_root() { return "tmp/file_route_live_ws"; }

void build_corpus() {
  fs::remove_all(ws_root());
  fs::create_directories(ws_root());

  Json doc;
  doc["services"] = Json::array();
  for (int i = 0; i < 60; ++i)
    doc["services"].push_back({{"name", "svc_" + std::to_string(i)},
                               {"ports", Json::array({8000 + i, 9000 + i})}});
  doc["services"][3] = {{"name", "pay"}, {"ports", Json::array({8080, 8443})}};
  write_text(ws_root() / "config.json", doc.dump(2));

  std::string yaml = "server:\n  host: localhost\n  ports:\n    - 80\n    - 443\n";
  for (int i = 0; i < 300; ++i) yaml += "filler_" + std::to_string(i) + ": \"填充 " + std::to_string(i) + "\"\n";
  write_text(ws_root() / "app.yaml", yaml);

  std::string log;
  for (int i = 0; i < 800; ++i) {
    if (i == 520)
      log += "2026-10-27T12:00:00Z ERROR code=0x9F3A handler=payment retry=3\n";
    else
      log += "2026-10-27T12:00:00Z INFO request id=" + std::to_string(100000 + i) +
             " status=200 path=/api/v1/orders\n";
  }
  log += "LAST-LINE-尾标记\n";
  write_text(ws_root() / "app.log", log);

  std::string md = "# 年度报告\n\n";
  for (int i = 1; i <= 12; ++i) {
    md += "## Section " + std::to_string(i) + "\n";
    if (i == 7) md += "模型评测结果：召回率 = 0.83，精确率 = 0.91。\n";
    for (int k = 0; k < 10; ++k)
      md += "本节正文段落 " + std::to_string(i) + "-" + std::to_string(k) + "：拉开篇幅的说明文字。\n";
    md += "\n";
  }
  write_text(ws_root() / "report.md", md);

  std::string novel;
  for (int i = 1; i <= 20; ++i) {
    novel += "第" + std::to_string(i) + "章 江湖夜雨\n";
    if (i == 5) novel += "李英琼提剑走入雨里，众人皆惊。\n";
    for (int k = 0; k < 14; ++k)
      novel += "山中夜雨，剑气纵横，众人沿石阶向前（第" + std::to_string(i) + "章第" +
               std::to_string(k) + "段）。\n";
    novel += "\n";
  }
  novel += "全书完。\n";
  write_text(ws_root() / "novel.txt", novel);
}

struct Question {
  std::string prompt;
  std::string expected;  // 答案必须包含的事实
  std::string kind;
};

Json route_ir() {
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "recall"}, {"kind", "load_history"},
                       {"path", "@config.workspace"}, {"session", "@config.session"},
                       {"next", "files"}});
  nodes.push_back(Json{{"id", "files"}, {"kind", "file_context"}, {"next", "think"}});
  nodes.push_back(Json{{"id", "think"}, {"kind", "model"}, {"model", "main"},
                       {"system_prompt",
                        "你是严谨的助手。若【文件取数路由】给到素材，严格依据素材回答；"
                        "素材没给到的内容不要编造，直接说没找到。"},
                       {"next", "_turn_end"}});
  return Json{{"id", "wf_route_live"}, {"nodes", std::move(nodes)}};
}

Json base_ir() {
  Json body = Json::array();
  body.push_back(Json{{"id", "think"}, {"kind", "model"}, {"model", "main"},
                      {"tools", "chat"},
                      {"system_prompt",
                       "你是严谨的助手。需要文件内容时调用工具"
                       "（file_grep / file_extract / file_read / file_sample）后再回答；"
                       "不要编造工具没给出的内容。"},
                      {"next", "route"}});
  body.push_back(Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                      {"cases", Json{{"tool_calls", "act"}, {"text", "_break"}}},
                      {"default", "_break"}});
  body.push_back(Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}});

  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "recall"}, {"kind", "load_history"},
                       {"path", "@config.workspace"}, {"session", "@config.session"},
                       {"next", "loop"}});
  nodes.push_back(Json{{"id", "loop"}, {"kind", "loop"}, {"max", 4},
                       {"next", "_turn_end"}, {"body", std::move(body)}});
  return Json{{"id", "wf_base_live"}, {"nodes", std::move(nodes)}};
}

struct ArmResult {
  bool ok = false;
  bool correct = false;
  std::string answer;
  long tool_rounds = 0;   // 带 tool_calls 的 AI 消息数
  long total_calls = 0;   // 工具调用总数
  long in_tokens = 0;
  long out_tokens = 0;
  std::string route = "-";
};

std::string last_ai_text(Json const& st) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return {};
  for (auto m = it->rbegin(); m != it->rend(); ++m) {
    if (m->value("type", "") != "ai") continue;
    if (m->contains("tool_calls") && !(*m)["tool_calls"].empty()) continue;
    std::string text;
    if (m->contains("content") && (*m)["content"].is_array())
      for (Json const& b : (*m)["content"])
        if (b.is_object() && b.contains("text")) text += b["text"].get<std::string>();
    return text;
  }
  return {};
}

ArmResult run(CompiledWorkflow& wf, std::string const& question, fs::path const& ws,
              std::string const& session, Json const& cfg) {
  // 本轮输入写会话日志（load_history 的输入源）
  {
    SessionLog log(session_path(ws.string(), session));
    log.engine_enter("r-1", "__input__", "input");
    log.engine_exit("r-1", "__input__", "input", true, "ok", 0,
                    Json{{"messages", Json::array({Json(message::human_message(question))})}});
  }
  Json state_in = cfg;
  state_in["workspace"] = ws.string();
  state_in["session"] = session;
  FireOptions fo;
  fo.thread_id = session;
  ArmResult r;
  auto res = fire_once(wf, state_in, fo);
  if (!res) {
    r.answer = "ERROR: " + res.error().message;
    return r;
  }
  Json const& st = *res;
  r.ok = true;
  r.answer = last_ai_text(st);
  if (auto const it = st.find("results"); it != st.end() && it->is_object())
    if (it->contains("files") && (*it)["files"].is_object())
      r.route = (*it)["files"].value("route", "-");
  if (auto const it = st.find("messages"); it != st.end() && it->is_array()) {
    for (Json const& m : *it) {
      if (m.value("type", "") != "ai") continue;
      if (m.contains("tool_calls") && !m["tool_calls"].empty()) {
        ++r.tool_rounds;
        r.total_calls += static_cast<long>(m["tool_calls"].size());
      }
      if (m.contains("usage") && m["usage"].is_object()) {
        r.in_tokens += m["usage"].value("input_tokens", 0LL);
        r.out_tokens += m["usage"].value("output_tokens", 0LL);
      }
    }
  }
  return r;
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）；真机评测需要真实端点。");
    return 2;
  }
  build_corpus();
  std::println("== problems/02 S6 真机评测 ==");
  std::println("工作区：{}  ·  模型：{}\n", ws_root().generic_string(),
               demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat"));

  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::ModelConfig mc;
  mc.api_key = key;
  mc.base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  mc.model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  mc.ca_info = ca_bundle_from_env();
  model::openai::OpenAIChatModel llm(mc, transport);

  tool::ToolRegistry registry;
  auto file_tools = tool::make_file_tools(tool::FileToolsConfig{ws_root()});
  std::vector<std::string> tool_names;
  for (auto& t : file_tools) {
    tool_names.push_back(t.name);
    registry.add(std::move(t));
  }

  ComponentRegistry components;
  components.models["main"] = &llm;
  components.toolsets["chat"] = tool_names;

  state::MemoryCheckpointer cp_route, cp_base;
  CompileOptions co;
  co.components = &components;

  auto compile = [&](Json ir, tool::ToolRegistry const& reg, state::MemoryCheckpointer& cp)
      -> std::optional<CompiledWorkflow> {
    auto parsed = WorkflowIR::parse(std::move(ir));
    if (!parsed) {
      std::println("IR 解析失败: {}", parsed.error());
      return std::nullopt;
    }
    ValidatorOptions vo;
    vo.registry = &reg;
    vo.components = &components;
    auto const v = validate(*parsed, vo);
    if (!v.ok()) {
      std::println("校验失败: {}", v.describe());
      return std::nullopt;
    }
    auto wf = compile_workflow(*parsed, reg, &cp, co);
    if (!wf) {
      std::println("编译失败: {}", wf.error().message);
      return std::nullopt;
    }
    return *wf;
  };

  auto route_wf = compile(route_ir(), registry, cp_route);
  auto base_wf = compile(base_ir(), registry, cp_base);
  if (!route_wf || !base_wf) return 1;

  std::vector<Question> const questions = {
      {"config.json 的 /services/3/ports/1 是多少？只回答数字。", "8443", "JSON 指针"},
      {"app.yaml 里 server.ports.1 是什么？只回答数字。", "443", "YAML 键路径"},
      {"app.log 里 code=0x9F3A 对应的 handler 是什么？只回答 handler 名。", "payment",
       "字面定位"},
      {"report.md 里 Section 7 的召回率是多少？只回答数字。", "0.83", "章节"},
      {"novel.txt 里有没有提到「李英琼」？回答并给出证据。", "李英琼", "引号字面"},
      {"app.log 的最后一行是什么？原样输出。", "LAST-LINE-尾标记", "全局/结尾"},
  };

  Json const cfg_on{{"file_route", Json{{"enabled", true}}}};
  Json const cfg_off{{"file_route", Json{{"enabled", false}}}};

  int route_ok = 0, base_ok = 0;
  long route_tokens = 0, base_tokens = 0;
  long route_calls = 0, base_calls = 0;
  std::println("{:<18} {:<6} {:<8} {:<10} {:<8} {}", "问题", "臂", "正确", "route", "工具轮", "tok(in/out)");
  for (std::size_t i = 0; i < questions.size(); ++i) {
    Question const& q = questions[i];
    ArmResult const a = run(*route_wf, q.prompt, ws_root(), "route_" + std::to_string(i), cfg_on);
    ArmResult const b = run(*base_wf, q.prompt, ws_root(), "base_" + std::to_string(i), cfg_off);

    auto correct = [&](ArmResult const& r) {
      return r.ok && r.answer.find(q.expected) != std::string::npos;
    };
    bool const ac = correct(a), bc = correct(b);
    route_ok += ac ? 1 : 0;
    base_ok += bc ? 1 : 0;
    route_tokens += a.in_tokens + a.out_tokens;
    base_tokens += b.in_tokens + b.out_tokens;
    route_calls += a.total_calls;
    base_calls += b.total_calls;

    std::println("{:<18} {:<6} {:<8} {:<10} {:<8} {}/{}", q.kind, "route", ac ? "✓" : "✗",
                 a.route, a.tool_rounds, a.in_tokens, a.out_tokens);
    std::println("{:<18} {:<6} {:<8} {:<10} {:<8} {}/{}", "", "base", bc ? "✓" : "✗", "-",
                 b.tool_rounds, b.in_tokens, b.out_tokens);
    if (!ac) std::println("    route 答案：{}", a.answer.substr(0, 120));
    if (!bc) std::println("    base  答案：{}", b.answer.substr(0, 120));
  }

  std::size_t const n = questions.size();
  std::println("\n== 汇总 ==");
  std::println("route 臂：正确 {}/{} · 模型工具调用 {} 次 · 总 token {}",
               route_ok, n, route_calls, route_tokens);
  std::println("base  臂：正确 {}/{} · 模型工具调用 {} 次 · 总 token {}",
               base_ok, n, base_calls, base_tokens);
  std::println("\n注：真机结果受模型随机性影响；本台只断言「答案含预埋事实」，"
               "不评风格。漏读率 = route 臂错误且 route=none/未命中；利用率 = route 命中但答案错。");
  std::println("验收结论：{}", route_ok == static_cast<int>(n)
                                   ? "route 臂全对（harness 预取够用，模型无需自选工具）"
                                   : "route 臂有未命中，见上方 ✗（先看 route 列再判断是漏读还是利用率）");
  return route_ok == static_cast<int>(n) ? 0 : 1;
}
