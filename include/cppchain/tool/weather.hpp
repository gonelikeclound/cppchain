// cppchain —— tool/weather.hpp
// 真实天气查询工具：地名 → 经纬度（geocoding）→ 实时天气 + 逐日预报（forecast）。
// 数据源 Open-Meteo（免费、无需密钥），本工具不做硬编码假设，逐次实时拉取。
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// Open-Meteo 天气工具配置（默认值指向官方公开端点）
struct WeatherToolConfig {
  std::string geocode_base = "https://geocoding-api.open-meteo.com";  // 地名→经纬度
  std::string forecast_base = "https://api.open-meteo.com";           // 经纬度→天气
  std::string ca_info;     // 企业代理/自签环境的自定义 CA bundle
  long timeout_ms = 20000;
  // 天气快照是"当下"的易变数据：30 分钟后即视为可能过期
  //（由 agent::StaleStateGuard 消费，见 04-problems/06 状态时效性）。
  std::chrono::seconds ttl = std::chrono::minutes(30);
};

// 生成 get_weather 工具。handler 返回结构化 JSON：
//   {summary, location, current, daily[], source}
// 参数：city（必填，中文/英文均可）、country（可选 ISO 3166-1 alpha-2，消歧用）、
//       days（可选 1–7，默认 1）。
// 地名找不到 / 网络失败 → unexpected(ToolError)，让模型自我修正或如实告知用户。
[[nodiscard]] ToolSpec make_weather_tool(std::shared_ptr<http::Transport> transport,
                                         WeatherToolConfig cfg = {});

// WMO 天气代码（weather_code）→ 简短中文描述（单独暴露便于复用与测试）
[[nodiscard]] std::string_view weather_code_name(int code);

}  // namespace cppchain::tool