// demo: proactive —— 主动智能 Workflow 生成（交互 / 批量双模式）
//
//  交互（默认 / --intent）：给出一个请求，生成期间遇澄清点用 ask_user 机制打编号
//  菜单（<问题>\n1. 选项1\n2. 选项2 …）等用户输入，选完继续，最终输出易读 IR。
//  批量（--question-answer / --batch / --cases）：澄清按规则自动作答，未命中 →
//  默认选项 1，全程非交互；--cases 把 12 例评测固件并入跑矩阵。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（约定同 agent_cli/wf_eval；
// 企业代理自动探测 ~/.geo/corp-ca.pem，或显式 LLM_CA_BUNDLE）。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/generator.hpp>
#include <cppchain/workflow/ir.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "proactive/ask.hpp"
#include "support/dotenv.hpp"
#include "support/fakes.hpp"
#include "support/features.hpp"
#include "support/richtools.hpp"

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::workflow;
using namespace demo::fakes;
using namespace demo::feature;
using namespace demo::richtools;
using namespace proactive;

namespace {

// 真机 model 构造（约定：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL；CA 可选）
std::shared_ptr<openai::OpenAIChatModel> make_model(
    std::unordered_map<std::string, std::string> const& env) {
  // 优先 LLM_CA_BUNDLE；否则探测常见的企业 CA 位置（存在即启用，见 agent_cli/chat_live）
  std::string ca_bundle = demo::env_or(env, "LLM_CA_BUNDLE");
  if (ca_bundle.empty()) {
    if (char const* home = std::getenv("HOME"); home != nullptr) {
      std::string const candidate = std::string(home) + "/.geo/corp-ca.pem";
      if (std::ifstream in(candidate); in) ca_bundle = candidate;
    }
  }
  ModelConfig cfg;
  cfg.api_key = demo::env_or(env, "LLM_API_KEY");
  cfg.base_url = demo::env_or(env, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  cfg.model = demo::env_or(env, "LLM_MODEL", "deepseek-chat");
  cfg.ca_info = ca_bundle;
  static auto transport = std::make_shared<http::CurlMultiTransport>();
  return std::make_shared<openai::OpenAIChatModel>(cfg, transport);
}

// ---- 易读 IR ----

void print_readable_ir(WorkflowIR const& ir) {
  std::println("Workflow IR: {}", ir.id);
  std::println("  trigger: time + schedule \"{}\"", ir.trigger.schedule);
  for (NodeIR const& n : ir.nodes) {
    switch (n.kind) {
      case NodeKind::Tool: {
        std::string params;
        if (n.parameters.is_object())
          for (auto it = n.parameters.begin(); it != n.parameters.end(); ++it)
            params += (params.empty() ? "" : ", ") + it.key();
        std::string onerr;
        if (n.on_error) {
          if (n.on_error->retry > 0)
            onerr += " (retry " + std::to_string(n.on_error->retry) + ")";
          if (n.on_error->fallback) onerr += " fallback=" + *n.on_error->fallback;
        }
        std::println("  [{}] tool {} ({}) → {}{}", n.id, n.tool, params, n.next, onerr);
        break;
      }
      case NodeKind::Expr:
        std::println("  [{}] expr \"{}\" → then={} else={}", n.id, n.expr, n.then, n.else_);
        break;
      case NodeKind::Label:
        std::println("  [{}] label", n.id);
        break;
      case NodeKind::Goto:
        std::println("  [{}] goto {} (max {})", n.id, n.goto_target, n.max);
        break;
      case NodeKind::Model:
        std::println("  [{}] model {} (tools={}) → {}", n.id, n.model,
                     n.toolset.empty() ? "-" : n.toolset, n.next);
        break;
      case NodeKind::Route: {
        std::string cs;
        for (auto const& [k, v] : n.cases) cs += (cs.empty() ? "" : " ") + k + "→" + v;
        std::println("  [{}] route on={} {{{}}} default={}", n.id, n.on, cs, n.default_);
        break;
      }
      case NodeKind::RunTools:
        std::println("  [{}] run_tools → {}", n.id, n.next);
        break;
      case NodeKind::LoadHistory:
        std::println("  [{}] load_history path={} session={} → {}", n.id, n.path,
                     n.session.empty() ? "-" : n.session, n.next);
        break;
      case NodeKind::SaveHistory:
        std::println("  [{}] save_history path={} session={} → {}", n.id, n.path,
                     n.session.empty() ? "-" : n.session, n.next);
        break;
    }
  }
  std::println("  ---- JSON 规范形 ----");
  std::println("{}", ir.to_json().dump(2));
}

// 跑完整生成：澄清按 router 作答续跑，直到 Generated / Rejected / 轮数超限。
WorkflowGenerationResult generate_with_clarify(WorkflowGenerator& gen,
                                                std::string const& intent,
                                                AnswerRouter& router,
                                                bool& cancelled) {
  auto r = gen.run(intent);
  for (int guard = 0;
       r.kind == WorkflowGenerationResult::Kind::Clarify && r.clarify; ++guard) {
    if (guard >= 10) break;  // 兜底（submit_answers 内部已管 clarify_max_rounds）
    std::vector<std::pair<std::string, std::string>> answers;
    for (ClarifyQuestion const& q : r.clarify->questions) {
      std::string a;
      if (!router.ask(q.text, q.options, a)) {
        cancelled = true;
        return r;
      }
      answers.emplace_back(q.id, a);
    }
    auto next = gen.submit_answers(r.clarify->session_id, answers);
    if (!next) {
      WorkflowGenerationResult rr;
      rr.kind = WorkflowGenerationResult::Kind::Rejected;
      rr.rejected = Rejection{next.error()};
      return rr;
    }
    r = std::move(*next);
  }
  return r;
}

void report_single(WorkflowGenerator& gen, std::string const& intent,
                   AnswerRouter& router) {
  bool cancelled = false;
  auto r = generate_with_clarify(gen, intent, router, cancelled);
  std::println("\n=== 意图：{} ===", intent);
  if (cancelled) {
    std::println("（已取消）");
    return;
  }
  switch (r.kind) {
    case WorkflowGenerationResult::Kind::Clarify:
      std::println("仍缺信息，需继续澄清：{} 个问题（轮数上限）",
                   r.clarify ? static_cast<int>(r.clarify->questions.size()) : 0);
      break;
    case WorkflowGenerationResult::Kind::Rejected:
      std::println("明确拒绝：{}", r.rejected ? r.rejected->reason : "未说明原因");
      break;
    case WorkflowGenerationResult::Kind::Failed:
      std::println("生成失败（非模型拒绝，多为端点/预算问题）：{}",
                   r.failed ? r.failed->reason : "未说明原因");
      break;
    case WorkflowGenerationResult::Kind::Generated:
      if (r.generated) print_readable_ir(r.generated->ir);
      break;
  }
}

// ---- 批量：12 例评测固件矩阵 ----

int run_cases(openai::OpenAIChatModel& model, tool::ToolRegistry const& registry,
              AnswerRouter& router) {
  int golden_ok = 0, golden_total = 0, boundary_ok = 0, boundary_total = 0;
  int fails = 0;
  UsageReport usage;  // 每例独立 gen（预算 per 会话），usage 跨例累计

  std::println("=== 主动智能 Workflow 生成验收矩阵（12 例 / proactive 批量，澄清自动作答）===\n");

  for (EvalCase const& c : eval_cases()) {
    WorkflowGenerator gen(model, registry);  // 预算 = GeneratorOptions(model_calls_budget) 按会话计
    bool pass = false;
    std::string got, detail;

    if (c.expect == "Clarify") {
      // 期望澄清：首轮澄清即判（must_ask 关键词命中），不继续追问
      auto r = gen.run(c.intent);
      got = "Clarify";
      if (r.kind == WorkflowGenerationResult::Kind::Clarify && r.clarify) {
        std::string all_text;
        for (auto const& q : r.clarify->questions) all_text += q.text + " ";
        bool ask_ok = !r.clarify->questions.empty();
        for (std::string const& kw : c.must_ask)
          if (all_text.find(kw) == std::string::npos) ask_ok = false;
        pass = ask_ok;
        detail = std::to_string(r.clarify->questions.size()) + " 个问题";
        if (!pass) detail += "（must_ask 关键词未全命中）";
      }
    } else {
      bool cancelled = false;
      auto r = generate_with_clarify(gen, c.intent, router, cancelled);
      if (cancelled) {
        got = "Cancelled";
        detail = "用户中断";
      } else {
        switch (r.kind) {
          case WorkflowGenerationResult::Kind::Rejected:
            got = "Rejected";
            pass = (c.expect == "Reject") && r.rejected && !r.rejected->reason.empty();
            detail = r.rejected ? r.rejected->reason : "";
            break;
          case WorkflowGenerationResult::Kind::Failed:
            // 生成失败 ≠ 模型拒绝：任何期望都不算通过（否则环境故障记成 ✓）
            got = "Failed";
            detail = r.failed ? r.failed->reason : "";
            break;
          case WorkflowGenerationResult::Kind::Clarify:
            got = "Clarify";  // 澄清轮数超限
            detail = std::to_string(r.clarify->questions.size()) + " 问";
            break;
          case WorkflowGenerationResult::Kind::Generated: {
            got = "Generated";
            if (r.generated) {
              bool feats = true;
              for (std::string const& f : c.must_have)
                if (!check(r.generated->ir, f)) feats = false;
              for (std::string const& f : c.must_not_have)
                if (check(r.generated->ir, f)) feats = false;
              bool compiles = compile_workflow(r.generated->ir, registry).has_value();
              pass = (c.expect == "Generated") && feats && compiles;
              detail = "nodes=" + std::to_string(r.generated->ir.nodes.size()) +
                       (compiles ? " 编译OK" : " 编译失败");
              if (!feats) detail += " 特征未全中";
            }
            break;
          }
        }
      }
    }

    usage.plan_in += gen.usage().plan_in;
    usage.plan_out += gen.usage().plan_out;
    usage.ir_in += gen.usage().ir_in;
    usage.ir_out += gen.usage().ir_out;
    usage.plan_calls += gen.usage().plan_calls;
    usage.ir_calls += gen.usage().ir_calls;

    if (c.expect == "Generated") {
      ++golden_total;
      if (pass) ++golden_ok;
    } else {
      ++boundary_total;
      if (pass) ++boundary_ok;
    }
    if (!pass) ++fails;

    std::println("  {:<22} expect={:<10} got={:<10} {} {}", c.id, c.expect, got,
                 pass ? "✓" : "✗", detail);
  }

  std::println("\n黄金用例 {}/{} · 边界期望一致 {}/{} · 未过 {}",
               golden_ok, golden_total, boundary_ok, boundary_total, fails);
  std::println("总 usage：plan {} tok + ir {} tok = {} tok，plan/ir 调用 {} / {}",
               usage.plan_in + usage.plan_out, usage.ir_in + usage.ir_out,
               usage.total_tokens(), usage.plan_calls, usage.ir_calls);
  return fails > 0 ? 1 : 0;
}

// ---- CLI ----

struct CliOptions {
  std::string intent;
  bool cases = false;
  bool batch = false;
  bool help = false;
  std::vector<std::pair<std::string, std::string>> qa;  // 问题正则 → 答案值
};

// 零依赖手写 argv 解析（项目惯例，见 agent_cli）
std::optional<CliOptions> parse_cli(int argc, char** argv, std::string& err) {
  CliOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == "--intent") {
      if (i + 1 >= argc) {
        err = "--intent 缺参数";
        return std::nullopt;
      }
      o.intent = argv[++i];
    } else if (a == "--cases") {
      o.cases = true;
    } else if (a == "--batch") {
      o.batch = true;
    } else if (a == "--question-answer") {
      if (i + 2 >= argc) {
        err = "--question-answer 缺参数（需 <问题正则> <序号|文本>）";
        return std::nullopt;
      }
      std::string const rule_re = argv[++i];
      std::string const rule_val = argv[++i];
      o.qa.emplace_back(std::move(rule_re), std::move(rule_val));
    } else if (a == "--help" || a == "-h") {
      o.help = true;
    } else {
      err = "未知参数：" + a;
      return std::nullopt;
    }
  }
  return o;
}

void print_usage() {
  std::println(R"(proactive —— 主动智能 Workflow 生成（交互 / 批量双模式）

用法：
  proactive                            交互 REPL：输入需求 → 澄清以 ask_user 菜单交互 → 输出易读 IR
  proactive --intent <请求>            单条生成（未带规则时澄清仍交互等待）
  proactive --intent <请求> --question-answer <问题正则> <序号|文本> ...
                                       单条批量：澄清按规则自动作答
  proactive --cases [--question-answer ...]
                                       批量跑 12 例评测 case 打矩阵

选项：
  --question-answer <正则> <值>        可重复；值：问题带选项且为数字 → 选项序号(1/2/3)，
                                       否则当文本答案；未命中规则 → 默认选项 1
  --batch                              强制非交互（配合 --intent，未给规则也默认选项 1）
  --help / -h                          本帮助

需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（约定同 wf_eval）。)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  auto cli = parse_cli(argc, argv, err);
  if (!cli) {
    std::println("参数错误：{}（--help 看用法）", err);
    return 2;
  }
  if (cli->help) {
    print_usage();
    return 0;
  }

  auto env = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(env, "LLM_API_KEY");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example / 主动智能 端点约定）");
    return 1;
  }
  auto model = make_model(env);
  FakeToolSinks sinks;
  RichSinks rsinks;
  // 生成目录 = 基础 10 工具 + 丰富分组工具（system/sms/email/reminder）+ tool_search
  auto registry = make_rich_registry(sinks, rsinks);
  WorkflowGenerator gen(*model, *registry);

  AnswerRouter router;
  router.interactive = !(cli->batch || cli->cases || !cli->qa.empty());
  for (auto const& [re, val] : cli->qa)
    router.rules.emplace_back(std::regex(re), val);

  if (cli->cases) {
    // 12 例评测固件按基础目录（10 工具）设计——用基准注册表跑矩阵，
    // 避免丰富目录改变生成行为导致既有期望回归。
    FakeToolSinks eval_sinks;
    tool::ToolRegistry eval_registry = make_fake_tools(eval_sinks);
    return run_cases(*model, eval_registry, router);
  }
  if (!cli->intent.empty()) {
    report_single(gen, cli->intent, router);
    return 0;
  }
  if (!router.interactive) {
    std::println("批量模式需要 --intent 或 --cases");
    return 2;
  }

  // ---- 交互 REPL ----
  std::println("=== proactive 主动智能 Workflow 生成（ask_user 菜单澄清）===");
  std::println("输入需求回车生成；exit / Ctrl-D 退出。");
  for (;;) {
    std::print("> ");
    std::fflush(stdout);
    std::string intent;
    if (!std::getline(std::cin, intent)) break;
    intent = demo::trim(intent);
    if (intent.empty()) continue;
    if (intent == "exit" || intent == "quit") break;
    report_single(gen, intent, router);
  }
  return 0;
}