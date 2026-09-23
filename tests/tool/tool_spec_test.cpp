// M3 测试：工具规格（schema/wire 转换、参数取值助手）
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cppchain/tool/tool.hpp"

using namespace cppchain;
using namespace cppchain::tool;

TEST(ToolSpec, to_openai_shape_matches_wire_format) {
  SchemaBuilder sb;
  sb.prop("city", "string", "城市名").prop("days", "integer", "天数", /*required=*/false);
  auto spec = make_tool("get_weather", "查询天气", sb.build(),
                        [](nlohmann::json const&, ToolContext&) {
                          return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                        });

  auto j = spec.to_openai();
  EXPECT_EQ(j.at("type").get<std::string>(), "function");
  EXPECT_EQ(j.at("function").at("name").get<std::string>(), "get_weather");
  EXPECT_EQ(j.at("function").at("description").get<std::string>(), "查询天气");
  auto const& params = j.at("function").at("parameters");
  EXPECT_EQ(params.at("type").get<std::string>(), "object");
  EXPECT_EQ(params.at("properties").at("city").at("type").get<std::string>(), "string");
  ASSERT_EQ(params.at("required").size(), 1u);
  EXPECT_EQ(params.at("required")[0].get<std::string>(), "city");
}

TEST(ToolArg, reads_value_and_reports_missing) {
  nlohmann::json const args = {{"city", "SF"}, {"days", 3}};

  auto city = arg<std::string>(args, "city");
  ASSERT_TRUE(city.has_value());
  EXPECT_EQ(*city, "SF");

  auto days = arg<int>(args, "days");
  ASSERT_TRUE(days.has_value());
  EXPECT_EQ(*days, 3);

  auto missing = arg<std::string>(args, "unit");
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().message.find("unit"), std::string::npos);

  // 非必需缺失 → 默认值
  auto optional_missing = arg<std::string>(args, "unit", /*required=*/false);
  ASSERT_TRUE(optional_missing.has_value());
  EXPECT_TRUE(optional_missing->empty());
}

TEST(ToolArg, type_mismatch_is_reported) {
  nlohmann::json const args = {{"days", "not-a-number"}};
  auto days = arg<int>(args, "days");
  ASSERT_FALSE(days.has_value());
  EXPECT_NE(days.error().message.find("days"), std::string::npos);
  EXPECT_NE(days.error().message.find("invalid"), std::string::npos);
}

TEST(SchemaBuilder, no_required_array_when_all_optional) {
  SchemaBuilder sb;
  sb.prop("a", "string", "aa", false);
  auto schema = sb.build();
  EXPECT_FALSE(schema.contains("required"));
  EXPECT_EQ(schema.at("properties").size(), 1u);
}

TEST(MakeTool, defaults_to_read_only) {
  auto spec = make_tool("noop", "does nothing", nlohmann::json::object(),
                        [](nlohmann::json const&, ToolContext&) {
                          return std::expected<nlohmann::json, ToolError>{nlohmann::json("x")};
                        });
  EXPECT_TRUE(spec.read_only);
  auto writer = make_tool("write_file", "writes", nlohmann::json::object(),
                          [](nlohmann::json const&, ToolContext&) {
                            return std::expected<nlohmann::json, ToolError>{nlohmann::json("x")};
                          },
                          /*read_only=*/false);
  EXPECT_FALSE(writer.read_only);
}