// cppchain —— model/options.hpp
// 调用参数与模型配置。未建模的参数一律走 extra 透传。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cppchain::model {

struct CallOptions {
  std::optional<std::string> model;        // 覆盖 ModelConfig.model
  std::optional<double> temperature;
  std::optional<int> max_tokens;
  std::vector<std::string> stop;
  // "auto"/"none"/"required"（字符串），或强制某函数：{"type":"function","function":{"name":…}}
  nlohmann::json tool_choice = nullptr;
  // 结构化输出用（如 {"type":"json_object"}）；非 null 时并入请求体
  nlohmann::json response_format = nullptr;
  nlohmann::json tools = nlohmann::json::array();  // M2 仅 wire 通道（M3 引入 Tool 抽象）
  nlohmann::json extra = nlohmann::json::object(); // 原样并入请求体（厂商私有参数）
};

struct ModelConfig {
  std::string api_key;
  std::string base_url = "https://api.openai.com/v1";
  std::string model;                       // 如 "deepseek-chat" / "gpt-4o-mini"
  long timeout_ms = 60000;
  std::vector<std::string> extra_headers;  // 形如 "X-Foo: bar"
  // 企业代理/自签证书环境的 CA bundle 路径（留空则用系统默认 CA）
  std::string ca_info;
};

}  // namespace cppchain::model