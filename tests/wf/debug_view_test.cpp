// wfr-⑤ 测试：debug_view（图导出 / 日志 loop 重放 / 单 HTML 渲染 / 裁减守卫）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/workflow/debug_view.hpp"
#include "cppchain/workflow/ir.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

// 与 wfr_demo 的 loop_wf 同构（label + tool + expr + goto 回边、无 trigger）
Json loop_ir_json() {
  return Json{{"id", "loop_wf"},
              {"nodes", Json::array(
                            {Json{{"id", "l0"}, {"kind", "label"}},
                             Json{{"id", "poll"},
                                  {"tool", "fetch_price"},
                                  {"parameters", Json{{"item_id", "P1"}}},
                                  {"output_schema", Json{{"price", "number"}}},
                                  {"next", "ck"}},
                             Json{{"id", "ck"}, {"expr", "$poll.result.price < 500"},
                                  {"then", "gp"}, {"else", "_turn_end"}},
                             Json{{"id", "gp"}, {"goto", "l0"}, {"max", 3}}})}};
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
  ASSERT_EQ(g["nodes"].size(), 4u);
  EXPECT_EQ(g["nodes"][0]["id"], "l0");
  EXPECT_EQ(g["nodes"][0]["kind"], "label");
  // 每个节点带完整原始定义（选中节点 JSON 面板的数据源）
  EXPECT_EQ(g["nodes"][1]["id"], "poll");
  EXPECT_EQ(g["nodes"][1]["def"]["tool"], "fetch_price");
  EXPECT_EQ(g["nodes"][1]["def"]["parameters"]["item_id"], "P1");

  auto edges = g["edges"];
  ASSERT_EQ(edges.size(), 4u);
  // poll → ck（next）
  EXPECT_EQ(edges[0]["from"], "poll");
  EXPECT_EQ(edges[0]["to"], "ck");
  EXPECT_EQ(edges[0]["forward"], true);
  // ck → then(gp) / else(_turn_end，终端)
  EXPECT_EQ(edges[1]["from"], "ck");
  EXPECT_EQ(edges[2]["from"], "ck");
  bool saw_terminal = false, saw_goto_back = false;
  for (auto const& e : edges) {
    if (e.value("terminal", false) && e["to"] == "_turn_end") saw_terminal = true;
    if (e["label"] == "goto" && e["forward"] == false && e["to"] == "l0") saw_goto_back = true;
  }
  EXPECT_TRUE(saw_terminal) << "else→_turn_end 应标为终端";
  EXPECT_TRUE(saw_goto_back) << "goto 应导出为回边（forward=false）";
}

TEST(DebugView, logs_replay_loop_counts_per_record) {
  std::vector<Json> records;
  records.push_back(rec("r-1", "gp", "exit", Json{{"loop_counts", Json{{"gp", 1}}}}));
  records.push_back(rec("r-1", "poll", "enter"));
  records.push_back(rec("r-1", "gp", "exit", Json{{"loop_counts", Json{{"gp", 2}}}}));
  records.push_back(rec("r-1", "poll", "enter"));
  records.push_back(rec("r-host", "x", "enter", Json::object(), "node"));  // 无 update：沿用当前

  Json runs = debug_view_logs(records);
  ASSERT_EQ(runs.size(), 2u);  // r-1 + 非 run 组（node 诊断带 run → 并入 r-1）
  // 注意：node 记录也带 run=r-host → 归到 r-host 组
  Json const& r1 = runs[0];
  EXPECT_EQ(r1["run"], "r-1");
  ASSERT_EQ(r1["records"].size(), 4u);
  EXPECT_EQ(r1["records"][0]["loop"]["gp"], 1);  // 首条 exit 自己挂上
  EXPECT_EQ(r1["records"][1]["loop"]["gp"], 1);  // enter 沿用
  EXPECT_EQ(r1["records"][2]["loop"]["gp"], 2);
  EXPECT_EQ(r1["records"][3]["loop"]["gp"], 2);
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