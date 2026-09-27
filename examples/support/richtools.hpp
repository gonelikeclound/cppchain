// cppchain —— examples/support/richtools.hpp
// 丰富 fake 工具库 + tool_search 能力检索工具（仅 demo/生成端用，不进库）。
//   · add_rich_tools：注册一组分组的 fake 工具（system_settings / sms / email / reminder），
//     确定性罐头行为，供 IR 生成目录选用（"只用于生成端演示"，不接真实设备服务）。
//   · add_tool_search：注册 tool_search 工具——按 groups(1–3) + keywords(1–3) 检索整库
//     （含基础 fake 工具），返回 {name, group, description, params} 命中清单。
//      约束：groups/keywords 各 ≤3（"不能太多了"）；两者全空 → 报错。
//   · make_rich_registry：一键构建 基础10工具 + 丰富分组工具 + tool_search 的整库注册表。
//
// 分组映射留在 demo 侧（工具 spec 不带 group 字段，避免动库）；tool_search 的 handler
// 捕获注册表的 shared_ptr，避免"构建期取裸地址、返回后悬垂"的坑。
#pragma once

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"

#include "support/fakes.hpp"  // FakeToolSinks / make_fake_tools（baseline 目录）

namespace demo::richtools {

// richtools 不嵌套在 cppchain 内，用 using-decl 显式引入工具库/基础 fake 名（同一实体，不歧义）
using cppchain::tool::make_tool;
using cppchain::tool::SchemaBuilder;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolError;
using cppchain::tool::ToolRegistry;
using cppchain::tool::ToolSpec;
using demo::fakes::FakeToolSinks;   // make_rich_registry 的 baseline 工厂
using demo::fakes::make_fake_tools;

// 收件箱/发送记录（可注入/断言，同 FakeToolSinks 的思路）
struct RichSinks {
  std::vector<std::string> sms_sent;    // sms_send / sms_reply 的发送记录
  std::vector<std::string> emails_sent; // email_send / email_notify 的发送记录
};

namespace detail {

inline std::expected<nlohmann::json,  ToolError> t_ok(nlohmann::json v) {
  return std::expected<nlohmann::json,  ToolError>{std::move(v)};
}
inline std::expected<nlohmann::json,  ToolError> t_fail(std::string m) {
  return std::unexpected( ToolError{std::move(m)});
}

// ASCII 小写（关键词命中不区分大小写）
inline std::string to_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

}  // namespace detail

// 工具名 → 分组（tool_search 的 groups 过滤维度）
inline std::unordered_map<std::string, std::string> const& groups() {
  static std::unordered_map<std::string, std::string> const g = {
      {"settings_get", "system_settings"},  {"settings_set", "system_settings"},
      {"device_status", "system_settings"},
      {"sms_send", "sms"},                  {"sms_inbox", "sms"},
      {"sms_reply", "sms"},
      {"email_send", "email"},              {"email_inbox", "email"},
      {"email_notify", "email"},
      {"calendar_query", "reminder"},       {"todo_add", "reminder"},
  };
  return g;
}

// 注册 10 个分组的丰富 fake 工具（system_settings ×3 / sms ×3 / email ×3 / reminder ×2）。
inline void add_rich_tools( ToolRegistry& reg, RichSinks& s) {
  using Json = nlohmann::json;
  using Builder =  SchemaBuilder;

  // ---- system_settings ----
  reg.add( make_tool(
      "settings_get", "读取某个系统设置项（只读，如勿扰模式/音量/亮度）",
      Builder().prop("key", "string", "设置项名", true).build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        std::string const key = args.value("key", "");
        return detail::t_ok(Json{{"key", key}, {"value", "auto"}});
      },
      /*read_only=*/true));
  reg.add( make_tool(
      "settings_set", "修改某个系统设置项（有副作用；离线 stub 只记不改）",
      Builder().prop("key", "string", "设置项名", true)
          .prop("value", "string", "新值", true)
          .build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        return detail::t_ok(Json{{"applied", true},
                                 {"key", args.value("key", "")},
                                 {"value", args.value("value", "")}});
      },
      /*read_only=*/false));
  reg.add( make_tool(
      "device_status", "当前设备状态总览（电量/存储/系统，只读）",
      Builder().build(),
      [](Json const&,  ToolContext&) -> std::expected<Json,  ToolError> {
        return detail::t_ok(Json{{"battery_pct", 78}, {"storage_free_gb", 42},
                                 {"os", "cppchain Linux C++26"}});
      },
      /*read_only=*/true));

  // ---- sms ----
  reg.add( make_tool(
      "sms_send", "向指定手机号发送短信（有副作用；stub 写入 sms_sent 便于断言）",
      Builder().prop("to", "string", "手机号", true).prop("text", "string", "短信内容", true).build(),
      [&s](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        std::string const to = args.value("to", "");
        std::string const text = args.value("text", "");
        s.sms_sent.push_back(to + "：" + text);
        return detail::t_ok(Json{{"sent", true}, {"to", to}, {"chars", text.size()}});
      },
      /*read_only=*/false));
  reg.add( make_tool(
      "sms_inbox", "查最近收到的短信（只读；stub 返回罐头收件箱）",
      Builder().prop("limit", "integer", "取最近 N 条（默认 5）", false).build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        int const limit = args.value("limit", 5);
        Json box = Json::array({Json{{"from", "+86-138-0000-0001"},
                                     {"text", "您的验证码是 4832"},
                                     {"time", "09:12"}},
                                Json{{"from", "+86-138-0000-0002"},
                                     {"text", "本周三 14:00 会议提醒"},
                                     {"time", "08:47"}}});
        std::size_t const take = std::min(box.size(), static_cast<std::size_t>(std::max(limit, 0)));
        Json sliced = Json::array();
        for (std::size_t i = 0; i < take; ++i) sliced.push_back(box[i]);
        return detail::t_ok(Json{{"count", sliced.size()}, {"messages", sliced}});
      },
      /*read_only=*/true));
  reg.add( make_tool(
      "sms_reply", "回复最近一条收到的短信（有副作用）",
      Builder().prop("text", "string", "回复内容", true).build(),
      [&s](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        std::string const text = args.value("text", "");
        s.sms_sent.push_back("(reply)：" + text);
        return detail::t_ok(Json{{"replied", true}, {"to", "最近发信人"}, {"text", text}});
      },
      /*read_only=*/false));

  // ---- email ----
  reg.add( make_tool(
      "email_send", "发送一封邮件（有副作用；stub 写入 emails_sent 便于断言）",
      Builder().prop("to", "string", "收件人", true)
          .prop("subject", "string", "主题", true)
          .prop("body", "string", "正文（可选）", false)
          .build(),
      [&s](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        std::string const to = args.value("to", "");
        std::string const sub = args.value("subject", "");
        s.emails_sent.push_back(sub + " → " + to);
        return detail::t_ok(Json{{"sent", true}, {"to", to}, {"subject", sub}});
      },
      /*read_only=*/false));
  reg.add( make_tool(
      "email_inbox", "查最近收到的邮件（只读；stub 返回罐头收件箱）",
      Builder().prop("limit", "integer", "取最近 N 封（默认 5）", false).build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        int const limit = args.value("limit", 5);
        Json box = Json::array({Json{{"from", "dev@example.com"},
                                     {"subject", "本周发布计划"},
                                     {"time", "2026-09-25 10:02"}},
                                Json{{"from", "hr@example.com"},
                                     {"subject", "入职材料补充"},
                                     {"time", "2026-09-24 16:31"}}});
        std::size_t const take = std::min(box.size(), static_cast<std::size_t>(std::max(limit, 0)));
        Json sliced = Json::array();
        for (std::size_t i = 0; i < take; ++i) sliced.push_back(box[i]);
        return detail::t_ok(Json{{"count", sliced.size()}, {"mails", sliced}});
      },
      /*read_only=*/true));
  reg.add( make_tool(
      "email_notify", "把未读邮件汇总后提醒用户（有副作用）",
      Builder().prop("summary_only", "boolean", "只发摘要（默认 true）", false).build(),
      [&s](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        bool const only = args.value("summary_only", true);
        s.emails_sent.push_back(std::string(only ? "[摘要] 2 封未读邮件" : "全文 2 封未读"));
        return detail::t_ok(Json{{"notified", true}, {"unread", 2}, {"summary_only", only}});
      },
      /*read_only=*/false));

  // ---- reminder ----
  reg.add( make_tool(
      "calendar_query", "查某天的日历安排（只读；stub 返回罐头日程）",
      Builder().prop("date", "string", "日期 YYYY-MM-DD 或今天/明天", true).build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        Json events = Json::array({Json{{"time", "09:30"}, {"title", "站会"}},
                                   Json{{"time", "14:00"}, {"title", "评审"}}});
        return detail::t_ok(Json{{"date", args.value("date", "")}, {"events", events}});
      },
      /*read_only=*/true));
  reg.add( make_tool(
      "todo_add", "往待办列表加一条（有副作用）",
      Builder().prop("text", "string", "待办内容", true)
          .prop("due", "string", "截止（可选）", false)
          .build(),
      [](Json const& args,  ToolContext&) -> std::expected<Json,  ToolError> {
        return detail::t_ok(Json{{"added", true}, {"text", args.value("text", "")},
                                 {"due", args.value("due", "")}});
      },
      /*read_only=*/false));
}

// 注册 tool_search 能力检索工具；handler 捕获 target（shared_ptr，保证存活不悬垂）。
// groups/keywords 各 ≤3、≥1（两者不能同时为空）；groups 取并集、keywords 需全部命中。
inline void add_tool_search( ToolRegistry& reg,
                            std::shared_ptr< ToolRegistry const> target) {
  using Json = nlohmann::json;
  auto const& gm = groups();

  reg.add( make_tool(
      "tool_search",
      "按分组和关键词检索本目录可用工具（发现在哪里、能做什么）。"
      "groups、keywords 各给 1–3 个，不要太多；两者不能同时为空。"
      "常用分组：system_settings / sms / email / reminder。",
      Json{{"type", "object"},
           {"properties",
            {{"groups",
              {{"type", "array"}, {"items", {{"type", "string"}}},
               {"minItems", 1}, {"maxItems", 3}}},
             {"keywords",
              {{"type", "array"}, {"items", {{"type", "string"}}},
               {"minItems", 1}, {"maxItems", 3}}}}},
           {"required", Json::array()}},
      [target, &gm](Json const& args,  ToolContext&)
          -> std::expected<Json,  ToolError> {
        Json const g_arr = args.value("groups", Json::array());
        Json const k_arr = args.value("keywords", Json::array());
        if (!g_arr.is_array() || !k_arr.is_array())
          return detail::t_fail("groups/keywords 必须是字符串数组");
        if (g_arr.size() > 3 || k_arr.size() > 3)
          return detail::t_fail("groups/keywords 各最多 3 个（不能太多了）");
        if (g_arr.empty() && k_arr.empty())
          return detail::t_fail("groups、keywords 至少给一组（各 1–3 个）");

        std::vector<std::string> gs;
        for (Json const& e : g_arr)
          if (e.is_string()) gs.push_back(e.get<std::string>());
        std::vector<std::string> ks;
        for (Json const& e : k_arr)
          if (e.is_string()) ks.push_back(detail::to_lower(e.get<std::string>()));

        Json hits = Json::array();
        for ( ToolSpec const& t : target->all()) {
          std::string const grp = [&] {
            auto it = gm.find(t.name);
            return it != gm.end() ? it->second : std::string{};
          }();
          if (!gs.empty()) {
            bool in_grp = false;
            for (std::string const& g : gs)
              if (grp == g) in_grp = true;
            if (!in_grp) continue;
          }
          if (!ks.empty()) {
            std::string const hay = detail::to_lower(t.name + " " + t.description + " " + grp);
            bool all_hit = true;
            for (std::string const& kw : ks)
              if (hay.find(kw) == std::string::npos) all_hit = false;
            if (!all_hit) continue;
          }
          std::vector<std::string> params;
          if (t.parameters.is_object() && t.parameters["properties"].is_object())
            for (auto it = t.parameters["properties"].begin();
                 it != t.parameters["properties"].end(); ++it)
              params.push_back(it.key());
          hits.push_back(Json{{"name", t.name},
                              {"group", grp},
                              {"description", t.description},
                              {"params", params}});
        }
        return detail::t_ok(Json{{"count", hits.size()}, {"hits", hits}});
      },
      /*read_only=*/true));
}

// 一键构建整库注册表：基础 10 工具 + 丰富分组工具 + tool_search。
// 返回 shared_ptr：tool_search 的 handler 持有它，注册表随它存活，无悬垂。
inline std::shared_ptr< ToolRegistry> make_rich_registry(FakeToolSinks& sinks,
                                                              RichSinks& rsinks) {
  auto reg = std::make_shared< ToolRegistry>(make_fake_tools(sinks));
  add_rich_tools(*reg, rsinks);
  add_tool_search(*reg, reg);
  return reg;
}

}  // namespace demo::richtools