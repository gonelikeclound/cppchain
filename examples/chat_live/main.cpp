// demo: chat_live —— 真实端点联调（默认 DeepSeek，读 .env / 环境变量）
//
// 需要 .env 提供：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（见 .env.example）
// 密钥只从环境读取，不打印。
//
// 本 demo 同时是"超长工具返回"问题域的观测点：记录首字延迟与总耗时。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::model;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

nlohmann::json weather_tool() {
  return nlohmann::json::array({{
      {"type", "function"},
      {"function",
       {{"name", "get_weather"},
        {"description", "查询指定城市的当前天气"},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"city", {{"type", "string"}, {"description", "城市名"}}}}},
          {"required", nlohmann::json::array({"city"})}}}}}}});
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const api_key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const model_name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");

  if (api_key.empty()) {
    std::println("缺少 LLM_API_KEY：请在 .env 或环境变量中提供（参考 .env.example）");
    return 1;
  }

  // CA bundle：企业代理/自签环境需要。
  // 优先 LLM_CA_BUNDLE；否则探测常见的企业 CA 位置（存在即启用）。
  std::string ca_bundle = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca_bundle.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const candidate = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(candidate)) ca_bundle = candidate;
    }
  }

  std::println("端点: {} | 模型: {} | 密钥: 已加载(长度 {}, 不显示)", base_url, model_name,
               api_key.size());
  std::println("CA bundle: {}", ca_bundle.empty() ? "(系统默认)" : ca_bundle);

  ModelConfig cfg;
  cfg.api_key = api_key;
  cfg.base_url = base_url;
  cfg.model = model_name;
  cfg.timeout_ms = 60000;
  cfg.ca_info = ca_bundle;

  auto transport = std::make_shared<http::CurlMultiTransport>();
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);

  int failures = 0;

  // ---- 1. 流式：首字延迟 + 打字机输出 ----
  {
    std::vector<message::Message> msgs{
        message::system_message("你是简洁的助手，回答控制在两句话内。"),
        message::human_message("用一句话介绍你自己。")};
    std::println("\n=== 1. 流式（首字延迟观测）===");
    auto t0 = std::chrono::steady_clock::now();
    double first_ms = -1;
    std::string text;
    try {
      for (auto const& chunk : model->stream(msgs, CallOptions{})) {
        for (auto const& b : chunk.content)
          if (auto const* t = std::get_if<message::TextBlock>(&b)) {
            if (first_ms < 0) first_ms = ms_since(t0);
            text += t->text;
            std::print("{}", t->text);
          }
      }
      std::println("\n首字延迟 {:.0f} ms | 总耗时 {:.0f} ms | 字符数 {}", first_ms, ms_since(t0),
                   text.size());
    } catch (ModelException const& e) {
      std::println("失败: {} ({})", e.err.message, kind_str(e.err.kind));
      ++failures;
    }
  }

  // ---- 2. 工具调用 wire 通道（真实端点返回 tool_calls）----
  {
    std::vector<message::Message> msgs{message::human_message("旧金山现在天气怎么样？")};
    CallOptions opts;
    opts.tools = weather_tool();
    std::println("\n=== 2. 工具调用（tools wire 通道）===");
    auto r = model->invoke(msgs, opts);
    if (!r) {
      std::println("失败: {} ({})", r.error().message, kind_str(r.error().kind));
      ++failures;
    } else if (r->tool_calls.empty()) {
      std::println("模型未发起工具调用（回复: {}）", r->text());
    } else {
      for (auto const& tc : r->tool_calls)
        std::println("tool_call: {}({}) args={}", tc.name, tc.id, tc.args.dump());
      std::println("finish_reason={}", r->response_metadata.value("finish_reason", "?"));
    }
  }

  // ---- 3. DeepSeek reasoner：验证 reasoning_content → ReasoningBlock ----
  if (base_url.find("deepseek") != std::string::npos) {
    std::vector<message::Message> msgs{message::human_message("9.11 和 9.9 哪个大？")};
    CallOptions opts;
    opts.model = "deepseek-reasoner";  // 覆盖默认模型
    std::println("\n=== 3. deepseek-reasoner 推理内容映射 ===");
    auto r = model->invoke(msgs, opts);
    if (!r) {
      std::println("失败: {} ({})", r.error().message, kind_str(r.error().kind));
      ++failures;
    } else {
      std::size_t blocks = 0;
      std::size_t chars = 0;
      std::string preview;
      for (auto const& b : r->content)
        if (auto const* rb = std::get_if<message::ReasoningBlock>(&b)) {
          ++blocks;
          chars += rb->reasoning.size();
          if (preview.empty()) preview = rb->reasoning.substr(0, 60);
        }
      std::println("文本回复: {}", r->text());
      std::println("推理块: {} 个, 共 {} 字符 | 开头: {}", blocks, chars, preview);
      std::println("reasoning 映射: {}", blocks > 0 ? "✓ 成功" : "✗ 未捕获");
      if (blocks == 0) ++failures;
    }
  }

  std::println("\n=== 结果: {} ===", failures == 0 ? "全部通过 ✓" : "有失败项 ✗");
  return failures == 0 ? 0 : 1;
}