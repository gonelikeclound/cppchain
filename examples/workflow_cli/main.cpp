// demo: workflow_cli —— 命令行 agent：**跑 workflow**，跨命令连续对话（M12 / D 阶段，真机）
//
//   ./workflow_cli --workflow wf.json "你的输入"
//
// 与 examples/session_resume 的区别：那个直接用 agent::Agent；这个走**完整 workflow** ——
// history 是图上的普通节点（load_history → …），agent 循环是图上的回边。
//
// 输入进 state 的路径（docs/02-arch-design/12 §7，④ 起）：**本轮输入经 fire_once 写进会话日志**
// （引擎在 run 开头写 input 事件，source=engine），load_history 对该日志做 replay 投影 ——
// 顺序天然正确（历史在前、本轮在后），且不再单独 save_history（日志就是唯一事实来源）。
//
//   --workdir DIR   工作路径（决定会话日志落点；缺省 cwd）
//   --session NAME  同路径下多段会话（缺省 "cli"）
//   --workflow FILE 手写 IR（必填；生成端产出这些节点是 F 阶段的事）
//   --show          不起模型，只打印已存历史
//   --reset         清空该会话的日志
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（企业代理见 LLM_CA_BUNDLE）
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>  // Message ↔ JSON（input_messages 需要）
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/session_log.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

std::string ca_bundle_from_env() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const c = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(c)) ca = c;
    }
  }
  return ca;
}

std::string clip(std::string const& s, std::size_t n) {
  if (s.size() <= n) return s;
  std::size_t end = n;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

std::string last_ai_text(Json const& state) {
  if (!state.contains("messages") || !state["messages"].is_array()) return {};
  auto const& arr = state["messages"];
  for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
    if (it->value("type", "") != "ai") continue;
    std::string out;
    for (auto const& blk : (*it)["content"])
      if (blk.value("type", "") == "text") out += blk.value("text", "");
    return out;
  }
  return {};
}

void print_session(std::string const& path) {
  auto const msgs = replay_session_messages(path);  // 历史即日志的 replay 投影（④）
  std::println("会话日志: {}（{} 条消息）", path, msgs.size());
  for (auto const& m : msgs)
    std::println("  [{}] {}", std::string(message::role_str(m.type)), clip(m.text(), 160));
}

}  // namespace

int main(int argc, char** argv) {
  std::string workdir = ".", session = "cli", workflow_file, input;
  bool reset = false, show = false;

  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == "--workdir" && i + 1 < argc) workdir = argv[++i];
    else if (a == "--session" && i + 1 < argc) session = argv[++i];
    else if (a == "--workflow" && i + 1 < argc) workflow_file = argv[++i];
    else if (a == "--reset") reset = true;
    else if (a == "--show") show = true;
    else if (a == "--help" || a == "-h") {
      std::println("用法: workflow_cli --workflow FILE [--workdir DIR] [--session NAME] "
                   "[--reset] [--show] \"你的输入\"");
      return 0;
    } else input = input.empty() ? a : input + " " + a;
  }

  std::string const path = session_path(workdir, session);

  if (reset) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::println("已清空会话: {}", path);
  }
  if (show) {
    print_session(path);
    return 0;
  }

  if (input.empty()) {
    // 没输入就不该跑：空对话调模型只会得到幻觉（实测模型会自己编"假设我已完成工具调用"）
    print_session(path);
    std::println("\n（未给输入。加一句输入即续跑；--reset 清空）");
    return 0;
  }
  if (workflow_file.empty()) {
    std::println("缺少 --workflow FILE（手写 IR）。用 --show 可只看历史。");
    return 2;
  }
  {
    std::ifstream in(workflow_file);
    if (!in) {
      std::println("读不到 workflow 文件: {}", workflow_file);
      return 2;
    }
  }

  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  if (key.empty() && !input.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）");
    return 1;
  }

  // ---- 组件注册（宿主侧：IR 只说名字，这里绑对象）----
  demo::fakes::FakeToolSinks sinks;
  tool::ToolRegistry registry = demo::fakes::make_fake_tools(sinks);

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  cfg.model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  cfg.ca_info = ca_bundle_from_env();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  ComponentRegistry comps;
  comps.models["main"] = &model;
  std::vector<std::string> all_tools;
  for (tool::ToolSpec const& t : registry.all()) all_tools.push_back(t.name);
  comps.toolsets["all"] = all_tools;
  comps.toolsets["ts"] = all_tools;

  // ---- 编译 + 触发 ----
  std::ifstream wf_in(workflow_file);
  Json ir_json;
  try {
    wf_in >> ir_json;
  } catch (std::exception const& e) {
    std::println("workflow JSON 解析失败: {}", e.what());
    return 2;
  }
  auto ir = WorkflowIR::parse(ir_json);
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 2;
  }

  CompileOptions opts;
  opts.components = &comps;
  opts.config_keys = std::vector<std::string>{"workspace", "session"};  // B2 契约：@config 键白名单
  auto wf = compile_workflow(*ir, registry, nullptr, opts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 2;
  }
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());

  // ---- 会话日志：宿主按工作路径开启（.cppchain/session-<name>.log.jsonl）----
  // ④（docs/12 §7）：日志是历史的事实来源；本轮输入经 FireOptions 由引擎写成 input 事件，
  // fire_once 之后不需要保存任何东西 —— 没有 save_history 了。
  SessionLog slog(path);
  std::println("会话日志: {}", path);
  std::println("已有历史: {} 条消息", replay_session_messages(path).size());

  FireOptions fo;
  fo.log = &slog;
  if (!input.empty()) {
    Json inp = Json::array();
    inp.push_back(message::human_message(input));
    fo.input_messages = std::move(inp);
  }
  auto r = fire_once(*wf, Json{{"workspace", workdir}, {"session", session}}, std::move(fo));
  if (!r) {
    std::println("触发失败: {}", r.error().message);
    return 1;
  }

  std::string const answer = last_ai_text(*r);
  std::println("\n回答: {}", answer.empty() ? "(无文本回复)" : clip(answer, 500));
  std::println("会话已更新: {} 条消息", replay_session_messages(path).size());
  return 0;
}
