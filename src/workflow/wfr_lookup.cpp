// cppchain —— src/workflow/wfr_lookup.cpp（见头注释的查找约定）
#include "cppchain/workflow/wfr_lookup.hpp"
#include "cppchain/workflow/wfr.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::workflow {

namespace {

// 读单个 settings.json（不存在/坏 → 空对象，不阻断）
Json read_settings_file(std::filesystem::path const& p) {
  std::ifstream in(p);
  if (!in) return Json::object();
  try {
    Json j = Json::parse(in);
    return j.is_object() ? std::move(j) : Json::object();
  } catch (std::exception const&) {
    return Json::object();
  }
}

// 把 settings 的 workflow_dirs 追加进 dirs；相对路径按 base 解析。
void append_workflow_dirs(std::vector<std::string>& dirs, Json const& settings,
                          std::filesystem::path const& base) {
  if (!settings.contains("workflow_dirs") || !settings["workflow_dirs"].is_array()) return;
  for (Json const& entry : settings["workflow_dirs"]) {
    if (!entry.is_string() || entry.get_ref<std::string const&>().empty()) continue;
    std::filesystem::path const d(entry.get<std::string>());
    dirs.push_back((d.is_absolute() ? d : base / d).lexically_normal().string());
  }
}

}  // namespace

Json wfr_read_settings(std::string const& workdir) {
  return read_settings_file(wfr_root(workdir) / "settings.json");
}

std::vector<std::string> wfr_lookup_dirs(std::string const& workdir, Json const& settings) {
  std::vector<std::string> dirs;
  // 1) workspace settings.workflow_dirs（相对 ws 解析；顺序即优先级）
  append_workflow_dirs(dirs, settings, std::filesystem::path(workdir));
  // 2) workspace 自带（固定，不可被覆盖）
  dirs.push_back((wfr_root(workdir) / "workflows").string());
  // 3) 全局 settings.workflow_dirs（~/.wfr/settings.json；相对 HOME 解析，优先于全局默认目录）
  append_workflow_dirs(dirs, read_settings_file(std::filesystem::path(wfr_home_root()) / "settings.json"),
                       std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ""));
  // 4) 全局用户（~/.wfr/workflows）
  dirs.push_back((std::filesystem::path(wfr_home_root()) / "workflows").string());
  return dirs;
}

std::optional<std::string>
wfr_lookup_workflow(std::string const& workdir, std::string const& name, Json const& settings,
                    std::vector<std::string> const& extra_dirs) {
  auto dirs = wfr_lookup_dirs(workdir, settings);
  for (auto const& d : extra_dirs) dirs.push_back(d);  // 发布自带：全局之后兜底
  for (std::string const& d : dirs) {
    std::filesystem::path const p = std::filesystem::path(d) / (name + ".json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) return p.string();
  }
  return std::nullopt;
}

std::vector<std::string> wfr_list_workflows_in(std::string const& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    std::string const p = it->path().filename().string();
    if (p.size() > 5 && p.ends_with(".json")) out.push_back(p.substr(0, p.size() - 5));
  }
  std::sort(out.begin(), out.end());
  return out;
}

WfrCronBatch wfr_cron_batch(std::string const& workdir, Json const& job, std::string const& self) {
  WfrCronBatch out;
  std::string const T = job.value("task", "");
  std::string const P = job.value("project", "");
  std::string const G = job.value("goal", "");
  if (T.empty()) return out;  // 无效 job：task 必填
  out.task = T;
  out.project_mode = !P.empty();
  std::string goal = G;
  std::string seed;  // plan_steps（project 模式才注入）
  if (out.project_mode) {
    out.workflow = "lot_project";
    out.session = job.value("session", P + "." + T);  // 每任务独立对话（arch 26 ⑤，两段 session）
    // 读 .wfr/projects/<p>/project.json 该任务 steps（无 → 单步兜底，与 drive 自适应一致）；
    // goal 缺省取任务 goal（job.goal 优先）。
    std::filesystem::path const pj = wfr_root(workdir) / "projects" / P / "project.json";
    if (std::ifstream in(pj); in)
      try {
        Json po = Json::parse(in);
        if (po.is_object() && po.contains("tasks") && po["tasks"].is_array())
          for (Json const& t : po["tasks"])
            if (t.is_object() && t.value("id", "") == T) {
              if (goal.empty()) goal = t.value("goal", "");
              if (t.contains("steps") && t["steps"].is_array() && !t["steps"].empty())
                seed = t["steps"].dump();
              break;
            }
      } catch (std::exception const&) {
      }
    if (seed.empty())
      seed = Json::array({Json{{"id", "s1"},
                               {"goal", goal.empty() ? ("任务" + T) : goal}}})
                 .dump();
  } else {
    out.workflow = "lot";
    out.session = job.value("session", T);
  }
  out.args.push_back(self);
  out.args.push_back(out.workflow);
  out.args.push_back("--workdir");
  out.args.push_back(workdir);
  out.args.push_back("--session");
  out.args.push_back(out.session);
  if (out.project_mode) {
    out.args.push_back("--set");
    out.args.push_back("project=" + P);
  }
  out.args.push_back("--set");
  out.args.push_back("task=" + T);
  out.args.push_back("--set");
  out.args.push_back("approve_policy=allow");
  if (out.project_mode) {
    out.args.push_back("--set");
    out.args.push_back("plan_steps=" + seed);
  }
  if (!goal.empty()) {
    out.args.push_back("--set");
    out.args.push_back("goal=" + goal);
  }
  out.args.push_back("cron 定时批");
  return out;
}

}  // namespace cppchain::workflow