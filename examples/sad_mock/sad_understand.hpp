// examples/sad_mock/sad_understand.hpp
// SAD 理解层（真机 LLM）：全局事件 → 任务规格 JSON（模板必须来自预编排清单）。
//
// 纪律：
//   · 这是 SAD 唯一允许调用模型的接缝（理解 gate）；本地事件不经过这里。
//   · 输出必须通过 validate_task_spec；不合法回喂 issue 重试 ≤3，仍不过则 fail-closed。
//   · 没有为任何具体事件/书名/服务写特例；模板选择由模型在受限清单内决定。
#pragma once

#include "sad_mock.hpp"

#ifdef SAD_LLM_LIVE
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "support/dotenv.hpp"
#endif

namespace cppchain::sad {

#ifdef SAD_LLM_LIVE

namespace detail {

inline std::string extract_json_text(std::string const& text) {
  std::size_t const b = text.find('{');
  std::size_t const e = text.rfind('}');
  if (b == std::string::npos || e == std::string::npos || e < b) return {};
  return text.substr(b, e - b + 1);
}

}  // namespace detail

inline UnderstandFn make_live_understand() {
  auto const dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const api_key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const model_name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  std::string ca_bundle = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca_bundle.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const candidate = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(candidate)) ca_bundle = candidate;
    }
  }
  if (api_key.empty()) {
    return [](Event const&, std::string& error, int& attempts) -> std::optional<Json> {
      attempts = 0;
      error = "缺少 LLM_API_KEY（.env）";
      return std::nullopt;
    };
  }

  model::ModelConfig cfg;
  cfg.api_key = api_key;
  cfg.base_url = base_url;
  cfg.model = model_name;
  cfg.timeout_ms = 90000;
  cfg.ca_info = ca_bundle;
  auto transport = std::make_shared<http::CurlMultiTransport>();
  auto const model = std::make_shared<model::openai::OpenAIChatModel>(cfg, transport);

  std::string const sys =
      "你是 SystemAgent Daemon（SAD）的理解层。输入一个需要全局协调的系统事件，"
      "输出一个任务规格 JSON，只输出 JSON，不要解释、不要 Markdown。\n\n"
      "可用任务模板（template 必须严格来自清单）：\n" +
      task_template_catalog_prompt() +
      "\n任务规格 schema：\n"
      "{\"goal\":\"...\",\"template\":\"<模板名>\",\"scope\":[\"...\"],"
      "\"domains\":[\"...\"],\"priority\":\"P0|P1|P2|P3\","
      "\"budget\":{\"tokens\":20000,\"minutes\":15,\"wakeups\":3},"
      "\"acceptance\":[\"...\"],\"constraints\":[\"...\"],\"dedup_key\":\"...\"}\n"
      "规则：template 必须来自清单；dedup_key 稳定（同一事件重放应相同）；"
      "scope/acceptance 用字符串数组；只基于事件内容，不要编造未提供的资源。";

  return [model, sys](Event const& e, std::string& error,
                      int& attempts) -> std::optional<Json> {
    attempts = 0;
    Json const ev{{"id", e.id},
                  {"source", e.source},
                  {"type", e.type},
                  {"severity", e.severity},
                  {"payload", e.payload}};
    std::vector<message::Message> msgs{message::system_message(sys),
                              message::human_message("全局事件：\n" + ev.dump(2))};
    std::string last_raw;
    std::vector<std::string> issues;
    for (int a = 0; a < 3; ++a) {
      ++attempts;
      model::CallOptions opt;
      opt.temperature = 0.2;
      opt.response_format = Json{{"type", "json_object"}};
      auto const resp = model->invoke(msgs, opt);
      if (!resp) {
        error = "模型调用失败：" + resp.error().message;
        return std::nullopt;
      }
      last_raw = resp->text();
      Json p = Json::parse(detail::extract_json_text(last_raw), nullptr,
                           /*allow_exceptions=*/false);
      if (!p.is_discarded()) {
        issues = validate_task_spec(p);
        if (issues.empty()) return p;
      } else {
        issues = {"输出不是合法 JSON"};
      }
      msgs.push_back(message::ai_message(last_raw));
      std::string feedback = "任务规格不合法：\n";
      for (std::string const& s : issues) feedback += "- " + s + "\n";
      feedback += "请只输出修正后的 JSON（不要解释、不要 Markdown）。";
      msgs.push_back(message::human_message(feedback));
    }
    error = "理解重试 3 次仍不合法";
    if (!issues.empty()) error += "：" + issues.front();
    return std::nullopt;
  };
}

#else

inline UnderstandFn make_live_understand() {
  return [](Event const&, std::string& error, int& attempts) -> std::optional<Json> {
    attempts = 0;
    error = "本构建未启用 HTTP/模型（无法做 SAD 理解）";
    return std::nullopt;
  };
}

#endif

}  // namespace cppchain::sad
