// demo: agent_cli —— 命令行的 single-agent / react-agent（--mode 切换策略，交互式 / 文件批量）
//
// 用途：myplane.md 架构模式实验的 CLI 载体。同一个可执行体用 --mode 切四种策略，
//      在相同输入上对比「效果」差异（无工具基线 / 工具循环 / 历史压缩 / 副作用审批）。
//
// 四种模式（全部复用 Agent 循环 + 中间件，见 cppchain/agent/agent.hpp）：
//   · direct        —— 不挂任何工具，模型直接回答（对照基线：拿不到外部信息）
//   · react         —— 标准 ReAct：get_datetime + calculator（纯离线）；.env 有
//                      WEB_SEARCH_API_KEY 时追加 web_search（真联网）
//   · react-trim    —— react + TrimHistory 历史压缩（长会话下对比 token 成本）；
//                      --summarize 时用 LLM 摘要器压旧史（保记忆，代价是多一次模型调用），
//                      否则朴素折叠占位（省 token 但早期事实会丢）
//   · react-approve —— react + 假副作用工具 send_email / delete_file + ToolApproval
//                      名单拒绝：模型调用即被拒并作为 error ToolMessage 回喂
//
// 输入源（三选一，互斥）：
//   · -i / --interactive  交互式 REPL（缺省；同一 thread + MemoryCheckpointer 多轮记忆）
//   · -p / --prompt TEXT  单条 prompt，跑完即止
//   · -f / --file FILE    文件批量：整行 `---` 分隔多区块、`#` 开头整行为注释、块内多行
//
// 输出：
//   · 缺省：每轮可读轨迹（工具调用 / 状态）+ stats 行（迭代数 / 工具往返 / 拒绝 / 压缩 /
//     token / 耗时）
//   · --json：stdout 只输出一个 JSON 记录数组（dump(2)），任何人类可读行都不出现
//     → 同一输入文件跑不同 --mode 再 jq / diff，即可对比
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL；可选 WEB_SEARCH_API_KEY
//（企业代理环境自动探测 ~/.geo/corp-ca.pem，或显式 LLM_CA_BUNDLE）
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/web_search.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <chrono>
#include <cstdio>
#include <iostream>

#include <unistd.h>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

namespace {

// ============================================================ 参数与帮助

enum class Mode { Direct, React, ReactTrim, ReactApprove };
enum class InputSource { Interactive, Prompt, File };

struct CliOptions {
  Mode mode = Mode::React;
  InputSource input = InputSource::Interactive;  // 缺省交互式
  std::string prompt;
  std::string file;
  bool json = false;
  bool summarize = false;  // react-trim 用 LLM 摘要器压缩（保记忆），否则朴素占位
  bool show_help = false;
  bool list_modes = false;
};

std::string_view mode_str(Mode m) {
  switch (m) {
    case Mode::Direct: return "direct";
    case Mode::React: return "react";
    case Mode::ReactTrim: return "react-trim";
    case Mode::ReactApprove: return "react-approve";
  }
  return "?";
}

std::optional<Mode> parse_mode(std::string_view s, std::string& err) {
  if (s == "direct") return Mode::Direct;
  if (s == "react") return Mode::React;
  if (s == "react-trim") return Mode::ReactTrim;
  if (s == "react-approve") return Mode::ReactApprove;
  err = "未知模式 '" + std::string(s) + "'（可选: direct | react | react-trim | react-approve）";
  return std::nullopt;
}

// 零依赖手写 argv 解析（项目无 getopt / CLI11 先例）。
// 支持 --k=v 与 -k v（以及 -m 短选项粘连 -mdirect）；输入源 -i/-p/-f 互斥。
std::optional<CliOptions> parse_cli(int argc, char** argv, std::string& err) {
  CliOptions o;
  bool input_set = false;

  // 取下一个 argv 作为值（-k v 形态）
  auto take_value = [&](int& i, std::string_view opt) -> std::optional<std::string> {
    if (i + 1 >= argc) {
      err = "选项 " + std::string(opt) + " 需要一个参数";
      return std::nullopt;
    }
    return std::string(argv[++i]);
  };

  // 输入源三选一：第一次设置即锁定，第二次不同值 → 报错
  auto set_input = [&](InputSource src) -> bool {
    if (input_set && o.input != src) {
      err = "输入源互斥：-i / -p / -f 三选一（出现多个）";
      return false;
    }
    o.input = src;
    input_set = true;
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    std::string_view const t(argv[i]);
    std::string_view opt = t;
    std::string_view inline_val;
    bool has_inline = false;
    auto const eq = t.find('=');
    if (eq != t.npos) {
      opt = t.substr(0, eq);
      inline_val = t.substr(eq + 1);
      has_inline = true;
    }

    auto resolve = [&](std::string_view flag) -> std::optional<std::string> {
      if (has_inline) return std::string(inline_val);
      return take_value(i, flag);
    };

    if (opt == "-h" || opt == "--help") {
      o.show_help = true;
    } else if (opt == "--list-modes") {
      o.list_modes = true;
    } else if (opt == "--json") {
      o.json = true;
    } else if (opt == "--summarize") {
      o.summarize = true;
    } else if (opt == "-i" || opt == "--interactive") {
      if (!set_input(InputSource::Interactive)) return std::nullopt;
    } else if (opt == "-p" || opt == "--prompt") {
      auto v = resolve(opt);
      if (!v) return std::nullopt;
      if (v->empty()) {
        err = "-p / --prompt 的 prompt 为空";
        return std::nullopt;
      }
      if (!set_input(InputSource::Prompt)) return std::nullopt;
      o.prompt = std::move(*v);
    } else if (opt == "-f" || opt == "--file") {
      auto v = resolve(opt);
      if (!v) return std::nullopt;
      if (v->empty()) {
        err = "-f / --file 的路径为空";
        return std::nullopt;
      }
      if (!set_input(InputSource::File)) return std::nullopt;
      o.file = std::move(*v);
    } else if (opt == "-m" || opt == "--mode") {
      auto v = resolve(opt);
      if (!v) return std::nullopt;
      if (auto m = parse_mode(*v, err)) {
        o.mode = *m;
      } else {
        return std::nullopt;
      }
    } else if (opt.size() > 2 && opt[0] == '-' && opt[1] == 'm' && !has_inline) {
      // 短选项粘连值：-mdirect
      if (auto m = parse_mode(opt.substr(2), err)) {
        o.mode = *m;
      } else {
        return std::nullopt;
      }
    } else if (!t.empty() && t.front() == '-') {
      err = "未知参数: " + std::string(t);
      return std::nullopt;
    } else {
      err = "不接受位置参数: " + std::string(t);
      return std::nullopt;
    }
  }
  return o;
}

void usage(std::ostream& os) {
  std::println(os,
               "用法: agent_cli [选项]\n"
               "\n"
               "单一 agent / react agent 命令行：--mode 切换策略，对比不同效果。\n"
               "\n"
               "选项:\n"
               "  -m, --mode <模式>    agent 策略：direct | react | react-trim | react-approve\n"
               "                        （缺省 react）\n"
               "      --list-modes     列出全部模式及说明\n"
               "      --summarize      与 react-trim 搭配：压缩时用 LLM 摘要器（保留记忆，\n"
               "                        每触发一次压缩多一次模型调用），不指定则朴素折叠占位\n"
               "  -i, --interactive    交互式 REPL（缺省输入源；需终端，同一 thread 多轮记忆）\n"
               "  -p, --prompt <文本>  单条 prompt，跑完即止\n"
               "  -f, --file <路径>    文件批量：整行 --- 分隔多区块，# 开头整行为注释\n"
               "      --json           stdout 只输出 JSON 记录数组（配合 jq / diff 做模式对比）\n"
               "  -h, --help           显示本帮助\n"
               "\n"
               "需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（可选 WEB_SEARCH_API_KEY）\n"
               "\n"
               "示例:\n"
               "  agent_cli -m direct -p \"1+1 等于几？\"\n"
               "  agent_cli -m react  -p \"请用 calculator 精确计算 17*43+256/8\"\n"
               "  agent_cli -m react -f questions.txt --json > out.json\n"
               "  agent_cli -m react-trim -i");
}

void list_modes(std::ostream& os) {
  std::println(os,
               "模式（-m / --mode 指定，缺省 react）:\n"
               "  direct        不挂工具，模型直接回答（对照基线：拿不到外部信息）\n"
               "  react         标准 ReAct：get_datetime + calculator（离线）；.env 有\n"
               "                WEB_SEARCH_API_KEY 时追加 web_search（真联网）\n"
               "  react-trim    react + TrimHistory 历史压缩（长会话下对比 token 成本；\n"
               "                配套 --summarize 用 LLM 摘要器保记忆，否则朴素折叠会丢早期事实）\n"
               "  react-approve react + 假副作用工具 send_email / delete_file + ToolApproval\n"
               "                名单拒绝：模型调用即被拒并作为 error ToolMessage 回喂");
}

// ============================================================ 工具 Setup

// Agent / ToolNode 只存非拥有指针 → 所有工具必须具名、比 Agent 活得久。
// Setup 在 main 作用域先构造，Agent 再从它建（先 Setup → 再 Agent → 再跑）。
struct Setup {
  tool::ToolRegistry registry;                 // direct 模式保持空（即"无工具"）
  std::shared_ptr<ToolApproval> approval;      // react-approve 注册
  std::shared_ptr<TrimHistory> trim;           // react-trim 注册
};

// LLM 摘要器：把被折叠的历史压成 ≤60 字摘要，只保留用户关心的事实与偏好。
// 每触发一次压缩多一次模型调用（token 成本换记忆保留）；参照 agent_loop ④。
TrimHistory::Summarizer build_llm_summarizer(model::ChatModel& model) {
  return [&model](std::vector<Message> const& dropped)
      -> std::expected<std::string, model::ModelError> {
    std::vector<Message> prompt{message::system_message(
        "把下面的对话压缩成不超过 60 字的摘要，只保留用户关心的事实与偏好，直接输出摘要。")};
    for (auto const& m : dropped) {
      if (m.type == Role::Ai && !m.tool_calls.empty()) continue;  // 工具调用细节可丢
      prompt.push_back(m);
    }
    auto r = model.invoke(prompt, model::CallOptions{});
    if (!r) return std::unexpected(r.error());
    return r->text();
  };
}

void configure_setup(Setup& s, Mode mode, model::ChatModel& model,
                     std::shared_ptr<http::Transport> const& transport,
                     std::string const& ws_base, std::string const& ws_key,
                     std::string const& ca, bool summarize) {
  if (mode == Mode::Direct) return;  // registry 留空

  tool::DatetimeToolConfig dtc;
  dtc.default_offset = "+08:00";  // 默认北京时间
  s.registry.add(tool::make_datetime_tool(dtc));
  s.registry.add(tool::make_calculator_tool());

  if (!ws_key.empty()) {
    tool::WebSearchConfig sc;
    sc.base_url = ws_base;
    sc.api_key = ws_key;
    sc.ca_info = ca;
    s.registry.add(tool::make_web_search_tool(transport, std::move(sc)));
  }

  if (mode == Mode::ReactApprove) {
    // 假副作用工具：handler 立即"成功"返回（真副作用由审批中间件拦下，
    // 与我们承诺的"副作用 = 不可撤回"的语义演示点一致）
    s.registry.add(tool::make_tool(
        "send_email", "给指定收件人发送一封邮件（有副作用）",
        tool::SchemaBuilder().prop("to", "string", "收件人邮箱").prop("body", "string", "正文").build(),
        [](nlohmann::json const& args,
           tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
          auto to = tool::arg<std::string>(args, "to");
          if (!to) return std::unexpected(to.error());
          return nlohmann::json("（假）已发送给 " + *to);
        },
        /*read_only=*/false));
    s.registry.add(tool::make_tool(
        "delete_file", "删除服务器上的文件（危险，有副作用）",
        tool::SchemaBuilder().prop("path", "string", "文件路径").build(),
        [](nlohmann::json const& args,
           tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
          auto p = tool::arg<std::string>(args, "path");
          if (!p) return std::unexpected(p.error());
          return nlohmann::json("（假）已删除 " + *p);
        },
        /*read_only=*/false));
    s.approval =
        std::make_shared<ToolApproval>(std::vector<std::string>{"send_email", "delete_file"});
  }

  // --summarize：LLM 摘要器压缩（保记忆，成本 = 每次压缩多一次模型调用）
  if (mode == Mode::ReactTrim)
    s.trim = summarize ? std::make_shared<TrimHistory>(6, build_llm_summarizer(model))
                       : std::make_shared<TrimHistory>(6);
}

std::string system_prompt_for(Mode mode, Setup const& s) {
  if (mode == Mode::Direct)
    return "你是直接回答问题的助手：没有任何可调用工具，严格依据自身知识回答，"
           "不要编造工具调用或中间过程。回答保持简短。";

  std::string names;
  for (auto const& t : s.registry.all()) {
    if (!names.empty()) names += "、";
    names += t.name;
  }
  std::string p = "你是会调用工具的助手。可用工具：" + names +
                  "。需要精确计算、当前时间或联网资料时调用工具，回答保持简短。"
                  "只调用上面列出的工具，其余工具不存在、调用必然失败。";
  if (mode == Mode::ReactApprove)
    p += " send_email 与 delete_file 是有副作用的工具，可能被审批策略拒绝：一旦被拒绝，"
         "向用户说明原因，不要重复尝试同一调用。";
  if (mode == Mode::ReactTrim)
    p += " 长对话的历史可能被自动压缩为摘要，相关信息以摘要与最新消息为准。";
  return p;
}

// ============================================================ 展示 / 输出

// UTF-8 安全的字节截断（避免把中文切断导致非法 UTF-8）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void show_transcript(std::vector<Message> const& tr, std::size_t from) {
  for (std::size_t i = from; i < tr.size(); ++i) {
    auto const& m = tr[i];
    std::string const role = std::string(message::role_str(m.type));
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [ai] 调用工具 {}({})", tc.name, tc.args.dump());
      if (!m.text().empty()) std::println("  [ai] {}", clip(m.text(), 140));
    } else if (m.type == Role::Tool) {
      std::println("  [tool:{}] {}", m.status, clip(m.text(), 140));
    } else {
      std::println("  [{}] {}", role, clip(m.text(), 160));
    }
  }
}

// ============================================================ 单轮运行

struct Ctrs {
  std::size_t denials = 0;
  std::size_t compressions = 0;
};

Ctrs counters_of(Setup const& s) {
  return {s.approval ? s.approval->denials() : 0, s.trim ? s.trim->compressions() : 0};
}

// 跑一轮（Agent 可能已含历史）。human=true 时打印轨迹与 stats。
// 本轮切片起点 = invoke 后转录里**最后一条 Human 消息**（本轮输入；模型与工具
// 都不会产生 Human）。不用「invoke 前 transcript 长度」做基准：TrimHistory 等
// 中间件会重建消息列表（折叠旧史 → 净长度不变甚至变短），按下标差切会得到空片，
// 使 usage / 工具计数漏计（实测 react-trim REPL 压缩后轮次 stats 全 0 即此）。
nlohmann::json run_one(Agent& agent, Setup const& setup, std::string_view mode_name,
                       std::string const& prompt, bool human) {
  auto const t0 = std::chrono::steady_clock::now();
  Ctrs const c0 = counters_of(setup);

  auto r = agent.invoke({message::human_message(prompt)});

  auto const& tr = agent.transcript();
  std::size_t from = 0;
  for (std::size_t i = tr.size(); i-- > 0;)
    if (tr[i].type == Role::Human) {
      from = i;
      break;
    }
  std::size_t const d_denials = counters_of(setup).denials - c0.denials;
  std::size_t const d_trim = counters_of(setup).compressions - c0.compressions;

  if (human) {
    std::println("\n── 输入: {}", clip(prompt, 200));
    show_transcript(tr, from);
  }

  nlohmann::json rec;
  rec["mode"] = mode_name;
  rec["prompt"] = prompt;
  rec["iterations"] = agent.iterations();
  rec["stats"] = nlohmann::json{{"ms", ms_since(t0)}};

  if (!r) {
    if (human) std::println("  失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
    rec["ok"] = false;
    rec["error"] = r.error().message;
    return rec;
  }

  std::size_t tool_turns = 0, tool_errors = 0;
  long in_tok = 0, out_tok = 0, total_tok = 0;
  nlohmann::json tx = nlohmann::json::array();
  for (std::size_t i = from; i < tr.size(); ++i) {
    auto const& m = tr[i];
    nlohmann::json j;
    j["role"] = message::role_str(m.type);
    if (!m.text().empty()) j["text"] = m.text();
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      auto arr = nlohmann::json::array();
      for (auto const& tc : m.tool_calls) arr.push_back({{"name", tc.name}, {"args", tc.args}});
      j["tool_calls"] = std::move(arr);
    }
    if (m.type == Role::Tool) {
      ++tool_turns;
      if (m.status == "error") ++tool_errors;
    }
    if (m.usage) {
      in_tok += m.usage->input_tokens;
      out_tok += m.usage->output_tokens;
      total_tok += m.usage->total_tokens;
    }
    tx.push_back(std::move(j));
  }

  rec["ok"] = true;
  rec["answer"] = r->text();
  rec["stats"] = {{"tool_turns", tool_turns},
                  {"tool_errors", tool_errors},
                  {"denials", d_denials},
                  {"compressions", d_trim},
                  {"ms", ms_since(t0)},
                  {"usage_in_tokens", in_tok},
                  {"usage_out_tokens", out_tok},
                  {"usage_total_tokens", total_tok}};
  rec["transcript"] = std::move(tx);

  if (human) {
    std::println("  stats: iters={} tool_turns={} tool_errors={} denials={} compressions={} "
                 "usage={}/{}/{} tok, {} ms",
                 agent.iterations(), tool_turns, tool_errors, d_denials, d_trim, in_tok, out_tok,
                 total_tok, ms_since(t0));
    std::println("  → {}", clip(r->text(), 400));
  }
  return rec;
}

// ============================================================ 三种输入源

void run_repl(model::ChatModel& model, Setup const& setup, Mode mode, bool as_json) {
  // 持久 Agent：同一 thread + checkpointer → 多轮记忆；中间件只 add 一次，
  // ToolApproval / TrimHistory 计数器跨轮累计（先例 context_budget/main.cpp）
  state::MemoryCheckpointer cp;
  AgentOptions opts;
  opts.system_prompt = system_prompt_for(mode, setup);
  opts.checkpointer = &cp;
  opts.thread_id = "agent-cli-session";
  Agent agent(model, setup.registry, std::move(opts));
  if (setup.approval) agent.add_middleware(setup.approval);
  if (setup.trim) agent.add_middleware(setup.trim);

  nlohmann::json records = nlohmann::json::array();
  if (!as_json)
    std::println("交互模式（{}）：输入 exit / quit / 空行 退出，Ctrl-D 结束。", mode_str(mode));

  std::size_t turns = 0;
  for (;;) {
    if (!as_json) {
      std::print("you> ");
      std::fflush(stdout);
    }
    std::string line;
    if (!std::getline(std::cin, line)) break;  // EOF（Ctrl-D）
    std::string const t = demo::trim(line);
    if (t.empty()) continue;
    if (t == "exit" || t == "quit") break;
    ++turns;
    records.push_back(run_one(agent, setup, mode_str(mode), t, !as_json));
  }

  if (as_json) {
    std::println("{}", records.dump(2));
  } else if (turns > 0) {
    std::println("\n会话结束，共 {} 轮。", turns);
  }
}

void run_batch(model::ChatModel& model, Setup const& setup, Mode mode,
               std::vector<std::string> const& prompts, bool as_json) {
  // 每块新建 Agent（无 checkpointer）：转录独立、中间件计数每块自然归零，
  // 不会跨块串计数
  AgentOptions opts;
  opts.system_prompt = system_prompt_for(mode, setup);

  nlohmann::json records = nlohmann::json::array();
  std::size_t ok = 0;
  for (std::size_t i = 0; i < prompts.size(); ++i) {
    if (!as_json) std::println("\n──────── 问题 {}/{} ────────", i + 1, prompts.size());
    Agent agent(model, setup.registry, opts);
    if (setup.approval) agent.add_middleware(setup.approval);
    if (setup.trim) agent.add_middleware(setup.trim);
    auto rec = run_one(agent, setup, mode_str(mode), prompts[i], !as_json);
    if (rec.value("ok", false)) ++ok;
    records.push_back(std::move(rec));
  }

  if (as_json) {
    std::println("{}", records.dump(2));
  } else {
    std::println("\n共 {} 条，成功 {} / 失败 {}", prompts.size(), ok, prompts.size() - ok);
  }
}

// ============================================================ 文件 / 环境

std::vector<std::string> parse_batch_file(std::string const& path, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = "无法打开文件: " + path;
    return {};
  }
  std::vector<std::string> blocks;
  std::string cur;
  std::string line;
  while (std::getline(in, line)) {
    std::string const t = demo::trim(line);
    if (t.empty()) continue;      // 空行跳过
    if (t == "---") {             // 分块符：整行恰好 ---
      if (!cur.empty()) blocks.push_back(demo::trim(cur));
      cur.clear();
      continue;
    }
    if (t.front() == '#') continue;  // 整行注释
    cur += t + "\n";                 // 块内保留多行
  }
  if (!cur.empty()) blocks.push_back(demo::trim(cur));
  return blocks;
}

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

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  auto cli = parse_cli(argc, argv, err);
  if (!cli) {
    std::println(std::cerr, "错误: {}", err);
    usage(std::cerr);
    return 2;
  }
  if (cli->show_help) {
    usage(std::cout);
    return 0;
  }
  if (cli->list_modes) {
    list_modes(std::cout);
    return 0;
  }

  auto const dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const model_name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (key.empty()) {
    std::println(std::cerr, "缺少 LLM_API_KEY（参考 .env.example）");
    return 1;
  }

  bool const repl = cli->input == InputSource::Interactive;
  if (repl && !::isatty(STDIN_FILENO)) {
    std::println(std::cerr,
                 "stdin 不是交互终端（缺省输入源是交互 REPL）：请用 -p / --prompt "
                 "或 -f / --file 指定输入。");
    return 2;
  }

  // 文件模式先解析（尽早报错），prompt 模式直接折叠成单元素列表
  std::vector<std::string> prompts;
  if (cli->input == InputSource::File) {
    std::string ferr;
    prompts = parse_batch_file(cli->file, ferr);
    if (!ferr.empty()) {
      std::println(std::cerr, "错误: {}", ferr);
      return 1;
    }
    if (prompts.empty()) {
      std::println(std::cerr, "文件 {} 里没有可用的输入区块。", cli->file);
      return 1;
    }
  } else if (cli->input == InputSource::Prompt) {
    prompts = {cli->prompt};
  }

  std::string const ca = ca_bundle_from_env();
  bool const web_ok = !demo::env_or(dotenv, "WEB_SEARCH_API_KEY").empty();
  std::string const ws_base = demo::env_or(dotenv, "WEB_SEARCH_BASE_URL",
                                           "https://open.feedcoopapi.com/search_api/global_search");
  std::string const ws_key = demo::env_or(dotenv, "WEB_SEARCH_API_KEY");

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base_url;
  cfg.model = model_name;
  cfg.timeout_ms = 60000;
  cfg.ca_info = ca;
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  // --summarize 只对 react-trim 有意义（压缩才发生）；其余模式给个提示再忽略
  if (cli->summarize && cli->mode != Mode::ReactTrim)
    std::println(std::cerr, "提示: --summarize 只在 react-trim 模式生效（其它模式无历史压缩），已忽略。");

  // 先构造 Setup（工具具名）→ 再建 Agent（句柄非拥有）→ 再跑
  Setup setup;
  configure_setup(setup, cli->mode, model, transport, ws_base, ws_key, ca, cli->summarize);

  if (!cli->json) {
    std::println("模式 {} | 模型 {} | 工具 {} 个 | web_search {}",
                 mode_str(cli->mode), model_name, setup.registry.size(),
                 web_ok ? "已配置" : "未配置（不注册，模型按知识作答）");
    if (setup.trim)
      std::println("压缩: TrimHistory(6){}", cli->summarize ? " + LLM 摘要器" : "（朴素占位折叠）");
  }

  if (repl)
    run_repl(model, setup, cli->mode, cli->json);
  else
    run_batch(model, setup, cli->mode, prompts, cli->json);
  return 0;
}