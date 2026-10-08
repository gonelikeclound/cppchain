// tests/wf —— 编译与执行端到端（workflow/compiler + fakes）
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

// 线性价格检查：fetch_price → expr(价格<800) → notify / 结束
Json price_ir() {
  return Json{
      {"id", "wf_price"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8,20 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "fetch_p"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"price", "number"}, {"in_stock", "boolean"}}},
                          {"next", "check_p"}},
                     Json{{"id", "check_p"},
                          {"expr", "$_.result.price != null && $_.result.price < 800"},
                          {"then", "notify"}, {"else", "_turn_end"}},
                     Json{{"id", "notify"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "降价了"}, {"body", "价格 $fetch_p.result.price"}}},
                          {"output_schema", Json{{"ok", "boolean"}}},
                          {"next", "_turn_end"}}})}};
}

// 轮询循环（loop 形态）：loop{max:4, next:notify, on_max:_turn_end, body:[fetch → check]}
Json poll_ir() {
  return Json{
      {"id", "wf_poll"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 * * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 4},
                          {"next", "notify"}, {"on_max", "_turn_end"},
                          {"body", Json::array(
                                       {Json{{"id", "fetch_st"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "@config.product_id"}}},
                                             {"output_schema", Json{{"price", "number"},
                                                                    {"in_stock", "boolean"}}},
                                             {"next", "check_st"}},
                                        Json{{"id", "check_st"},
                                             {"expr", "$fetch_st.result.in_stock == true"},
                                             {"then", "_break"}, {"else", "_continue"}}})}},
                     Json{{"id", "notify"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "到货了"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
}

}  // namespace

TEST(WfCompiler, linear_price_flow) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());
  ASSERT_TRUE(ir);

  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << (wf.error().kind == WorkflowError::Kind::Invalid
                          ? "Invalid: " + wf.error().message
                          : wf.error().message);
  EXPECT_EQ(wf->registered_nodes(), 3u);

  // 默认价 799 < 800 → 发通知
  auto r1 = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r1) << r1.error().message;
  EXPECT_EQ(sinks.notifications.size(), 1u);
  EXPECT_EQ(sinks.notifications[0], "降价了：价格 799");

  // 换价格：现在 ≥800 → 不发
  sinks.price_lookup = [](std::string const&) {
    return Json{{"price", 900}, {"in_stock", true}, {"currency", "CNY"}};
  };
  auto r2 = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r2);
  EXPECT_EQ(sinks.notifications.size(), 1u);  // 没新增
  EXPECT_EQ((*r2)["results"]["fetch_p"]["price"].get<int>(), 900);
  EXPECT_EQ((*r2)["_last_exec"].get<std::string>(), "fetch_p");
}

TEST(WfCompiler, poll_loop_bounded) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(poll_ir());
  ASSERT_TRUE(ir);
  sinks.price_lookup = [](std::string const&) {
    return Json{{"price", 10}, {"in_stock", false}, {"currency", "CNY"}};  // 始终缺货
  };

  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf);
  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  // 缺货 → 每轮走 else→_continue，4 次后按 on_max 截止（body 恰好跑 max 次，无通知）
  EXPECT_TRUE(sinks.notifications.empty());
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 4);
}

TEST(WfCompiler, on_error_retry_succeeds) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  int attempts = 0;
  reg.add(cppchain::tool::make_tool(
      "flaky", "前 1 次失败", Json::object(),
      [&attempts](Json const&, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
        ++attempts;
        if (attempts == 1) return std::unexpected(tool::ToolError{"boom"});
        return std::expected<Json, tool::ToolError>{Json{{"price", 777}}};
      }));

  Json ir_json = Json{
      {"id", "wf_flaky"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "run"}, {"tool", "flaky"}, {"parameters", Json::object()},
                          {"output_schema", Json{{"price", "number"}}},
                          {"next", "_turn_end"}, {"on_error", Json{{"retry", 2}}}}})}};
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, Json::object());
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(attempts, 2);  // 1 失败 + 1 成功
  EXPECT_EQ((*r)["results"]["run"]["price"].get<int>(), 777);
}

// 回归：on_error.fallback 指向 **label** 时，编译期必须把它解析成该标签的续接节点。
// 原先 fallback 直接用原始 id 构造 Command.goto，而 label 并不注册为图节点
// → validate 通过、compile 成功，一 fire 就 "Runtime: unknown node: L0"。
TEST(WfCompiler, on_error_fallback_may_target_a_loop) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  reg.add(cppchain::tool::make_tool(
      "always_fail", "永远失败", Json::object(),
      [](Json const&, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
        return std::unexpected(tool::ToolError{"boom"});
      }));

  // fallback 指向 loop 节点（新机制下的"特殊目标"）：必须与指向普通节点一样能解析编译。
  Json ir_json = Json{
      {"id", "wf_fb_loop"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "main"}, {"tool", "always_fail"}, {"parameters", Json::object()},
                          {"output_schema", Json{{"x", "number"}}}, {"next", "poll"},
                          {"on_error", Json{{"retry", 0}, {"fallback", "poll"}}}},
                     Json{{"id", "poll"}, {"kind", "loop"}, {"max", 2}, {"next", "_turn_end"},
                          {"body", Json::array(
                                       {Json{{"id", "backup"}, {"tool", "echo"},
                                             {"parameters", Json{{"v", "fallback!"}}},
                                             {"output_schema", Json{{"value", "string"}}},
                                             {"next", "_break"}}})}}})}};

  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  // main 恒失败 → 唯一到达 backup 的路径就是 fallback
  auto r = fire_once(*wf, Json::object());
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ((*r)["results"]["backup"]["value"].get<std::string>(), "fallback!");
}

TEST(WfCompiler, on_error_fallback_runs_and_runtime_thrown) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  reg.add(cppchain::tool::make_tool(
      "always_fail", "永远失败", Json::object(),
      [](Json const&, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
        return std::unexpected(tool::ToolError{"boom"});
      }));

  {
    // fallback：主链失败 → 跳到降级节点
    Json ir_json = Json{
        {"id", "wf_fb"},
        {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                             {"schedule", "0 8 * * *"}}}}},
        {"nodes", Json::array(
                      {Json{{"id", "main"}, {"tool", "always_fail"}, {"parameters", Json::object()},
                            {"output_schema", Json{{"x", "number"}}}, {"next", "_turn_end"},
                            {"on_error", Json{{"retry", 0}, {"fallback", "backup"}}}},
                       Json{{"id", "backup"}, {"tool", "echo"}, {"parameters", Json{{"v", "fallback!"}}},
                            {"output_schema", Json{{"value", "string"}}}, {"next", "_turn_end"}}})}};
    auto ir = WorkflowIR::parse(ir_json);
    ASSERT_TRUE(ir) << ir.error();
    auto wf = compile_workflow(*ir, reg);
    ASSERT_TRUE(wf) << wf.error().message;
    auto r = fire_once(*wf, Json::object());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ((*r)["results"]["backup"]["value"].get<std::string>(), "fallback!");
  }
  {
    // 无 fallback：失败耗尽 → WorkflowError::Runtime（fail-closed，不假装成功）
    Json ir_json = Json{
        {"id", "wf_nofb"},
        {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                             {"schedule", "0 8 * * *"}}}}},
        {"nodes", Json::array(
                      {Json{{"id", "main"}, {"tool", "always_fail"}, {"parameters", Json::object()},
                            {"output_schema", Json{{"x", "number"}}}, {"next", "_turn_end"},
                            {"on_error", Json{{"retry", 0}}}}})}};
    auto ir = WorkflowIR::parse(ir_json);
    ASSERT_TRUE(ir);
    auto wf = compile_workflow(*ir, reg);
    ASSERT_TRUE(wf);
    auto r = fire_once(*wf, Json::object());
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, WorkflowError::Kind::Runtime);
  }
}

TEST(WfCompiler, invalid_ir_rejected) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());
  ASSERT_TRUE(ir);
  ir->trigger->raw["condition"] = true;  // 违反 VT3 → compile 必须 fail-closed
  auto wf = compile_workflow(*ir, reg);
  EXPECT_FALSE(wf.has_value());
  EXPECT_EQ(wf.error().kind, WorkflowError::Kind::Invalid);
}

TEST(WfCompiler, simulate_multi_fire_independent) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());
  ASSERT_TRUE(ir);
  auto wf = compile_workflow(*ir, reg);

  std::vector<Json> configs{
      Json{{"product_id", "P1"}},  // 默认 799 → 花钱
      Json{{"product_id", "P2"}},  // 同样 799
  };
  sinks.price_lookup = [](std::string const&) {
    return Json{{"price", 599}, {"in_stock", true}, {"currency", "CNY"}};  // 两次都<800
  };
  auto states = simulate_fires(*wf, configs);
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(sinks.notifications.size(), 2u);  // 每次触发干净独立
}
// 回归：parameters 里**嵌套在数组/对象中**的引用也必须在运行期解析。
// validator 的 VR1/VR2 是递归走进容器的（所以嵌套引用会被校验为合法），
// 而 resolve_parameter 原先对非字符串直接原样返回 —— 嵌套引用以字面量
// "$src.result.price" 丢给工具，校验过的引用静默失效（传错数据且不报错）。
TEST(WfCompiler, nested_refs_inside_containers_are_resolved) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  // 探针：原样回吐收到的 arguments，用于断言"运行期究竟传了什么"
  reg.add(cppchain::tool::make_tool(
      "args_probe", "回吐收到的参数",
      cppchain::tool::SchemaBuilder()
          .prop("top", "string", "顶层引用", false)
          .prop("list", "array", "数组内的引用", false)
          .prop("obj", "object", "对象内的引用", false)
          .build(),
      [](Json const& args, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
        return args;
      }));

  Json ir_json = Json{
      {"id", "wf_nested_ref"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "src"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"price", "number"}}}, {"next", "sink"}},
                     Json{{"id", "sink"},
                          {"tool", "args_probe"},
                          {"parameters",
                           Json{{"top", "$src.result.price"},
                                {"list", Json::array({"$src.result.price", "literal"})},
                                {"obj", Json{{"nested", "$src.result.price"}}}}},
                          {"output_schema", Json::object()},
                          {"next", "_turn_end"}}})}};

  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json{{"product_id", "p1"}});
  ASSERT_TRUE(r) << r.error().message;

  Json const& got = (*r)["results"]["sink"];
  EXPECT_EQ(got["top"].get<int>(), 799);            // 顶层（原本就对）
  EXPECT_EQ(got["list"][0].get<int>(), 799);        // 修复前是字面量 "$src.result.price"
  EXPECT_EQ(got["obj"]["nested"].get<int>(), 799);  // 修复前是字面量
  EXPECT_EQ(got["list"][1].get<std::string>(), "literal");  // 无引用项原样保留
}

// 回归：工具 handler **抛异常** 时，on_error 的 retry / fallback 必须照样生效。
// 原先 compiler 直接调 handler，异常穿透节点 → 重试循环与 fallback 全被绕过，
// 而 IR 明明声明了 on_error（真机验收里模型产出的 IR 正是这个形态）。
// 这与 ToolNode::execute_one 的纪律不一致：那里是异常转错误回喂，不让它穿透。
TEST(WfCompiler, on_error_applies_when_handler_throws) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  int calls = 0;
  reg.add(cppchain::tool::make_tool(
      "boom", "总是抛异常", cppchain::tool::SchemaBuilder().build(),
      [&calls](Json const&, tool::ToolContext&) -> std::expected<Json, tool::ToolError> {
        ++calls;
        throw std::runtime_error("工具内部炸了");  // 抛，而不是返回 unexpected
      }));

  // with_backup=false 时不留 backup 节点 —— 没有 fallback 指向它，它就是死节点（VG7）
  auto ir_of = [](Json on_error, bool with_backup) {
    Json nodes = Json::array(
        {Json{{"id", "main"}, {"tool", "boom"}, {"parameters", Json::object()},
              {"output_schema", Json{{"x", "number"}}}, {"next", "_turn_end"},
              {"on_error", std::move(on_error)}}});
    if (with_backup)
      nodes.push_back(Json{{"id", "backup"}, {"tool", "echo"},
                           {"parameters", Json{{"v", "fallback!"}}},
                           {"output_schema", Json{{"value", "string"}}}, {"next", "_turn_end"}});
    return Json{
        {"id", "wf_throw"},
        {"trigger", Json{{"source", "time"},
                         {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
        {"nodes", std::move(nodes)}};
  };

  {  // 有 fallback：异常也算失败 → 走降级，而不是把异常抛穿整张图
    auto ir = WorkflowIR::parse(ir_of(Json{{"retry", 2}, {"fallback", "backup"}}, true));
    ASSERT_TRUE(ir) << ir.error();
    auto wf = compile_workflow(*ir, reg);
    ASSERT_TRUE(wf) << wf.error().message;
    auto r = fire_once(*wf, Json::object());
    ASSERT_TRUE(r) << r.error().message;  // 修复前：Runtime: node 'main' threw: 工具内部炸了
    EXPECT_EQ((*r)["results"]["backup"]["value"].get<std::string>(), "fallback!");
    EXPECT_EQ(calls, 3);  // retry=2 → 共 3 次尝试，异常同样计入重试
  }

  {  // 无 fallback：耗尽后 fail-closed，且错误里要带上底层原因（便于排查）
    auto ir = WorkflowIR::parse(ir_of(Json{{"retry", 0}}, false));
    ASSERT_TRUE(ir) << ir.error();
    auto wf = compile_workflow(*ir, reg);
    ASSERT_TRUE(wf) << wf.error().message;
    auto r = fire_once(*wf, Json::object());
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("工具内部炸了"), std::string::npos) << r.error().message;
  }
}

// B2 回归：编译路径也要认这份 config 契约 —— 否则手写/存下来的 IR 会绕过
// 生成端的约束，照样在运行期拿到 null。
TEST(WfCompiler, compile_enforces_config_contract) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(price_ir());  // 用 @config.product_id
  ASSERT_TRUE(ir) << ir.error();

  CompileOptions ok_opts;
  ok_opts.config_keys = std::vector<std::string>{"product_id"};
  EXPECT_TRUE(compile_workflow(*ir, reg, nullptr, ok_opts).has_value());

  CompileOptions bad_opts;
  bad_opts.config_keys = std::vector<std::string>{"other_key"};
  auto bad = compile_workflow(*ir, reg, nullptr, bad_opts);
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().message.find("VR4"), std::string::npos) << bad.error().message;

  // 不声明 → 不检查（向后兼容）
  EXPECT_TRUE(compile_workflow(*ir, reg).has_value());
}

// trigger 可选：无 trigger 的 IR 也能编译 + fire（wfr 手工拉起路径，D1 语义）
TEST(WfCompiler, no_trigger_compiles_and_fires) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  Json j = price_ir();
  ASSERT_TRUE(j.contains("trigger"));
  j.erase("trigger");  // 手工触发：去掉调度元数据
  auto ir = WorkflowIR::parse(j);
  ASSERT_TRUE(ir) << ir.error();
  EXPECT_FALSE(ir->trigger.has_value());

  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;
  EXPECT_FALSE(wf->trigger.has_value());

  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(sinks.notifications.size(), 1u);  // 默认价 799 < 800 → 发通知
}
