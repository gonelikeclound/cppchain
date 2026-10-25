// tests/wf —— wfr cron 批次装配（arch 27 §十：project 感知）
//   覆盖：legacy（无 project → lot，现状兼容）/ project 模式（lot_project + 两段 session +
//   plan_steps 注入）/ plan_steps 兜底（无预置 steps → 单步）/ job.goal 覆盖 / session 显式覆盖 /
//   task 缺失 → 空 args。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cppchain/workflow/wfr_lookup.hpp"

using namespace cppchain::workflow;

namespace {

struct CronFixture {
  std::filesystem::path ws;
  CronFixture()
      : ws(std::filesystem::temp_directory_path() /
           ("wf_cron_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(ws / ".wfr" / "projects" / "monthly_ops");
    std::ofstream out(ws / ".wfr" / "projects" / "monthly_ops" / "project.json");
    out << R"({"id":"monthly_ops","goal":"运营综述","tasks":[
          {"id":"weekly","goal":"周报","steps":[{"id":"s1","goal":"写周报"}]},
          {"id":"seedless","goal":"无种子任务"}]})";
  }
  ~CronFixture() {
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
  }
  bool has_arg(std::vector<std::string> const& args, std::string const& prefix) const {
    for (std::string const& a : args)
      if (a.rfind(prefix, 0) == 0) return true;
    return false;
  }
  std::string arg_value(std::vector<std::string> const& args, std::string const& key) const {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == key) return args[i + 1];
    return {};
  }
  // wfr 的 --set 是两元素（"--set", "<key>=<value>"）
  bool set_has(std::vector<std::string> const& args, std::string const& key) const {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == "--set" && args[i + 1].rfind(key + "=", 0) == 0) return true;
    return false;
  }
  std::string set_val(std::vector<std::string> const& args, std::string const& key) const {
    std::string const p = key + "=";
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == "--set" && args[i + 1].rfind(p, 0) == 0) return args[i + 1].substr(p.size());
    return {};
  }
};

}  // namespace

// 现状兼容：无 project → 触发 lot，session 缺省 task，不带 project/plan_steps
TEST(WfCronBatch, legacy_job_triggers_lot) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(),
                          Json{{"task", "weekly"}, {"every_minutes", 30}},
                          "/usr/bin/wfr");
  EXPECT_FALSE(b.project_mode);
  EXPECT_EQ(b.workflow, "lot");
  EXPECT_EQ(b.session, "weekly");
  EXPECT_EQ(b.args[0], "/usr/bin/wfr");
  EXPECT_TRUE(fx.set_has(b.args, "task"));
  EXPECT_TRUE(fx.set_has(b.args, "approve_policy"));
  EXPECT_FALSE(fx.set_has(b.args, "project"));
  EXPECT_FALSE(fx.set_has(b.args, "plan_steps"));
}

// project 模式：lot_project + 两段 session + plan_steps（读 project.json 预置 steps）+ 任务 goal
TEST(WfCronBatch, project_job_reads_steps_and_goal) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(),
                          Json{{"task", "weekly"}, {"project", "monthly_ops"}, {"every_minutes", 30}},
                          "/usr/bin/wfr");
  EXPECT_TRUE(b.project_mode);
  EXPECT_EQ(b.workflow, "lot_project");
  EXPECT_EQ(b.session, "monthly_ops.weekly");  // 两段（每任务独立对话）
  EXPECT_TRUE(fx.set_has(b.args, "project"));
  // plan_steps = 预置 steps dump
  std::string const seed = fx.set_val(b.args, "plan_steps");
  EXPECT_NE(seed.find("写周报"), std::string::npos) << seed;
  // goal 缺省取任务 goal
  EXPECT_EQ(fx.set_val(b.args, "goal"), "周报");
}

// 无预置 steps → 单步兜底 plan（与 drive 自适应一致，免 missing_seed 空转）
TEST(WfCronBatch, project_job_falls_back_single_step_plan) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(),
                          Json{{"task", "seedless"}, {"project", "monthly_ops"},
                               {"every_minutes", 30}},
                          "/usr/bin/wfr");
  EXPECT_EQ(b.workflow, "lot_project");
  auto seed = Json::parse(fx.set_val(b.args, "plan_steps"));
  ASSERT_TRUE(seed.is_array());
  ASSERT_EQ(seed.size(), 1u);
  EXPECT_EQ(seed[0]["id"], "s1");
  EXPECT_EQ(seed[0]["goal"], "无种子任务");
}

// job.goal 显式覆盖任务 goal
TEST(WfCronBatch, job_goal_overrides_task_goal) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(),
                          Json{{"task", "weekly"}, {"project", "monthly_ops"},
                               {"every_minutes", 30}, {"goal", "本次聚焦数字"}},
                          "/usr/bin/wfr");
  EXPECT_TRUE(fx.set_val(b.args, "goal") == "本次聚焦数字");
}

// session 显式覆盖两段缺省
TEST(WfCronBatch, explicit_session_wins) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(),
                          Json{{"task", "weekly"}, {"project", "monthly_ops"},
                               {"every_minutes", 30}, {"session", "my-sess"}},
                          "/usr/bin/wfr");
  EXPECT_EQ(b.session, "my-sess");
  EXPECT_EQ(fx.arg_value(b.args, "--session"), "my-sess");
}

// 无效 job：task 缺失 → args 空（cron 分支跳过）
TEST(WfCronBatch, empty_args_when_task_missing) {
  CronFixture fx;
  auto b = wfr_cron_batch(fx.ws.string(), Json{{"every_minutes", 30}}, "/usr/bin/wfr");
  EXPECT_TRUE(b.args.empty());
}