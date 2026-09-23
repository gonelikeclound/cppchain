// demo: tools_basic —— 工具调用完整闭环（定义 → 绑定 → 模型发起 → ToolNode 执行 → 回喂）
//
// langchain 参照：
//   tools = [get_weather]            # 定义
//   model.bind_tools(tools)          # 绑定（wire）
//   ai = model.invoke(msgs)          # 模型返回 tool_calls
//   tool_msgs = ToolNode(tools).invoke(...)   # 执行 → ToolMessage 回填
//   final = model.invoke(msgs + [ai] + tool_msgs)  # 再问一次拿最终答复
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/tool_node.hpp>

#include <memory>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::tool;
using cppchain::message::Message;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }

// 模型第一轮：发起工具调用
http::FakeTransport::Script tool_call_script() {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"我来查一下。"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
          R"("function":{"name":"get_weather","arguments":"{\"city\":\"旧金山\"}"}}]}}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
      "data: [DONE]\n\n",
  };
  return s;
}

// 模型第二轮：拿到工具结果后给出最终答复
http::FakeTransport::Script final_script() {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"旧金山现在是晴天，24°C。"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"),
      "data: [DONE]\n\n",
  };
  return s;
}

// 一个会失败的工具，用来演示"错误回喂"
http::FakeTransport::Script bad_args_script() {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_9",)"
          R"("function":{"name":"get_weather","arguments":"{}"}}]}}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
      "data: [DONE]\n\n",
  };
  return s;
}

void fill_tools(ToolRegistry& reg) {
  SchemaBuilder sb;
  sb.prop("city", "string", "城市名");
  reg.add(make_tool("get_weather", "查询指定城市的当前天气", sb.build(),
                    [](nlohmann::json const& args,
                       ToolContext&) -> std::expected<nlohmann::json, ToolError> {
                      auto city = arg<std::string>(args, "city");
                      if (!city) return std::unexpected(city.error());  // 参数缺失 → 错误回喂
                      return nlohmann::json("Sunny, 24C in " + *city);
                    }));
}

}  // namespace

int main() {
  ToolRegistry registry;
  fill_tools(registry);

  auto transport = std::make_shared<http::FakeTransport>();
  ModelConfig cfg;
  cfg.api_key = "demo";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-model";
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);
  ToolNode tools(&registry);

  // ---- 1. 完整闭环 ----
  std::println("=== 1. 工具调用闭环 ===");
  std::vector<Message> conv{message::human_message("旧金山天气怎么样？")};

  CallOptions opts;
  opts.tools = registry.to_openai_tools();  // 绑定工具（wire）

  transport->push(tool_call_script());
  auto ai = model->invoke(conv, opts);
  if (!ai) {
    std::println("模型调用失败: {}", kind_str(ai.error().kind));
    return 1;
  }
  conv.push_back(*ai);
  std::println("模型发起 {} 个工具调用: {}", ai->tool_calls.size(),
               ai->tool_calls.empty() ? "" : ai->tool_calls[0].name);

  auto tool_msgs = tools.run(*ai);
  for (auto const& m : tool_msgs) {
    std::println("工具结果 [{}] status={} content={}", m.tool_call_id.value_or("?"), m.status,
                 m.text());
    conv.push_back(m);
  }
  std::println("协议不变式校验: {}",
               message::validate_sequence(conv) ? "✗ 失败" : "✓ 通过");

  transport->push(final_script());
  auto final_msg = model->invoke(conv, opts);
  if (!final_msg) return 1;
  std::println("最终答复: {}", final_msg->text());

  // ---- 2. 错误回喂 ----
  std::println("\n=== 2. 工具出错 → 回喂模型（可自愈）===");
  transport->push(bad_args_script());
  auto ai2 = model->invoke(std::vector<Message>{message::human_message("天气？")}, opts);
  if (!ai2) return 1;
  auto err_msgs = tools.run(*ai2);  // 参数缺失 → error ToolMessage
  for (auto const& m : err_msgs)
    std::println("工具结果 [{}] status={} content={}", m.tool_call_id.value_or("?"), m.status,
                 m.text());

  // ---- 3. 送给模型的 tools schema 长什么样 ----
  std::println("\n=== 3. tools wire 形态 ===");
  std::println("{}", registry.to_openai_tools().dump(2));
  return 0;
}