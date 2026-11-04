// tests/sad/sad_mock_test.cpp
// S10a（problems/64 §5.7.7）：常驻 SAD 离线 mock 的门/路由/预算/审计验收。
#include "sad_mock/sad_mock.hpp"

#include <gtest/gtest.h>

using namespace cppchain::sad;

TEST(SadMock, ScenarioPassesAllChecks) {
  auto const s = run_scenario();
  for (auto const& c : s.checks) EXPECT_TRUE(c.ok) << c.what;
  EXPECT_TRUE(s.ok);
}

TEST(SadMock, DropsAreAllExplained) {
  auto const s = run_scenario();
  EXPECT_GT(s.r.drops.size(), 900u);
  for (auto const& d : s.r.drops) {
    EXPECT_FALSE(d.rule_id.empty()) << d.event_id;
    EXPECT_FALSE(d.reason.empty()) << d.event_id;
    EXPECT_FALSE(d.gate.empty()) << d.event_id;
  }
}

TEST(SadMock, WhitelistAlwaysReachesDigest) {
  auto const s = run_scenario();
  long whitelisted = 0;
  for (auto const& d : s.r.digest)
    if (d.reason == "whitelist") ++whitelisted;
  EXPECT_EQ(whitelisted, s.r.whitelist_bypass);
  EXPECT_GE(s.r.whitelist_bypass, 2);
}

TEST(SadMock, ShadowAuditFindsInjectedMiss) {
  auto const s = run_scenario();
  EXPECT_GE(s.r.shadow_sampled, 1);
  EXPECT_GE(s.r.shadow_miss, 1);
  EXPECT_LE(s.r.miss_rate, 0.005);
}

TEST(SadMock, ExactWaitBypassAndBudgetExhaustion) {
  auto const s = run_scenario();
  EXPECT_GE(s.r.exact_wait_bypass, 1);
  EXPECT_GE(s.r.budget_exhausted, 1);
  EXPECT_LE(s.r.wakeups, 5);
}

TEST(SadMock, SafetyWakesOverrideBudget) {
  auto const s = run_scenario();
  EXPECT_GE(s.r.safety_wakeups, 1);
  bool security_ran = false;
  for (auto const& run : s.r.runs)
    if (run.dag == "security_incident") security_ran = true;
  EXPECT_TRUE(security_ran);
}
