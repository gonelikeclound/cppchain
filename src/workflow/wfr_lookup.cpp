// cppchain —— src/workflow/wfr_lookup.cpp（见头注释的查找约定）
#include "cppchain/workflow/wfr_lookup.hpp"
#include "cppchain/workflow/wfr.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::workflow {

Json wfr_read_settings(std::string const& workdir) {
  std::filesystem::path const p = wfr_root(workdir) / "settings.json";
  std::ifstream in(p);
  if (!in) return Json::object();
  try {
    Json j = Json::parse(in);
    return j.is_object() ? std::move(j) : Json::object();
  } catch (std::exception const&) {
    return Json::object();  // 坏 settings：按无额外目录处理，不阻断
  }
}

std::vector<std::string> wfr_lookup_dirs(std::string const& workdir, Json const& settings) {
  std::vector<std::string> dirs;
  // 1) settings.workflow_dirs（相对 ws 解析；顺序即优先级）
  if (settings.contains("workflow_dirs") && settings["workflow_dirs"].is_array())
    for (Json const& entry : settings["workflow_dirs"]) {
      if (!entry.is_string() || entry.get_ref<std::string const&>().empty()) continue;
      std::filesystem::path const d(entry.get<std::string>());
      dirs.push_back((d.is_absolute() ? d : std::filesystem::path(workdir) / d).lexically_normal()
                         .string());
    }
  // 2) workspace 自带（固定，不可被覆盖）
  dirs.push_back((wfr_root(workdir) / "workflows").string());
  // 3) 全局用户（~/.wfr/workflows）
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

}  // namespace cppchain::workflow