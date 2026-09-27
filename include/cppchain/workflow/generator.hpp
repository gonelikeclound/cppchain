// cppchain —— workflow/generator.hpp
// 生成端（LLM 侧）：意图 → IR 的两段编排（规范 主动智能/12-generation-process.md）。
//
//   1. PlanStage —— 一次模型调用，三态只选一：
//        {"plan": {...}}                          计划就绪 → 进入 IRStage
//        {"questions": [{id, text}, ...]}         需要澄清 → 挂起会话
//        {"reject": {"reason": "..."}}            表达不了/能力缺口 → 显式拒绝
//    解析/分派失败 → 回喂重试（≤ plan_max_retries）；仍失败 → 拒绝。
//   2. IRStage —— 一次性生成整段 IR，随后过 parse + validate：
//    校验不过 → 把 describe() 整体回喂、要求**整篇重写**（≤ ir_max_retries）；
//    预算耗尽 → fail-closed 显式拒绝，绝不静默产坏 IR。
//   3. 澄清会话 —— 最多 clarify_max_rounds 轮；会话挂 Store 持久化，
//      submit_answers 续跑；answers 必须恰好覆盖提出过的问题（id 一一对应）。
//
// LLM 依赖收敛到 model::ChatModel&（invoke 即可）→ 测试可用 stub 模型全离线驱动。
// 计量：每次调用的 usage（Message.usage）按段累计，plan/ir 可拆分；总调用数受预算约束。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppchain/model/chat_model.hpp"
#include "cppchain/state/store.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/ir.hpp"

namespace cppchain::workflow {

using Json = nlohmann::json;

struct ClarifyQuestion {
  std::string id;
  std::string text;
  std::vector<std::string> options;  // 可选：2–4 项供用户按数字选择；空 = 自由文本问答
};

struct Clarification {
  std::string session_id;
  std::vector<ClarifyQuestion> questions;
  int round = 1;
};

struct Rejection {
  std::string reason;
};

struct GeneratedWorkflow {
  WorkflowIR ir;
  Json raw;  // 模型原始输出（含 trigger 原样，供审计/落盘）
};

struct UsageReport {
  long plan_in = 0;
  long plan_out = 0;
  long ir_in = 0;
  long ir_out = 0;
  int plan_calls = 0;
  int ir_calls = 0;
  [[nodiscard]] long total_tokens() const { return plan_in + plan_out + ir_in + ir_out; }
};

struct WorkflowGenerationResult {
  enum class Kind { Clarify, Rejected, Generated };
  Kind kind = Kind::Rejected;
  std::optional<Clarification> clarify;
  std::optional<Rejection> rejected;
  std::optional<GeneratedWorkflow> generated;
};

struct GeneratorOptions {
  int plan_max_retries = 3;    // PlanStage 回喂重试上限
  int ir_max_retries = 2;      // IRStage 修复轮数（1 首出 + 2 修）
  int clarify_max_rounds = 3;  // 澄清轮数上限
  int model_calls_budget = 12; // 总调用预算；耗尽 → fail-closed 拒绝
  std::string system_prompt;   // 空 → 内置默认模板（含工具目录/诚实规则/IR 语法要点）
};

class WorkflowGenerator {
 public:
  // model / registry 必须比 generator 活得久（非拥有）；store 可空 → 内部 MemoryStore。
  WorkflowGenerator(model::ChatModel& model, tool::ToolRegistry const& registry,
                    state::Store* store = nullptr, GeneratorOptions opts = {});
  ~WorkflowGenerator();

  WorkflowGenerator(WorkflowGenerator const&) = delete;
  WorkflowGenerator& operator=(WorkflowGenerator const&) = delete;

  // 单步：PlanStage → 计划就绪 → IRStage。返回 Clarify 时用 submit_answers 续跑。
  WorkflowGenerationResult run(std::string const& intent);

  // 挂着澄清问答续跑（submit_answers 内部用；测试也可直接调用）。
  WorkflowGenerationResult run_with_qa(std::string const& intent,
                                       std::vector<std::pair<std::string, std::string>> const& qa);

  // 续跑澄清：answers 必须恰好覆盖会话提出的问题（id 一一对应）。
  [[nodiscard]] std::expected<WorkflowGenerationResult, std::string> submit_answers(
      std::string const& session_id,
      std::vector<std::pair<std::string, std::string>> const& answers);

  [[nodiscard]] UsageReport const& usage() const;
  // 调试：最后一次模型原始输出（直观诊断生成质量）。
  [[nodiscard]] std::string const& last_raw() const;

 private:
  struct Impl;  // 持有全部状态（含 usage 与 last_raw，转发给 this 的访问器）
  std::shared_ptr<Impl> impl_;
};

}  // namespace cppchain::workflow