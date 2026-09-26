// demo: m9_artifact —— 超长工具结果落盘，prompt 只放引用（离线确定性）
//
//  看点：工具返回大段文本时，ArtifactToolResults 把全文落盘（artifact 字段指向文件），
//  注入模型的内容换成"头部 + 引用行"，token 明显下降；全程用 FakeTransport 确定性复现。
#include <cppchain/agent/agent.hpp>
#include <cppchain/agent/artifact.hpp>
#include <cppchain/agent/tokens.hpp>
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

http::FakeTransport::Script tool_script(std::string const& name, std::string const& args) {
  http::FakeTransport::Script s;
  nlohmann::json delta = {{"tool_calls", nlohmann::json::array(
                                              {{{"index", 0},
                                                {"id", "call_1"},
                                                {"function", {{"name", name}, {"arguments", args}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

// 一段真正长的"工具结果"（约 60 个片段）
std::string big_payload() {
  std::string s;
  for (int i = 1; i <= 60; ++i)
    s += "片段 " + std::to_string(i) +
         "：这是一段很长的公开文档正文，用于演示超长工具返回的落盘处理，包含编号、正文与结束。\n";
  return s;
}

}  // namespace

int main() {
  auto transport = std::make_shared<http::FakeTransport>();
  model::ModelConfig cfg;
  cfg.api_key = "k";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "m";
  model::openai::OpenAIChatModel model(cfg, transport);

  tool::ToolRegistry tools;
  tools.add(tool::make_tool("search_docs", "搜索内部文档返回匹配片段",
                            tool::SchemaBuilder().prop("query", "string", "检索词").build(),
                            [](nlohmann::json const&, tool::ToolContext&) {
                              return std::expected<nlohmann::json, tool::ToolError>{
                                  nlohmann::json(big_payload())};
                            }));

  // 落盘目录用 demo-out（已 gitignore），开跑前清空
  std::string const dir = "demo-out/m9-artifacts";
  std::filesystem::remove_all(dir);

  // 脚本：模型一档调 search_docs，二档给最终答复
  transport->push(tool_script("search_docs", R"({"query":"文档"})"));
  transport->push(text_script("好的，我基于检索到的文档作答。"));

  AgentOptions opts;
  opts.system_prompt = "你是助手，检索文档后基于结果回答。";
  Agent agent(model, tools, opts);
  agent.add_middleware(std::make_shared<ArtifactToolResults>(
      ArtifactToolResults::Options{.dir = dir, .max_inline_tokens = 20}));

  auto r = agent.invoke({message::human_message("帮我查一下文档里关于这个主题的内容")});
  if (!r) {
    std::println("失败: {}", r.error().message);
    return 1;
  }

  std::println("=== m9_artifact：超长结果落盘 ===");
  std::string const original = big_payload();
  std::size_t inline_tokens = 0;
  std::println("原文规模：约 {} tokens / {} 字节", tokens::estimate(original), original.size());

  for (auto const& m : agent.transcript()) {
    if (m.type == Role::Tool) {
      std::println("— 注入模型的 ToolMessage —");
      std::println("  content(前 120 字): {}", std::string_view{m.text()}.substr(0, 120));
      inline_tokens = tokens::estimate(m.text());
      std::println("  inline 估算: {} tokens", inline_tokens);
      if (m.artifact)
        std::println("  完整结果已落盘: {}（{} 字节）", *m.artifact, original.size());
      else
        std::println("  artifact 字段未设置（异常）");
    } else if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [ai] 调用工具 {}({})", tc.name, tc.args.dump());
    } else {
      std::println("  [{}] {}", std::string(message::role_str(m.type)), m.text());
    }
  }
  std::println("\n结论：模型看到的 inline 从约 {} tokens 降到约 {} tokens（全文在文件里，prompt 只放引用行）。",
               tokens::estimate(original), inline_tokens);
}