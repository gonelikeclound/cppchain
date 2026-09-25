// cppchain —— src/tool/datetime.cpp
#include "cppchain/tool/datetime.hpp"

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cppchain::tool {
namespace {

using namespace std::chrono;

// UTC 偏移文本 → 秒数。"" / "UTC" / "Z" → 0；"+HH:MM" / "-HH:MM" → 秒。
// 无效格式抛 std::invalid_argument（给模型可读的纠正提示）。
long parse_offset_seconds(std::string_view off) {
  if (off.empty() || off == "UTC" || off == "Z" || off == "utc") return 0;
  if (off.size() != 6 || (off[0] != '+' && off[0] != '-') || off[3] != ':')
    throw std::invalid_argument(
        "无效的 offset：「" + std::string(off) + "」。应为 ±HH:MM（如 +08:00、-05:00）或 UTC。");
  auto const digit = [&](std::size_t i) { return off[i] >= '0' && off[i] <= '9'; };
  if (!digit(1) || !digit(2) || !digit(4) || !digit(5))
    throw std::invalid_argument(
        "无效的 offset：「" + std::string(off) + "」。应为 ±HH:MM（如 +08:00、-05:00）。");
  int const hh = (off[1] - '0') * 10 + (off[2] - '0');
  int const mm = (off[4] - '0') * 10 + (off[5] - '0');
  if (hh > 23 || mm > 59)
    throw std::invalid_argument("无效的 offset：「" + std::string(off) + "」。小时/分钟越界。");
  long const secs = static_cast<long>(hh) * 3600 + static_cast<long>(mm) * 60;
  return off[0] == '-' ? -secs : secs;
}

// 已应用 offset 的年月日时分秒 + 星期（0 = 星期日）。纯标准 chrono，线程安全。
struct BrokenDown {
  int year, month, day;
  int hour, minute, second;
  int weekday;
};

BrokenDown break_down(sys_seconds tp, long offset_secs) {
  auto const t = floor<seconds>(tp) + seconds{offset_secs};
  sys_days const sys{floor<days>(t)};
  year_month_day const ymd{sys};
  hh_mm_ss const hms{t - sys};
  // 星期：1970-01-01 是周四（周日=0 计 4），days 序数对 7 取模平移得到
  auto const days_since_epoch = sys.time_since_epoch().count();
  int const weekday =
      (static_cast<int>(((days_since_epoch % 7) + 7) % 7) + 4) % 7;  // 0=Sunday
  return {static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
          static_cast<int>(static_cast<unsigned>(ymd.day())),
          static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
          static_cast<int>(hms.seconds().count()), weekday};
}

constexpr std::string_view kWeekdayEn[] = {"Sunday",     "Monday", "Tuesday", "Wednesday",
                                          "Thursday",   "Friday", "Saturday"};
constexpr std::string_view kWeekdayCn[] = {"星期日", "星期一", "星期二", "星期三",
                                           "星期四", "星期五", "星期六"};

std::string iso(std::string_view off_text) { return (off_text == "Z") ? "Z" : std::string(off_text); }

}  // namespace

nlohmann::json format_datetime(system_clock::time_point tp, std::string_view offset) {
  bool const utc = offset.empty() || offset == "UTC" || offset == "Z" || offset == "utc";
  long const off = parse_offset_seconds(offset);
  auto const tp_sec = floor<seconds>(tp);
  auto const b = break_down(tp_sec, off);
  auto const bu = break_down(tp_sec, 0);  // utc_now 参照
  std::string const off_text = utc ? "Z" : std::string(offset);
  return nlohmann::json{
      {"datetime", std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", b.year, b.month,
                               b.day, b.hour, b.minute, b.second)},
      {"iso8601", std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}{}", b.year, b.month,
                              b.day, b.hour, b.minute, b.second, iso(off_text))},
      {"date", std::format("{:04d}-{:02d}-{:02d}", b.year, b.month, b.day)},
      {"time", std::format("{:02d}:{:02d}:{:02d}", b.hour, b.minute, b.second)},
      {"weekday_en", std::string(kWeekdayEn[b.weekday])},
      {"weekday_cn", std::string(kWeekdayCn[b.weekday])},
      {"offset", off_text},
      {"utc_now", std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", bu.year, bu.month,
                              bu.day, bu.hour, bu.minute, bu.second)},
      {"source", "system clock"},
  };
}

ToolSpec make_datetime_tool(DatetimeToolConfig cfg) {
  ToolSpec spec = make_tool(
      "get_datetime",
      "获取当前日期与时间。模型自身不知道「现在几点/今天几号/星期几」，"
      "涉及时间日期问题、或需要给答案附带时间依据时使用。默认返回 UTC，"
      "如需特定时区请传 offset。返回结构化 JSON："
      "{datetime, iso8601, date, time, weekday_en, weekday_cn, offset, utc_now}。",
      SchemaBuilder()
          .prop("offset", "string",
                "UTC 时区偏移：+08:00（北京时间）、-05:00（美东）等；传 UTC 或省略表示标准时间",
                /*required=*/false)
          .build(),
      [cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        std::string offset = cfg.default_offset;
        if (auto o = arg<std::string>(args, "offset", /*required=*/false); o) offset = *o;
        try {
          return format_datetime(cfg.clock(), offset);
        } catch (std::invalid_argument const& e) {
          return std::unexpected(ToolError{e.what()});
        }
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // "现在"快照 60 秒后即视为可能过期
  return spec;
}

}  // namespace cppchain::tool