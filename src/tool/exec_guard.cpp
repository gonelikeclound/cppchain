// cppchain —— src/tool/exec_guard.cpp
#include "cppchain/tool/exec_guard.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "cppchain/message/utf8.hpp"

namespace cppchain::tool {

namespace {

using cppchain::message::Message;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;

// 错误回喂：status="error" 的 ToolMessage（与 ToolNode::execute_one 同款）
Message error_tool_message(std::string const& tool_call_id, std::string const& text) {
  Message m = cppchain::message::tool_message("", tool_call_id, "error");
  m.content.clear();
  m.content.push_back(TextBlock{text, std::nullopt});
  return m;
}

// 参数指纹：递归按键排序 → 稳定 dump（{"a":1,"b":2} 与 {"b":2,"a":1} 同一指纹）。
// 与 agent.cpp 的 canonical_json 同算法 —— 独立实现避免跨层依赖。
std::string canonical_json(nlohmann::json const& v) {
  if (!v.is_object()) return v.is_string() ? v.get<std::string>() : v.dump();
  std::vector<std::string> keys;
  keys.reserve(v.size());
  for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
  std::sort(keys.begin(), keys.end());
  std::string out{'{'};
  bool first = true;
  for (auto const& k : keys) {
    if (!first) out += ',';
    first = false;
    out += k;
    out += ':';
    out += canonical_json(v.at(k));
  }
  out += '}';
  return out;
}

// ISO 8601 本地时间（审计 ts）
std::string iso_now() {
  auto const now = std::chrono::system_clock::now();
  std::time_t const t = std::chrono::system_clock::to_time_t(now);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  std::ostringstream oss;
  oss << std::put_time(&tmv, "%Y-%m-%dT%H:%M:%S");
  return oss.str();
}

}  // namespace

std::string ExecutionGuard::key_for(ToolCall const& call, ExecKeyMode mode) {
  if (mode == ExecKeyMode::ToolCallId)
    return call.id.empty() ? call.name : call.id;
  return call.name + "|" + canonical_json(call.args);
}

ExecutionGuard::LogState ExecutionGuard::lookup(ToolCall const& call,
                                                ToolSpec const* spec) const {
  // 只读工具免防重放（重跑查询无害），也走不到日志/审计 —— 与 SideEffectGuard::lookup 同纪律
  if (spec != nullptr && spec->read_only) return {};
  if (opts_.log == nullptr) return {};
  return opts_.log(key_for(call, opts_.key_mode));
}

void ExecutionGuard::audit(AuditRecord rec) const {
  rec.ts = iso_now();
  {
    std::lock_guard<std::mutex> lk(mu_);
    mem_audit_.push_back(rec);
  }
  if (opts_.audit) opts_.audit(rec);
}

ExecutionGuard::Prepared ExecutionGuard::prepare(ToolCall const& call, ToolSpec const* spec,
                                                 std::string_view who) const {
  // 只读工具完全免防护：不查日志、不审计、直接执行（重跑查询无害，无副作用可重复）
  if (spec != nullptr && spec->read_only) return {.execute = true};

  LogState const st = lookup(call, spec);

  // 未开始在日志里 → 正常执行
  if (!st.intent && !st.completed) {
    audit({.tool = call.name, .key = key_for(call, opts_.key_mode),
           .decision = "allow", .who = std::string(who), .why = "未见过 → 正常执行"});
    return {.execute = true};
  }

  // 已完成（有 ToolMessage）：成功 → 重放；失败 → 默认拒跑（可配 retry 后放行）
  if (st.completed) {
    if (st.status == "error") {
      if (!opts_.retry_on_failure) {
        ++blocked_;  // 失败过且不允许重试 → fail-closed
        audit({.tool = call.name, .key = key_for(call, opts_.key_mode),
               .decision = "blocked", .who = std::string(who),
               .why = "此前执行失败（未配置 retry_on_failure），拒绝重跑"});
        return {.execute = false,
                .report = error_tool_message(call.id,
                                             "该工具此前执行失败，为防重复副作用不重跑；"
                                             "如需重试请人工确认。")};
      }
      // retry_on_failure：失败视作可重跑 → 继续当"未开始"放行
      audit({.tool = call.name, .key = key_for(call, opts_.key_mode),
             .decision = "allow", .who = std::string(who),
             .why = "此前执行失败但允许重试 → 放行"});
      return {.execute = true};
    }
    ++replays_;
    audit({.tool = call.name, .key = key_for(call, opts_.key_mode),
           .decision = "replay", .who = std::string(who), .why = "已完成 → 重放日志结果"});
    Message m = cppchain::message::tool_message(st.content, call.id, st.status);
    return {.execute = false, .report = std::move(m)};
  }

  // Intent 残留（有发起、无结果）→ probe 兜底判定
  ProbeResult pr = ProbeResult::Unknown;
  if (spec != nullptr && spec->probe) {
    try {
      pr = spec->probe(call.args);
    } catch (...) {
      pr = ProbeResult::Unknown;  // probe 自身失败 → 按 Unknown fail-closed，绝不当作 Absent
    }
  }
  std::string const key = key_for(call, opts_.key_mode);
  switch (pr) {
    case ProbeResult::Complete:
      ++replays_;
      audit({.tool = call.name, .key = key, .decision = "probe_complete",
             .who = std::string(who),
             .why = "Intent 残留 + 探测到产物已完整 → 视为已完成，不重跑"});
      return {.execute = false,
              .report = error_tool_message(
                  call.id, "探测到该副作用已执行（执行被中断、结果未回执），为防重复不重跑；"
                           "请按探测到的现状处理。")};
    case ProbeResult::Partial:
      ++blocked_;
      audit({.tool = call.name, .key = key, .decision = "probe_partial",
             .who = std::string(who),
             .why = "Intent 残留 + 探测到产物不完整（部分写入）→ fail-closed"});
      return {.execute = false,
              .report = error_tool_message(call.id,
                                           "探测到产物不完整（可能写了一半）；不重复执行，"
                                           "请人工检查并决定如何修复。")};
    case ProbeResult::Absent:
      audit({.tool = call.name, .key = key, .decision = "probe_absent",
             .who = std::string(who), .why = "Intent 残留 + 产物不存在 → 视为未发生，可执行"});
      return {.execute = true};
    case ProbeResult::Unknown:
    default:
      ++blocked_;
      audit({.tool = call.name, .key = key, .decision = "blocked",
             .who = std::string(who),
             .why = std::string("Intent 残留 + 无法探测（probe 缺失或失败）→ fail-closed")});
      return {.execute = false,
              .report = error_tool_message(call.id,
                                           "该工具的执行状态未知（曾发起但结果未回执），"
                                           "为防重复副作用不重跑；请人工确认后处理。")};
  }
}

Message ExecutionGuard::run(ToolCall const& call, ToolSpec const* spec, nlohmann::json const& args) const {
  // 与 ToolNode::execute_one 同纪律：handler 异常/返回 unexpected → error 回喂，不抛出
  if (spec == nullptr || !spec->handler) {
    ++executions_;  // 计入一次（实际走了执行路径）
    return error_tool_message(call.id, spec == nullptr ? "unknown tool: " + call.name
                                                       : "tool has no handler: " + call.name);
  }

  ToolContext ctx;
  ctx.tool_call_id = call.id;

  std::expected<nlohmann::json, ToolError> result;
  try {
    result = spec->handler(args, ctx);
  } catch (std::exception const& e) {
    result = std::unexpected(ToolError{std::string("tool threw exception: ") + e.what()});
  } catch (...) {
    result = std::unexpected(ToolError{"tool threw exception: (non-std)"});
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    ++executions_;  // 真实执行（不管成败都算一次，语义=Handler 被调用了）
  }

  if (!result) {
    return error_tool_message(call.id, result.error().message);
  }
  std::string content = result->is_string() ? result->get<std::string>()
                                            : cppchain::message::utf8_safe_dump(*result);
  cppchain::message::sanitize_utf8(content);  // 双保险：字符串型结果也可能带非法字节
  return cppchain::message::tool_message(std::move(content), call.id);
}

Message ExecutionGuard::execute(ToolCall const& call, ToolSpec const* spec,
                                nlohmann::json const& args, std::string_view who) const {
  Prepared const p = prepare(call, spec, who);
  if (!p.execute) return p.report;
  return run(call, spec, args);
}

}  // namespace cppchain::tool