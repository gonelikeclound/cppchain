// tests/wf —— project_drive 自管理 loop 第一刀（arch 27）
//   覆盖：规则档选第一个未完成 / 全完成停机 / 模型档决策（含选已 done 回落）/ 驱动循环跑完项目任务
//   （每任务独立 session + 子 workflow 动态分派 input.config 合并）/ loop 上界防病态。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

// 确定性决策模型：按脚本队列吐 {task, done, reason}。
class ScriptedDrive final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::size_t invoked = 0;
  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    ++invoked;
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
    return m;
  }
  std::generator<message::MessageChunk> stream(std::span<Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

struct DriveFixture {
  std::filesystem::path ws;
  std::filesystem::path proj_dir;
  DriveFixture()
      : ws(std::filesystem::temp_directory_path() /
           ("wf_drive_" + std::to_string(static_cast<unsigned>(std::random_device{}())))),
        proj_dir(ws / ".wfr" / "projects" / "monthly_ops") {
    std::filesystem::create_directories(proj_dir / "tasks");
    write("project.json",
          R"({"id":"monthly_ops","goal":"产出本月运营综述",
              "completion_criteria":"全部产物落盘",
              "discipline":["数字一致性"],
              "tasks":[
                {"id":"weekly","goal":"周报","steps":[{"id":"s1","goal":"写周报","verify":{"kind":"probe","probe":"file","path":".out/done_weekly.md","contains":"done"}}]},
                {"id":"monthly","goal":"月报","steps":[{"id":"s1","goal":"写月报","verify":{"kind":"probe","probe":"file","path":".out/done_monthly.md","contains":"done"}}]},
                {"id":"review","goal":"复盘","steps":[{"id":"s1","goal":"做复盘","verify":{"kind":"probe","probe":"file","path":".out/done_review.md","contains":"done"}}]}
              ]})");
    write("lessons.json", "[]");
  }
  ~DriveFixture() {
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
  }
  void write(std::string const& rel, std::string const& text) const {
    std::ofstream out(proj_dir / rel);
    out << text;
  }
  // 造"该任务已产出"的证据文件（verify probe 判 done 的磁盘依据）
  void mark_product(std::string const& tid) const {
    std::error_code ec;
    std::filesystem::create_directories(ws / ".out", ec);
    {
      std::ofstream out(ws / ".out" / ("done_" + tid + ".md"));
      out << "done for " << tid << "\n";
    }
  }
  Json progress() const {
    std::ifstream in(proj_dir / "project.json");
    Json j;
    in >> j;
    return j.value("progress", Json::object());
  }
  void seed_progress(Json const& p) const {
    std::ifstream in(proj_dir / "project.json");
    Json j;
    in >> j;
    j["progress"] = p;
    std::ofstream out(proj_dir / "project.json");
    out << j.dump();
  }
};

// 单个 drive 节点（规则/模型档单元用）
Json drive_only_ir() {
  return Json::parse(R"json({
    "id": "wf_drive_only",
    "nodes": [
      {"id": "drive", "kind": "project_drive", "project": "monthly_ops", "model": "m",
       "system_prompt": "", "next": "_turn_end"}
    ]
  })json");
}

// 驱动循环：drive → gate → run(subworkflow child)（integration 用）
Json drive_loop_ir() {
  return Json::parse(R"json({
    "id": "wf_drive_loop",
    "nodes": [
      {"id": "loop", "kind": "loop", "max": 10, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "drive", "kind": "project_drive", "project": "monthly_ops", "next": "gate"},
         {"id": "gate", "kind": "expr", "expr": "$drive.result.next_task != null",
          "then": "run", "else": "_break"},
         {"id": "run", "kind": "subworkflow", "workflow": "wf_drive_child", "mode": "sync",
          "input": {"config": {"task": "$drive.result.next_task",
                               "project": "monthly_ops",
                               "plan_steps": "$drive.result.plan_steps",
                               "goal": "$drive.result.goal"}},
          "next": "_continue"}
       ]}
    ]
  })json");
}

// 子 workflow：单步探针任务（probe 文件存在 → done）。全离线、无模型、无工具。
std::string child_json() {
  return R"json({
    "id": "wf_drive_child",
    "nodes": [
      {"id": "p_begin", "kind": "project", "project": "@config.project", "phase": "begin", "next": "t_begin"},
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 1, "next": "t_end", "on_max": "t_end",
       "body": [
         {"id": "ann", "kind": "verify", "phase": "announce", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]},
      {"id": "t_end", "kind": "task", "task": "@config.task", "phase": "end", "next": "p_end"},
      {"id": "p_end", "kind": "project", "project": "@config.project", "phase": "end", "next": "_turn_end"}
    ]
  })json";
}

std::optional<Json> fire_parsed(Json ir, Json config, CompileOptions const& copts) {
  auto parsed = WorkflowIR::parse(ir);
  if (!parsed) return std::nullopt;
  auto v = validate(*parsed, ValidatorOptions{});
  if (!v.ok()) return std::nullopt;
  tool::ToolRegistry reg;
  auto wf = compile_workflow(*parsed, reg, nullptr, copts);
  if (!wf) return std::nullopt;
  auto r = fire_once(*wf, config);
  if (!r) return std::nullopt;
  return std::optional<Json>{*r};
}

}  // namespace

// 规则档：空进度 → 第一个未完成任务（weekly），带该任务的 steps 种子
TEST(WfProjectDrive, rule_selects_first_unfinished) {
  DriveFixture fx;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");
  EXPECT_EQ(d["done"], false);
  EXPECT_NE(d["reason"].get<std::string>().find("顺序推进"), std::string::npos);
  EXPECT_FALSE(d["plan_steps"].get<std::string>().empty());  // tasks[].steps 种子带出
}

// 规则档：全部 done → next_task=null（停机判据）
TEST(WfProjectDrive, rule_stops_when_all_done) {
  DriveFixture fx;
  fx.seed_progress(Json{{"weekly", "done"}, {"monthly", "done"}, {"review", "done"}});
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["next_task"].is_null());
  EXPECT_EQ(d["done"], true);
}

// 模型档：模型挑 monthly（非规则顺序）
TEST(WfProjectDrive, model_selects_task) {
  DriveFixture fx;
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"monthly","done":false,"reason":"月报最要紧"})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "monthly");
  EXPECT_NE(d["reason"].get<std::string>().find("模型档"), std::string::npos);
}

// 模型档：选已 done 任务 → 拒绝并回落规则档（fail-closed 语义）
TEST(WfProjectDrive, model_picking_done_task_falls_back_to_rule) {
  DriveFixture fx;
  fx.seed_progress(Json{{"weekly", "done"}});  // weekly 已 done，monthly/review 未开始
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"想重做周报"})"));  // 选已 done
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "monthly");  // 规则档兜底：第一个未完成
  EXPECT_NE(d["reason"].get<std::string>().find("回落"), std::string::npos);
}

// 驱动循环：3 任务经 subworkflow 动态分派全部跑完（progress 全 done）、末轮停机、loop 未触顶
TEST(WfProjectDrive, loop_runs_all_tasks_and_stops) {
  DriveFixture fx;
  for (std::string const& tid : {"weekly", "monthly", "review"}) fx.mark_product(tid);
  CompileOptions copts;
  copts.subworkflow_loader = [](std::string const& spec) -> std::optional<Json> {
    if (spec == "wf_drive_child") return Json::parse(child_json());
    return std::nullopt;
  };
  auto st = fire_parsed(drive_loop_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  // 三个任务 progress 全 done，末轮停机
  EXPECT_EQ(fx.progress()["weekly"], "done");
  EXPECT_EQ(fx.progress()["monthly"], "done");
  EXPECT_EQ(fx.progress()["review"], "done");
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["done"] == true);
  EXPECT_TRUE(d["next_task"].is_null());
  // 子 workflow 动态分派：任务档案落在项目目录（input.config 合并进子 config，主 workspace 未丢）
  for (std::string const& tid : {"weekly", "monthly", "review"})
    EXPECT_TRUE(std::filesystem::exists(fx.proj_dir / "tasks" / (tid + ".json"))) << tid;
}

// 防病态：任务永不 done（probe 文件缺）→ drive 反复选同一任务，loop max 兜底收尾（不挂死）
TEST(WfProjectDrive, loop_max_bounds_runaway) {
  DriveFixture fx;
  // 不 mark_product：weekly 永远 in_progress
  CompileOptions copts;
  copts.subworkflow_loader = [](std::string const& spec) -> std::optional<Json> {
    if (spec == "wf_drive_child") return Json::parse(child_json());
    return std::nullopt;
  };
  // 缩减 max：把 loop.max 改成 2 验证兜底（模板默认 10，测试用小值）
  auto ir = drive_loop_ir();
  ir["nodes"][0]["max"] = 2;
  auto st = fire_parsed(ir, Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  // 触顶收尾（on_max=_turn_end）：weekley 仍 not done，但整体正常结束（没挂死/没抛异常）
  EXPECT_EQ(fx.progress()["weekly"], "in_progress");
  EXPECT_TRUE((*st).contains("results"));
}