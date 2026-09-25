// M3 测试：get_datetime 当前时间工具（可注入固定时钟 → 完全确定性）
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "cppchain/tool/datetime.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;
namespace chr = std::chrono;
using namespace std::chrono_literals;

namespace {

ToolContext make_ctx() {
  ToolContext c;
  c.tool_call_id = "call_clock";
  return c;
}

ToolSpec make_clock_tool(chr::system_clock::time_point tp) {
  DatetimeToolConfig cfg;
  cfg.clock = [tp] { return tp; };
  return make_datetime_tool(std::move(cfg));
}

// 直接构造一个 UTC 时间点（测试专用；参数名避免遮蔽 chrono 类型）
chr::system_clock::time_point tp_utc(int y, int mo, int d, int hh = 0, int mi = 0, int ss = 0) {
  using namespace std::chrono;
  auto const t = chr::sys_days{year{y} / mo / d} + hours{hh} + minutes{mi} + seconds{ss};
  return chr::system_clock::time_point{chr::floor<chr::seconds>(t)};
}

}  // namespace

TEST(DateTime, epoch_is_1970_01_01_thursday_utc) {
  auto j = format_datetime(chr::system_clock::time_point{}, "UTC");
  EXPECT_EQ(j.at("datetime").get<std::string>(), "1970-01-01 00:00:00");
  EXPECT_EQ(j.at("date").get<std::string>(), "1970-01-01");
  EXPECT_EQ(j.at("time").get<std::string>(), "00:00:00");
  EXPECT_EQ(j.at("weekday_en").get<std::string>(), "Thursday");
  EXPECT_EQ(j.at("weekday_cn").get<std::string>(), "星期四");
  EXPECT_EQ(j.at("offset").get<std::string>(), "Z");
  EXPECT_EQ(j.at("iso8601").get<std::string>(), "1970-01-01T00:00:00Z");
}

TEST(DateTime, offset_shifts_clock_forward) {
  auto j = format_datetime(chr::system_clock::time_point{}, "+08:00");
  EXPECT_EQ(j.at("date").get<std::string>(), "1970-01-01");
  EXPECT_EQ(j.at("time").get<std::string>(), "08:00:00");
  EXPECT_EQ(j.at("offset").get<std::string>(), "+08:00");
  EXPECT_EQ(j.at("iso8601").get<std::string>(), "1970-01-01T08:00:00+08:00");
  EXPECT_EQ(j.at("utc_now").get<std::string>(), "1970-01-01T00:00:00Z");  // utc_now 恒为 UTC 参照
}

TEST(DateTime, negative_offset_rolls_back_to_previous_day) {
  auto j = format_datetime(chr::system_clock::time_point{}, "-05:00");
  EXPECT_EQ(j.at("date").get<std::string>(), "1969-12-31");
  EXPECT_EQ(j.at("time").get<std::string>(), "19:00:00");
  EXPECT_EQ(j.at("weekday_en").get<std::string>(), "Wednesday");  // 1969-12-31 是周三
}

TEST(DateTime, known_date_weekday) {
  // 2026-01-01 = 星期四（2025 元旦是周三，平年 +365 天）
  auto j = format_datetime(tp_utc(2026, 1, 1), "UTC");
  EXPECT_EQ(j.at("date").get<std::string>(), "2026-01-01");
  EXPECT_EQ(j.at("weekday_en").get<std::string>(), "Thursday");
  EXPECT_EQ(j.at("weekday_cn").get<std::string>(), "星期四");
}

TEST(DateTime, invalid_offset_throws) {
  auto const epoch = chr::system_clock::time_point{};
  // (void) 抑制 [[nodiscard]] · EXPECT_THROW 包装语句的 -Wunused-result
  EXPECT_THROW((void)format_datetime(epoch, "+8:00"), std::invalid_argument);  // 格式错
  EXPECT_THROW((void)format_datetime(epoch, "ABC"), std::invalid_argument);
  EXPECT_THROW((void)format_datetime(epoch, "+25:00"), std::invalid_argument);  // 小时越界
  EXPECT_THROW((void)format_datetime(epoch, "+08;00"), std::invalid_argument);
}

TEST(DateTime, empty_offset_means_utc) {
  auto j = format_datetime(chr::system_clock::time_point{}, "");
  EXPECT_EQ(j.at("offset").get<std::string>(), "Z");
  EXPECT_EQ(j.at("datetime").get<std::string>(), "1970-01-01 00:00:00");
}

TEST(DateTime, handler_uses_injected_clock) {
  auto spec = make_clock_tool(tp_utc(2026, 9, 25));
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("date").get<std::string>(), "2026-09-25");
  EXPECT_EQ(r->at("source").get<std::string>(), "system clock");
}

TEST(DateTime, handler_applies_offset_arg) {
  auto spec = make_clock_tool(tp_utc(2026, 9, 25));
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"offset", "+08:00"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("datetime").get<std::string>(), "2026-09-25 08:00:00");
  EXPECT_EQ(r->at("weekday_cn").get<std::string>(), "星期五");  // 2026-09-25 是周五
}

TEST(DateTime, invalid_offset_in_handler_is_fed_back) {
  auto spec = make_clock_tool(chr::system_clock::time_point{});
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"offset", "beijing"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("offset"), std::string::npos);
}