// M3 测试：真实天气工具（Open-Meteo，通过 FakeTransport 离线跑）
// 只给脚本化响应，验证：请求形状 / 消歧 / 错误路径 / WMO 代码映射。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/tool/weather.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

ToolContext make_ctx() {
  ToolContext ctx;
  ctx.tool_call_id = "call_test";
  return ctx;
}

auto invoke_handler(ToolSpec const& spec, json const& args) {
  ToolContext ctx = make_ctx();
  return spec.handler(args, ctx);
}

// 构造 FakeTransport 脚本：一次 200 响应，正文 = body 的 JSON 序列化
http::FakeTransport::Script script(json const& body) {
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {body.dump()};
  return s;
}

json geocode_response(std::initializer_list<json> rows) {
  json b = json::object();
  b["results"] = json::array();
  for (auto const& r : rows) b["results"].push_back(r);
  return b;
}

}  // namespace

TEST(WeatherCodeName, known_codes_map_to_chinese) {
  EXPECT_EQ(weather_code_name(0), "晴");
  EXPECT_EQ(weather_code_name(1), "基本晴");
  EXPECT_EQ(weather_code_name(2), "局部多云");
  EXPECT_EQ(weather_code_name(61), "小雨");
  EXPECT_EQ(weather_code_name(95), "雷暴");
  EXPECT_EQ(weather_code_name(999), "未知");
}

TEST(WeatherTool, geocode_then_forecast_happy_path) {
  auto tp = std::make_shared<http::FakeTransport>();
  // ① 地名解析（zh 命中北京市，PPLC 居民点）
  tp->push(script(geocode_response({{
      {"name", "北京市"}, {"latitude", 39.9042}, {"longitude", 116.4074},
      {"country", "中国"}, {"country_code", "CN"}, {"admin1", "北京市"},
      {"feature_code", "PPLC"}, {"population", 21540000},
  }})));
  // ② 天气预报（含 current + 2 天 daily）
  json forecast = {
      {"latitude", 39.9},
      {"longitude", 116.4},
      {"timezone", "Asia/Shanghai"},
      {"current",
       {{"time", "2026-09-24T09:00"},
        {"temperature_2m", 24.0},
        {"apparent_temperature", 23.1},
        {"relative_humidity_2m", 40},
        {"wind_speed_10m", 11.0},
        {"wind_direction_10m", 45},
        {"precipitation", 0.0},
        {"weather_code", 1}}},
      {"daily",
       {{"time", {"2026-09-24", "2026-09-25"}},
        {"weather_code", {1, 2}},
        {"temperature_2m_max", {27.0, 28.0}},
        {"temperature_2m_min", {18.0, 19.0}}}}};
  tp->push(script(forecast));

  auto spec = make_weather_tool(tp);
  auto r = invoke_handler(spec, json{{"city", "北京"}, {"days", 2}});
  ASSERT_TRUE(r.has_value()) << r.error().message;

  auto const& v = *r;
  EXPECT_EQ(v.at("location").at("country_code").get<std::string>(), "CN");
  EXPECT_DOUBLE_EQ(v.at("location").at("latitude").get<double>(), 39.9042);
  EXPECT_DOUBLE_EQ(v.at("current").at("temperature_c").get<double>(), 24.0);
  EXPECT_EQ(v.at("current").at("weather").get<std::string>(), "基本晴");
  EXPECT_TRUE(v.at("current").at("summary").get<std::string>().find("北京市") !=
              std::string::npos);
  ASSERT_EQ(v.at("daily").size(), 2u);
  EXPECT_DOUBLE_EQ(v.at("daily")[0].at("tmax_c").get<double>(), 27.0);

  // 恰好 2 次请求：先 geocode，后 forecast；URL 与 GET 方法正确
  auto const& reqs = tp->requests();
  ASSERT_EQ(reqs.size(), 2u);
  EXPECT_EQ(reqs[0].method, "GET");
  EXPECT_EQ(reqs[1].method, "GET");
  EXPECT_NE(reqs[0].url.find("geocoding-api.open-meteo.com/v1/search"), std::string::npos);
  EXPECT_NE(reqs[0].url.find("name=%E5%8C%97%E4%BA%AC"), std::string::npos);  // 北京 URL 编码
  EXPECT_NE(reqs[1].url.find("api.open-meteo.com/v1/forecast"), std::string::npos);
  EXPECT_NE(reqs[1].url.find("latitude=39.9"), std::string::npos);
  EXPECT_NE(reqs[1].url.find("forecast_days=2"), std::string::npos);
}

TEST(WeatherTool, unknown_place_returns_actionable_error) {
  auto tp = std::make_shared<http::FakeTransport>();
  tp->push(script(geocode_response({})));  // zh 无结果
  tp->push(script(geocode_response({})));  // en 无结果（"zzz" 无后缀，不再剥）

  auto spec = make_weather_tool(tp);
  auto r = invoke_handler(spec, json{{"city", "zzz"}});
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("zzz"), std::string::npos);
  EXPECT_NE(r.error().message.find("找不到城市"), std::string::npos);
  EXPECT_EQ(tp->requests().size(), 2u);
}

TEST(WeatherTool, country_filter_disambiguates) {
  auto tp = std::make_shared<http::FakeTransport>();
  // zh 里同时出现两个居民点：北京市(CN, pop 21.5M) 与 Springfield(US, pop 1K)
  tp->push(script(geocode_response({
      {{"name", "北京市"}, {"latitude", 39.9}, {"longitude", 116.4},
       {"country", "中国"}, {"country_code", "CN"}, {"feature_code", "PPLC"},
       {"population", 21540000}},
      {{"name", "Springfield"}, {"latitude", 39.8}, {"longitude", -89.6},
       {"country", "美国"}, {"country_code", "US"}, {"feature_code", "PPL"},
       {"population", 1000}},
  })));
  // ② 预报：极小响应即可（此用例只断言 location）
  tp->push(script(json{{"current", json::object()},
                       {"daily",
                        {{"time", json::array()}, {"temperature_2m_max", json::array()},
                         {"temperature_2m_min", json::array()}, {"weather_code", json::array()}}}}));

  auto spec = make_weather_tool(tp);
  auto r = invoke_handler(spec, json{{"city", "springfield"}, {"country", "US"}});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("location").at("country_code").get<std::string>(), "US");
  EXPECT_EQ(r->at("location").at("name").get<std::string>(), "Springfield");
}

TEST(WeatherTool, http_error_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 503;  // geocode 服务不可用
  tp->push(std::move(s));
  auto spec = make_weather_tool(tp);
  auto r = invoke_handler(spec, json{{"city", "北京"}});
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("HTTP 503"), std::string::npos);
  EXPECT_EQ(tp->requests().size(), 1u);  // 传输/HTTP 错误直接上抛，不再盲目换语言
}

TEST(WeatherTool, network_error_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.error = "connection refused";
  tp->push(std::move(s));
  auto spec = make_weather_tool(tp);
  auto r = invoke_handler(spec, json{{"city", "北京"}});
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("网络错误"), std::string::npos);
}