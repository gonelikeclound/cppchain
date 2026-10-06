// wfr —— workflow runner：按名执行 workflow（绑定工作目录 + .wfr meta 目录）。
//
// 用法:
//   wfr <name-or-path> [--workdir DIR] [--session NAME] [--reset] [--allow-import] <query>...
//   wfr list [--workdir DIR]
//   wfr debug <...>    （wfr-⑤ 可视化，后续接上）
//   wfr -h | --help
//
// 定位：<name> → <workdir>/.wfr/workflows/<name>.json（见 workflow/wfr.hpp）；含 / 或
// *.json 且存在 → 直接当文件。query 作为本轮输入（input_messages，引擎写 __input__ 事件）。
// trigger 可省略：无 trigger 的 workflow 由 wfr 手工拉起（D1）。
// 会话统一落 <workdir>/.wfr/sessions/（session_path 迁移后的契约）。退出码：0 成功 / 2
// 用法・找不到・解析校验编译失败 / 1 fire 运行时错误。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>  // Message ↔ JSON
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/debug_view.hpp>
#include <cppchain/workflow/import.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/wfr.hpp>
#include <cppchain/workflow/plugin_node.hpp>  // load_workflow_imports（动态库，v1 仅顶层）
#include <support/dotenv.hpp>
#include <support/fakes.hpp>

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
using namespace demo::fakes;

namespace {

std::string ca_bundle_from_env() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      auto const Geo = std::filesystem::path(home) / ".geo" / "corp-ca.pem";
      if (std::filesystem::exists(Geo)) ca = Geo.string();
    }
  }
  return ca;
}

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

// <name-or-path> → 文件路径：含 / 或 *.json 且存在 → 当文件；否则 <ws>/.wfr/workflows/<name>.json
std::string resolve_workflow(std::string const& workdir, std::string const& spec) {
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
  std::map<std::string, std::string> sets;  // --set key=value（注入 @config.* 的额外键）

  auto usage = [] {
    std::println(
        "用法:\n"
        "  wfr <name-or-path> [--workdir DIR] [--session NAME] [--set key=value]... "
        "[--reset] [--allow-import] <query>...\n"
        "  wfr list [--workdir DIR]\n"
        "  wfr debug <name-or-path> ...   （可视化调试台，后续阶段接入）\n"
        "  wfr -h | --help\n"
        "<name> 按 .wfr/workflows/<name>.json 解析；含 / 或 *.json 且存在则当文件。\n"
        "--set 注入 @config.* 的额外键；保留名 debug/list/help 作为首参时为子命令"
        "（要跑同名 workflow 用路径形式转义）。");
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

  // ================= 子命令：debug（可视化调试台，wfr-⑤） =================
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
    if (session_arg.empty() && !all) all = true;  // 缺省看全部会话

    // 定位 + 展开 + 解析（与主流程同源；debug 展示展开后的扁平图）
    std::string const dpath = resolve_workflow(workdir, dname);
    std::ifstream din(dpath);
    if (!din) {
      std::println("找不到 workflow '{}'（searched: {}）", dname, dpath);
      return 2;
    }
    Json dir_json;
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
    auto dir_ir = WorkflowIR::parse(dir_json);
    if (!dir_ir) {
      std::println("IR 解析失败: {}", dir_ir.error());
      return 2;
    }

    // 会话日志（--session 单会话 / --all 合并 .wfr/sessions/*.log.jsonl）
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
        auto r = slog.records();
        recs.insert(recs.end(), r.begin(), r.end());
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

  // ---- list 子命令 ----
  if (cmd == "list") {
    auto const names = wfr_list_workflows(workdir);
    if (names.empty()) {
      std::println("（{} 下没有 workflows/*.json）", wfr_root(workdir).string());
      return 0;
    }
    std::println("{} 个 workflow:", names.size());
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
      std::size_t const nodes = j.value("nodes", Json::array()).size();
      std::println("  {:<12} nodes={}  trigger={}", n, nodes, trig);
    }
    return 0;
  }

  // ---- 主流程：定位 workflow 文件 → 读 → 导入/展开 → parse → 编译 → fire ----
  std::string const path = resolve_workflow(workdir, name_or_path);
  std::ifstream in(path);
  if (!in) {
    std::println("找不到 workflow '{}'（searched: {}）", name_or_path, path);
    return 2;
  }
  Json ir_json;
  try {
    in >> ir_json;
  } catch (std::exception const& e) {
    std::println("workflow JSON 解析失败: {}", e.what());
    return 2;
  }

  // 动态库导入（任意代码，默认拒绝）—— 必须先于 parse（按名字判别动态节点）
  if (ir_json.contains("imports")) {
    auto loaded = load_workflow_imports(ir_json, allow_import, /*host_log=*/nullptr);
    if (!loaded) {
      std::println("动态库 import 失败: {}", loaded.error());
      return 2;
    }
  }
  // workflow import（复用其他 workflow 的节点/子链）：展开后是单 IR，引擎零改动
  if (ir_json.contains("import")) {
    auto expanded = expand_workflow_imports(ir_json, workdir);
    if (!expanded) {
      std::println("workflow import 展开失败: {}", expanded.error());
      return 2;
    }
  }

  auto ir = WorkflowIR::parse(ir_json);
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 2;
  }

  // ---- 组件：fakes 目录工具（离线确定性）；真实模型仅当 IR 里有 model 节点才需要 key ----
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  ComponentRegistry comps;
  std::vector<std::string> all_tools;
  for (tool::ToolSpec const& t : registry.all()) all_tools.push_back(t.name);
  comps.toolsets["all"] = all_tools;
  comps.toolsets["ts"] = all_tools;

  std::optional<model::openai::OpenAIChatModel> model;
  std::shared_ptr<http::Transport> transport;  // 由 model 持有 shared（模型构造后用）
  if (has_model_node(ir_json)) {
    std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
    if (key.empty()) {
      std::println("workflow 含 model 节点，但缺少 LLM_API_KEY（见 .env.example）");
      return 2;
    }
    model::ModelConfig cfg;
    cfg.api_key = key;
    cfg.base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
    cfg.model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
    cfg.ca_info = ca_bundle_from_env();
    transport = std::make_shared<http::CurlMultiTransport>();
    model.emplace(cfg, transport);
    comps.models["main"] = &*model;
  }

  CompileOptions opts;
  opts.components = &comps;
  // 不做 config_keys 白名单：白名单是生成器侧的防护（防模型发明键）；
  // wfr 跑的是**手写** workflow，作者引用什么键就提供什么（--set 注入，缺省取 workspace/session）。
  auto wf = compile_workflow(*ir, registry, nullptr, opts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 2;
  }
  std::string const trig_desc =
      wf->trigger ? "trigger: schedule " + wf->trigger->schedule : "trigger: manual（wfr 拉起）";
  std::println("workflow: {}（{}）  ·  {} 个节点", wf->ir_id, trig_desc, ir->nodes.size());
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());

  // ---- 会话日志（.wfr/sessions/session-<name>.log.jsonl；session_path 已迁入 .wfr）----
  std::string const slog_path = session_path(workdir, session);
  if (reset) {
    std::error_code ec;
    std::filesystem::remove(slog_path, ec);
    std::println("已清空会话: {}", slog_path);
  }
  SessionLog slog(slog_path);
  if (input.empty()) {
    std::println("（未给输入。加一句输入即续跑；--reset 清空会话）");
    return 0;
  }

  FireOptions fo;
  fo.log = &slog;
  Json inp = Json::array();
  inp.push_back(message::human_message(input));
  fo.input_messages = std::move(inp);

  Json config{{"workspace", workdir}, {"session", session}};
  for (auto const& [k, v] : sets) config[k] = v;  // --set 注入 @config.* 的额外键
  auto r = fire_once(*wf, std::move(config), std::move(fo));
  if (!r) {
    std::println("触发失败: {}", r.error().message);
    return 1;
  }
  std::string const answer = last_ai_text(*r);
  std::println("\n回答: {}", answer.empty() ? "(无文本回复)" : clip(answer, 600));
  std::println("会话已更新: {} 条消息", replay_session_messages(slog_path).size());
  return 0;
}