// demo: project_run —— Project 自管理第一刀（arch 26，纯离线）
//
// 演示 project 化长程任务的三个核心机制（全不发模型请求，用 task/project 节点的档案与注入）：
//   ① 目录化多文件 —— project = .wfr/projects/<p>/{project.json, lessons.json, tasks/<t>.json}
//   ② 多任务 + 独立对话 —— 3 个任务各跑**独立 session**（任务历史互不污染），共享同一 project
//   ③ 项目级上下文注入 —— project begin 把 目标/完成标准/纪律/经验 拼进 state[system_context]
//      （与 Context 节点同形状，真机 model 装配时自动带上）
// 外加：任务档案 @config.project 下沉（task 开始/结束都写项目目录）、end 写回 lessons/progress。
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/refs.hpp>
#include <cppchain/workflow/validator.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <system_error>

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

// 一个任务 = project begin → task begin → task end → project end（真机用 lot_project 模板，
// 中间塞 PEV 批循环；这里只演示结构性节点，零模型）。
Json task_wf() {
  return Json::parse(R"json({
    "id": "wf_proj_task",
    "nodes": [
      {"id": "p_begin", "kind": "project", "project": "@config.project", "phase": "begin", "next": "t_begin"},
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "t_end"},
      {"id": "t_end",   "kind": "task", "task": "@config.task", "phase": "end",   "next": "p_end"},
      {"id": "p_end",   "kind": "project", "project": "@config.project", "phase": "end", "next": "_turn_end"}
    ]
  })json");
}

}  // namespace

int main() {
  // 工作区放仓库根 tmp/（项目纪律）。
  std::filesystem::path const root =
      std::filesystem::path("tmp") / ("project_run_" +
                                      std::to_string(static_cast<unsigned>(std::random_device{}())));
  std::filesystem::path const proj = root / ".wfr" / "projects" / "monthly_ops";
  std::error_code ec;
  std::filesystem::create_directories(proj / "tasks", ec);
  {
    std::ofstream w(proj / "project.json");
    w << R"({"id":"monthly_ops","goal":"产出本月运营综述：周报、月报、复盘三份产物互相衔接",
             "completion_criteria":"三份产物落盘且数字前后一致",
             "discipline":["每任务产物落盘 .out/","数字一致性","引用上一步产物路径"],
             "tasks":[{"id":"weekly","name":"周报"},{"id":"monthly","name":"月报"},
                      {"id":"review","name":"复盘"}]})";
    std::ofstream l(proj / "lessons.json");
    l << R"([{"when":"2026-10-20","applicable_if":"做周报","conclusion":"先出图再写文字","evidence":"试点" }])";
  }

  auto ir = WorkflowIR::parse(task_wf());
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 1;
  }
  auto v = validate(*ir, ValidatorOptions{});
  if (!v.ok()) {
    std::println("校验失败: {}", v.describe());
    return 1;
  }
  auto wf = compile_workflow(*ir, /*registry=*/tool::ToolRegistry{});
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 1;
  }

  std::println("=== project_run demo（arch 26 · 纯离线）===");
  struct TaskCase {
    std::string id, goal;
  };
  const TaskCase tasks[] = {{"weekly", "写本周周报"}, {"monthly", "写本月月报"}, {"review", "做本月复盘"}};
  std::size_t injected = 0;
  for (std::size_t i = 0; i < 3; ++i) {
    // ② 每任务独立 session（独立历史/上下文）；共享同一 project
    Json const config{{"workspace", root.string()},
                      {"project", "monthly_ops"},
                      {"task", tasks[i].id},
                      {"goal", tasks[i].goal},
                      {"session", tasks[i].id},
                      {"plan_steps", std::string(R"([{"id":"s1","goal":")") + tasks[i].goal + R"(" }])"},
                      {"lesson",
                       Json{{"applicable_if", "处理" + tasks[i].id + "时"},
                            {"conclusion", "数据源先核对再落笔（demo 经验）"}}}};
    auto fire = fire_once(*wf, config);
    if (!fire) {
      std::println("任务 {} 运行失败: {}", tasks[i].id, fire.error().message);
      return 1;
    }
    Json const& st = *fire;
    // ③ 项目级注入可见（system_context 里拼进了 目标/纪律/经验/本任务）
    bool has_goal = false, has_task = false;
    for (Json const& m : st.at(kSystemContextKey))
      if (m.contains("content") && m["content"].is_array())
        for (Json const& b : m["content"])
          if (b.is_object() && b.value("text", "").find("本月运营综述") != std::string::npos)
            has_goal = true;
    for (Json const& m : st.at(kSystemContextKey))
      if (m.contains("content") && m["content"].is_array())
        for (Json const& b : m["content"])
          if (b.is_object() && b.value("text", "").find("【本任务】" + tasks[i].id) !=
                                   std::string::npos)
            has_task = true;
    if (has_goal && has_task) ++injected;
    // ① 档案落盘位置（project 目录，非旧 .wfr/tasks）
    std::filesystem::path const arch = proj / "tasks" / (tasks[i].id + ".json");
    std::println("任务 {}：session={} → 档案 {} · 注入[目标/本任务]={}",
                 tasks[i].id, tasks[i].id, arch.filename().string(),
                 (has_goal && has_task ? "✅" : "❌"));
    if (!std::filesystem::exists(arch)) {
      std::println("  ✗ 档案未落盘: {}", arch.string());
      return 1;
    }
  }

  // lessons 沉淀 + progress 更新（project end 收口）
  {
    std::ifstream l(proj / "lessons.json");
    Json lessons; l >> lessons;
    std::ifstream p(proj / "project.json");
    Json po; p >> po;
    std::println("经验沉淀 lessons: {} 条（原 1 + 新增 3）", lessons.size());
    std::println("progress: weekly={} monthly={} review={}",
                 po["progress"].value("weekly", "?"),
                 po["progress"].value("monthly", "?"),
                 po["progress"].value("review", "?"));
  }
  std::println("（清理提示：此 demo 写盘到 {}/，可 rm -rf）", root.string());
  return 0;
}