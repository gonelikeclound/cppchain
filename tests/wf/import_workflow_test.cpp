// wfr-③ 测试：workflow import 展开（import.hpp / import.cpp）
// 注入自定义 loader（内存 JSON，不碰文件），覆盖：指针/$<id> 改写、as 前缀、
// 嵌套、fail-closed（循环/找不到/重名/动态库拒）、编译+fire 端到端。
#include <gtest/gtest.h>

#include <map>
#include <string>

#include "cppchain/workflow/import.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

Json notify_wf_json() {  // B：单工具节点，供被 import
  return Json{{"id", "notify_wf"},
              {"trigger", Json{{"source", "time"},
                               {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
              {"nodes", Json::array(
                            {Json{{"id", "n"},
                                  {"tool", "send_notification"},
                                  {"parameters", Json{{"title", "降价提醒"}}},
                                  {"output_schema", Json{{"ok", "boolean"}}},
                                  {"next", "_turn_end"}}})}};
}

Json main_wf_json() {  // A：import B，own 里用 B 原始 id "n" 引用
  return Json{{"id", "main_wf"},
              {"import", Json{{"workflow", "notify_wf"}}},
              {"nodes", Json::array(
                            {Json{{"id", "fetch_p"},
                                  {"tool", "fetch_price"},
                                  {"parameters", Json{{"item_id", "@config.product_id"}}},
                                  {"output_schema", Json{{"price", "number"}}},
                                  {"next", "ck"}},
                             Json{{"id", "ck"},
                                  {"expr", "$fetch_p.result.price < 500"},
                                  {"then", "n"},  // ← 引用被引 workflow 的原始 id
                                  {"else", "_turn_end"}}})}};
}

// 内存 loader：spec → 预置 JSON
WorkflowImportLoader mem_loader(std::map<std::string, Json> libs) {
  WorkflowImportLoader l;
  l.read = [libs = std::move(libs)](std::string const&, std::string const& spec, Json& out,
                                    std::string& err) {
    auto it = libs.find(spec);
    if (it == libs.end()) {
      err = "找不到 workflow '" + spec + "'";
      return false;
    }
    out = it->second;
    return true;
  };
  return l;
}

}  // namespace

TEST(ImportWorkflow, basic_rewrite_and_concat) {
  Json main = main_wf_json();
  auto loader = mem_loader({{"notify_wf", notify_wf_json()}});
  auto r = expand_workflow_imports(main, ".", loader);
  ASSERT_TRUE(r) << r.error();

  EXPECT_FALSE(main.contains("import"));
  ASSERT_TRUE(main["nodes"].is_array());
  ASSERT_EQ(main["nodes"].size(), 3u);  // own(fetch_p,ck) + 展开的 notify_wf__n
  // id 前缀化
  EXPECT_EQ(main["nodes"][0]["id"], "fetch_p");
  EXPECT_EQ(main["nodes"][1]["id"], "ck");
  EXPECT_EQ(main["nodes"][2]["id"], "notify_wf__n");
  // own 的指针引用子链原始 id → 前缀 id
  EXPECT_EQ(main["nodes"][1]["then"], "notify_wf__n");
  // $<id> 引用本身不动（引用的是 own id）
  EXPECT_EQ(main["nodes"][1]["expr"], "$fetch_p.result.price < 500");
  // 子链徽标：被 import 的节点带 origin_wf（其直属工作流名）；own 节点没有
  EXPECT_EQ(main["nodes"][2]["origin_wf"], "notify_wf");
  EXPECT_FALSE(main["nodes"][0].contains("origin_wf"));
}

TEST(ImportWorkflow, as_prefix_overrides_default) {
  Json main = main_wf_json();
  main["import"] = Json{{"workflow", "notify_wf"}, {"as", "reminder"}};
  auto loader = mem_loader({{"notify_wf", notify_wf_json()}});
  ASSERT_TRUE(expand_workflow_imports(main, ".", loader));
  EXPECT_EQ(main["nodes"][1]["then"], "reminder__n");
  EXPECT_EQ(main["nodes"][2]["id"], "reminder__n");
}

TEST(ImportWorkflow, nested_two_levels) {
  Json b = notify_wf_json();
  b["nodes"] = Json::array();  // 纯组合：b 无 own 节点，全由 import 提供
  Json a = Json{{"id", "a"},
                {"import", Json{{"workflow", "b"}, {"as", "b2"}}},
                {"nodes", Json::array()}};
  b["import"] = Json{{"workflow", "notify_wf"}};  // b 再 import notify_wf
  auto loader = mem_loader({{"b", b}, {"notify_wf", notify_wf_json()}});
  auto const r = expand_workflow_imports(a, ".", loader);
  ASSERT_TRUE(r) << (r ? "" : r.error());
  // b2__notify_wf__n：两层前缀
  ASSERT_EQ(a["nodes"].size(), 1u);
  EXPECT_EQ(a["nodes"][0]["id"], "b2__notify_wf__n");
  // 徽标保留最深处归属：b2__notify_wf__n 来自 notify_wf（不是中间层 b）
  EXPECT_EQ(a["nodes"][0]["origin_wf"], "notify_wf");
}

TEST(ImportWorkflow, circular_import_rejected) {
  Json self = Json{{"id", "self"}, {"import", Json{{"workflow", "self"}}}, {"nodes", Json::array()}};
  auto loader = mem_loader({{"self", self}});
  auto r = expand_workflow_imports(self, ".", loader);
  ASSERT_FALSE(r);
  EXPECT_TRUE(r.error().find("循环 import") != std::string::npos);
}

TEST(ImportWorkflow, missing_workflow_rejected) {
  Json main = main_wf_json();
  auto loader = mem_loader({});  // 空库
  auto r = expand_workflow_imports(main, ".", loader);
  ASSERT_FALSE(r);
  EXPECT_TRUE(r.error().find("找不到") != std::string::npos);
}

TEST(ImportWorkflow, duplicate_node_rejected) {
  // 两个 import 都定义同名原始节点 → 重名错
  Json c1 = Json{{"id", "c1"}, {"nodes", Json::array({Json{{"id", "x"}, {"kind", "label"}}})}};
  Json c2 = Json{{"id", "c2"}, {"nodes", Json::array({Json{{"id", "x"}, {"kind", "label"}}})}};
  Json main = Json{{"id", "m"},
                   {"import", Json::array({Json{{"workflow", "c1"}}, Json{{"workflow", "c2"}}})},
                   {"nodes", Json::array()}};
  auto loader = mem_loader({{"c1", c1}, {"c2", c2}});
  auto r = expand_workflow_imports(main, ".", loader);
  ASSERT_FALSE(r);
  EXPECT_TRUE(r.error().find("重名") != std::string::npos);
}

TEST(ImportWorkflow, dynamic_import_rejected_in_imported_wf) {
  Json c = Json{{"id", "c"},
                {"nodes", Json::array()},
                {"imports", Json::array({Json{{"lib", "x.so"}}})}};
  Json main = Json{{"id", "m"}, {"import", Json{{"workflow", "c"}}}, {"nodes", Json::array()}};
  auto loader = mem_loader({{"c", c}});
  auto r = expand_workflow_imports(main, ".", loader);
  ASSERT_FALSE(r);
  EXPECT_TRUE(r.error().find("动态库") != std::string::npos);
}

// 端到端：A import B + 无 trigger → 编译 + fire（离线假工具）
TEST(ImportWorkflow, compiles_and_fires_end_to_end) {
  FakeToolSinks sinks;
  tool::ToolRegistry reg = make_fake_tools(sinks);
  Json main = main_wf_json();
  auto loader = mem_loader({{"notify_wf", notify_wf_json()}});
  ASSERT_TRUE(expand_workflow_imports(main, ".", loader));

  auto ir = WorkflowIR::parse(main);
  ASSERT_TRUE(ir) << ir.error();
  ASSERT_EQ(ir->nodes.size(), 3u);
  EXPECT_EQ(ir->nodes[2].kind, NodeKind::Tool);  // 展开的 n 是 tool
  EXPECT_FALSE(ir->trigger.has_value());         // 被引 wf 的 trigger 已丢弃

  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;
  // 默认价 799 不 < 500 → else（_turn_end），通知不触发；改价后才走 imported n
  auto r1 = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r1) << r1.error().message;
  EXPECT_EQ(sinks.notifications.size(), 0u);

  sinks.price_lookup = [](std::string const&) { return Json{{"price", 300}}; };
  auto r2 = fire_once(*wf, Json{{"product_id", "P1"}});
  ASSERT_TRUE(r2) << r2.error().message;
  EXPECT_EQ(sinks.notifications.size(), 1u) << "then 指向 imported 节点的边应被展开后正确连通";
  EXPECT_EQ(sinks.notifications[0], "降价提醒");
}