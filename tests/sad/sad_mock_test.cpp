// tests/sad/sad_mock_test.cpp
// S10（problems/64 §5.7）：SAD 粗循环 / Task Manager / Task Agent 的离线验收。
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

TEST(SadMock, GlobalEventsGoThroughSadLocalDirect) {
  auto const s = run_scenario();
  EXPECT_GE(s.r.global_events, 5);
  EXPECT_GE(s.r.local_events, 4);
  EXPECT_GE(s.r.tasks_created, 7);
  EXPECT_EQ(s.r.sad_traces.size(),
            static_cast<std::size_t>(s.r.global_events + s.r.local_events));
  for (auto const& t : s.r.sad_traces) {
    if (t.layer == "global") {
      EXPECT_FALSE(t.task_id.empty()) << t.event_id;
      bool understand = false, submit = false;
      for (auto const& st : t.steps) {
        if (st.node == "understand") understand = true;
        if (st.node == "submit") submit = true;
      }
      EXPECT_TRUE(understand) << t.event_id;
      EXPECT_TRUE(submit) << t.event_id;
    } else {
      for (auto const& st : t.steps) {
        EXPECT_NE(st.node, "understand") << t.event_id;
      }
    }
  }
}

TEST(SadMock, TasksHaveOutputs) {
  auto const s = run_scenario();
  EXPECT_FALSE(s.r.tasks.empty());
  for (auto const& t : s.r.tasks) {
    if (t.state == "completed") {
      EXPECT_FALSE(t.outputs.empty()) << t.id;
    }
  }
}

TEST(SadMock, SimulateGlobalMessageGoesThroughSad) {
  Event e;
  e.id = "sim-msg";
  e.source = "notification";
  e.type = "notification.message";
  e.severity = "warn";
  e.dedup_key = "sim-msg";
  e.payload = {{"app", "wechat"}, {"from", "妈妈"}, {"from_is_contact", true},
               {"intent", "question"}, {"text", "明天来吃饭吗？"}};
  auto const s = simulate_one(e);
  EXPECT_TRUE(s.ok);
  EXPECT_EQ(s.layer, "global");
  EXPECT_EQ(s.decision, "sad_task");
  EXPECT_FALSE(s.task_id.empty());
  ASSERT_FALSE(s.outputs.empty());
  EXPECT_EQ(s.outputs.front().tool, "read_message");
}

TEST(SadMock, SimulateLocalDownloadIsDirectTask) {
  Event e;
  e.id = "sim-dl";
  e.source = "download";
  e.type = "download.done";
  e.severity = "info";
  e.dedup_key = "sim-dl";
  e.payload = {{"file", "report.pdf"}};
  auto const s = simulate_one(e);
  EXPECT_TRUE(s.ok);
  EXPECT_EQ(s.layer, "local");
  EXPECT_EQ(s.decision, "direct_task");
  ASSERT_FALSE(s.outputs.empty());
  EXPECT_EQ(s.outputs.front().tool, "check_download");
}

TEST(SadMock, SimulateLocalDeviceSleepIsDirectTask) {
  Event e;
  e.id = "sim-dev";
  e.source = "device";
  e.type = "device.context";
  e.severity = "info";
  e.payload = {{"mode", "sleep"}};
  auto const s = simulate_one(e);
  EXPECT_TRUE(s.ok);
  EXPECT_EQ(s.layer, "local");
  EXPECT_EQ(s.decision, "direct_task");
  EXPECT_FALSE(s.task_id.empty());
  ASSERT_EQ(s.outputs.size(), 5u);
  EXPECT_EQ(s.outputs[0].tool, "set_volume");
  EXPECT_EQ(s.outputs[2].tool, "enable_dnd");
}

TEST(SadMock, SimulateNoiseIsDropped) {
  Event e;
  e.id = "sim-noise";
  e.source = "sensor";
  e.type = "sensor.motion";
  e.severity = "info";
  auto const s = simulate_one(e);
  EXPECT_TRUE(s.ok);
  EXPECT_EQ(s.decision, "dropped");
  EXPECT_TRUE(s.outputs.empty());
}

TEST(SadMock, TaskSpecValidation) {
  Json ok{{"goal", "处理事件"}, {"template", "reply_and_remind"}, {"priority", "P1"},
          {"dedup_key", "k1"}};
  EXPECT_TRUE(validate_task_spec(ok).empty());
  Json bad_tmpl = ok;
  bad_tmpl["template"] = "no_such_template";
  EXPECT_FALSE(validate_task_spec(bad_tmpl).empty());
  Json bad_goal = ok;
  bad_goal["goal"] = "";
  EXPECT_FALSE(validate_task_spec(bad_goal).empty());
}

TEST(SadMock, ReportHasDispositionsTasksTracesDags) {
  auto const s = run_scenario();
  EXPECT_EQ(s.r.events.size(), static_cast<std::size_t>(s.r.total));
  EXPECT_FALSE(s.r.tasks.empty());
  EXPECT_FALSE(s.r.sad_traces.empty());
  EXPECT_GE(dags_json().size(), 7u);
  EXPECT_FALSE(sad_dag_json().at("nodes").empty());
}
