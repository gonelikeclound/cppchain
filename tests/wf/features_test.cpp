// tests/wf —— 评测特征（demo::feature，见 examples/support/features.hpp）
#include <gtest/gtest.h>

#include <string>

#include "cppchain/workflow/ir.hpp"
#include "support/features.hpp"

using namespace cppchain::workflow;
using namespace demo::feature;

namespace {

Json simple_ir(std::string trigger_schedule, Json nodes) {
  return Json{
      {"id", "wf_f"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"},
                                       {"schedule", trigger_schedule}}}}},
      {"nodes", std::move(nodes)}};
}

}  // namespace

TEST(WfFeatures, predicates) {
  // 一份"价格检查 + 通知"的 IR
  Json j = simple_ir(
      "0 8,20 * * *",
      Json::array(
          {Json{{"id", "fetch"}, {"tool", "fetch_price"},
                {"parameters", Json{{"item_id", "@config.product_id"}}},
                {"output_schema", Json{{"price", "number"}, {"in_stock", "boolean"}}},
                {"next", "check"}, {"on_error", Json{{"retry", 2}, {"fallback", "web"}}}},
           Json{{"id", "web"}, {"tool", "web_search"}, {"parameters", Json{{"query", "price"}}},
                {"output_schema", Json{{"html", "string"}}}, {"next", "check"}},
           Json{{"id", "check"}, {"expr", "$_.result.price != null && $_.result.price < 800"},
                {"then", "notify"}, {"else", "_turn_end"}},
           Json{{"id", "notify"}, {"tool", "send_notification"},
                {"parameters", Json{{"title", "降价了"}}},
                {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}}));
  auto ir = WorkflowIR::parse(j);
  ASSERT_TRUE(ir);

  EXPECT_TRUE(check(*ir, "取价节点"));
  EXPECT_TRUE(check(*ir, "价格分支"));
  EXPECT_TRUE(check(*ir, "通知节点"));
  EXPECT_TRUE(check(*ir, "time触发+schedule"));
  EXPECT_TRUE(check(*ir, "on_error降级"));
  EXPECT_TRUE(check(*ir, "无trigger.condition"));
  EXPECT_FALSE(check(*ir, "trigger.condition"));
  EXPECT_FALSE(check(*ir, "llm_extract提取"));
  EXPECT_FALSE(check(*ir, "label/goto结构"));
  EXPECT_FALSE(check(*ir, "声称已下单"));
}

TEST(WfFeatures, purchase_claims_detected) {
  Json j = simple_ir("0 8 * * *",
                     Json::array(
                         {Json{{"id", "notify"}, {"tool", "send_notification"},
                               {"parameters", Json{{"title", "已自动下单"}}},
                               {"output_schema", Json{{"ok", "boolean"}}},
                               {"next", "_turn_end"}}}));
  auto ir = WorkflowIR::parse(j);
  ASSERT_TRUE(ir);
  EXPECT_TRUE(check(*ir, "声称已下单"));  // 诚实性审计触发
}

TEST(WfFeatures, eval_cases_fixture_sanity) {
  auto const& cases = eval_cases();
  ASSERT_EQ(cases.size(), 12u);  // 7 golden + 5 boundary（转写 10-eval-cases.md）
  EXPECT_EQ(cases[0].id, "wf_taobao_price_drop");
  EXPECT_EQ(cases[6].id, "wf_manual_order");
  EXPECT_EQ(cases[7].expect, "Reject");
  EXPECT_EQ(cases[11].id, "wf_ambiguous_morning");
  EXPECT_EQ(cases[11].expect, "Clarify");
  EXPECT_FALSE(cases[11].must_ask.empty());
}

TEST(WfFeatures, unknown_feature_false) {
  auto ir = WorkflowIR::parse(simple_ir("0 8 * * *", Json::array()));
  ASSERT_TRUE(ir);
  EXPECT_FALSE(check(*ir, "不存在的特征名"));
}
