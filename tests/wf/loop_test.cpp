// tests/wf —— wfr-㉚：loop 循环容器（替代 label + goto）。
//
// C1 只覆盖 IR 层：解析/序列化往返、`v_loop` 与 VG11/VG13 的负例、边的回边判定，
// 以及"节点头确实把控制流送进了 body 并计了数"这条最小执行路径。
// body 内的 `_continue` 解析（真正的多轮循环）与作用域校验在 C2。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/ir_walk.hpp"
#include "cppchain/workflow/node_type.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

// 轮询循环（loop 形态）：loop{max:3, body:[fetch_st → _turn_end]}。
// body 出口暂时写 `_turn_end`（C1 里 `_continue` 还没接），所以 body 只会跑 1 轮。
Json poll_loop_ir(Json loop_overrides = Json::object()) {
  Json loop{{"id", "poll"}, {"kind", "loop"}, {"max", 3}, {"next", "_turn_end"}};
  for (auto it = loop_overrides.begin(); it != loop_overrides.end(); ++it)
    loop[it.key()] = it.value();
  if (!loop.contains("body"))
    loop["body"] = Json::array({Json{{"id", "fetch_st"},
                                     {"tool", "fetch_price"},
                                     {"parameters", Json{{"item_id", "@config.product_id"}}},
                                     {"output_schema",
                                      Json{{"price", "number"}, {"in_stock", "boolean"}}},
                                     {"next", "_turn_end"}}});
  return Json{{"id", "wf_poll_loop"},
              {"nodes", Json::array({loop})}};
}

}  // namespace

// ---- 解析 / 序列化 ----

TEST(WfLoop, parse_write_round_trip_keeps_the_body_subtree) {
  Json const src = Json{
      {"id", "wf_lp"},
      {"nodes", Json::array(
                    {Json{{"id", "after"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "done"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}},
                     Json{{"id", "poll"}, {"kind", "loop"}, {"max", 3}, {"next", "after"},
                          {"body", Json::array(
                                       {Json{{"id", "f"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "ck"}},
                                        Json{{"id", "ck"}, {"expr", "$f.result.price != null"},
                                             {"then", "_break"}, {"else", "_continue"}}})}}})}};

  auto ir = WorkflowIR::parse(src);
  ASSERT_TRUE(ir) << ir.error();

  // 顶层仍是 2 个节点；loop 的子树在 body 里（不是被拍平进 nodes）
  ASSERT_EQ(ir->nodes.size(), 2u);
  NodeIR const& lp = ir->nodes[1];
  ASSERT_EQ(lp.kind, NodeKind::Loop);
  EXPECT_EQ(lp.max, 3);
  EXPECT_EQ(lp.next, "after");
  ASSERT_EQ(lp.body.size(), 2u);
  EXPECT_EQ(lp.body[0].id, "f");
  EXPECT_EQ(lp.body[0].next, "ck");
  EXPECT_EQ(lp.body[1].then, "_break");
  EXPECT_EQ(lp.body[1].else_, "_continue");

  // 往返：再 parse 一次必须逐字段一致（body 递归写回）
  Json const round = ir->to_json();
  auto again = WorkflowIR::parse(round);
  ASSERT_TRUE(again) << again.error();
  ASSERT_EQ(again->nodes.size(), 2u);
  ASSERT_EQ(again->nodes[1].body.size(), 2u);
  EXPECT_EQ(again->nodes[1].body[1].then, "_break");
  EXPECT_EQ(again->nodes[1].max, 3);
  EXPECT_EQ(round["nodes"][1]["body"].size(), 2u);
}

TEST(WfLoop, bad_node_inside_body_reports_the_loop_it_came_from) {
  Json ir_json = poll_loop_ir(Json{{"body", Json::array({Json{{"id", "x"}, {"kind", "nope"}}})}});
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_FALSE(ir);
  EXPECT_NE(ir.error().find("loop 'poll' 的 body"), std::string::npos) << ir.error();
}

// ---- v_loop / VG11 / VG13 ----

namespace {

ValidationResult validate_loop(Json const& loop_overrides) {
  auto ir = WorkflowIR::parse(poll_loop_ir(loop_overrides));
  if (!ir) return ValidationResult{{ValidationError{"parse", ir.error()}}};
  return validate(*ir);
}

bool mentions(ValidationResult const& v, char const* rule, char const* needle) {
  for (auto const& e : v.errors)
    if (e.rule == rule && e.message.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST(WfLoop, missing_next_is_rejected) {
  auto const v = validate_loop(Json{{"next", ""}});
  EXPECT_TRUE(mentions(v, "VN15", "缺少 next")) << v.describe();
}

TEST(WfLoop, non_positive_max_is_rejected) {
  auto const v = validate_loop(Json{{"max", 0}});
  EXPECT_TRUE(mentions(v, "VN15", "正整数")) << v.describe();
}

TEST(WfLoop, empty_body_is_rejected) {
  auto const v = validate_loop(Json{{"body", Json::array()}});
  EXPECT_TRUE(mentions(v, "VN15", "body 不能为空")) << v.describe();
}

TEST(WfLoop, reserved_names_are_not_valid_exits) {
  for (char const* bad : {"_continue", "_break"}) {
    auto const v = validate_loop(Json{{"next", bad}});
    EXPECT_TRUE(mentions(v, "VN15", "保留名")) << bad << ": " << v.describe();
  }
}

TEST(WfLoop, nested_loop_is_rejected) {
  Json const inner{{"id", "inner"}, {"kind", "loop"}, {"max", 2}, {"next", "_turn_end"},
                   {"body", Json::array({Json{{"id", "f2"}, {"tool", "fetch_price"},
                                              {"parameters", Json{{"item_id", "P1"}}},
                                              {"output_schema", Json{{"price", "number"}}},
                                              {"next", "_turn_end"}}})}};
  auto const v = validate_loop(Json{{"body", Json::array({inner})}});
  EXPECT_TRUE(mentions(v, "VG11", "不支持嵌套循环")) << v.describe();
}

TEST(WfLoop, load_history_inside_the_body_is_rejected) {
  Json const lh{{"id", "recall"}, {"kind", "load_history"}, {"path", "@config.workspace"},
                {"next", "_turn_end"}};
  auto const v = validate_loop(Json{{"body", Json::array({lh})}});
  EXPECT_TRUE(mentions(v, "VG13", "load_history")) << v.describe();
}

TEST(WfLoop, a_legal_loop_passes_validation) {
  auto const v = validate_loop(Json::object());
  EXPECT_TRUE(v.ok()) << v.describe();
}

// ---- 边：回边判定 ----

TEST(WfLoop, loop_edges_are_forward_body_and_next) {
  auto ir = WorkflowIR::parse(poll_loop_ir());
  ASSERT_TRUE(ir);
  NodeType const* t = node_type_of(NodeKind::Loop);
  ASSERT_NE(t, nullptr);
  auto const& n = ir->nodes[0];

  auto const es = t->edges(n);
  ASSERT_EQ(es.size(), 3u);
  EXPECT_EQ(es[0].target, "fetch_st");
  EXPECT_EQ(es[0].label, "body");
  EXPECT_TRUE(es[0].forward);
  EXPECT_EQ(es[1].target, "_turn_end");
  EXPECT_EQ(es[1].label, "next");  // _break 的落点
  EXPECT_TRUE(es[1].forward);
  // on_max 缺省 → _turn_end（旧 goto 超限跳 kEnd 的行为），也是可达性要看的一条出口边
  EXPECT_EQ(es[2].target, "_turn_end");
  EXPECT_EQ(es[2].label, "on_max");
  EXPECT_TRUE(es[2].forward);
}

TEST(WfLoop, continue_is_the_only_backward_target) {
  // 这是 VG2（前向无环）在新机制下不用改判据的原因：回边判定纯粹看向量本身。
  EXPECT_TRUE(edge_is_backward("_continue"));
  EXPECT_FALSE(edge_is_backward("_break"));
  EXPECT_FALSE(edge_is_backward("_turn_end"));
  EXPECT_FALSE(edge_is_backward("some_node"));

  // 真节点上验一遍：body 里指针写 `_continue` 的那条边必须是回边
  Json const src = Json{
      {"id", "wf_lp"},
      {"nodes", Json::array(
                    {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 2}, {"next", "_turn_end"},
                          {"body", Json::array(
                                       {Json{{"id", "f"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "_continue"}}})}}})}};
  auto ir2 = WorkflowIR::parse(src);
  ASSERT_TRUE(ir2) << ir2.error();
  NodeIR const& body_head = ir2->nodes[0].body[0];
  auto const es = node_type_of(NodeKind::Tool)->edges(body_head);
  ASSERT_EQ(es.size(), 1u);
  EXPECT_EQ(es[0].target, "_continue");
  EXPECT_FALSE(es[0].forward);  // ← 回边
}

// ---- 遍历助手 ----

TEST(WfLoop, walk_helpers_expose_the_body_and_its_scope) {
  Json const src = Json{
      {"id", "wf_lp"},
      {"nodes", Json::array(
                    {Json{{"id", "head"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "P1"}}},
                          {"output_schema", Json{{"price", "number"}}}, {"next", "poll"}},
                     Json{{"id", "poll"}, {"kind", "loop"}, {"max", 2}, {"next", "tail"},
                          {"body", Json::array(
                                       {Json{{"id", "b1"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "_continue"}}})}},
                     Json{{"id", "tail"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "done"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
  auto ir = WorkflowIR::parse(src);
  ASSERT_TRUE(ir) << ir.error();

  std::vector<std::string> order;
  std::vector<std::string> scoped;
  for_each_node(*ir, [&](NodeIR const& n, LoopScope const* scope) {
    order.push_back(n.id);
    if (scope != nullptr) scoped.push_back(n.id + "@" + scope->loop_id);
    return true;
  });
  // 前序：loop 头 → 它的 body → 其后节点
  EXPECT_EQ(order, (std::vector<std::string>{"head", "poll", "b1", "tail"}));
  EXPECT_EQ(scoped, (std::vector<std::string>{"b1@poll"}));

  auto const idx = flatten_index(*ir);
  EXPECT_EQ(idx.at("head"), 0u);
  EXPECT_EQ(idx.at("poll"), 1u);
  EXPECT_EQ(idx.at("b1"), 2u);   // body 排在 loop 之后的节点之前 → "上游"判定正确
  EXPECT_EQ(idx.at("tail"), 3u);

  auto const owner = loop_owner(*ir);
  EXPECT_EQ(owner.at("b1"), "poll");
  EXPECT_FALSE(owner.contains("tail"));
}

// ---- 最小执行路径：节点头把控制流送进 body 并计数 ----

TEST(WfLoop, head_counts_and_routes_into_the_body) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(poll_loop_ir());
  ASSERT_TRUE(ir) << ir.error();

  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;
  // loop 节点头 + body 里的 tool = 2 个图节点
  EXPECT_EQ(wf->registered_nodes(), 2u);

  int calls = 0;
  sinks.price_lookup = [&calls](std::string const&) {
    ++calls;
    return Json{{"price", 10}, {"in_stock", false}, {"currency", "CNY"}};
  };

  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  // 第 1 轮：节点头计数 → 进 body；body 的 next 是 _turn_end → 整轮结束
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 1);
  EXPECT_EQ(calls, 1);
}

// ---- C2：真正的多轮循环（`_continue` 回边 / `_break` 早退）----

namespace {

// 轮询循环（loop 形态）：loop{max:4, body:[fetch_st → check_st(_break/_continue)]} → notify
Json poll_loop_full_ir() {
  return Json{
      {"id", "wf_poll_loop"},
      {"nodes", Json::array(
                    {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 4}, {"next", "notify"},
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

TEST(WfLoop, continue_back_edge_runs_the_body_up_to_max) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(poll_loop_full_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  int calls = 0;
  sinks.price_lookup = [&calls](std::string const&) {
    ++calls;
    return Json{{"price", 10}, {"in_stock", false}, {"currency", "CNY"}};  // 始终缺货
  };

  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  // 缺货 → 每轮走 else→_continue；body 恰好跑 max 次，计数终值 = max
  EXPECT_EQ(calls, 4);
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 4);  // 键是 **loop 节点**的 id
  // 关键：跑满 max 走的是 **on_max**（本合约缺省 _turn_end），不是 next（next 是 _break 的落点）
  // —— 所以"查满 4 次仍没货"不会去 notify。这正是与旧 goto（超限跳 kEnd）行为对齐的地方。
  EXPECT_TRUE(sinks.notifications.empty());
}

TEST(WfLoop, on_max_can_redirect_the_exhaustion_path) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  Json ir_json = poll_loop_full_ir();
  ir_json["nodes"][0]["on_max"] = "notify";  // 显式把"跑满"也接到 notify
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  int calls = 0;
  sinks.price_lookup = [&calls](std::string const&) {
    ++calls;
    return Json{{"price", 10}, {"in_stock", false}, {"currency", "CNY"}};
  };
  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(sinks.notifications.size(), 1u);  // 这次跑满也通知
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 4);
}

TEST(WfLoop, break_exits_the_loop_early) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir = WorkflowIR::parse(poll_loop_full_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  int calls = 0;
  sinks.price_lookup = [&calls](std::string const&) {
    ++calls;
    return Json{{"price", 10}, {"in_stock", true}, {"currency", "CNY"}};  // 有货
  };

  auto r = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(calls, 1);                             // 第 1 轮就 _break，不再回边
  EXPECT_EQ(sinks.notifications.size(), 1u);       // loop.next 被执行
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 1);
}

TEST(WfLoop, body_nodes_see_the_top_level_scope_for_ordinary_targets) {
  // body 里的指针写普通节点名（不写保留名）时，解析与顶层一致。
  Json const src = Json{
      {"id", "wf_lp"},
      {"nodes", Json::array(
                    {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 2}, {"next", "_turn_end"},
                          {"body", Json::array(
                                       {Json{{"id", "f"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "_continue"}}})}}})}};
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  auto ir2 = WorkflowIR::parse(src);
  ASSERT_TRUE(ir2) << ir2.error();
  auto wf = compile_workflow(*ir2, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  int calls = 0;
  sinks.price_lookup = [&calls](std::string const&) {
    ++calls;
    return Json{{"price", 1}, {"in_stock", false}, {"currency", "CNY"}};
  };
  auto r = fire_once(*wf, Json{{"item_id", "P1"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(calls, 2);  // max=2 → 两轮
  EXPECT_EQ((*r)["loop_counts"]["poll"].get<long>(), 2);
}

TEST(WfLoop, reserved_names_outside_a_loop_body_are_rejected) {
  // VG10：`_continue` 写在顶层链上 → 校验期就拒（不是运行期才炸）。
  Json const src = Json{
      {"id", "wf_bad"},
      {"nodes", Json::array({Json{{"id", "t1"}, {"tool", "fetch_price"},
                                  {"parameters", Json{{"item_id", "P1"}}},
                                  {"output_schema", Json{{"price", "number"}}},
                                  {"next", "_continue"}}})}};
  auto ir2 = WorkflowIR::parse(src);
  ASSERT_TRUE(ir2) << ir2.error();
  auto const v = validate(*ir2);
  EXPECT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VG10"), std::string::npos) << v.describe();
}

TEST(WfLoop, body_pointer_escaping_the_body_is_rejected) {
  // VG10：body 里的节点直接指向 body 外的普通节点 —— 多半是漏写 `_break`。
  Json const src = Json{
      {"id", "wf_bad2"},
      {"nodes", Json::array(
                    {Json{{"id", "outside"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "x"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}},
                     Json{{"id", "poll"}, {"kind", "loop"}, {"max", 2}, {"next", "outside"},
                          {"body", Json::array(
                                       {Json{{"id", "f"}, {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "outside"}}})}}})}};
  auto ir2 = WorkflowIR::parse(src);
  ASSERT_TRUE(ir2) << ir2.error();
  auto const v = validate(*ir2);
  EXPECT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VG10"), std::string::npos) << v.describe();
}
