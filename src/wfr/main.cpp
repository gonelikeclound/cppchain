// cppchain —— src/wfr/main.cpp
// wfr —— workflow runner（正式 CLI）：按名执行 workflow（绑定工作目录 + .wfr meta 目录）。
//
// 用法:
//   wfr <name-or-path> [--workdir DIR] [--session NAME] [--set k=v]... [--reset] [--wait]
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
#include <cppchain/message/utf8.hpp>        // 答案打印前净化断字节
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>  // wfr-㊸ @②：人审悬挂/恢复的进程内快照
#include <cppchain/tool/bash_tool.hpp>      // wfr-㊸ @①/@②：bash 工具（NeedHuman 人审）
#include <cppchain/tool/command_classify.hpp>  // wfr-㊸ ⑤：allow 前缀记录命令类别（封顶自动放行）
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/exec_guard.hpp>
#include <cppchain/tool/file_tools.hpp>
#include <cppchain/tool/registry.hpp>
#include <cppchain/tool/sandbox.hpp>
#include <cppchain/tool/spawn_runner.hpp>  // wfr-㊿ cron：重进本进程跑批
#include <cppchain/tool/trip_db.hpp>  // B2-B 出行规划 DB 面  // wfr-㊸ @③：沙箱执行器选择（bash 后端）
#include <cppchain/tool/weather.hpp>
#include <cppchain/tool/web_search.hpp>
#include <cppchain/tool/skill_tools.hpp>  // wfr-context（⑤）：read_skill / wiki_search
#include <cppchain/tool/sketch_verify.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/debug_view.hpp>
#include <cppchain/workflow/import.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/ir_walk.hpp>
#include <cppchain/workflow/session_lock.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/wfr.hpp>
#include <cppchain/workflow/wfr_lookup.hpp>
#include <cppchain/workflow/plugin_node.hpp>  // load_workflow_imports（动态库，v1 仅顶层）

#include "util.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>  // wfr-㊸ @②：人审 stdin
#include <map>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <unistd.h>  // getpid（会话锁持有者标识）
#include <vector>

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::tool::make_calculator_tool;
using cppchain::tool::make_datetime_tool;
using cppchain::tool::make_sketch_verify_tool;
using cppchain::tool::make_file_read_tool;
using cppchain::tool::make_file_grep_tool;
using cppchain::tool::make_file_extract_tool;
using cppchain::tool::make_file_sample_tool;
using cppchain::tool::make_list_dir_tool;
using cppchain::tool::make_weather_tool;
using cppchain::tool::make_web_search_tool;
using cppchain::tool::make_read_skill_tool;
using cppchain::tool::make_wiki_search_tool;
using cppchain::wfr::ca_bundle_from_env;
using cppchain::wfr::env_or;
using cppchain::wfr::load_dotenv;

namespace {


// --stream 的辅助：StreamEvent → JSON（node_start/node_end/interrupt/values + payload）
nlohmann::json ev_json(cppchain::graph::StreamEvent const& ev) {
  return nlohmann::json{{"type", ev.type}, {"node", ev.node}, {"node_kind", ev.node_kind},
                        {"step", ev.step}, {"payload", ev.payload}, {"ok", ev.ok}};
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
// 发布自带的固定 workflow 目录（排在查找末尾）：
//   1) WFR_WORKFLOWS_DIR 环境
//   2) 安装布局：<bin>/../share/wfr/workflows（从 /proc/self/exe 推断，搬到别处仍有效）
//   3) 编译期宏（开发构建 = 源码 share/wfr/workflows）兜底
std::vector<std::string> publish_workflow_dirs() {
  std::vector<std::string> out;
  if (char const* env = std::getenv("WFR_WORKFLOWS_DIR"); env && *env) out.push_back(env);
  {
    std::error_code ec;
    auto const exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) {
      std::filesystem::path const share = exe.parent_path().parent_path() / "share" / "wfr" / "workflows";
      if (std::filesystem::is_directory(share, ec)) out.push_back(share.string());
    }
  }
  out.push_back(WFR_SHARE_WORKFLOWS_DIR);
  return out;
}

// 发布自带的定 tool 库目录（tool_registry 按 `lib<from>.so` 在此找）：三档，仿 publish_workflow_dirs：
//   1) WFR_TOOL_LIB_DIRS 环境（':' 分隔多目录）
//   2) 安装布局：<bin>/../lib（从 /proc/self/exe 推断）
//   3) 编译期宏（开发构建 = tests 目录，由 tests/CMakeLists.txt 对 wfr 注入）兜底
std::vector<std::string> publish_tool_lib_dirs() {
  std::vector<std::string> out;
  if (char const* env = std::getenv("WFR_TOOL_LIB_DIRS"); env && *env) {
    std::string s(env);
    std::size_t pos = 0;
    while ((pos = s.find(':')) != std::string::npos) {
      if (!s.substr(0, pos).empty()) out.push_back(s.substr(0, pos));
      s.erase(0, pos + 1);
    }
    if (!s.empty()) out.push_back(s);
  }
  {
    std::error_code ec;
    auto const exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) {
      std::filesystem::path const lib = exe.parent_path().parent_path() / "lib";
      if (std::filesystem::is_directory(lib, ec)) out.push_back(lib.string());
    }
  }
  if (WFR_TOOL_LIB_DIRS != nullptr && WFR_TOOL_LIB_DIRS[0] != '\0')  // 编译宏兜底（空则跳过）
    out.push_back(WFR_TOOL_LIB_DIRS);
  return out;
}

// 按名/路径定位 workflow 文件：显式路径存在 → 之；否则按 lookup 约定
//（settings.workflow_dirs → ws → 全局 → 发布自带）名字查找。找不到 → nullopt。
std::optional<std::string> locate(std::string const& workdir, std::string const& spec,
                                  Json const& settings) {
  std::filesystem::path maybe(spec);
  bool const looks_path = spec.find('/') != std::string::npos || spec.ends_with(".json");
  std::string cand;
  if (looks_path) cand = (maybe.is_absolute() ? maybe : std::filesystem::path(workdir) / maybe).string();
  if (maybe.is_absolute()) cand = maybe.string();
  if (!cand.empty() && std::filesystem::exists(cand)) return cand;
  return wfr_lookup_workflow(workdir, spec, settings, publish_workflow_dirs());
}

// 递归扫（含 loop body）：迁移后 chat 的 model 节点在 loop 里，漏了就会把
// "需要 LLM" 的 workflow 误判成无需 key，运行到一半才炸。
bool has_model_node_in(Json const& nodes) {
  for (Json const& n : nodes) {
    if (!n.is_object()) continue;
    if (n.value("kind", "") == "model" || n.contains("model")) return true;
    if (n.contains("body") && n["body"].is_array() && has_model_node_in(n["body"])) return true;
  }
  return false;
}

bool has_model_node(Json const& ir_json) {
  return has_model_node_in(ir_json.value("nodes", Json::array()));
}

}  // namespace

int main(int argc, char** argv) {
  std::string workdir = ".", session = "default", name_or_path, input;
  bool reset = false, allow_import = false;
  bool wait_lock = false;       // --wait：会话被占用时轮询等待（默认直接拒绝）
  bool stream_events = false;  // --stream：把引擎事件（node_start/node_end）逐条打 JSON 行到 stdout
  std::size_t max_parallel = 1;  // wfr-㊾ 批③：Send 并行波宽度（编译期选项）
  std::map<std::string, std::string> sets;

  auto usage = [] {
    std::println(
        "用法:\n"
        "  wfr <name-or-path> [--workdir DIR] [--session NAME] [--set key=value]... "
        "[--max-parallel N] [--reset] [--allow-import] [--stream] [--wait] <query>...\n"
        "  wfr list [--workdir DIR]\n"
        "  wfr debug <name-or-path> [--session N|--all|--graph-only] [--out F] [--open]\n"
        "        --graph-only：只按 workflow 出页面（不内嵌任何会话），\n"
        "        打开后把 .wfr/sessions/*.log.jsonl 拖进左栏、或在左栏填日志 URL，即可看日志。\n"
        "  wfr -h | --help\n"
        "--max-parallel N（wfr-㊾ 批③）：Send 并行波宽度（编译期选项，默认 1=串行）。\n"
        "        >1 时 `parallel`/`executor` 编排的同一批 sends 在 worker 线程并发执行（波=屏障，\n"
        "        join 后按声明序归约），与串行最终 state 逐键相等；仅 mch_par 这类含 parallel 的\n"
        "        workflow 有意义。\n"
        "内置（发布自带 share/wfr/workflows）：chat（最简单聊天，多轮续聊；文件取数 "
        "file_context 预取 + **≤1 轮工具 + 强制收口**，--set file_route=on 开预取；需 LLM "
        "key，见 .env.example）；chat2（chat 的超集：历史渲染三档 + 思考回传/厂商参数开关，"
        "用 --set 切换 —— history_mode=full|drop_round|keep_evidence、"
        "reasoning_echo=drop|tool_calls|always、reasoning_effort=…）。\n"
        "副作用 demo：approve_bash_demo（bash 人审悬挂/恢复；--set allow_prefixes=on 会话级"
        "allow 前缀复用，--set approve_policy=allow|deny|deny:<理由> 可 mock 人工审批免交互）、"
        "sidefx_approve_demo（file_write deny 硬拦）、sidefx_mock_demo（可逆性梯度 mock："
        "订单幂等/微信窗口可撤/短信不可撤/电话任状态可关 + 模拟时钟；需 tool_registry 动态库，"
        "dev 构建自动命中，release 用 WFR_TOOL_LIB_DIRS env 指向 lib 目录）。\n"
        "<name> 查找顺序：显式路径 → <ws>/.wfr/settings.json 的 workflow_dirs → "
        "<ws>/.wfr/workflows → ~/.wfr/workflows → 发布自带。\n"
        "workspace 的 .wfr 路径本身不可被覆盖；settings 只能追加 workflow_dirs。\n"
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
    bool all = false, open = false, graph_only = false;
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
      else if (a == "--graph-only") graph_only = true;   // 只按 workflow 出图，日志靠页面拖入
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
    if (session_arg.empty() && !all && !graph_only) all = true;

    Json dir_json;
    Json const settings = wfr_read_settings(workdir);
    auto dpath = locate(workdir, dname, settings);
    if (!dpath) {
      std::println("找不到 workflow '{}'（按 settings.workflow_dirs → <ws>/.wfr/workflows → "
                   "~/.wfr/workflows → 发布自带 查找）", dname);
      return 2;
    }
    {
      std::ifstream din(*dpath);
      if (!din) {
        std::println("找不到 workflow '{}'（searched: {}）", dname, *dpath);
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

    std::vector<Json> recs;   // --graph-only 时保持为空：页面只出图，日志靠拖入
    if (graph_only) {
      // 只按 workflow 出页面：不内嵌任何会话，打开后把 .log.jsonl 拖进去即可
    } else if (all) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(wfr_root(workdir) / "sessions", ec), end;
           it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) ||
            !it->path().filename().string().ends_with(".log.jsonl"))
          continue;
        SessionLog slog(it->path().string());
        // 每个会话的 run 都从 r-1 起编号 —— 跨会话会撞车：run 前缀「会话名」，轮次才不会并组
        std::filesystem::path stem = it->path().filename();
        stem.replace_extension("");
        stem.replace_extension("");
        std::string const sess = stem.string();
        for (auto& r : slog.records()) {
          r["session"] = sess;
          std::string const run = r.value("run", "");
          if (!run.empty() && run.find(sess + "/") != 0) r["run"] = sess + "/" + run;
          recs.push_back(std::move(r));
        }
      }
    } else {
      SessionLog slog(session_path(workdir, session_arg));
      recs = slog.records();
      // 与 --all 分支对齐：run 带上会话前缀。否则前端"会话/轮次"二级选择器按
      // run.split('/')[0] 分桶，每个 r-N 都会被当成一个独立会话（下拉里显示整串）。
      for (Json& r : recs) {
        r["session"] = session_arg;
        std::string const run = r.value("run", "");
        if (!run.empty() && run.find(session_arg + "/") != 0) r["run"] = session_arg + "/" + run;
      }
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
      [[maybe_unused]] int const sys_rc = std::system(cmdline.c_str());
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
    else if (a == "--wait") wait_lock = true;  // 会话忙时排队等待（默认拒绝）
    else if (a == "--allow-import") allow_import = true;
    else if (a == "--stream") stream_events = true;
    else if (a == "--max-parallel" && i + 1 < argc) {
      // 编译期并行度（不写 IR；host 决定）。非法值 → 报错（fail-closed，不用 0 当串行掩盖）。
      try { max_parallel = std::stoul(argv[++i]); }
      catch (std::exception const&) {
        std::println("--max-parallel 需要正整数，收到 '{}'", argv[i]);
        return 2;
      }
      if (max_parallel == 0) { std::println("--max-parallel 需要 ≥1（1=串行，默认）"); return 2; }
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else
      input = input.empty() ? a : input + " " + a;
  }

  // ---- list：按查找约定列出各来源目录的 workflow ----
  if (cmd == "list") {
    Json const settings = wfr_read_settings(workdir);
    std::vector<std::string> dirs = publish_workflow_dirs();  // 发布自带（内置/固定）
    auto base = wfr_lookup_dirs(workdir, settings);           // settings → ws → 全局
    dirs.insert(dirs.end(), base.begin(), base.end());
    // 去重（按绝对规整路径）
    std::vector<std::string> seen, unique;
    for (auto const& d : dirs) {
      std::string const a = std::filesystem::absolute(d).lexically_normal().string();
      if (std::find(seen.begin(), seen.end(), a) == seen.end()) {
        seen.push_back(a);
        unique.push_back(d);
      }
    }
    for (auto const& d : unique) {
      auto names = wfr_list_workflows_in(d);
      if (names.empty()) continue;
      std::println("{}:", d);
      for (std::string const& n : names) {
        std::ifstream in(std::filesystem::path(d) / (n + ".json"));
        Json j;
        try {
          in >> j;
        } catch (...) {
          std::println("  {:<14} （读不到/坏 JSON）", n);
          continue;
        }
        std::string trig;
        if (j.contains("trigger") && j["trigger"].is_object()) {
          Json const& timing = j["trigger"].value("timing", Json::object());
          trig = "schedule " + timing.value("schedule", "");
        } else {
          trig = "manual（手工触发）";
        }
        std::println("  {:<14} nodes={}  trigger={}", n,
                     j.value("nodes", Json::array()).size(), trig);
      }
    }
    return 0;
  }

  // 批③：轻量常驻调度（wfr-㊿）—— 读 .wfr/cron.json 任务表，到期重进本进程跑一个 lot 批；--once = 跑一轮即收。
  if (cmd == "cron") {
    bool once = false;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--once") once = true;
    auto self = [&]() -> std::string {
      std::error_code ec; auto const e = std::filesystem::read_symlink("/proc/self/exe", ec);
      return ec ? std::string("wfr") : e.string();
    }();
    long sleep_secs = 10;
    long max_rounds = 0;  // 0 = 常驻
    for (int i = 2; i + 1 < argc; ++i) {
      std::string const a = argv[i];
      if (a == "--sleep-secs" && i + 2 < argc) sleep_secs = std::stol(argv[++i]);
      if (a == "--max-rounds" && i + 2 < argc) max_rounds = std::stol(argv[++i]);
    }
    long rounds = 0;
    for (;;) {
      std::filesystem::path const cj = std::filesystem::path(workdir) / ".wfr" / "cron.json";
      Json jobs = Json::array();
      if (std::ifstream in(cj); in) { try { in >> jobs; jobs = jobs.value("jobs", Json::array()); } catch (...) {} }
      long const now = static_cast<long>(std::time(nullptr));
      bool fired = false;
    for (auto& job : jobs) {
      if (!job.is_object()) continue;
      std::string const T = job.value("task", "");
      if (T.empty()) continue;
      long const every = job.value("every_minutes", 0L);
      long const last = job.value("last_run", 0L);
      if (every <= 0 || now - last < every * 60) continue;
      // arch 27 §十：project 感知装配（job.project → lot_project；否则 lot 现状兼容）
      WfrCronBatch const batch = wfr_cron_batch(workdir, job, self);
      if (batch.args.empty()) continue;  // task 缺失
      std::println("⏱ cron ➜ 触发批次 workflow={} task={} session={}", batch.workflow,
                   batch.task, batch.session);
      auto const spawned = cppchain::tool::detail::run_argv(
          batch.args, std::nullopt, cppchain::tool::detail::SpawnLimits{},
          std::chrono::minutes(30), 1u << 20);
      if (spawned.exit_code != 0)  // 拉起失败不丢现场，但视为已触发（沿用原语义）
        std::println("⚠ cron ➜ 批次拉起失败（exit={}）{}", spawned.exit_code,
                     spawned.note.empty() ? spawned.output : spawned.note);
      job["last_run"] = now;
      fired = true;
    }
    if (fired) {
      std::error_code ec; std::filesystem::create_directories(cj.parent_path(), ec);
      if (std::ofstream out(cj); out) out << Json{{"jobs", jobs}}.dump(2);
    }
      ++rounds;
      std::println("cron 轮次 {} 完成（sleep {}s）", rounds, sleep_secs);
      if (once || (max_rounds > 0 && rounds >= max_rounds)) return 0;
      std::this_thread::sleep_for(std::chrono::seconds(sleep_secs));
    }
  }

  // ---- problems/07 §8 P0：会话单飞锁 ----
  // 同一 session 同一时刻只允许一轮 query：默认 busy 直接拒绝（fail-closed）；
  // --wait 则轮询等待（≤600s）。锁在进程退出/异常时由 RAII 释放；崩溃残留由 stale 回收。
  struct SessionLockReleaser {
    std::string ws;
    std::string session;
    bool held = false;
    ~SessionLockReleaser() {
      if (held) release_session_lock(ws, session);
    }
  } lock_releaser{workdir, session, false};
  {
    std::string const lock_run = "cli-" + std::to_string(static_cast<long>(::getpid()));
    SessionLockAcquire lk = acquire_session_lock(workdir, session, lock_run);
    if (!lk.acquired && lk.holder) {
      if (!wait_lock) {
        std::println(
            "会话 '{}' 正被另一轮查询占用（pid={} run={}）；可加 --wait 等待，或取消该轮后重试。",
            session, lk.holder->pid, lk.holder->run);
        return 2;
      }
      std::println("会话 '{}' 正被 pid={} run={} 占用，等待中（最多 600s）…", session,
                   lk.holder->pid, lk.holder->run);
      for (int i = 0; i < 3000 && !lk.acquired && lk.error.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        lk = acquire_session_lock(workdir, session, lock_run);
      }
    }
    if (!lk.acquired) {
      if (lk.holder)
        std::println("等待超时：会话 '{}' 仍被 pid={} run={} 占用", session, lk.holder->pid,
                     lk.holder->run);
      else
        std::println("会话锁获取失败：{}", lk.error.empty() ? "未知原因" : lk.error);
      return 2;
    }
    lock_releaser.held = true;  // 生命周期到 main 结束（覆盖后续所有 return）
  }

  // ---- 定位 workflow（settings.workflow_dirs → ws → 全局 → 发布自带）----
  Json ir_json;
  bool model_needed_key = false;
  Json const settings = wfr_read_settings(workdir);
  auto const path = locate(workdir, name_or_path, settings);
  if (!path) {
    std::println("找不到 workflow '{}'（按 settings.workflow_dirs → <ws>/.wfr/workflows → "
                 "~/.wfr/workflows → 发布自带 查找）",
                 name_or_path);
    return 2;
  }
  {
    std::ifstream in(*path);
    if (!in) {
      std::println("找不到 workflow '{}'（searched: {}）", name_or_path, *path);
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

  // ---- 组件：内置宿主工具（datetime / calculator / weather / web_search）+ 真实模型 ----
  auto dotenv = load_dotenv({".env", "../.env", "../../.env"});
  // transport 供工具与模型共用（天气/网络搜索/LLM 都走它；纯离线 workflow 无副作用）
  auto transport = std::make_shared<http::CurlMultiTransport>();
  tool::ToolRegistry registry;
  registry.add(make_datetime_tool());
  registry.add(make_sketch_verify_tool());
  registry.add(make_calculator_tool());
  registry.add(make_weather_tool(transport, {}));              // Open-Meteo，免密钥
  {
    tool::WebSearchConfig ws_cfg;
    ws_cfg.api_key = env_or(dotenv, "WEB_SEARCH_API_KEY");     // feedcoop Bearer；未配则运行时搜索报错
    registry.add(make_web_search_tool(transport, std::move(ws_cfg)));
  }
  // wfr-㊶：文件系统对（file_read / list_dir / **file_write**）——sandbox 根 = 工作区。
  // 只读对兑现 artifact 引用行；file_write（read_only=false）由 ExecutionGuard 防护
  // （wfr-㊷）：防重放/幂等以会话日志为事实源，崩溃缺环由磁盘 probe 兜底。
  tool::FileToolsConfig ft_cfg;
  ft_cfg.root_dir = std::filesystem::absolute(workdir);  // 沙箱根 = 工作区；越界/绝对路径/链接逃逸由工具拒
  registry.add(make_file_read_tool(ft_cfg));
  registry.add(make_list_dir_tool(ft_cfg));
  registry.add(make_file_grep_tool(ft_cfg));  // problems/02 追加 S2：确定性定位（大文件不整段喂）
  registry.add(make_file_extract_tool(ft_cfg));  // S4：结构化提取（json/yaml/md/xml）
  registry.add(make_file_sample_tool(ft_cfg));   // S5：采样跳读（head/middle/tail）
  registry.add(make_file_write_tool(ft_cfg));
  // wfr-context（⑤）：HMI 编辑器 skill 文档 + LLM Wiki 检索（目录由 WFR_SKILL_DIR /
  // WFR_WIKI_DIR 指定；未配置 → 工具运行时报错，不影响其它工具）。
  registry.add(make_read_skill_tool());
  registry.add(make_wiki_search_tool());
  // 编译自检回环：写/改完页面由模型调用，host 跑 hmi 编译管线并把日志回显。
  // wfr-㊸ @①/@②/@③：bash 工具 —— 副作用任意（NeedHuman 人审 + 词面静态门禁）。
  // 执行后端 = SandboxManager 解析的沙箱（Auto：bwrap 可用→bwrap；否则受限子进程兜底）；
  // 无任何可用 provider → bash 以**空执行器**注册，handler 侧 fail-closed 拒跑（绝不裸奔）。
  // sbx 必须活得比 registry 久（spec.executor 是非拥有指针）。
  tool::SandboxConfig sbx_cfg;
  sbx_cfg.workspace = std::filesystem::absolute(workdir);
  tool::SandboxManager const sbx(sbx_cfg);
  if (sbx.available()) {
    registry.add(tool::make_bash_tool(sbx.executor()));
    std::println("bash 执行后端: {}", sbx.describe());
  } else {
    registry.add(tool::make_bash_tool(nullptr));
    std::println("⚠️ bash 不可用：{} —— bash 调用将 fail-closed 拒绝", sbx.describe());
  }
  ComponentRegistry comps;
  std::vector<std::string> all_tools;
  for (tool::ToolSpec const& t : registry.all()) all_tools.push_back(t.name);
  comps.toolsets["all"] = all_tools;
  comps.toolsets["ts"] = all_tools;
  // 内置 chat 专用：常用件 + 文件对（天气/搜索之外，兑现 artifact 引用 + 写文件）
  comps.toolsets["chat"] = {"get_weather", "web_search", "file_read", "list_dir", "file_grep",
                            "file_extract", "file_sample", "file_write", "read_skill",
                            "wiki_search"};
  // 题目修订工作流：只给一个自检工具（模型生成 sketch 后必须调用）。
  comps.toolsets["question"] = {"verify_sketch"};
  // wfr-㊸ @②：chat 的增强档 —— 带上 bash（副作用任意 → NeedHuman 人审悬挂）。
  // 独立 toolset，不污染既有 chat 的既有行为（approve_bash_demo 等用它）。
  comps.toolsets["chat_bash"] = {"get_weather", "web_search", "file_read",
                                 "list_dir",    "file_grep",  "file_extract",
                                 "file_sample", "file_write", "bash",
                                 "read_skill",  "wiki_search"};
  // B2-B 出行规划工具面：只读业务 DB 查询 + submit_result（自动验证器写 .out/trip_verdict.json）
  {
    auto ts = tool::make_trip_tools();
    std::vector<std::string> names;
    for (auto& t : ts) { names.push_back(t.name); registry.add(std::move(t)); }
    names.push_back("get_weather");
    // A2：D1 L2 用 file 落盘判据；problems/02 S2：file_grep 供确定性定位
    names.insert(names.end(), {"file_read", "file_write", "list_dir", "file_grep",
                               "file_extract", "file_sample"});
    comps.toolsets["trip"] = std::move(names);
  }
  // A6（problems/39）：验收 subagent 的只读工具面（verify kind=agent 用）。
  // 只放读工具——写工具不在 registry 里，验收者模型发不出写调用（"不存在而非被拒"）。
  comps.toolsets["verify_ro"] = {"file_read", "list_dir", "file_grep", "file_extract",
                                 "file_sample"};

  // 治理（problems/33 · G4 包装层自检）：有副作用却未声明效果分类的工具 → 启动即失败。
  // 缺省 Read = 只读；read_only=false 必须显式声明 Idempotent/Irreversible（否则运行期遗漏是静默的）。
  // 位置：所有内置工具（含 bash/file_write/submit_result）注册完成后 —— ⑤ 动态插件在其后注册的
  // 非只读工具需自行复调 effect_class_violations。
  {
    auto const v = tool::effect_class_violations(registry);
    if (!v.empty()) {
      std::println("治理自检失败：以下工具 read_only=false 但未声明效果分类（effect_class）：");
      for (std::string const& n : v) std::println("  - {}", n);
      std::println("请用 .with_effect_class(EffectClass::Idempotent|Irreversible) 声明（见 tool.hpp）");
      return 2;
    }
  }

  std::optional<model::openai::OpenAIChatModel> model;
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
    model.emplace(cfg, transport);
    comps.models["main"] = &*model;
  }

  // ---- wfr-㊷：副作用执行点防护 ----
  // guard 以**会话日志为事实源**：跨进程续聊时读旧会话历史，判定 file_write 等副作用
  // 工具的执行状态（未开始/已完成/Intent 残留）。审计信**写到本会话目录**（.wfr/audit/）。
  // 注：guard 在 main 作用域持有 → 生命周期覆盖 compile + 全部 fire_once。
  std::vector<message::Message> hist_msgs;
  {
    std::string const hp = session_path(workdir, session);
    std::error_code ec;
    if (std::filesystem::exists(hp, ec))
      hist_msgs = replay_session_messages(hp);  // 首跑无文件 → 空历史（未开始 → 可安全执行）
  }
  std::size_t audit_written = 0;  // 审计落盘计数（供调试验证，非同步关键路径）
  tool::ExecutionGuard::Options guard_opts;
  guard_opts.log = cppchain::workflow::replay_exec_log_reader(hist_msgs);  // 只建一次，运行期只查
  guard_opts.audit = [&](tool::AuditRecord const& rec) {
    // 追加写 .wfr/audit/session-<name>.jsonl（原子改写：临时 + rename，同 FileJournal 纪律）。
    // 审计不是关键路径：写失败只计数不打断（fail-open 于可观测性，fail-closed 于副作用本身）。
    std::filesystem::path const af =
        wfr_root(workdir) / "audit" / ("session-" + (session.empty() ? "default" : session) +
                                      ".audit.jsonl");
    std::error_code ec;
    std::filesystem::create_directories(af.parent_path(), ec);
    std::string tmp = af.string() + ".tmp";
    {
      std::ifstream in(af);
      std::ofstream out(tmp, std::ios::app);
      if (in && out) out << in.rdbuf();  // 拷贝现有再追加（简单原子）；会话量小，可接受
      if (out) out << nlohmann::json{{"ts", rec.ts},   {"tool", rec.tool},
                                     {"key", rec.key}, {"decision", rec.decision},
                                     {"who", rec.who}, {"why", rec.why}}
                          .dump()
                   << "\n";
    }
    if (!ec) std::filesystem::rename(tmp, af, ec);  // rename 失败 → tmp 残留文件（无害，下轮覆盖）
    ++audit_written;
  };
  tool::ExecutionGuard guard{std::move(guard_opts)};

  // wfr-㊹ 治理（problems/34）：tool_registry 节点的共享注册表 + 动态库搜索目录。
  //   shared_registry   —— tool_registry 运行期把 `*.so` 工具加载进它；独立于内置表，
  //                        消费点（model/run_tools/approve_tools）共享优先、内置兜底。
  //   tool_lib_dirs     —— 按 `lib<from>.so` 找库的目录；三档：env → 安装布局 → 编译宏。
  tool::ToolRegistry shared_registry;
  std::vector<std::string> const tool_lib_dirs = publish_tool_lib_dirs();

  CompileOptions opts;
  opts.components = &comps;
  // P1.5（arch 25）：声明式配置基准目录 = 主 workflow 文件所在目录（declarations.configs 的
  // from 相对它解析；子 workflow 装载沿用同一根）。显式路径与按名定位都从这里取。
  opts.config_dir = std::filesystem::path(*path).parent_path().string();
  opts.tool_policy = &guard;  // wfr-㊷：挂进 c_tool / c_run_tools 两个执行点
  opts.shared_registry = &shared_registry;  // wfr-㊹：tool_registry 加载目标（可空则 workflow 里该节点报错）
  if (!tool_lib_dirs.empty()) opts.tool_lib_dirs = &tool_lib_dirs;  // wfr-㊹：`lib<from>.so` 搜索目录
  opts.max_parallel = max_parallel;  // wfr-㊾ 批③：Send 并行波宽度（>1 时 parallel/executor 生效）
  // wfr-㊿：subworkflow（proxyNode）子 workflow 装载器 —— 按名/路径定位 + 静态 import 展开。
  // 与主 workflow 的装载同路径（locate + expand_workflow_imports），只多一层"子"语义。
  opts.subworkflow_loader = [&settings, &workdir](std::string const& spec) -> std::optional<Json> {
    auto const p = locate(workdir, spec, settings);
    if (!p) return std::nullopt;
    std::ifstream in(*p);
    if (!in) return std::nullopt;
    Json j;
    try {
      in >> j;
    } catch (...) {
      return std::nullopt;
    }
    if (j.contains("import")) {  // 静态展开（base_dir = 子文件所在目录），动态库 import 同主流程约定
      auto const base = std::filesystem::path(*p).parent_path().string();
      auto e = expand_workflow_imports(j, base);
      if (!e) return std::nullopt;
    }
    return j;
  };
  // 不做 config_keys 白名单（生成器侧防护；wfr 跑手写/内置 workflow 由 --set 提供键）
  // wfr-㊸ @②：人审悬挂（interrupt）需要 checkpointer + thread_id。用**进程内**
  // MemoryCheckpointer：悬挂↔恢复只发生在同一次 wfr 调用内；跨进程续聊仍走会话
  // 日志 replay（cp 每次进程重建，"日志即事实源"语义不变）。
  state::MemoryCheckpointer mcp;
  auto wf = compile_workflow(*ir, registry, &mcp, opts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 2;
  }
  std::string const trig_desc =
      wf->trigger ? "trigger: schedule " + wf->trigger->schedule : "trigger: manual（wfr 拉起）";
  {
    std::size_t count = 0;
    cppchain::workflow::for_each_node(*ir, [&count](cppchain::workflow::NodeIR const&,
                                                    cppchain::workflow::LoopScope const*) {
      ++count;
      return true;
    });
    std::println("workflow: {}（{}）  ·  {} 个节点", wf->ir_id, trig_desc, count);
  }
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());

  // ---- 会话日志（.wfr/sessions/；session_path 已迁入 .wfr）----
  std::string const slog_path = session_path(workdir, session);
  if (reset) {
    std::error_code ec;
    std::filesystem::remove(slog_path, ec);
    std::println("已清空会话: {}", slog_path);
  }
  SessionLog slog(slog_path, wf->ir_id);
  seed_run_seq(slog.max_run_seq());   // 跨进程续编号：本轮 run = 该会话已有最大 + 1（轮次不重号）
  if (input.empty()) {
    std::println("（未给输入。加一句输入即续跑/聊一句；--reset 清空会话）");
    return 0;
  }

  FireOptions fo;
  fo.log = &slog;
  fo.thread_id = "fire";  // wfr-㊸ @②：人审悬挂/恢复共用的线程（进程内 cp）
  // --stream：引擎事件逐条打 JSON 行到 stdout（CGI/前端流式渲染思考/工具/文本的通道）。
  // 事件含 node_start/node_end + node_kind + step + payload（= 节点 update，model→messages
  // 含 AI/reasoning/tool_calls，run_tools→Tool 消息）。逐行 flush 保证边跑边出。
  // 同时投影到会话日志（project_stream_to_log）：--stream 让 wrapper 结构日志让位（streaming=true），
  // 若只打 stdout 不投影，AI/工具消息不会落会话日志 → 下轮 replay / CGI history / debug 读不到。
  if (stream_events) {
    fo.stream = [wf_ptr = &wf.value()](cppchain::graph::StreamEvent const& ev) {
      std::cout << ev_json(ev).dump() << '\n';
      std::cout.flush();
      // 投影到会话日志：与默认 wrapper 同信封（engine_enter/exit + update），
      // 保证会话历史完整可回放（CGI 的 history/debug 依赖它）。
      if (wf_ptr->sink->log != nullptr) {
        cppchain::workflow::project_stream_to_log(*wf_ptr->sink->log, wf_ptr->sink->run,
                                                  ev, "");
      }
    };
  }
  Json inp = Json::array();
  inp.push_back(message::human_message(input));
  fo.input_messages = std::move(inp);

  Json config{{"workspace", workdir}, {"session", session}};
  for (auto const& [k, v] : sets) config[k] = v;

  // wfr-㊸ @④：会话级 allow 前缀（范围不变量）。开关 = `--set allow_prefixes=on`：
  //   - 把 <session>.allowlist.jsonl 的批准记录灌进 config → approve_tools 自动放行同前缀命令
  //   - 批准时把命令原文追加进 allowlist（下文 fire 循环 approve 分支）
  // 关（缺省）→ 不读不写，每次 NeedHuman 依旧悬挂人审（既有行为不变）。
  // 沙箱身份无论开关都注入：范围不变量 = 自动放行只在「同会话 + 同沙箱 provider」下生效。
  if (sbx.available()) config["sandbox_provider"] = sbx.provider_name();
  bool const allow_persist = sets.contains("allow_prefixes") && sets.at("allow_prefixes") == "on";
  if (allow_persist) {
    config["approved_prefixes"] = cppchain::workflow::wfr_load_allow_prefixes(workdir, session);
    std::println("会话级 allow 前缀: {} 条（{}）", config["approved_prefixes"].size(),
                 cppchain::workflow::wfr_allowlist_path(workdir, session).string());
  }

  // wfr-㊸ @②：一次触发 = fire 循环直至跑完。每逢 approve_tools 悬挂（bash 等
  // NeedHuman 工具），打印待批请求、读一行人工裁决（approve / reject <理由> / abort），
  // 再 resume 续跑。不悬挂的 workflow 只走一次 fire，与既有行为一致。
  Json result_state;
  for (;;) {
    auto r = fire_once(*wf, config, fo);
    if (!r) {
      std::println("触发失败: {}", r.error().message);
      return 1;
    }
    result_state = *r;
    // wfr-㊹：动态工具在 tool_registry 运行期才进共享表 → 治理自检延后到 fire 后复调
    //（插件 descriptor 契约本已声明；这是纵深防御，防"新库漏声明"成为静默盲区）。
    {
      auto const v2 = tool::effect_class_violations(shared_registry);
      if (!v2.empty()) {
        std::println("治理自检失败：动态加载工具 read_only=false 未声明 effect_class：");
        for (std::string const& n : v2) std::println("  - {}", n);
        return 2;
      }
    }
    auto const& pend = wf->graph.last_interrupt();
    if (!pend.interrupted) break;  // 跑完（或本就不需要人审）

    // —— 悬挂分支：plan_review（步骤边界确认） / approve_tools（工具批） ——
    if (pend.payload.value("kind", "") == "plan_review_request") {
      std::println();
      std::println("⏸ 步骤边界确认（节点 {}）：", pend.node);
      Json const pl = pend.payload.value("plan", Json::object());
      std::println("   目标: {}", pl.value("goal", ""));
      std::println("   进度: {} / {}   已完成步骤摘要:\n   {}", pl.value("cursor", 0L),
                   pl.value("count", 0L), pl.value("done", ""));
      std::println("   下一步: {}", pl.value("next", ""));
      std::string const policy = config.value("approve_policy", "prompt");
      Json resume;
      if (policy != "prompt") {
        std::println("  下一步？[continue | rollback_to <K> | abort]： → mock 边界裁决（approve_policy={}）", policy);
        // allow/continue → 放行；其余（deny/未知）→ abort（fail-closed，留痕）
        resume = (policy == "allow" || policy == "continue") ? Json{{"action", "continue"}}
                                                             : Json{{"action", "abort"}};
      } else {
        std::print("  下一步？[continue | rollback_to <K> | abort]：");
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) {
          std::println();
          // 治理（problems/33）：stdin 结束也走 abort 续跑 → _cancelled 终态成档
          //（不再 return 0 悬挂残留，取消必须是可查的终态）
          std::println("（stdin 结束，未获裁决 —— 按取消处理，落 _cancelled 档。）");
          resume = Json{{"action", "abort"}};
        } else {
          std::string const t = cppchain::wfr::trim(line);
          if (t.empty() || t == "abort" || t == "a" || t == "q" || t == "quit") {
            std::println("（已中止 —— 落 _cancelled 档。）");
            resume = Json{{"action", "abort"}};
          } else if (t == "continue" || t == "c") {
            resume = Json{{"action", "continue"}};
          } else if (t.starts_with("rollback_to") || t == "r" || t.starts_with("r ")) {
            long to = 0;
            for (char ch : t)
              if (ch >= '0' && ch <= '9') to = to * 10 + (ch - '0');
            resume = Json{{"action", "rollback_to"}, {"to", to}};
          } else {
            resume = Json{{"action", "abort"}};  // 未识别 → abort（fail-closed，留痕）
          }
        }
        std::println("  → 裁决 {}", resume.dump());
      }
      fo.resume = std::move(resume);
      fo.input_messages.clear();  // 恢复续跑：input 由引擎忽略
      continue;
    }

    // —— 悬挂：人工审批 ——
    std::println();
    std::println("⏸ 需人工审批（节点 {}）—— 本轮执行停在审批点：", pend.node);
    if (pend.payload.contains("calls") && pend.payload["calls"].is_array()) {
      for (Json const& c : pend.payload["calls"])
        std::println("  · tool={}  id={}  args={}", c.value("tool", "?"), c.value("id", "?"),
                     c.value("args", Json::object()).dump());
    } else {
      std::println("  待批请求: {}", pend.payload.dump());
    }
    // 裁决来源：mock（--set approve_policy=allow|deny|deny:理由，免交互自动化测试）
    // 或人工 stdin（缺省 prompt，现状）。mock 未知值 fail-closed 拒绝。
    std::string const policy = config.value("approve_policy", "prompt");
    Json resume;
    if (policy == "prompt") {
      std::print("  批准？[approve | reject <理由> | abort]：");
      std::cout.flush();
      std::string line;
      if (!std::getline(std::cin, line)) {
        std::println();
        std::println("（stdin 结束，未获裁决 —— 本次触发止于审批点，可留待下次续审。）");
        return 0;
      }
      std::string const t = cppchain::wfr::trim(line);
      if (t.empty() || t == "abort" || t == "q" || t == "quit") {
        std::println("（已中止 —— 本次触发止于审批点。）");
        return 0;
      }
      if (t == "y" || t.starts_with("approve")) {
        resume = Json{{"action", "approve"}};
      } else if (t == "n") {
        resume = Json{{"action", "reject"}};
      } else if (t.starts_with("reject") || t.starts_with("拒")) {
        std::string const fb = cppchain::wfr::trim(t.substr(std::string("reject").size()));
        resume = Json{{"action", "reject"}, {"feedback", fb}};
      } else {
        // 未识别输入 → 按拒绝处理（fail-closed：不执行）
        resume = Json{{"action", "reject"}, {"feedback", t}};
      }
    } else {
      std::println("  批准？[approve | reject <理由> | abort]： → mock 审批（approve_policy={}）", policy);
      if (policy == "allow") resume = Json{{"action", "approve"}};
      else if (policy == "deny") resume = Json{{"action", "reject"}};
      else if (policy.starts_with("deny:")) {
        // 只在这里对 "deny:" 取后缀——policy 是 allow/deny/空等短串时不可 substr(5)（越界）
        std::string const fb = cppchain::wfr::trim(policy.substr(5));
        resume = Json{{"action", "reject"}, {"feedback", fb}};
      }
      else resume = Json{{"action", "reject"}, {"feedback", "未知 approve_policy: " + policy}};
    }

    // 公共（mock 与人工一致）：批准 → 会话级 allow 前缀落盘（wfr-㊸ @④）。
    // 只记「命令类」调用（bash 的 command 参数）；无 command 的 NeedHuman 不持久化。
    if (resume.value("action", "") == "approve" && allow_persist) {
      std::size_t added = 0;
      if (pend.payload.contains("calls") && pend.payload["calls"].is_array()) {
        for (Json const& c : pend.payload["calls"]) {
          if (c.value("tool", "") != "bash") continue;
          auto const& args = c.value("args", Json::object());
          if (!args.is_object() || !args.contains("command") || !args["command"].is_string())
            continue;
          std::string const cmd = args["command"].get<std::string>();
          if (cmd.empty()) continue;
          // wfr-㊸ ⑤：落盘时记命令的**词面类别** —— allow 前缀自动放行按类别封顶，
          // 防止「批过只读前缀 → 注入/串联危险后缀」被自动放行。
          cppchain::workflow::wfr_append_allow_prefix(
              workdir, session, cmd, config.value("sandbox_provider", ""),
              tool::command_class_name(tool::classify_bash_command(cmd)));
          ++added;
        }
      }
      if (added > 0)
        std::println("  ↳ 已把 {} 条批准命令记入本会话 allow 前缀（同前缀后续自动放行）", added);
    }
    fo.resume = std::move(resume);
    fo.input_messages.clear();  // 恢复续跑：input 由引擎忽略（快照已含本轮前半段）
    std::println("  → 裁决 {}\n", fo.resume->dump());
  }

  std::string answer = last_ai_text(result_state);
  message::sanitize_utf8(answer);  // 模型串流拼接可能带断字节：净化后完整打印（不截断）
  // --stream：事件流就是输出（CGI 按行解析 JSON），抑制人类可读打印避免污染。
  if (!stream_events)
    std::println("\n{}", answer.empty() ? "(无文本回复)" : answer);
  // wfr-㊷：副作用防护统计（验证陷阱纪律：计数/审计行数证明被测路径真被走到）
  if (!stream_events && guard.has_log())
    std::println("副作用防护: 执行 {} · 重放 {} · fail-closed {} · 审计 {} 行",
                 guard.executions(), guard.replays(), guard.blocked(), audit_written);
  if (!stream_events)
    std::println("会话已更新: {} 条消息", replay_session_messages(slog_path).size());
  return 0;
}