// cppchain —— workflow/wfr_lookup.hpp
// wfr 的 workflow 查找约定（发布后）：
//
//   查找顺序（名字命中即停，首个 win）：
//     1. 显式路径（含 / 或 *.json 且存在）
//     2. <ws>/.wfr/settings.json 的 "workflow_dirs"（数组，顺序即优先级；相对 ws 解析）
//     3. <ws>/.wfr/workflows/<name>.json        ← workspace 自带（此路径不可被覆盖）
//     4. ~/.wfr/settings.json 的 "workflow_dirs"（相对 ~ 解析；优先于全局默认目录）
//     5. ~/.wfr/workflows/<name>.json           ← 全局用户 workflow
//     6. 发布自带 share/wfr/workflows/<name>.json（内置/固定；可由调用方经 extra_dirs
//        传入，排在全局之后）
//
// 注意：workspace 的 `.wfr/` 路径本身不参与"可覆盖" —— settings 只能**追加**
// workflow_dirs 到查找集合（插在本层默认目录之前），不能改 ws 路径。
// ws 与全局 settings 的 workflow_dirs 语义一致，只是相对基准不同（ws / ~）。
// settings 形状：{ "workflow_dirs": ["/abs", "相对路径", ...] }
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

// 读 <ws>/.wfr/settings.json：不存在→空对象；是对象→返回；坏 JSON→空对象（不阻断）。
[[nodiscard]] Json wfr_read_settings(std::string const& workdir);

// 查找目录集（顺序即优先级，不含发布自带 share）：settings.workflow_dirs → ws → 全局
[[nodiscard]] std::vector<std::string> wfr_lookup_dirs(std::string const& workdir,
                                                       Json const& settings);

// 按名查找 workflow 文件（<name>.json），命中返回路径。
// extra_dirs：发布自带目录（排在全局之后），如 { share/wfr/workflows, env(WFR_WORKFLOWS_DIR) }。
[[nodiscard]] std::optional<std::string>
wfr_lookup_workflow(std::string const& workdir, std::string const& name, Json const& settings,
                    std::vector<std::string> const& extra_dirs = {});

// 统计一个目录里现存的 workflow 名（*.json），供 `wfr list` 汇总。目录不存在→空。
[[nodiscard]] std::vector<std::string> wfr_list_workflows_in(std::string const& dir);

// arch 27 §十：wfr cron 批次装配（project 感知）。job 为 .wfr/cron.json 的一个条目：
//   {task, project?, every_minutes, session?, goal?}
// project 非空 → 触发 **lot_project**（project 版单批），config 注入 project/task/plan_steps
//   （plan_steps 读 .wfr/projects/<p>/project.json 该任务 steps；无 → 单步兜底）/goal；
//   session 缺省 = "<project>.<task>"（每任务独立对话，arch 26 ⑤）。
// project 空 → 触发 **lot**（现状兼容），session 缺省 = task。
// 返回 argv（args[0] = self，调用方填 wfr 可执行路径），供 wfr cron 拉起批次。
struct WfrCronBatch {
  std::vector<std::string> args;
  std::string workflow;    // "lot" | "lot_project"
  std::string task;        // 有效任务名（非空）
  std::string session;
  bool project_mode = false;
};
[[nodiscard]] WfrCronBatch wfr_cron_batch(std::string const& workdir, Json const& job,
                                          std::string const& self);

}  // namespace cppchain::workflow