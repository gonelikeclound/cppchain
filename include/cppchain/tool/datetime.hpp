// cppchain —— tool/datetime.hpp
// 当前时间工具（get_datetime）：模型不知道"现在几点了 / 今天几号 / 星期几"，
// 联网答时间类问题、判断日期、给答案附带时间依据时都需要它。
// 纯本地、零网络零密钥；时区用固定偏移（±HH:MM），不依赖 tzdb 的 IANA 名称。
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct DatetimeToolConfig {
  // 默认时区偏移。空 = UTC（Agent 内部统一用 UTC 最不易错，模型可传 offset 自行换算）。
  // 格式："+08:00" / "-05:00" / "UTC"。
  std::string default_offset;
  // 可注入时钟（离线确定性测试用）；默认系统时钟。
  std::function<std::chrono::system_clock::time_point()> clock =
      [] { return std::chrono::system_clock::now(); };
  // "现在"快照的时效性：60 秒后即视为可能已过期
  //（由 agent::StaleStateGuard 消费，见 04-problems/06）。
  std::chrono::seconds ttl = std::chrono::seconds(60);
};

// 纯函数：时间点 → 结构化快照（供离线单测直接调用）。
// offset："" / "UTC" / "+08:00" 等；无效 offset 抛 std::invalid_argument。
[[nodiscard]] nlohmann::json format_datetime(std::chrono::system_clock::time_point tp,
                                             std::string_view offset);

// 生成 get_datetime 工具。
// 参数：offset（可选，±HH:MM 或 "UTC"；缺省用配置里的 default_offset）。返回：
//   {datetime, iso8601, date, time, weekday_en, weekday_cn, offset, utc_now, source}
[[nodiscard]] ToolSpec make_datetime_tool(DatetimeToolConfig cfg = {});

}  // namespace cppchain::tool