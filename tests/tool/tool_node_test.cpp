// M3 测试：ToolNode（执行/错误回喂/并行/顺序/协议闭环）
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/tool/tool_node.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using cppchain::message::Message;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;

namespace {

using Json = nlohmann::json;
using Res = std::expected<nlohmann::json, ToolError>;

SchemaBuilder one_int_arg(std::string name, std::string desc) {
  SchemaBuilder sb;
  sb.prop(std::move(name), "integer", std::move(desc));
  return sb;
}

void fill(ToolRegistry& r) {
  r.add(make_tool("echo", "回显输入",
                  SchemaBuilder().prop("text", "string", "要回显的文本").build(),
                  [](Json const& args, ToolContext&) -> Res {
                    auto text = arg<std::string>(args, "text");
                    if (!text) return std::unexpected(text.error());
                    return Json(*text);
                  }));
  r.add(make_tool("boom", "总是失败", Json::object(), [](Json const&, ToolContext&) -> Res {
                    return std::unexpected(ToolError{"intentional failure"});
                  }));
  r.add(make_tool("throws", "抛异常", Json::object(), [](Json const&, ToolContext&) -> Res {
                    throw std::runtime_error("kaboom");
                  }));
  r.add(make_tool("object_result", "返回对象", Json::object(), [](Json const&, ToolContext&) -> Res {
                    return Json{{"temp", 24}};
                  }));
  r.add(make_tool("slow", "睡眠指定毫秒", one_int_arg("ms", "毫秒").build(),
                  [](Json const& args, ToolContext&) -> Res {
                    auto ms = arg<int>(args, "ms", /*required=*/false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms.value_or(0)));
                    return Json("slept");
                  }));
}

ToolCall call(std::string id, std::string name, Json args = Json::object()) {
  return ToolCall{std::move(id), std::move(name), std::move(args)};
}

std::string text_of(Message const& m) { return m.text(); }

long elapsed_ms(std::function<void()> const& fn) {
  auto const t0 = std::chrono::steady_clock::now();
  fn();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

TEST(ToolNode, executes_and_backfills_tool_call_ids) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);

  std::vector<ToolCall> calls{call("c1", "echo", Json{{"text", "hi"}}),
                              call("c2", "echo", Json{{"text", "there"}})};
  auto msgs = node.run(calls);

  ASSERT_EQ(msgs.size(), 2u);
  EXPECT_EQ(msgs[0].type, message::Role::Tool);
  EXPECT_EQ(msgs[0].tool_call_id.value_or(""), "c1");
  EXPECT_EQ(text_of(msgs[0]), "hi");
  EXPECT_EQ(msgs[0].status, "success");
  EXPECT_EQ(msgs[1].tool_call_id.value_or(""), "c2");
  EXPECT_EQ(text_of(msgs[1]), "there");
}

TEST(ToolNode, unknown_tool_becomes_error_message) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);

  auto msgs = node.run(std::vector<ToolCall>{call("c1", "no_such_tool")});
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].status, "error");
  EXPECT_NE(text_of(msgs[0]).find("unknown tool"), std::string::npos);
  EXPECT_EQ(msgs[0].tool_call_id.value_or(""), "c1");
}

TEST(ToolNode, handler_error_result_becomes_error_message) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);
  auto msgs = node.run(std::vector<ToolCall>{call("c1", "boom")});
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].status, "error");
  EXPECT_NE(text_of(msgs[0]).find("intentional failure"), std::string::npos);
}

TEST(ToolNode, handler_exception_becomes_error_message) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);
  auto msgs = node.run(std::vector<ToolCall>{call("c1", "throws")});
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].status, "error");
  EXPECT_NE(text_of(msgs[0]).find("kaboom"), std::string::npos);
}

TEST(ToolNode, missing_required_argument_surfaces_to_model) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);
  // echo 需要 text，这里故意不给
  auto msgs = node.run(std::vector<ToolCall>{call("c1", "echo", Json::object())});
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].status, "error");
  EXPECT_NE(text_of(msgs[0]).find("text"), std::string::npos);
}

TEST(ToolNode, json_result_serialized_as_content) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);
  auto msgs = node.run(std::vector<ToolCall>{call("c1", "object_result")});
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].status, "success");
  auto parsed = Json::parse(text_of(msgs[0]));
  EXPECT_EQ(parsed.at("temp").get<int>(), 24);
}

TEST(ToolNode, catch_errors_false_propagates_exception) {
  ToolRegistry reg;
  fill(reg);
  ToolNodeOptions opts;
  opts.catch_errors = false;
  ToolNode node(&reg, opts);

  // run() 标了 [[nodiscard]]，包一层 lambda 避免与 EXPECT_THROW 冲突
  auto run_boom = [&] { (void)node.run(std::vector<ToolCall>{call("c1", "boom")}); };
  auto run_unknown = [&] { (void)node.run(std::vector<ToolCall>{call("c1", "no_such_tool")}); };
  EXPECT_THROW(run_boom(), ToolException);
  EXPECT_THROW(run_unknown(), ToolException);
}

TEST(ToolNode, runs_tool_calls_in_parallel) {
  ToolRegistry reg;
  fill(reg);
  ToolNodeOptions opts;
  opts.max_parallel = 4;
  ToolNode node(&reg, opts);

  std::vector<ToolCall> calls;
  for (int i = 0; i < 4; ++i) calls.push_back(call("c" + std::to_string(i), "slow", Json{{"ms", 60}}));

  long const ms = elapsed_ms([&] { (void)node.run(calls); });
  // 串行需 ~240ms；并行应显著更快（阈值放宽避免抖动）
  EXPECT_LT(ms, 160) << "elapsed=" << ms << "ms —— 未并行执行？";
}

TEST(ToolNode, max_parallel_bounds_concurrency) {
  ToolRegistry reg;
  fill(reg);
  ToolNodeOptions opts;
  opts.max_parallel = 2;
  ToolNode node(&reg, opts);

  std::vector<ToolCall> calls;
  for (int i = 0; i < 4; ++i) calls.push_back(call("c" + std::to_string(i), "slow", Json{{"ms", 50}}));

  long const ms = elapsed_ms([&] { (void)node.run(calls); });
  // 4 个 50ms 任务、并发上限 2 → 两批 ≈ 100ms
  EXPECT_GE(ms, 80);
  EXPECT_LT(ms, 200);
}

TEST(ToolNode, result_order_matches_call_order) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);

  // 故意让后面的更快完成，验证输出仍按调用顺序
  std::vector<ToolCall> calls{call("c1", "slow", Json{{"ms", 60}}),
                              call("c2", "slow", Json{{"ms", 0}}),
                              call("c3", "slow", Json{{"ms", 20}})};
  auto msgs = node.run(calls);
  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[0].tool_call_id.value_or(""), "c1");
  EXPECT_EQ(msgs[1].tool_call_id.value_or(""), "c2");
  EXPECT_EQ(msgs[2].tool_call_id.value_or(""), "c3");
}

TEST(ToolNode, output_closes_the_message_protocol) {
  ToolRegistry reg;
  fill(reg);
  ToolNode node(&reg);

  Message ai = message::ai_message("查一下");
  ai.tool_calls = {call("c1", "echo", Json{{"text", "a"}}), call("c2", "boom")};

  auto tool_msgs = node.run(ai);

  std::vector<Message> conv{ai};
  conv.insert(conv.end(), tool_msgs.begin(), tool_msgs.end());
  // M1 的协议不变式：tool_calls 必须有对应 ToolMessage 回填
  EXPECT_FALSE(message::validate_sequence(conv).has_value());
}