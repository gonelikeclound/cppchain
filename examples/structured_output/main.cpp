// demo: structured_output —— Schema 驱动的结构化输出（M4）
//
// 三段：
//   ① 结构化抽取（自由文本 → 指定 schema 的 JSON）
//   ② 路由/评判（classify：从标签集里选一个 —— M6 条件路由的前置件）
//   ③ 对照实验：ToolCall 策略 vs JsonObject 策略的 schema 遵循度（各跑 N 次）
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/model/structured.hpp>

#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::model;
using cppchain::message::Message;

namespace {

// 一个"有点难"的 schema：必填 + 数字 + 枚举 + 数组 —— 便于暴露策略差异
nlohmann::json weather_report_schema() {
  return nlohmann::json{
      {"type", "object"},
      {"properties",
       {{"city", {{"type", "string"}}},
        {"temp_c", {{"type", "number"}}},
        {"condition",
         {{"type", "string"},
          {"enum", nlohmann::json::array({"sunny", "cloudy", "rainy", "snowy"})}}},
        {"advice", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
      {"required", nlohmann::json::array({"city", "temp_c", "condition"})}};
}

std::shared_ptr<openai::OpenAIChatModel> make_model(std::string const& key, std::string const& base,
                                                    std::string const& name, std::string const& ca) {
  ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base;
  cfg.model = name;
  cfg.ca_info = ca;
  static auto transport = std::make_shared<http::CurlMultiTransport>();
  return std::make_shared<openai::OpenAIChatModel>(cfg, transport);
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）");
    return 1;
  }
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const c = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(c)) ca = c;
    }
  }

  auto model = make_model(key, base, name, ca);
  std::println("模型: {} | 结构化策略: ToolCall(默认) / JsonObject\n", name);

  // ---- ① 结构化抽取 ----
  {
    std::println("=== ① 结构化抽取 ===");
    StructuredOutput so(*model, weather_report_schema());
    std::vector<Message> msgs{message::human_message(
        "帮我把这段话整理成结构化数据：昨天旧金山白天多云，最高 18 度，晚上下了点雨，"
        "建议带件外套和雨伞。")};
    auto r = so.invoke(msgs, CallOptions{});
    if (!r) {
      std::println("失败: {} ({})", r.error().message, kind_str(r.error().kind));
      return 1;
    }
    std::println("{}", r->dump(2));
  }

  // ---- ② 路由 / 评判 ----
  {
    std::println("\n=== ② 路由/评判（classify）===");
    StructuredOutput so(*model, nlohmann::json::object());
    std::vector<std::string> const labels{"weather", "stock", "email", "translate"};
    for (auto const& q : {"明天要带伞吗？", "帮我看看 AAPL 的股价", "把这句话翻成英文"}) {
      std::vector<Message> msgs{message::human_message(q)};
      auto r = so.classify(msgs, labels, CallOptions{});
      std::println("  {:<22} → {}", q, r ? *r : ("失败: " + r.error().message));
    }
  }

  // ---- ③ 两策略遵循度对照 ----
  {
    std::println("\n=== ③ 策略对照（同一 schema，各跑 5 次）===");
    int const kRuns = 5;
    struct Stat {
      int ok = 0;
      double ms = 0;
    };
    auto run = [&](StructuredMode mode) {
      StructuredOptions opts;
      opts.mode = mode;
      opts.max_retries = 0;  // 关掉重试，才能看清"一次成型"的能力
      StructuredOutput so(*model, weather_report_schema(), opts);
      Stat st;
      for (int i = 0; i < kRuns; ++i) {
        std::vector<Message> msgs{message::human_message(
            "旧金山今天晴，25 度。请给出结构化结果（第 " + std::to_string(i) + " 次）")};
        auto const t0 = std::chrono::steady_clock::now();
        auto r = so.invoke(msgs, CallOptions{});
        st.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                     .count();
        if (r) ++st.ok;
      }
      return st;
    };

    auto const tc = run(StructuredMode::ToolCall);
    auto const jo = run(StructuredMode::JsonObject);
    std::printf("  %-14s 一次成型 %d/%d   平均 %.0f ms\n", "ToolCall", tc.ok, kRuns,
                tc.ms / kRuns);
    std::printf("  %-14s 一次成型 %d/%d   平均 %.0f ms\n", "JsonObject", jo.ok, kRuns,
                jo.ms / kRuns);
    std::println("\n说明：本端点不支持 response_format=json_schema（实测 400），"
                 "所以 schema 遵循靠 ①服务端按工具参数塑形 或 ②模型自觉。");
    std::println("      默认策略为 ToolCall；JsonObject 更适合端点不支持工具调用的场景。");
  }
  return 0;
}