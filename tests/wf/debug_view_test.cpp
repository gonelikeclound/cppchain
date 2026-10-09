// wfr-⑤ 测试：debug_view（图导出 / 单 HTML 渲染 / 裁减守卫）。
// 注：日志的**推导**（分组 / loop 迭代号 / 边使用）wfr-㉟ 已搬到页面里的 JS ——
// 那部分由 tests/js/debug_derive_test.mjs 覆盖（ctest 里找得到 node 才注册）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/workflow/debug_view.hpp"
#include "cppchain/workflow/ir.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

// 与 wfr_demo 的 loop_wf 同构（loop 容器 + tool + expr、无 trigger）
Json loop_ir_json() {
  return Json{{"id", "loop_wf"},
              {"nodes", Json::array(
                            {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 3},
                                  {"next", "_turn_end"},
                                  {"body", Json::array(
                                               {Json{{"id", "fetch"},
                                                     {"tool", "fetch_price"},
                                                     {"parameters", Json{{"item_id", "P1"}}},
                                                     {"output_schema", Json{{"price", "number"}}},
                                                     {"next", "ck"}},
                                                Json{{"id", "ck"},
                                                     {"expr", "$fetch.result.price < 500"},
                                                     {"then", "_break"},
                                                     {"else", "_continue"}}})}}})}};
}

Json rec(std::string run, std::string node, std::string phase, Json update = Json::object(),
         std::string source = "engine") {
  Json r{{"run", run}, {"node", node}, {"phase", phase}, {"source", source}};
  if (!update.empty()) r["update"] = update;
  return r;
}

}  // namespace

TEST(DebugView, graph_exports_nodes_and_edges) {
  auto ir = WorkflowIR::parse(loop_ir_json());
  ASSERT_TRUE(ir) << ir.error();

  Json g = debug_view_graph(*ir);
  ASSERT_EQ(g["nodes"].size(), 3u);  // loop 容器平铺 + body 两个节点
  EXPECT_EQ(g["nodes"][0]["id"], "poll");
  EXPECT_EQ(g["nodes"][0]["kind"], "loop");
  // 每个节点带完整原始定义（选中节点 JSON 面板的数据源）
  EXPECT_EQ(g["nodes"][1]["id"], "fetch");
  EXPECT_EQ(g["nodes"][1]["def"]["tool"], "fetch_price");
  EXPECT_EQ(g["nodes"][1]["def"]["parameters"]["item_id"], "P1");

  auto edges = g["edges"];
  // poll→body、poll→next(_break 落点)、poll→on_max(跑满落点)、fetch→ck、ck→_break、ck→_continue(回边)
  ASSERT_EQ(edges.size(), 6u);
  // poll 的三条出边（body 入口、_break 落点、跑满落点）都是前向
  bool saw_body = false, saw_terminal = false, saw_continue_back = false;
  for (auto const& e : edges) {
    if (e["from"] == "poll" && e["label"] == "body" && e["to"] == "fetch" && e["forward"] == true)
      saw_body = true;
    if (e.value("terminal", false) && e["to"] == "_turn_end") saw_terminal = true;
    // `_continue` 就地解析成 loop 节点头，且是唯一回边
    if (e["label"] == "else" && e["to"] == "poll" && e["forward"] == false)
      saw_continue_back = true;
  }
  EXPECT_TRUE(saw_body) << "loop→body 入口应是前向边";
  EXPECT_TRUE(saw_terminal) << "_turn_end 应标为终端";
  EXPECT_TRUE(saw_continue_back) << "_continue 应导出为指向 loop 节点头的回边";
}

TEST(DebugView, render_html_is_self_contained_and_escaped) {
  auto ir = WorkflowIR::parse(loop_ir_json());
  ASSERT_TRUE(ir);

  std::vector<Json> records;
  records.push_back(rec("r-1", "poll", "exit",
                        Json{{"messages", Json::array({"</script><script>alert(1)"})}}));
  Json payload = build_debug_payload(*ir, records);
  std::string html = render_debug_html(payload);

  EXPECT_NE(html.find("<title>wfr debug — loop_wf</title>"), std::string::npos) << html.substr(0, 160);
  EXPECT_NE(html.find("const P = "), std::string::npos);
  // 新布局骨架：中列详情面板 + 树形日志组 + 分栏拖动条
  EXPECT_NE(html.find("id=\"detail\""), std::string::npos);
  EXPECT_NE(html.find("details.run"), std::string::npos);
  EXPECT_NE(html.find("id=\"sv1\""), std::string::npos);
  // 数据里的 </script 必须被转义（浏览器端不执行注入的脚本）
  EXPECT_EQ(html.find("</script><script>alert"), std::string::npos);
  // 但 escape 后数据仍在（< 形式）
  EXPECT_NE(html.find("\\u003c"), std::string::npos);
  // 文件以完整 HTML 收尾
  EXPECT_TRUE(html.ends_with("</html>"));
}

TEST(DebugView, oversized_payload_trims_node_diag) {
  std::vector<Json> records;
  for (int i = 0; i < 20001; ++i) {
    Json r = rec("r-1", "n" + std::to_string(i % 3), i % 2 ? "exit" : "enter");
    if (i % 3 == 1) r["source"] = "node";  // 混入节点诊断
    records.push_back(std::move(r));
  }
  auto ir = WorkflowIR::parse(loop_ir_json());
  ASSERT_TRUE(ir);
  Json payload = build_debug_payload(*ir, records);
  EXPECT_EQ(payload["trimmed_node_logs"], true);
  for (auto const& run : payload["runs"])
    for (auto const& r : run["records"]) EXPECT_NE(r.value("source", ""), "node");
}
// ---- wfr-㉚：loop 节点在调试台里的呈现与归属 ----

namespace {

// loop 形态的循环：loop{max:3, body:[poll → ck(_break/_continue)]} → notify
Json loop_node_ir_json() {
  return Json{
      {"id", "loop_node_wf"},
      {"nodes", Json::array(
                    {Json{{"id", "poll"}, {"kind", "loop"}, {"max", 3}, {"next", "notify"},
                          {"body", Json::array(
                                       {Json{{"id", "fetch"},
                                             {"tool", "fetch_price"},
                                             {"parameters", Json{{"item_id", "P1"}}},
                                             {"output_schema", Json{{"price", "number"}}},
                                             {"next", "ck"}},
                                        Json{{"id", "ck"}, {"expr", "$fetch.result.price < 500"},
                                             {"then", "_break"}, {"else", "_continue"}}})}},
                     Json{{"id", "notify"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "到货"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
}

Json const* find_node(Json const& g, std::string const& id) {
  for (Json const& n : g["nodes"])
    if (n.value("id", "") == id) return &n;
  return nullptr;
}

Json const* find_edge(Json const& g, std::string const& from, std::string const& label) {
  for (Json const& e : g["edges"])
    if (e.value("from", "") == from && e.value("label", "") == label) return &e;
  return nullptr;
}

}  // namespace

TEST(DebugView, loop_nodes_are_exported_flat_with_body_attribution) {
  auto ir = WorkflowIR::parse(loop_node_ir_json());
  ASSERT_TRUE(ir) << ir.error();
  auto const g = debug_view_graph(*ir);

  // 图是平铺的：loop 节点头与 body 里的节点是同级节点（不做容器框）
  EXPECT_EQ(g["nodes"].size(), 4u);
  ASSERT_NE(find_node(g, "poll"), nullptr);
  EXPECT_EQ((*find_node(g, "poll"))["kind"], "loop");
  EXPECT_EQ((*find_node(g, "notify"))["kind"], "tool");

  // body 里的节点带 `loop` 归属（前端据此做迭代分组）；loop 自身与 loop 之后的节点不带
  ASSERT_NE(find_node(g, "fetch"), nullptr);
  EXPECT_EQ((*find_node(g, "fetch"))["loop"], "poll");
  EXPECT_EQ((*find_node(g, "ck"))["loop"], "poll");
  EXPECT_FALSE(find_node(g, "poll")->contains("loop"));
  EXPECT_FALSE(find_node(g, "notify")->contains("loop"));
}

TEST(DebugView, loop_edges_are_resolved_into_graph_targets) {
  auto ir = WorkflowIR::parse(loop_node_ir_json());
  ASSERT_TRUE(ir) << ir.error();
  auto const g = debug_view_graph(*ir);

  // loop 节点头：body 入口与出口都是前向边
  Json const* body_edge = find_edge(g, "poll", "body");
  ASSERT_NE(body_edge, nullptr);
  EXPECT_EQ((*body_edge)["to"], "fetch");
  EXPECT_TRUE((*body_edge)["forward"].get<bool>());
  Json const* next_edge = find_edge(g, "poll", "next");
  ASSERT_NE(next_edge, nullptr);
  EXPECT_EQ((*next_edge)["to"], "notify");
  EXPECT_TRUE((*next_edge)["forward"].get<bool>());

  // `_continue` → loop 节点头，且标成**回边**（前端据此画绕行弧线）
  Json const* back = find_edge(g, "ck", "else");
  ASSERT_NE(back, nullptr);
  EXPECT_EQ((*back)["to"], "poll");
  EXPECT_FALSE((*back)["forward"].get<bool>());

  // `_break` → loop 的 next，仍是前向边
  Json const* brk = find_edge(g, "ck", "then");
  ASSERT_NE(brk, nullptr);
  EXPECT_EQ((*brk)["to"], "notify");
  EXPECT_TRUE((*brk)["forward"].get<bool>());
}

TEST(DebugView, html_carries_the_loop_kind_and_colour) {
  auto ir = WorkflowIR::parse(loop_node_ir_json());
  ASSERT_TRUE(ir) << ir.error();
  auto const html = render_debug_html(build_debug_payload(*ir, {}));
  EXPECT_NE(html.find("kind-loop"), std::string::npos);   // CSS 配色
  EXPECT_NE(html.find("loop:{f:'#e6dcf7'"), std::string::npos);  // 图例
  EXPECT_NE(html.find("n.kind==='loop'"), std::string::npos);    // 分组维度 = loop 节点
}

// 内嵌 JS 是一个**字符串**：编辑它时少闭合一个花括号，C++ 编译期毫无察觉，浏览器里却会
// 整页空白（"初始化失败"那行 catch 都跑不到）。这里做一次字符串感知的括号平衡体检，
// 把这类错误钉在测试里。基准值是"平衡"的（模板里有个先出现的 `{`，故不为 0）。
TEST(DebugView, embedded_js_braces_are_balanced) {
  auto ir = WorkflowIR::parse(loop_node_ir_json());
  ASSERT_TRUE(ir) << ir.error();
  auto const html = render_debug_html(build_debug_payload(*ir, {}));

  auto const sa = html.find("<script>");
  auto const sb = html.rfind("</script>");
  ASSERT_NE(sa, std::string::npos);
  ASSERT_NE(sb, std::string::npos);
  std::string const js = html.substr(sa + 8, sb - sa - 8);

  // 剥掉字符串/模板串/注释后再数花括号
  long depth = 0;
  char st = 0;
  for (std::size_t i = 0; i < js.size(); ++i) {
    char const c = js[i];
    char const n = i + 1 < js.size() ? js[i + 1] : '\0';
    if (st == 0) {
      if (c == '"' || c == '\'' || c == '`') { st = c; continue; }
      if (c == '/' && n == '*') { auto e = js.find("*/", i); i = (e == std::string::npos) ? js.size() : e + 1; continue; }
      if (c == '/' && n == '/') { auto e = js.find('\n', i); i = (e == std::string::npos) ? js.size() : e; continue; }
      if (c == '{') ++depth;
      if (c == '}') --depth;
      continue;
    }
    if (c == '\\') { ++i; continue; }
    if (c == st) st = 0;
  }
  // 基准 +1：模板里 CSS/JS 交界处有个先出现的 `{`（老代码在引入 loop 前就是这个值）。
  // 无论基准是多少，**只要有人改出不平衡，这个数就会变** —— 这才是要守的性质。
  EXPECT_EQ(depth, 1) << "内嵌 JS 的括号不平衡（浏览器里会整页空白）";
}

// wfr-㉞：本轮**实际走过哪些边、各多少次** —— 从引擎的 enter 序列推出（相邻两个执行节点
// 就是走了一条边）。终端边（_turn_end/_fail）不产生 enter，靠"唯一 terminal 出边"补上。
