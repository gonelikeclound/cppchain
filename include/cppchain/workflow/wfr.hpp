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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

// 全局用户目录（~/.wfr）：发布后每用户共享一份（跨工作区复用 workflow）
[[nodiscard]] inline std::string wfr_home_root() {
  std::filesystem::path home = ".";
  if (char const* h = std::getenv("HOME")) home = h;
  return (home / ".wfr").string();
}

// ~/.wfr/workflows/<name>.json
[[nodiscard]] inline std::string wfr_global_workflow_path(std::string const& name) {
  return (std::filesystem::path(wfr_home_root()) / "workflows" / (name + ".json")).string();
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

// ---- wfr-㊸ @④：会话级 allow 前缀（范围不变量） ----
//
// 批准一条 bash 命令后，把命令**原文**作为"前缀"持久化到本会话
// （`.wfr/sessions/<session>.allowlist.jsonl`，每行一个 JSON 记录）。
// 语义（OpenAI exec-policy prefix 同款，明确记载的斜面）：
//   同会话 + 同沙箱 provider 下，后续**任何以该前缀开头的命令**自动放行，不再打断人审。
// 范围不变量（三约束，problems/22 §… 有完整讨论）：
//   1) 文件作用域 = 本会话：不跨会话、不跨工作区（删除会话日志时应连带清掉 allowlist）；
//   2) 匹配 = 命令字符串 starts_with(前缀) —— 允许 P 即允许 `P; …` 的串联，批准者须接受；
//   3) 只在与「批准时记录的 sandbox provider」一致时生效 —— 沙箱降级后旧批准自动失效。
// 坏行 fail-open：跳过不致命，只是那一条不自动放行。
[[nodiscard]] inline std::filesystem::path wfr_allowlist_path(std::string const& workdir,
                                                              std::string const& session) {
  return wfr_root(workdir) / "sessions" / (session + ".allowlist.jsonl");
}

[[nodiscard]] inline std::vector<nlohmann::json> wfr_load_allow_prefixes(
    std::string const& workdir, std::string const& session) {
  std::vector<nlohmann::json> out;
  std::ifstream in(wfr_allowlist_path(workdir, session));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto const j =
        nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded() && j.is_object()) out.push_back(j);
  }
  return out;
}

inline void wfr_append_allow_prefix(std::string const& workdir, std::string const& session,
                                    std::string const& prefix, std::string const& provider,
                                    std::string const& command_class) {
  std::error_code ec;
  std::filesystem::create_directories(wfr_root(workdir) / "sessions", ec);
  std::ofstream out(wfr_allowlist_path(workdir, session), std::ios::app);
  if (!out) return;  // 持久化失败 → 本次不写；不终止运行（下次仍要人审，fail-open）
  out << nlohmann::json{{"prefix", prefix}, {"provider", provider}, {"class", command_class}}.dump()
      << '\n';
}

}  // namespace cppchain::workflow