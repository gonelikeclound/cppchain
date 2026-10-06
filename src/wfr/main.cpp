// cppchain —— src/wfr/main.cpp
// wfr —— workflow runner（正式 CLI）：按名执行 workflow（绑定工作目录 + .wfr meta 目录）。
//
// 用法:
//   wfr <name-or-path> [--workdir DIR] [--session NAME] [--set k=v]... [--reset]
//       [--allow-import] <query>...
//   wfr list [--workdir DIR]
//   wfr debug <name-or-path> [--session N|--all] [--out F] [--open]
//   wfr -h | --help
//
// <name>：内置名（chat：最简单聊天，model-only + load_history）优先；否则按
// `.wfr/workflows/<name>.json` 解析；含 / 或 *.json 且存在则当文件。
// query 作为本轮输入（input_messages，引擎写 __input__ 事件）；--set 注入 @config.* 键。
// trigger 可省略：无 trigger 的 workflow 由 wfr 手工拉起（D1）。
// 会话统一落 <workdir>/.wfr/sessions/。退出码：0 成功 / 2 用法・找不到・解析校验编译失败 /
// 1 fire 运行时错误。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>  // Message ↔ JSON
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/registry.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/debug_view.hpp>
#include <cppchain/workflow/import.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/wfr.hpp>
#include <cppchain/workflow/plugin_node.hpp>  // load_workflow_imports（动态库，v1 仅顶层）

#include "builtin_workflows.hpp"
#include "util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::tool::make_calculator_tool;
using cppchain::tool::make_datetime_tool;
using cppchain::wfr::ca_bundle_from_env;
using cppchain::wfr::env_or;
using cppchain::wfr::load_dotenv;

namespace {

std::string clip(std::string_view s, std::size_t max) {
  return s.size() <= max ? std::string(s) : std::string(s.substr(0, max)) + "…";
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

// 内置名（chat…）优先；否则路径/按名。
// 返回 {kind: builtin|file, ir/路径} —— 现仅内置直接命中返回；文件走 resolve_workflow_path。
std::string resolve_workflow_path(std::string const& workdir, std::string const& spec) {
  std::filesystem::path maybe(spec);
  bool const looks_path = spec.find('/') != std::string::npos || spec.ends_with(".json");
  if (looks_path && !maybe.is_absolute() &&
      std::filesystem::exists(std::filesystem::path(workdir) / maybe))
    return (std::filesystem::path(workdir) / maybe).string();
  if (maybe.is_absolute() && std::filesystem::exists(maybe)) return maybe.string();
  return wfr_workflow_path(workdir, spec);
}

bool has_model_node(Json const& ir_json) {
  for (Json const& n : ir_json.value("nodes", Json::array()))
    if (n.value("kind", "") == "model" || n.contains("model")) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::string workdir = ".", session = "default", name_or_path, input;
  bool reset = false, allow_import = false;
  std::map<std::string, std::string> sets;

  auto usage = [] {
    std::println(
        "用法:\n"
        "  wfr <name-or-path> [--workdir DIR] [--session NAME] [--set key=value]... "
        "[--reset] [--allow-import] <query>...\n"
        "  wfr list [--workdir DIR]\n"
        "  wfr debug <name-or-path> [--session N|--all] [--out F] [--open]\n"
        "  wfr -h | --help\n"
        "内置 workflow：chat（最简单聊天，多轮续聊；需 LLM key，见 .env.example）。\n"
        "<name> 内置名优先；否则按 .wfr/workflows/<name>.json 解析；含 / 或 *.json 且存在"
        "则当文件。\n"
        "--set 注入 @config.* 的额外键；保留名 debug/list/help 作为首参时为子命令。");
  };
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string const cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") {
    usage();
    return 0;
  }

  // ================= 子命令：debug（可视化调试台） =================
  if (cmd == "debug") {
    std::string session_arg, out_file;
    bool all = false, open = false;
    if (argc < 3) {
      usage();
      return 2;
    }
    std::string const dname = argv[2];
    for (int j = 3; j < argc; ++j) {
      std::string const a = argv[j];
      if (a == "--workdir" && j + 1 < argc) workdir = argv[++j];
      else if (a == "--session" && j + 1 < argc) session_arg = argv[++j];
      else if (a == "--all") all = true;
      else if (a == "--out" && j + 1 < argc) out_file = argv[++j];
      else if (a == "--open") open = true;
      else if (a == "-h" || a == "--help") {
        usage();
        return 0;
      } else {
        std::println("未知 debug 选项: {}", a);
        return 2;
      }
    }
    if (session_arg.empty() && !all) all = true;

    Json dir_json;
    if (auto b = cppchain::wfr::resolve_builtin(dname)) {
      dir_json = std::move(*b);
    } else {
      std::string const dpath = resolve_workflow_path(workdir, dname);
      std::ifstream din(dpath);
      if (!din) {
        std::println("找不到 workflow '{}'（searched: {}）", dname, dpath);
        return 2;
      }
      try {
        din >> dir_json;
      } catch (std::exception const& e) {
        std::println("workflow JSON 解析失败: {}", e.what());
        return 2;
      }
      if (dir_json.contains("imports"))
        if (!load_workflow_imports(dir_json, true, nullptr)) {
          std::println("动态库 import 失败");
          return 2;
        }
      if (dir_json.contains("import")) {
        auto expanded = expand_workflow_imports(dir_json, workdir);
        if (!expanded) {
          std::println("workflow import 展开失败: {}", expanded.error());
          return 2;
        }
      }
    }
    auto dir_ir = WorkflowIR::parse(dir_json);
    if (!dir_ir) {
      std::println("IR 解析失败: {}", dir_ir.error());
      return 2;
    }

    std::vector<Json> recs;
    if (all) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(wfr_root(workdir) / "sessions", ec), end;
           it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) ||
            !it->path().filename().string().ends_with(".log.jsonl"))
          continue;
        SessionLog slog(it->path().string());
        auto rr = slog.records();
        recs.insert(recs.end(), rr.begin(), rr.end());
      }
    } else {
      SessionLog slog(session_path(workdir, session_arg));
      recs = slog.records();
    }

    Json const payload = build_debug_payload(*dir_ir, recs);
    std::string const html = render_debug_html(payload);
    std::string const out =
        out_file.empty() ? (wfr_root(workdir) / "debug" / (dname + ".html")).string() : out_file;
    {
      std::filesystem::create_directories(std::filesystem::path(out).parent_path());
      std::ofstream o(out, std::ios::trunc);
      o << html;
    }
    std::println("调试台已生成: {}  ({} bytes, {} 条日志记录)", out, html.size(), recs.size());
    if (open) [[unlikely]] {
      std::string const cmdline = "xdg-open \"" + out + "\" >/dev/null 2>&1 &";
      std::system(cmdline.c_str());
    }
    return 0;
  }

  // ---- 解析选项（首参=workflow 名/路径；其余 --opt 与 query）----
  int i = 2;
  if (cmd != "list") name_or_path = cmd;
  for (; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == "--workdir" && i + 1 < argc) workdir = argv[++i];
    else if (a == "--session" && i + 1 < argc) session = argv[++i];
    else if (a == "--set" && i + 1 < argc) {
      std::string const kv = argv[++i];
      auto const eq = kv.find('=');
      sets[kv.substr(0, eq)] = eq == std::string::npos ? std::string{} : kv.substr(eq + 1);
    } else if (a == "--reset") reset = true;
    else if (a == "--allow-import") allow_import = true;
    else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else
      input = input.empty() ? a : input + " " + a;
  }

  // ---- list：内置 + 文件 ----
  if (cmd == "list") {
    std::println("内置 workflow:");
    for (std::string const& n : cppchain::wfr::builtin_names())
      std::println("  {:<12} (内置)", n);
    auto const names = wfr_list_workflows(workdir);
    if (!names.empty()) {
      std::println("\n<workdir>/.wfr/workflows/:");
      for (std::string const& n : names) {
        std::string const p = wfr_workflow_path(workdir, n);
        std::ifstream in(p);
        Json j;
        try {
          in >> j;
        } catch (...) {
          std::println("  {:<12} （读不到/坏 JSON）", n);
          continue;
        }
        std::string trig;
        if (j.contains("trigger") && j["trigger"].is_object()) {
          Json const& timing = j["trigger"].value("timing", Json::object());
          trig = "schedule " + timing.value("schedule", "");
        } else {
          trig = "manual（手工触发）";
        }
        std::println("  {:<12} nodes={}  trigger={}", n,
                     j.value("nodes", Json::array()).size(), trig);
      }
    }
    return 0;
  }

  // ---- 定位 workflow：内置优先；否则 文件/.wfr ----
  Json ir_json;
  bool model_needed_key = false;
  if (auto b = cppchain::wfr::resolve_builtin(name_or_path)) {
    ir_json = std::move(*b);
    model_needed_key = true;  // 聊天即 model
  } else {
    std::string const path = resolve_workflow_path(workdir, name_or_path);
    std::ifstream in(path);
    if (!in) {
      std::println("找不到 workflow '{}'（searched: {}）", name_or_path, path);
      return 2;
    }
    try {
      in >> ir_json;
    } catch (std::exception const& e) {
      std::println("workflow JSON 解析失败: {}", e.what());
      return 2;
    }
    model_needed_key = has_model_node(ir_json);
    if (ir_json.contains("imports")) {
      auto loaded = load_workflow_imports(ir_json, allow_import, nullptr);
      if (!loaded) {
        std::println("动态库 import 失败: {}", loaded.error());
        return 2;
      }
    }
    if (ir_json.contains("import")) {
      auto expanded = expand_workflow_imports(ir_json, workdir);
      if (!expanded) {
        std::println("workflow import 展开失败: {}", expanded.error());
        return 2;
      }
    }
  }

  auto ir = WorkflowIR::parse(ir_json);
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 2;
  }

  // ---- 组件：内置宿主工具（datetime / calculator）+ 真实模型（仅含 model 节点时需要 key）----
  tool::ToolRegistry registry;
  registry.add(make_datetime_tool());
  registry.add(make_calculator_tool());
  auto dotenv = load_dotenv({".env", "../.env", "../../.env"});
  ComponentRegistry comps;
  std::vector<std::string> all_tools;
  for (tool::ToolSpec const& t : registry.all()) all_tools.push_back(t.name);
  comps.toolsets["all"] = all_tools;
  comps.toolsets["ts"] = all_tools;

  std::optional<model::openai::OpenAIChatModel> model;
  std::shared_ptr<http::Transport> transport;
  if (model_needed_key) {
    std::string const key = env_or(dotenv, "LLM_API_KEY");
    if (key.empty()) {
      std::println("workflow 含 model 节点，但缺少 LLM_API_KEY（见 .env.example）");
      return 2;
    }
    model::ModelConfig cfg;
    cfg.api_key = key;
    cfg.base_url = env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
    cfg.model = env_or(dotenv, "LLM_MODEL", "deepseek-chat");
    cfg.ca_info = ca_bundle_from_env();
    transport = std::make_shared<http::CurlMultiTransport>();
    model.emplace(cfg, transport);
    comps.models["main"] = &*model;
  }

  CompileOptions opts;
  opts.components = &comps;
  // 不做 config_keys 白名单（生成器侧防护；wfr 跑手写/内置 workflow 由 --set 提供键）
  auto wf = compile_workflow(*ir, registry, nullptr, opts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 2;
  }
  std::string const trig_desc =
      wf->trigger ? "trigger: schedule " + wf->trigger->schedule : "trigger: manual（wfr 拉起）";
  std::println("workflow: {}（{}）  ·  {} 个节点", wf->ir_id, trig_desc, ir->nodes.size());
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());

  // ---- 会话日志（.wfr/sessions/；session_path 已迁入 .wfr）----
  std::string const slog_path = session_path(workdir, session);
  if (reset) {
    std::error_code ec;
    std::filesystem::remove(slog_path, ec);
    std::println("已清空会话: {}", slog_path);
  }
  SessionLog slog(slog_path);
  if (input.empty()) {
    std::println("（未给输入。加一句输入即续跑/聊一句；--reset 清空会话）");
    return 0;
  }

  FireOptions fo;
  fo.log = &slog;
  Json inp = Json::array();
  inp.push_back(message::human_message(input));
  fo.input_messages = std::move(inp);

  Json config{{"workspace", workdir}, {"session", session}};
  for (auto const& [k, v] : sets) config[k] = v;
  auto r = fire_once(*wf, std::move(config), std::move(fo));
  if (!r) {
    std::println("触发失败: {}", r.error().message);
    return 1;
  }
  std::string const answer = last_ai_text(*r);
  std::println("\n{}", answer.empty() ? "(无文本回复)" : clip(answer, 800));
  std::println("会话已更新: {} 条消息", replay_session_messages(slog_path).size());
  return 0;
}