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
  std::vector<std::string> last_humans;  // 每次 invoke 的最后 human 文本（简报可见性审计）
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    ++invoked;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Human) {
        last_humans.push_back(it->text());
        break;
      }
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
  // 覆盖 project.json 的 tasks（经验加权顺序测试用），extra 可并入顶层键
  void set_project_tasks(Json tasks, Json const& extra = Json::object()) const {
    std::ifstream in(proj_dir / "project.json");
    Json j;
    in >> j;
    j["tasks"] = std::move(tasks);
    for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
    std::ofstream out(proj_dir / "project.json");
    out << j.dump();
  }
  void write_lessons(Json lessons) const {
    std::ofstream out(proj_dir / "lessons.json");
    out << lessons.dump();
  }
};

// 构造单步探针任务（deps 可选；steps 单步 probe 判 done）
Json probe_task(std::string const& id, std::string const& goal, Json deps) {
  Json verify = Json{{"kind", "probe"}, {"probe", "file"},
                     {"path", ".out/done_" + id + ".md"}, {"contains", "done"}};
  Json step = Json{{"id", "s1"}, {"goal", goal}, {"verify", std::move(verify)}};
  return Json{{"id", id},
              {"goal", goal},
              {"deps", std::move(deps)},
              {"steps", Json::array({std::move(step)})}};
}

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

// ---- meta 从 lessons 学习改进决策（arch 27 第二刀）----

// 经验加权：文件序 monthly 在前，但一条 applicable_if 命中 "weekly" 的经验 → 规则档先做 weekly
TEST(WfProjectDrive, rule_lesson_weighting_reorders) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array(
      {Json{{"id", "monthly"}, {"goal", "月报"}, {"steps", Json::array()}},
       Json{{"id", "review"}, {"goal", "复盘"}, {"steps", Json::array()}},
       Json{{"id", "weekly"}, {"goal", "周报"}, {"steps", Json::array()}}}));
  fx.write_lessons(Json::array({Json{{"when", "2026-10-24"}, {"applicable_if", "weekly"},
                                     {"conclusion", "周报务必先做"}, {"evidence", "上次体验"}}}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");  // 经验把 weekly 提到最前
  EXPECT_NE(d["reason"].get<std::string>().find("经验优先"), std::string::npos);
  EXPECT_EQ(d["lessons_used"], 1L);
}

// 通用经验（applicable_if 空）：总是相关，但不改变文件序（无 weighted 目标）
TEST(WfProjectDrive, rule_generic_lesson_reported_without_reorder) {
  DriveFixture fx;
  fx.write_lessons(Json::array({Json{{"when", "2026-10-24"},
                                     {"applicable_if", ""},
                                     {"conclusion", "先确认数据源再落笔"},
                                     {"evidence", ""}}}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");  // 无命中任务 → 仍按文件序
  EXPECT_EQ(d["lessons_used"], 1L);     // 但计数（简报/模型档可见）
  EXPECT_EQ(d["reason"].get<std::string>().find("顺序推进") != std::string::npos, true);
}

// 无关经验被过滤：related=0、lessons_used=0，选择不受影响
TEST(WfProjectDrive, rule_ignores_unrelated_lessons) {
  DriveFixture fx;
  fx.write_lessons(Json::array({Json{{"when", "2026-10-24"},
                                     {"applicable_if", "洋务运动"},
                                     {"conclusion", "无关经验"},
                                     {"evidence", ""}}}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");
  EXPECT_EQ(d["lessons_used"], 0L);
}

// 加权只作用于未完成任务：weekly 已 done → 经验命中 weekly 也不插手，规则选第一个未完成（monthly）
TEST(WfProjectDrive, rule_lesson_weight_skips_done) {
  DriveFixture fx;
  fx.seed_progress(Json{{"weekly", "done"}});
  fx.write_lessons(Json::array({Json{{"when", "2026-10-24"}, {"applicable_if", "weekly"},
                                     {"conclusion", "周报优先"}, {"evidence", ""}}}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "monthly");  // weekly done，规则未完成第一个是 monthly
  EXPECT_EQ(d["reason"].get<std::string>().find("顺序推进") != std::string::npos, true);
}

// 模型档：简报含【项目经验】段且只含相关条目；lessons_used 对齐；模型 reason 可引用 [lesson N]
TEST(WfProjectDrive, model_brief_includes_related_lessons) {
  DriveFixture fx;
  fx.write_lessons(Json::array(
      {Json{{"when", "2026-10-24"}, {"applicable_if", "weekly"},
            {"conclusion", "周报务必先做"}, {"evidence", "上次"}},
       Json{{"when", "2026-10-23"}, {"applicable_if", "洋务运动"},
            {"conclusion", "无关"}, {"evidence", ""}}}));
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"按 [lesson 1]：周报务必先做"})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");
  EXPECT_EQ(d["lessons_used"], 1L);  // 只计相关
  // 简报含相关经验、不含无关经验
  ASSERT_FALSE(mdl.last_humans.empty());
  std::string const brief = mdl.last_humans.back();
  EXPECT_NE(brief.find("【项目经验（lessons"), std::string::npos) << brief;
  EXPECT_NE(brief.find("周报务必先做"), std::string::npos) << brief;
  EXPECT_EQ(brief.find("洋务运动"), std::string::npos) << brief;
  // 模型选 weekly + reason 引用 [lesson 1]（审计锚点）
  EXPECT_NE(d["reason"].get<std::string>().find("[lesson 1]"), std::string::npos);
}

// ---- 任务依赖图调度（arch 27 第三刀）----

// 依赖门槛：weekly 文件序在前，但 deps:[monthly] 未完成 → 规则档不选它，选就绪的 monthly
TEST(WfProjectDrive, rule_dependency_gates_earlier_task) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("weekly", "周报", Json::array({"monthly"})),
                                    probe_task("monthly", "月报", Json::array())}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "monthly");     // 挡掉 weekly，选依赖已就绪的 monthly
  EXPECT_EQ(d["ready"], 1L);
  EXPECT_EQ(d["blocked"], false);
  EXPECT_FALSE(d["reason"].get<std::string>().empty());
}

// 解锁：dep 完成后 weekly 就绪 → 下次 drive 选它
TEST(WfProjectDrive, rule_dependency_unlocks_once_dep_done) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("weekly", "周报", Json::array({"monthly"})),
                                    probe_task("monthly", "月报", Json::array())}));
  fx.seed_progress(Json{{"monthly", "done"}});
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");      // monthly 已 done → weekly 依赖满足
  EXPECT_EQ(d["done"], false);
}

// 环（A↔B 互依、均未完成）→ 依赖未就绪阻塞停机（不静默死循环）
TEST(WfProjectDrive, rule_stops_blocked_on_cycle) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("a", "A", Json::array({"b"})),
                                    probe_task("b", "B", Json::array({"a"}))}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["next_task"].is_null());
  EXPECT_EQ(d["done"], false);              // 非"全部完成"停机
  EXPECT_EQ(d["blocked"], true);            // 依赖阻塞停机
  EXPECT_NE(d["reason"].get<std::string>().find("依赖未就绪"), std::string::npos) << d["reason"];
}

// 缺失依赖（deps 指向不存在的任务）→ fail-closed 阻塞停机
TEST(WfProjectDrive, rule_stops_blocked_on_missing_dep) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("weekly", "周报", Json::array({"ghost"}))}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["next_task"].is_null());
  EXPECT_EQ(d["blocked"], true);
}

// 模型选了个依赖未就绪的任务 → 拒绝并回落规则档（就绪候选）
TEST(WfProjectDrive, model_rejects_dep_not_ready_falls_back) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("weekly", "周报", Json::array({"monthly"})),
                                    probe_task("monthly", "月报", Json::array())}));
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"想先做周报"})"));  // 依赖 monthly 未 done
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "monthly");     // 规则档兜底选就绪任务
  EXPECT_NE(d["reason"].get<std::string>().find("依赖未就绪"), std::string::npos) << d["reason"];
}

// 集成：依赖链任务经 drive 循环全部按序完成（monthly → weekly → review）
TEST(WfProjectDrive, loop_runs_tasks_in_dependency_order) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array(
      {probe_task("monthly", "月报", Json::array()),
       probe_task("weekly", "周报", Json::array({"monthly"})),
       probe_task("review", "复盘", Json::array({"monthly", "weekly"}))}));
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
  EXPECT_EQ(fx.progress()["monthly"], "done");
  EXPECT_EQ(fx.progress()["weekly"], "done");
  EXPECT_EQ(fx.progress()["review"], "done");
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["done"] == true);
  EXPECT_TRUE(d["next_task"].is_null());
}

// ---- 自适应 plan_steps（arch 27 第四刀）----

// 规则档：无/空预置 steps → 单步目标兜底（不再 missing_seed 空转）
TEST(WfProjectDrive, rule_fallback_single_step_plan_without_seed) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array(
      {Json{{"id", "weekly"}, {"goal", "周报"}, {"deps", Json::array()}},
       Json{{"id", "monthly"}, {"goal", "月报"}, {"deps", Json::array()}}}));
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        CompileOptions{});
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");
  EXPECT_EQ(d["plan_source"], "fallback");
  EXPECT_NE(d["reason"].get<std::string>().find("自适应单步兜底"), std::string::npos);
  auto seed = Json::parse(d["plan_steps"].get<std::string>());
  ASSERT_TRUE(seed.is_array());
  ASSERT_EQ(seed.size(), 1u);
  EXPECT_EQ(seed[0]["id"], "s1");
  EXPECT_EQ(seed[0]["goal"], "周报");
}

// 模型档：无预置 steps → 模型产 plan_steps → 用模型计划（plan_source=model）
TEST(WfProjectDrive, model_plans_for_seedless_task) {
  DriveFixture fx;
  fx.set_project_tasks(
      Json::array({Json{{"id", "weekly"}, {"goal", "周报"}, {"deps", Json::array()}}}));
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"先周报",
          "plan_steps":[{"id":"p1","goal":"先核对数据源"},{"id":"p2","goal":"再写周报"}]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["next_task"], "weekly");
  EXPECT_EQ(d["plan_source"], "model");
  auto seed = Json::parse(d["plan_steps"].get<std::string>());
  ASSERT_TRUE(seed.is_array());
  ASSERT_EQ(seed.size(), 2u);
  EXPECT_EQ(seed[0]["id"], "p1");
  EXPECT_EQ(seed[1]["goal"], "再写周报");
}

// 模型档：plan_steps 非法 → 回落单步兜底（fail-closed）
TEST(WfProjectDrive, model_plan_invalid_falls_back_single) {
  DriveFixture fx;
  fx.set_project_tasks(
      Json::array({Json{{"id", "weekly"}, {"goal", "周报"}, {"deps", Json::array()}}}));
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"先周报","plan_steps":"not-an-array"})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["plan_source"], "fallback");
  auto seed = Json::parse(d["plan_steps"].get<std::string>());
  ASSERT_EQ(seed.size(), 1u);
}

// 预置 steps 优先：模型即使给了 plan_steps 也不覆盖预置（幂等可预期）
TEST(WfProjectDrive, preset_steps_beat_model_plan) {
  DriveFixture fx;
  fx.set_project_tasks(Json::array({probe_task("weekly", "周报", Json::array())}));
  ScriptedDrive mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"task":"weekly","done":false,"reason":"先周报",
          "plan_steps":[{"id":"x1","goal":"模型想改计划"}]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire_parsed(drive_only_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  Json const& d = (*st)["results"]["drive"];
  EXPECT_EQ(d["plan_source"], "preset");       // 预置优先
  EXPECT_NE(d["plan_steps"].get<std::string>().find("done_weekly"), std::string::npos);
}

// 集成：无预置 steps 的任务经 drive 循环建档并跑完（单步兜底 → unverified 诚实推进 → done，不 starvation）
TEST(WfProjectDrive, loop_runs_seedless_task_without_starvation) {
  DriveFixture fx;
  fx.set_project_tasks(
      Json::array({Json{{"id", "weekly"}, {"goal", "周报"}, {"deps", Json::array()}}}));
  CompileOptions copts;
  copts.subworkflow_loader = [](std::string const& spec) -> std::optional<Json> {
    if (spec == "wf_drive_child") return Json::parse(child_json());
    return std::nullopt;
  };
  auto st = fire_parsed(drive_loop_ir(),
                        Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"}},
                        copts);
  ASSERT_TRUE(st);
  EXPECT_EQ(fx.progress()["weekly"], "done");   // 建档 + 推进 + end 写回
  EXPECT_TRUE(std::filesystem::exists(fx.proj_dir / "tasks" / "weekly.json"));
  Json const& d = (*st)["results"]["drive"];
  EXPECT_TRUE(d["done"] == true);               // 末轮停机（不是 blocked）
}