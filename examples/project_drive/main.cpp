// demo: project_drive —— 自管理 loop 第一刀（arch 27，纯离线）
//
// 演示"meta 决定下一个任务 → 动态分派跑一批 → 收口 → 再决定"的调度循环：
//   · project_drive 节点（规则档，无模型）读 project.json 的 progress，
//     选第一个未完成任务（带该任务 plan steps 种子）或停机（全部 done）；
//   · expr 门：next_task != null → subworkflow(sync) 跑子任务 workflow，否则 _break 收尾；
//   · 子 workflow 由 `input.config` 逐键合并进子 config（task/project/plan_steps），
//     子 task begin 建档、verify probe（文件探针）判 done、project end 写回 progress；
//   · loop 上界（这里 10）做防病态兜底。
// 全离线：probe 判据的产物文件由本 demo 预置（模拟任务"已产出"），子 workflow 无模型无工具。
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/validator.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <system_error>

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

Json drive_loop_json() {
  return Json::parse(R"json({
    "id": "wf_drive_demo",
    "nodes": [
      {"id": "loop", "kind": "loop", "max": 10, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "drive", "kind": "project_drive", "project": "@config.project", "next": "gate"},
         {"id": "gate", "kind": "expr", "expr": "$drive.result.next_task != null",
          "then": "run", "else": "_break"},
         {"id": "run", "kind": "subworkflow", "workflow": "wf_drive_child", "mode": "sync",
          "input": {"config": {"task": "$drive.result.next_task",
                               "project": "@config.project",
                               "plan_steps": "$drive.result.plan_steps",
                               "goal": "$drive.result.goal"}},
          "next": "_continue"}
       ]}
    ]
  })json");
}

constexpr char kChildJson[] = R"json({
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

}  // namespace

int main() {
  // 工作区放仓库根 tmp/（项目纪律）。
  std::filesystem::path const root =
      std::filesystem::path("tmp") / ("project_drive_" +
                                      std::to_string(static_cast<unsigned>(std::random_device{}())));
  std::filesystem::path const proj = root / ".wfr" / "projects" / "monthly_ops";
  std::error_code ec;
  std::filesystem::create_directories(proj / "tasks", ec);
  std::filesystem::create_directories(root / ".out", ec);
  {
    std::ofstream w(proj / "project.json");
    w << R"({"id":"monthly_ops","goal":"产出本月运营综述：周报→月报→复盘 依次完成",
             "completion_criteria":"三份产物落盘且数字一致",
             "discipline":["数字一致性"],
             "tasks":[
               {"id":"weekly","goal":"周报","steps":[{"id":"s1","goal":"写周报","verify":{"kind":"probe","probe":"file","path":".out/done_weekly.md","contains":"done"}}]},
               {"id":"monthly","goal":"月报","steps":[{"id":"s1","goal":"写月报","verify":{"kind":"probe","probe":"file","path":".out/done_monthly.md","contains":"done"}}]},
               {"id":"review","goal":"复盘","steps":[{"id":"s1","goal":"做复盘","verify":{"kind":"probe","probe":"file","path":".out/done_review.md","contains":"done"}}]}
             ]})";
    std::ofstream l(proj / "lessons.json");
    l << "[]";
  }
  // 预置三份"已产出"证据（probe 判 done 的磁盘依据）。
  for (std::string const& tid : {"weekly", "monthly", "review"}) {
    std::ofstream out(root / ".out" / ("done_" + tid + ".md"));
    out << "done for " << tid << "\n";
  }

  auto ir = WorkflowIR::parse(drive_loop_json());
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 1;
  }
  auto v = validate(*ir, ValidatorOptions{});
  if (!v.ok()) {
    std::println("校验失败: {}", v.describe());
    return 1;
  }
  CompileOptions copts;
  copts.subworkflow_loader = [](std::string const& spec) -> std::optional<Json> {
    if (spec == "wf_drive_child") return Json::parse(kChildJson);
    return std::nullopt;
  };
  auto wf = compile_workflow(*ir, /*registry=*/tool::ToolRegistry{}, nullptr, copts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 1;
  }
  auto fire = fire_once(*wf, Json{{"workspace", root.string()}, {"project", "monthly_ops"}});
  if (!fire) {
    std::println("运行失败: {}", fire.error().message);
    return 1;
  }

  std::println("=== project_drive demo（arch 27 · 自管理 loop · 纯离线）===");
  // 每个任务的调度轮次在 results 里（同一节点 id 每次覆盖，最终是末轮=停机轮）。
  Json const& last = (*fire)["results"]["drive"];
  std::println("末轮决策: next_task={} done={}",
               last["next_task"].is_null() ? "(null·停机)" : last["next_task"].get<std::string>(),
               last["done"].get<bool>());
  std::println("终止理由: {}", last["reason"].get<std::string>());
  {
    std::ifstream p(proj / "project.json");
    Json po; p >> po;
    std::println("progress: weekly={} monthly={} review={}",
                 po["progress"].value("weekly", "?"),
                 po["progress"].value("monthly", "?"),
                 po["progress"].value("review", "?"));
  }
  std::vector<std::string> archs;
  for (std::string const& tid : {"weekly", "monthly", "review"})
    archs.push_back(std::filesystem::exists(proj / "tasks" / (tid + ".json")) ? "✓" : "✗");
  std::println("任务档案落项目目录: weekly={} monthly={} review={}", archs[0], archs[1], archs[2]);
  std::println("（清理提示：此 demo 写盘到 {}/，可 rm -rf）", root.string());
  return 0;
}