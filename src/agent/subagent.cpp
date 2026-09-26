// cppchain —— src/agent/subagent.cpp
#include "cppchain/agent/subagent.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace cppchain::agent {

namespace {

using cppchain::message::Role;

// 统计转录里出现的工具调用总数（AI 消息的 tool_calls 求和）。
std::size_t count_tool_turns(std::vector<message::Message> const& msgs) {
  std::size_t n = 0;
  for (auto const& m : msgs)
    if (m.type == Role::Ai) n += m.tool_calls.size();
  return n;
}

nlohmann::json usage_json(message::Message const& m) {
  if (!m.usage) return nlohmann::json::object();
  return {{"input_tokens", m.usage->input_tokens},
          {"output_tokens", m.usage->output_tokens},
          {"total_tokens", m.usage->total_tokens}};
}

// 每个 ChatModel 实例一把串行锁：共享同一实例的**所有**子代理工具共用。
// 为什么按实例而不是按工具：批内两个不同专家并行时，仍会并发打同一 transport
// （FakeTransport 的 scripts_ 队列 / curl_multi 句柄），M2 未验证并发安全 →
// M8 以"串行换安全"（03-detail-design/05 §4.2）。条目永不删除：model 寿命长于工具。
std::mutex& serial_for(model::ChatModel const* m) {
  static std::mutex guard;
  static std::unordered_map<model::ChatModel const*, std::mutex> locks;
  std::lock_guard g(guard);
  return locks[m];
}

}  // namespace

tool::ToolSpec make_subagent_tool(SubAgentSpec const& spec, SubAgentToolOptions opts) {
  return tool::make_tool(
      spec.name, spec.description,
      tool::SchemaBuilder().prop(opts.arg_name, "string", "交给子代理处理的任务描述").build(),
      [spec, opts = std::move(opts)](
          nlohmann::json const& args,
          tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
        auto q = tool::arg<std::string>(args, opts.arg_name);
        if (!q) return std::unexpected(q.error());

        // 现构 + 串行：见头文件三条硬约束。
        std::lock_guard lock(serial_for(spec.model));
        Agent child(*spec.model, *spec.tools, spec.options);
        for (auto const& mw : spec.middlewares) child.add_middleware(mw);

        auto ret = child.invoke({message::human_message(*q)});
        if (!ret) return std::unexpected(tool::ToolError{ret.error().message});

        nlohmann::json body = {{"answer", ret->text()}};
        if (opts.include_trace)
          body["stats"] = {{"iterations", child.iterations()},
                           {"tool_turns", count_tool_turns(child.transcript())},
                           {"usage", usage_json(*ret)}};
        return body;
      },
      /*read_only=*/opts.read_only);
}

}  // namespace cppchain::agent