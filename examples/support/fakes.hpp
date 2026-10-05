// cppchain —— examples/support/fakes.hpp
// 假工具工厂（D4）——仅 demo/评测用，不进库；workflow 测试与 wf_eval 验收共用。
// 与 features.hpp 同属"workflow 评测设施"：demo 命名空间、纯 header。
//
// 设计：所有 handler 都从 FakeToolSinks 取行为（按引用捕获）——
//   · send_notification → 追加到 sinks.notifications（可断言"发了几次"）
//   · fetch_price       → sinks.price_lookup(item_id)；未设 → 固定默认价 799
//   · web_search        → sinks.web_search_fn(query)；未设 → 含 query 的罐头 html
//   · llm_extract / llm → sinks.chat（注入 ChatModel）或 sinks.llm_fn（prompt → text）
//   · http_get          → sinks.http_get_fn(url)；未设 → (200, "fake body for <url>")
//   · sum / format / echo / read → 纯本机确定性实现（read 不碰文件系统）
//
// OS 级工具（query_content / search_local / setting_get / setting_set）**故意不注册**
// —— validator 按注册表存在性自动拒绝它们（白名单 = 本工厂 + 已有 cppchain 工具）。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/ir.hpp"

namespace demo::fakes {

// 导入库名（using-decl 指向库内*同一实体*，consumer 同时 using 两个命名空间也不歧义）
using cppchain::message::Message;
using cppchain::tool::arg;
using cppchain::tool::make_tool;
using cppchain::tool::SchemaBuilder;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolError;
using cppchain::tool::ToolRegistry;
using cppchain::workflow::Json;

// 目录工具名常量（生成器提示词/测试断言直接引用，避免魔数字符串）
inline constexpr std::string_view kToolFetchPrice = "fetch_price";
inline constexpr std::string_view kToolWebSearch = "web_search";
inline constexpr std::string_view kToolLlmExtract = "llm_extract";
inline constexpr std::string_view kToolSendNotification = "send_notification";
inline constexpr std::string_view kToolHttpGet = "http_get";
inline constexpr std::string_view kToolLlm = "llm";
inline constexpr std::string_view kToolSum = "sum";
inline constexpr std::string_view kToolFormat = "format";
inline constexpr std::string_view kToolEcho = "echo";
inline constexpr std::string_view kToolRead = "read";

// 可注入的行为源。生命周期必须长于用它构造的注册表（handlers 按引用捕获）。
struct FakeToolSinks {
  std::vector<std::string> notifications;                              // send_notification 收件箱
  std::function<Json(std::string const&)> price_lookup;                // item_id → {price,in_stock,currency}
  std::function<Json(std::string const&)> web_search_fn;               // query → {html,...}
  cppchain::model::ChatModel* chat = nullptr;                          // llm_extract / llm 用
  std::function<Json(std::string const&)> llm_fn;                      // prompt → {text,...}（chat 为空时）
  std::function<std::pair<int, std::string>(std::string const&)>      // url → (status, body)
      http_get_fn;
};

namespace detail {

// ToolSpec::Handler 返回类型的便捷构造（命名函数，避免 lambda 捕获问题）
inline std::expected<Json, ToolError> t_ok(Json v) {
  return std::expected<Json, ToolError>{std::move(v)};
}
inline std::expected<Json, ToolError> t_fail(std::string m) {
  return std::unexpected(ToolError{std::move(m)});
}

}  // namespace detail

// 注册 §8 目录里 cppchain 能落地的 10 个假工具（fetch_price / web_search / llm_extract /
// send_notification / http_get / llm / sum / format / echo / read）。
inline ToolRegistry make_fake_tools(FakeToolSinks& sinks) {
  ToolRegistry reg;

  // send_notification —— 副作用工具（read_only=false）；追加到收件箱
  reg.add(make_tool(
      std::string(kToolSendNotification),
      "向用户发送一条通知（只能描述实际发生的事，不得编造）",
      SchemaBuilder().prop("title", "string", "通知标题", true)
          .prop("body", "string", "通知正文（可选）", false)
          .build(),
      [&notifs = sinks.notifications](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto title_arg = arg<std::string>(args, "title");
        if (!title_arg) return std::unexpected(title_arg.error());
        auto body_arg = arg<std::string>(args, "body", /*required=*/false);
        if (!body_arg) return std::unexpected(body_arg.error());
        std::string const& title = *title_arg;
        std::string const& body = *body_arg;
        notifs.push_back(body.empty() ? title : title + "：" + body);
        return detail::t_ok(Json{{"ok", true}});
      },
      /*read_only=*/false));

  // fetch_price —— 价格源；未注入 → 固定默认
  reg.add(make_tool(
      std::string(kToolFetchPrice),
      "查询某商品当前价格与库存（可定时轮询的价格源）",
      SchemaBuilder().prop("item_id", "string", "商品 id", true).build(),
      [&sinks](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto id_arg = arg<std::string>(args, "item_id");
        if (!id_arg) return std::unexpected(id_arg.error());
        std::string const& id = *id_arg;
        if (sinks.price_lookup) return detail::t_ok(sinks.price_lookup(id));
        return detail::t_ok(Json{{"price", 799}, {"in_stock", true}, {"currency", "CNY"}});
      }));

  // web_search —— 返回原始网页文档；未注入 → 罐头 html（含 query 便于断言）
  reg.add(make_tool(
      std::string(kToolWebSearch),
      "互联网关键词搜索，返回命中的原始网页文档列表（供 LLM 提炼）",
      SchemaBuilder().prop("query", "string", "搜索关键词", true).build(),
      [&sinks](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto q_arg = arg<std::string>(args, "query");
        if (!q_arg) return std::unexpected(q_arg.error());
        std::string const& q = *q_arg;
        if (sinks.web_search_fn) return detail::t_ok(sinks.web_search_fn(q));
        return detail::t_ok(Json{{"query", q},
                                 {"total", 1},
                                 {"documents",
                                  Json::array({Json{{"title", "stub for: " + q},
                                                    {"url", "https://example.invalid/" + q},
                                                    {"snippet", "离线 stub 文档：" + q}}})}});
      }));

  // llm_extract —— LLM 提取摘要；未注入模型 → 原样回文本（确定性）
  reg.add(make_tool(
      std::string(kToolLlmExtract),
      "让 LLM 从给定文本中按要求提取信息（摘要/关键点），返回 nl 字段",
      SchemaBuilder().prop("prompt", "string", "提取要求", true)
          .prop("text", "string", "待提取的原文", true)
          .build(),
      [&sinks](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto prompt_arg = arg<std::string>(args, "prompt");
        if (!prompt_arg) return std::unexpected(prompt_arg.error());
        auto text_arg = arg<std::string>(args, "text");
        if (!text_arg) return std::unexpected(text_arg.error());
        std::string const& prompt = *prompt_arg;
        std::string const& text = *text_arg;
        if (sinks.chat != nullptr) {
          auto r = sinks.chat->invoke(
              std::span<Message const>{
                  cppchain::message::system_message(
                      "你是提取器。只输出提取结果本身，不要解释。"),
                  cppchain::message::human_message("【要求】" + prompt + "\n【原文】" + text)},
              {});
          if (!r) return detail::t_fail("llm_extract: " + r.error().message);
          return detail::t_ok(Json{{"nl", r->text()}});
        }
        if (sinks.llm_fn) return detail::t_ok(sinks.llm_fn(text));
        return detail::t_ok(Json{{"nl", text}});  // 离线 stub：原样返回（确定性可测）
      }));

  // llm —— 通用文本生成；未注入模型 → "（stub）" + prompt 前若干字符
  reg.add(make_tool(
      std::string(kToolLlm),
      "通用 LLM 文本生成（自由回答）",
      SchemaBuilder().prop("prompt", "string", "提示词", true)
          .prop("max_tokens", "integer", "最大 token 数（可选）", false)
          .build(),
      [&sinks](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto prompt_arg = arg<std::string>(args, "prompt");
        if (!prompt_arg) return std::unexpected(prompt_arg.error());
        std::string const& prompt = *prompt_arg;
        if (sinks.chat != nullptr) {
          auto r = sinks.chat->invoke(
              std::span<Message const>{cppchain::message::human_message(prompt)}, {});
          if (!r) return detail::t_fail("llm: " + r.error().message);
          return detail::t_ok(Json{{"text", r->text()}});
        }
        if (sinks.llm_fn) return detail::t_ok(sinks.llm_fn(prompt));
        std::string head = prompt.size() > 40 ? prompt.substr(0, 40) : prompt;
        return detail::t_ok(Json{{"text", "（stub 回复）" + head}});
      }));

  // http_get —— url → {status, body}
  reg.add(make_tool(
      std::string(kToolHttpGet),
      "对任意 URL 发起 HTTP GET，返回状态码与响应体（可按 url 注入）",
      SchemaBuilder().prop("url", "string", "目标 URL", true)
          .prop("timeout_ms", "integer", "超时（可选）", false)
          .build(),
      [&sinks](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto url_arg = arg<std::string>(args, "url");
        if (!url_arg) return std::unexpected(url_arg.error());
        std::string const& url = *url_arg;
        if (sinks.http_get_fn) {
          auto [status, body] = sinks.http_get_fn(url);
          return detail::t_ok(Json{{"status", status}, {"body", body}});
        }
        return detail::t_ok(Json{{"status", 200},
                                 {"body", "fake body for " + url}});
      }));

  // sum / format / echo / read —— 纯本机确定性
  reg.add(make_tool(
      std::string(kToolSum),
      "两个数字求和",
      SchemaBuilder().prop("a", "number", "加数", true).prop("b", "number", "加数", true).build(),
      [](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto a_arg = arg<double>(args, "a");
        if (!a_arg) return std::unexpected(a_arg.error());
        auto b_arg = arg<double>(args, "b");
        if (!b_arg) return std::unexpected(b_arg.error());
        double const a = *a_arg;
        double const b = *b_arg;
        return detail::t_ok(Json{{"value", a + b}});
      }));

  reg.add(make_tool(
      std::string(kToolFormat),
      "把任意值格式化为字符串",
      SchemaBuilder().prop("v", "string", "待格式化值", true).build(),
      [](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        Json const& v = args.value("v", Json(nullptr));
        return detail::t_ok(Json{{"text", v.is_string() ? v.get<std::string>() : v.dump()}});
      }));

  reg.add(make_tool(
      std::string(kToolEcho),
      "原样返回输入（调试/打通链路用）",
      SchemaBuilder().prop("v", "string", "待回显值（可选）", false).build(),
      [](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        Json v = args.contains("v") ? args["v"] : Json("echo");
        return detail::t_ok(Json{{"value", v.is_string() ? v.get<std::string>() : v.dump()}});
      }));

  reg.add(make_tool(
      std::string(kToolRead),
      "读取一个本地文件并返回摘要（离线 stub：不碰文件系统）",
      SchemaBuilder().prop("path", "string", "文件路径", true).build(),
      [](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
        auto path_arg = arg<std::string>(args, "path");
        if (!path_arg) return std::unexpected(path_arg.error());
        std::string const& path = *path_arg;
        return detail::t_ok(Json{{"summary", "（离线 stub）" + path}});
      }));

  return reg;
}

}  // namespace demo::fakes