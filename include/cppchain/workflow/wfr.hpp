// cppchain —— workflow/wfr.hpp
// wfr 工具的工作目录元数据路径约定：<workdir>/.wfr/ 统一存放 workflow 运行态。
//
// 布局（docs 规划：wfr 命令行 + 调试可视化）：
//   <workdir>/.wfr/
//     workflows/<name>.json      # 按名查找的约定（wfr <name> → 此文件）
//     sessions/session[-<name>].log.jsonl   # 会话日志（唯一事实来源，含 __input__ 事件）
//     checkpoints/<session>.json # 预留（v1 不写；续跑走 FireOptions.fork）
//     .gitignore                 # "sessions/\ncheckpoints/\n"（workflows/ 源要提交）
//
// 与旧 `session_path`（.cppchain/）的区别：.wfr 统一托管 workflow 的数据；
// session_path 已迁移到 .wfr/sessions/（implemented in session_log.cpp）。
#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace cppchain::workflow {

// <workdir>/.wfr
[[nodiscard]] inline std::filesystem::path wfr_root(std::string const& workdir) {
  return std::filesystem::path(workdir) / ".wfr";
}

// <workdir>/.wfr/workflows/<name>.json
[[nodiscard]] inline std::string wfr_workflow_path(std::string const& workdir,
                                                   std::string const& name) {
  return (wfr_root(workdir) / "workflows" / (name + ".json")).string();
}

// <workdir>/.wfr/checkpoints/<session>.json（v1 仅预留路径，不写）
[[nodiscard]] inline std::string wfr_checkpoint_path(std::string const& workdir,
                                                     std::string const& session) {
  return (wfr_root(workdir) / "checkpoints" / (session + ".json")).string();
}

// 遍历 <workdir>/.wfr/workflows/*.json，返回去掉扩展名的名字（供 `wfr list`；按文件名排序）。
// 目录不存在 → 空。忽略不可读的条目（error_code 路径，不抛）。
[[nodiscard]] inline std::vector<std::string> wfr_list_workflows(std::string const& workdir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(wfr_root(workdir) / "workflows", ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    std::string const p = it->path().filename().string();
    if (p.size() > 5 && p.ends_with(".json")) out.push_back(p.substr(0, p.size() - 5));
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace cppchain::workflow