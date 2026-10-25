// tests/wf —— project 自管理（arch 26）：project 节点 begin/end + task 档案路径下沉
//   覆盖：project.json 读取与 fail-closed（缺档 / 任务不在列表）、项目级上下文注入
//   （system_context 含 目标/完成标准/纪律/经验/本任务）、state.project 快照、
//   @config.project 归档下沉（+.history 同目录）、旧路径兼容回归、
//   end 写回 lessons.json 与 project.json.progress。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

// 临时工作区：预置 .wfr/projects/monthly_ops/（project.json + lessons.json + tasks/）。
struct ProjFixture {
  std::filesystem::path ws;
  std::filesystem::path proj_dir;
  ProjFixture()
      : ws(std::filesystem::temp_directory_path() /
           ("wf_proj_" + std::to_string(static_cast<unsigned>(std::random_device{}())))),
        proj_dir(ws / ".wfr" / "projects" / "monthly_ops") {
    std::filesystem::create_directories(proj_dir / "tasks");
    write("project.json",
          R"({"id":"monthly_ops","goal":"产出本月运营综述",
              "completion_criteria":"每个任务产物落盘且互相衔接，数字前后一致",
              "discipline":["每任务产出落盘 .out/","数字一致性"],
              "tasks":[{"id":"weekly","name":"周报"},{"id":"monthly","name":"月报"}]})");
    write("lessons.json",
          R"([{"when":"2026-10-20","applicable_if":"做周报","conclusion":"先出图再写文字","evidence":"试点" }])");
  }
  ~ProjFixture() {
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
  }
  void write(std::string const& rel, std::string const& text) const {
    std::ofstream out(proj_dir / rel);
    out << text;
  }
};

// 项目 begin 直接触发的工作流（project 名走 @config.project）。
Json begin_only_ir() {
  return Json::parse(R"json({
    "id": "wf_proj_begin",
    "nodes": [
      {"id": "p", "kind": "project", "phase": "begin", "next": "_turn_end"}
    ]
  })json");
}

// 完整链：project begin → task begin → task end → project end（任务档案走 @config.project 下沉）。
Json chain_ir() {
  return Json::parse(R"json({
    "id": "wf_proj_chain",
    "nodes": [
      {"id": "p_begin", "kind": "project", "project": "monthly_ops", "phase": "begin", "next": "t_begin"},
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "t_end"},
      {"id": "t_end",   "kind": "task", "task": "@config.task", "phase": "end",   "next": "p_end"},
      {"id": "p_end",   "kind": "project", "project": "monthly_ops", "phase": "end", "next": "_turn_end"}
    ]
  })json");
}

// 仅 task 档案（兼容回归用：不带 project 节点、不传 @config.project）。
Json task_only_ir() {
  return Json::parse(R"json({
    "id": "wf_proj_task_only",
    "nodes": [
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "_turn_end"}
    ]
  })json");
}

struct Run {
  std::optional<Json> state;
  std::string error;
};

Run fire(Json ir, Json config) {
  auto parsed = WorkflowIR::parse(ir);
  if (!parsed) return {std::nullopt, "parse: " + parsed.error()};
  auto v = validate(*parsed, ValidatorOptions{});
  if (!v.ok()) return {std::nullopt, "validate: " + v.describe()};
  tool::ToolRegistry reg;
  auto wf = compile_workflow(*parsed, reg);
  if (!wf) return {std::nullopt, "compile: " + wf.error().message};
  auto r = fire_once(*wf, config);
  if (!r) return {std::nullopt, "fire: " + r.error().message};
  return {std::optional<Json>{*r}, ""};
}

}  // namespace

// begin：读 project.json → 校验任务 ∈ tasks → 拼 system_context（目标/纪律/经验/本任务）
TEST(WfProject, begin_injects_project_context) {
  ProjFixture fx;
  auto r = fire(begin_only_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}, {"task", "weekly"}});
  ASSERT_TRUE(r.state) << r.error;
  // system_context：project begin 拼的 system 消息数组（shape 与 Context 节点一致）
  ASSERT_TRUE((*r.state)[kSystemContextKey].is_array());
  std::string all;
  for (Json const& m : (*r.state)[kSystemContextKey])
    if (m.contains("content") && m["content"].is_array())
      for (Json const& b : m["content"])
        if (b.is_object() && b.contains("text")) all += b["text"].get<std::string>() + "\n";
  EXPECT_NE(all.find("本月运营综述"), std::string::npos) << all;       // 目标
  EXPECT_NE(all.find("完成标准"), std::string::npos) << all;           // 完成标准
  EXPECT_NE(all.find("数字一致性"), std::string::npos) << all;         // 纪律
  EXPECT_NE(all.find("先出图再写文字"), std::string::npos) << all;     // lessons 经验
  EXPECT_NE(all.find("【本任务】weekly"), std::string::npos) << all;   // 本任务
  // state.project 全量快照 + results
  EXPECT_EQ((*r.state)["project"]["id"], "monthly_ops");
  EXPECT_EQ((*r.state)["results"]["p"]["action"], "begin");
  EXPECT_EQ((*r.state)["results"]["p"]["lessons"], 1L);
}

// fail-closed：project 名不对（无档案）→ 运行期终止
TEST(WfProject, begin_fails_when_project_archive_missing) {
  ProjFixture fx;
  auto r = fire(begin_only_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "nope"}, {"task", "weekly"}});
  ASSERT_FALSE(r.state);
  EXPECT_NE(r.error.find("缺 project 档案"), std::string::npos) << r.error;
}

// fail-closed：任务不在 project.tasks 列表 → 运行期终止
TEST(WfProject, begin_fails_when_task_not_in_list) {
  ProjFixture fx;
  auto r = fire(begin_only_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}, {"task", "ghost"}});
  ASSERT_FALSE(r.state);
  EXPECT_NE(r.error.find("不在 project"), std::string::npos) << r.error;
}

// @config.project 非空 → task 档案（+task.revision .history 快照）下沉到项目目录
TEST(WfProject, task_archive_downsinked_under_project) {
  ProjFixture fx;
  auto r = fire(chain_ir(),
                Json{{"workspace", fx.ws.string()},
                     {"project", "monthly_ops"},  // 触发归档下沉
                     {"task", "weekly"},
                     {"goal", "周报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写周报"}] )"}});
  ASSERT_TRUE(r.state) << r.error;
  // 下沉：project 目录下有档案，旧路径没有
  std::filesystem::path const down = fx.proj_dir / "tasks" / "weekly.json";
  EXPECT_TRUE(std::filesystem::exists(down));
  EXPECT_FALSE(std::filesystem::exists(fx.ws / ".wfr" / "tasks" / "weekly.json"));
  // .history 快照同目录（task end 写）
  EXPECT_TRUE(std::filesystem::exists(fx.proj_dir / "tasks" / "weekly.history" / "batch1.json"));
  // begin/end 都走了 project 路径（batch_no=1，档案可读）
  auto const arch = [&] {
    std::ifstream in(down);
    Json j; in >> j; return j;
  }();
  EXPECT_EQ(arch.value("task", ""), "weekly");
  EXPECT_EQ(arch.value("batch_no", 0L), 1L);
}

// 兼容回归：无 @config.project → 档案仍走旧 .wfr/tasks/（长程零改动）
TEST(WfProject, task_archive_legacy_path_without_project) {
  ProjFixture fx;
  // 只用 task 节点（无 project 节点，任务不需要在 project.tasks 里）
  auto r = fire(task_only_ir(),
                Json{{"workspace", fx.ws.string()},
                     {"task", "legacy_t1"},
                     {"goal", "旧任务"},
                     {"plan_steps", R"([{"id":"s1","goal":"x"}] )"}});
  ASSERT_TRUE(r.state) << r.error;
  EXPECT_TRUE(std::filesystem::exists(fx.ws / ".wfr" / "tasks" / "legacy_t1.json"));
  EXPECT_FALSE(std::filesystem::exists(fx.proj_dir / "tasks" / "legacy_t1.json"));
}

// end：state.lesson（或 @config.lesson）→ lessons.json 追加；progress[task] 更新
TEST(WfProject, end_writes_lesson_and_progress) {
  ProjFixture fx;
  // @config.lesson 通道（宿主/任务配置提供；state.lesson 槽同构）
  auto r = fire(chain_ir(),
                Json{{"workspace", fx.ws.string()},
                     {"task", "weekly"},
                     {"goal", "周报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写周报"}] )"},
                     {"lesson", Json{{"applicable_if", "做周报时"},
                                     {"conclusion", "数据源先核对再落笔"}}}});
  ASSERT_TRUE(r.state) << r.error;
  // lessons.json 追加 1 条（原 1 → 2）
  auto const lessons = [&] {
    std::ifstream in(fx.proj_dir / "lessons.json");
    Json j; in >> j; return j;
  }();
  ASSERT_TRUE(lessons.is_array());
  ASSERT_EQ(lessons.size(), 2u);
  EXPECT_EQ(lessons[1]["applicable_if"], "做周报时");
  EXPECT_EQ(lessons[1]["conclusion"], "数据源先核对再落笔");
  EXPECT_FALSE(lessons[1].value("when", "").empty());  // when = 今天日期
  // progress[weekly]：plan cursor(0) < steps(1) → in_progress（确定断言）
  auto const po = [&] {
    std::ifstream in(fx.proj_dir / "project.json");
    Json j; in >> j; return j;
  }();
  EXPECT_EQ(po["progress"]["weekly"], "in_progress");
  // project end results 携带状态
  EXPECT_EQ((*r.state)["results"]["p_end"]["status"], "in_progress");
  EXPECT_EQ((*r.state)["results"]["p_end"]["lesson_written"], true);
}

// 不带 lesson 的 end → 不写 lessons.json（保持 1 条）、progress 照常
TEST(WfProject, end_skips_lesson_when_absent) {
  ProjFixture fx;
  auto r = fire(chain_ir(),
                Json{{"workspace", fx.ws.string()},
                     {"task", "monthly"}, {"goal", "月报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写月报"}] )"}});
  ASSERT_TRUE(r.state) << r.error;
  auto const lessons = [&] {
    std::ifstream in(fx.proj_dir / "lessons.json");
    Json j; in >> j; return j;
  }();
  ASSERT_EQ(lessons.size(), 1u);  // 未追加
  EXPECT_EQ((*r.state)["results"]["p_end"]["lesson_written"], false);
}

// 校验形状：phase 非 begin|end → VN25；缺 next → VN25
TEST(WfProject, validation_rejects_bad_phase) {
  auto ir = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_bad", "nodes": [
      {"id": "p", "kind": "project", "phase": "middle", "next": "_turn_end"}
    ]
  })json"));
  ASSERT_TRUE(ir);
  ValidationResult v = validate(*ir, {});
  ASSERT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VN25"), std::string::npos);
}