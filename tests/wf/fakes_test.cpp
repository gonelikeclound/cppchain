// tests/wf —— demo 假工具的参数取值契约
//
// 背景：fakes.hpp 的假工具原先一律用 `args.value(key, default)` 取参。nlohmann 的 value()
// 只在**键缺失**时用默认值；键存在但值为 null、或类型不符时会**抛异常**。异常穿透 workflow
// 的 tool 节点直接把节点炸掉（真机表现：`node 'extract' threw: type must be string,
// but is null / but is array`），既不走 on_error 重试，也拿不到可读原因。
//
// 真实工具（src/tool/*.cpp）一律用 `tool::arg<T>`：缺 / null / 类型错都返回 ToolError。
// 假工具是唯一的异类。本测试锁住这条纪律 —— 假工具也是评测矩阵的运行底座。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/tool/registry.hpp"
#include "support/fakes.hpp"

namespace tool = cppchain::tool;
using cppchain::workflow::Json;
using namespace demo::fakes;

namespace {

using R = std::expected<Json, tool::ToolError>;

// 取参数 + 断言"不抛异常且返回 ToolError，且错误里点名了出问题的键"
void expect_toolerror_not_throw(tool::ToolRegistry const& reg, std::string const& tool_name,
                                Json const& args, std::string const& key_word) {
  auto const* spec = reg.find(tool_name);
  ASSERT_NE(spec, nullptr) << tool_name;
  ASSERT_TRUE(spec->handler);

  tool::ToolContext ctx;
  R r = Json(nullptr);
  EXPECT_NO_THROW(r = spec->handler(args, ctx)) << tool_name << " 抛异常了（应为 ToolError）";
  ASSERT_FALSE(r.has_value()) << tool_name;
  EXPECT_NE(r.error().message.find(key_word), std::string::npos)
      << tool_name << " 的错误未点名 '" << key_word << "'：" << r.error().message;
}

}  // namespace

// 引用解析不到时参数就是 null（refs.hpp 的契约：目标槽缺失 → json null）；
// 模型也可能写出类型不符的参数（validator 只查参数名，不查类型）。
// 两者都必须变成干净的 ToolError。
TEST(Fakes, null_or_mistyped_args_return_toolerror_instead_of_throwing) {
  FakeToolSinks sinks;
  tool::ToolRegistry const reg = make_fake_tools(sinks);

  expect_toolerror_not_throw(reg, "llm_extract", {{"prompt", nullptr}, {"text", "正文"}}, "prompt");
  expect_toolerror_not_throw(reg, "llm_extract", {{"prompt", "提取"}, {"text", nullptr}}, "text");
  expect_toolerror_not_throw(reg, "fetch_price", {{"item_id", nullptr}}, "item_id");
  expect_toolerror_not_throw(reg, "web_search", {{"query", Json::array({"a"})}}, "query");
  expect_toolerror_not_throw(reg, "send_notification", {{"title", nullptr}}, "title");
  expect_toolerror_not_throw(reg, "sum", {{"a", "不是数字"}, {"b", 1}}, "a");
  expect_toolerror_not_throw(reg, "read", {{"path", Json::object()}}, "path");
}

// 正常入参不受影响（假工具是离线确定性的：llm_extract 未注入模型 → 原样回文本）
TEST(Fakes, valid_args_still_work) {
  FakeToolSinks sinks;
  tool::ToolRegistry const reg = make_fake_tools(sinks);
  tool::ToolContext ctx;

  auto const* ex = reg.find("llm_extract");
  ASSERT_NE(ex, nullptr);
  auto r = ex->handler(Json{{"prompt", "提取标题"}, {"text", "正文"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ((*r)["nl"].get<std::string>(), "正文");

  // 可选参数缺省 / 显式 null 都走默认值
  auto const* notify = reg.find("send_notification");
  ASSERT_NE(notify, nullptr);
  auto n = notify->handler(Json{{"title", "标题"}, {"body", nullptr}}, ctx);
  ASSERT_TRUE(n.has_value()) << n.error().message;
  EXPECT_EQ(sinks.notifications.back(), "标题");
}
