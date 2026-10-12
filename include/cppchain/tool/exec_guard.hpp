// cppchain —— tool/exec_guard.hpp
// 执行点防护（wfr-㊷）：防重放 / 幂等 / probe 兜底 —— 但**不含审批**（审批是独立节点 approve_tools）。
// 机制见 02-arch-design/18：防重放必须留在"不被快照打断的执行边界"内（graph 恢复按节点粒度
// 重放剩余包，前置节点检查会被绕过）；以**会话日志为事实源**判断执行状态，崩溃缺环（Intent
// 残留）由 ToolSpec::probe 兜底。
//
// 接口拆两步，对齐 ToolNode 的批内并行：
//   prepare —— 串行段：日志状态重建 + probe 判定 + 决策（单线程访问，宿主在调用 run 前逐 call 调）
//   run     —— 并行段：只有 Prepared.execute==true 才调；handler 直跑（含错误回喂），计 `executions_`
//   execute —— 单工具节点无并发时的便捷入口（prepare + run 一步）
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/tool/probe.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// 审计一次判定/探测。decision ∈ {allow, replay, blocked, probe_complete, probe_partial,
// probe_absent, command_readonly, command_write, command_risky, command_unknown}
// （后四者为 wfr-㊸ @① L2 词面门禁的判定，见 exec_guard.cpp）。
// 原文不落 audit（key 是参数指纹，正文在会话日志）。
struct AuditRecord {
  std::string ts = {};       // ISO 时间（ExecutionGuard::audit 统一填，调用方可不给）
  std::string tool;          // 工具名
  std::string key;           // 参数指纹幂等键
  std::string decision;      // 见上
  std::string who;           // host | model
  std::string why;           // 人读的缘由
};

// 幂等键闭合
enum class ExecKeyMode {
  NameAndArgs,  // 默认：tool_name + canonical(args) —— 防重放要看参数指纹（模型重试换 id 也能抓到）
  ToolCallId,   // 旧语义：模型生成的 call.id（rt 不稳定，但同 id 语义最明确）
};

class ExecutionGuard {
 public:
  // 从会话日志重建"该幂等键的执行状态"（三分）：
  //   intent=false, completed=false → 未开始 → 可执行
  //   intent=true,  completed=false → Intent 残留（有发起、无结果，崩溃缺环）→ probe 判定
  //   completed=true                → 有 ToolMessage（success/error）→ 重放其内容
  struct LogState {
    bool intent = false;             // 有 AI.tool_calls、无对应 ToolMessage
    bool completed = false;          // 有 ToolMessage（success 或 error）
    std::string content;             // completed 时：记录的 ToolMessage 内容（重放用）
    std::string status;              // "success" / "error"
  };
  using LogReader = std::function<LogState(std::string const& key)>;

  struct Options {
    LogReader log = nullptr;                   // 会话日志状态重建；空 = 不防重放（纯审计）
    std::function<void(AuditRecord const&)> audit = nullptr;  // 审计 sink；空 = 不审计
    bool retry_on_failure = false;             // Failed(有 error 结果) 是否允许重跑
    ExecKeyMode key_mode = ExecKeyMode::NameAndArgs;
  };

  explicit ExecutionGuard(Options opts) : opts_(std::move(opts)) {}

  // ---- 两步 API（c_run_tools batch：先串行 prepare 全部，再并行 run）----
  struct Prepared {
    bool execute = false;             // true → 真执行 handler；false → 用 report 当结果
    message::Message report = {};     // execute==false 时充当 ToolMessage（重放 / 拒跑说明）
    ProbeResult probe_res = ProbeResult::Unknown;
  };
  // pre_approved（wfr-㊸ ⑤）：该调用已过 approve_tools 人审（approved_ids）。
  // 人审看到的正是这个 args —— 词面门禁不再重复拦（写/危险命令放行后应真执行），
  // 但只对"已批准"的调用豁免；未批准的写/危险仍由 guard 保持 fail-closed（注入兜底）。
  [[nodiscard]] Prepared prepare(message::ToolCall const& call, ToolSpec const* spec,
                                 std::string_view who = "model",
                                 bool pre_approved = false) const;
  // 只有 Prepared.execute==true 才调。handler 直跑（异常→error 回喂），计 executions_。
  // 并行安全：不碰日志/审计。
  [[nodiscard]] message::Message run(message::ToolCall const& call, ToolSpec const* spec,
                                     nlohmann::json const& args) const;

  // ---- 一步 API（c_tool 单工具节点无并发）----
  [[nodiscard]] message::Message execute(message::ToolCall const& call, ToolSpec const* spec,
                                         nlohmann::json const& args,
                                         std::string_view who = "model",
                                         bool pre_approved = false) const;

  [[nodiscard]] std::size_t replays() const { return replays_; }
  [[nodiscard]] std::size_t blocked() const { return blocked_; }  // Intent→fail-closed 次数
  [[nodiscard]] std::size_t executions() const { return executions_; }
  [[nodiscard]] std::size_t audit_lines() const { return mem_audit_.size(); }
  // 是否配了日志事实源（决定防重放是否生效；logger 为 nullptr 时 guard 是纯审计穿透）
  [[nodiscard]] bool has_log() const { return opts_.log != nullptr; }

  [[nodiscard]] static std::string key_for(message::ToolCall const& call, ExecKeyMode mode);

 private:
  [[nodiscard]] LogState lookup(message::ToolCall const& call, ToolSpec const* spec) const;
  void audit(AuditRecord rec) const;  // 走 sink + 内存兜底，记 decision

  Options opts_;
  mutable std::size_t replays_ = 0;
  mutable std::size_t blocked_ = 0;
  mutable std::size_t executions_ = 0;
  mutable std::vector<AuditRecord> mem_audit_;  // 未落盘时的内存审计（离线测试断言用）
  mutable std::mutex mu_;               // 保护 executions_（run 并行段）与 mem_audit_
};

}  // namespace cppchain::tool