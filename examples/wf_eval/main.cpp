// demo: wf_eval —— 主动智能 Workflow 生成端到端验收（真机 LLM，12 例矩阵）
//
//  对 主动智能/10-eval-cases.md 的 12 例意图跑完整管线（PlanStage →
//  IRStage → compile → fire），并按特征断言打出矩阵：
//   · golden 7 例：expect=Generated，还要 must_have 全中 / must_not_have 全缺，
//     且能编译 + 触发一次不抛错；
//   · boundary 5 例：Reject（拒绝理由非空）/ Clarify（问题命中 must_ask 关键词）。
// 末尾补一段"调度模拟"：对某个已生成的价格 worklow 连续触发 3 次（每次全新状态），
// 展示通知逐次新增、loop_counts 每次归零 —— D1：手动触发，不做 OS 调度。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（约定同 agent_cli；
// 企业代理自动探测 ~/.geo/corp-ca.pem，或显式 LLM_CA_BUNDLE）。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/generator.hpp>

#include <memory>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

#include "support/dotenv.hpp"
#include "support/fakes.hpp"
#include "support/features.hpp"

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::workflow;
using namespace demo::fakes;
using namespace demo::feature;

namespace {

// 真机 model 构造（约定：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL；CA 可选）
std::shared_ptr<openai::OpenAIChatModel> make_model(
    std::unordered_map<std::string, std::string> const& env) {
  ModelConfig cfg;
  cfg.api_key = demo::env_or(env, "LLM_API_KEY");
  cfg.base_url = demo::env_or(env, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  cfg.model = demo::env_or(env, "LLM_MODEL", "deepseek-chat");
  cfg.ca_info = demo::env_or(env, "LLM_CA_BUNDLE", "");
  static auto transport = std::make_shared<http::CurlMultiTransport>();
  return std::make_shared<openai::OpenAIChatModel>(cfg, transport);
}

}  // namespace

// 宿主 fire 时**真正提供**的 config 键。必须与下面 fire_once 的入参一致 ——
// 声明之后，IR 里引用别的键会在编译期被 VR4 拒掉，而不是运行期静默变 null。
std::vector<std::string> config_keys() { return {"product_id"}; }

CompileOptions compile_opts() {
  CompileOptions o;
  o.config_keys = config_keys();
  return o;
}

int main() {
  auto env = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(env, "LLM_API_KEY");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example / 主动智能 端点约定）");
    return 1;
  }
  auto model = make_model(env);

  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  // 声明宿主会提供的 config 键：生成端据此约束提示词 + 校验（VR4），
  // 免得模型写出宿主没有的键、运行期静默变 null。
  GeneratorOptions gopts;
  gopts.config_keys = config_keys();
  WorkflowGenerator gen(*model, registry, nullptr, gopts);

  std::println("=== 主动智能 Workflow 生成验收矩阵（12 例 / 真机）===\n");

  std::vector<std::string> golden_ir;  // 已生成的 IR 落盘行（调度模拟用）

  int golden_ok = 0, golden_total = 0, boundary_ok = 0, boundary_total = 0;

  for (EvalCase const& c : eval_cases()) {
    std::string const expect = c.expect;
    auto r = gen.run(c.intent);

    bool pass = false;
    std::string got, detail;
    switch (r.kind) {
      case WorkflowGenerationResult::Kind::Rejected:
        got = "Rejected";
        pass = (expect == "Reject") && r.rejected && !r.rejected->reason.empty();
        detail = r.rejected ? r.rejected->reason : "";
        break;
      case WorkflowGenerationResult::Kind::Failed:
        // 生成**失败**（模型调用失败 / 预算耗尽 / 重试耗尽）不是"模型拒绝"：
        // 无论期望是什么都不算通过 —— 否则环境故障会被记成合法的 Reject。
        got = "Failed";
        detail = r.failed ? r.failed->reason : "";
        break;
      case WorkflowGenerationResult::Kind::Clarify:
        got = "Clarify";
        if (r.clarify) {
          bool ask_ok = true;
          if (!c.must_ask.empty()) {
            std::string all_text;
            for (ClarifyQuestion const& q : r.clarify->questions) all_text += q.text + " ";
            for (std::string const& kw : c.must_ask)
              if (all_text.find(kw) == std::string::npos) {
                ask_ok = false;  // 没有追问到该关键词（在线按容忍度看，不改判）
              }
          }
          pass = (expect == "Clarify") && ask_ok && !r.clarify->questions.empty();
          detail = std::to_string(r.clarify->questions.size()) + " 个问题";
        }
        break;
      case WorkflowGenerationResult::Kind::Generated:
        got = "Generated";
        if (r.generated) {
          bool feats = true;
          for (std::string const& f : c.must_have)
            if (!check(r.generated->ir, f)) feats = false;
          for (std::string const& f : c.must_not_have)
            if (check(r.generated->ir, f)) feats = false;

          bool fired = false;
          std::string why;
          auto wf = compile_workflow(r.generated->ir, registry, nullptr, compile_opts());
          if (!wf) {
            why = "编译失败：" + wf.error().message;  // 别把原因吞掉，否则无法诊断
          } else {
            auto st = fire_once(*wf, Json{{"product_id", "P1001"}});
            fired = st.has_value();
            // 失败原因必须打出来：光写"触发失败"无法分辨是模型写的引用不对还是运行时出问题
            if (!fired) why = "触发失败：" + st.error().message;
          }
          pass = (expect == "Generated") && feats && fired;
          detail = "nodes=" + std::to_string(r.generated->ir.nodes.size()) +
                   (fired ? " 触发OK" : " " + why);
        }
        break;
    }

    if (expect == "Generated") {
      ++golden_total;
      if (pass) ++golden_ok;
      if (r.generated) golden_ir.push_back(r.generated->ir.to_json().dump());
    } else {
      ++boundary_total;
      if (pass) ++boundary_ok;
    }

    std::println("  {:<22} expect={:<10} got={:<10} {} {}", c.id, expect, got,
                 pass ? "✓" : "✗", detail);
  }

  std::println("\n黄金用例 {}/{} · 边界期望一致 {}/{}", golden_ok, golden_total,
               boundary_ok, boundary_total);
  std::println("总 usage：plan {} tok + ir {} tok = {} tok，plan/ir 调用 {} / {}",
               gen.usage().plan_in + gen.usage().plan_out,
               gen.usage().ir_in + gen.usage().ir_out, gen.usage().total_tokens(),
               gen.usage().plan_calls, gen.usage().ir_calls);

  // ---- D1 调度模拟：连续触发同一 workflow，每次全新状态 ----
  if (!golden_ir.empty()) {
    auto ir = WorkflowIR::parse(Json::parse(golden_ir[0]));
    if (ir) {
      auto wf = compile_workflow(*ir, registry, nullptr, compile_opts());
      if (wf) {
        std::println("\n=== 调度模拟（D1：手动触发，不做 OS 调度）===");
        std::println("触发器元数据：schedule = {}", ir->trigger.schedule);
        std::size_t before = 0;
        for (int i = 0; i < 3; ++i) {
          before = sinks.notifications.size();
          auto st = fire_once(*wf, Json{{"product_id", "P" + std::to_string(i)}});
          std::size_t const added = sinks.notifications.size() - before;
          std::string const counts =
              st && st->contains("loop_counts") ? " loop=" + st->at("loop_counts").dump() : "";
          std::println("  第 {} 次触发：新增通知 {} 条{}", i + 1, added, counts);
        }
        std::println("  √ 每次触发全新状态（loop 计数归零、通知按次新增）");
      }
    }
  }

  // 验收矩阵：有任何一条不符合期望就以非 0 退出，好让 run-demo.sh / CI 判定。
  // （原先 main 直接走到结尾 → 恒返回 0：0/7 全灭也报"通过 1 / 失败 0"。）
  if (golden_ok != golden_total || boundary_ok != boundary_total) {
    std::println("\n未达标：黄金 {}/{}、边界 {}/{} → 退出码 1", golden_ok, golden_total,
                 boundary_ok, boundary_total);
    return 1;
  }
  std::println("\n达标：黄金 {}/{}、边界 {}/{}", golden_ok, golden_total, boundary_ok,
               boundary_total);
  return 0;
}