// cgi-bin/wfr_cgi_lib.hpp —— CGI WebUI 增强的纯逻辑（workspace/二级树/workflow 绑定/debug）
//
// header-only，只依赖标准库 + nlohmann：wfr_cgi.cpp（CGI 可执行）与 tests/webui/cgi_lib_test.cpp
// 共用同一份实现，测试不需要链接 CGI 可执行。
//
// 本文件是"纯函数"：不读 QUERY_STRING、不写 std::cout 响应，全部输入输出显式——
// 便于单测且与 CGI 主流程解耦。目录扫描用自包含副本（不链 cppchain_workflow 的
// wfr_list_workflows_in），测试与 CGI 各自持有一致实现。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace cppchain::cgi {

using Json = nlohmann::json;

// ---- session 名解析：`wfname.sessionid`（带点，workflow 绑定编码在名字里）----
struct SessionId {
  std::string workflow;
  std::string id;
};

// 按第一个点拆 `wfname.sessionid`。workflow 与 id 都做字符校验；无点/空段/多余点 → nullopt。
// id 允许字母数字 + `_-`（≤64）；workflow 允许字母数字 + `_.-`（但含点会被第一个点截断，实际只含首段）。
[[nodiscard]] inline std::optional<SessionId> parse_session_id(std::string const& s) {
  if (s.empty()) return std::nullopt;
  auto const dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return std::nullopt;
  std::string const wf = s.substr(0, dot);
  std::string const id = s.substr(dot + 1);
  // id 不得再含点（`a.b.c` → id 含点拒绝）
  if (id.find('.') != std::string::npos) return std::nullopt;
  auto ok = [](std::string const& x, bool allow_dot) {
    if (x.empty() || x.size() > 64) return false;
    for (char c : x)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' &&
          !(allow_dot && c == '.'))
        return false;
    return true;
  };
  if (!ok(wf, false) || !ok(id, false)) return std::nullopt;
  return SessionId{wf, id};
}

// ---- workflow 白名单 / 枚举 ----
// 列出给定目录下的 `*.json` workflow 名（去扩展名、排序）。自包含实现（与 wfr_lookup 语义一致）。
[[nodiscard]] inline std::vector<std::string> list_workflow_names(std::filesystem::path const& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) return out;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return out;
  std::filesystem::directory_iterator const end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code fec;
    if (!std::filesystem::is_regular_file(it->path(), fec) || fec) continue;
    std::string const fname = it->path().filename().string();
    if (fname.size() > 5 && fname.ends_with(".json")) out.push_back(fname.substr(0, fname.size() - 5));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// 多目录枚举：并集 + 按名去重（首个 win，目录序即优先级）→ 有序列表。
[[nodiscard]] inline std::vector<std::string> enumerate_workflow_names(
    std::vector<std::filesystem::path> const& dirs) {
  std::vector<std::string> out;
  for (auto const& d : dirs) {
    for (std::string const& n : list_workflow_names(d))
      if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
  }
  return out;
}

// workflow 是否在某目录集里注册（白名单成员判定；chat/debug 用它拒未知 workflow）。
[[nodiscard]] inline bool is_registered_workflow(std::vector<std::filesystem::path> const& dirs,
                                                 std::string const& name) {
  for (auto const& d : dirs) {
    std::vector<std::string> const names = list_workflow_names(d);
    if (std::find(names.begin(), names.end(), name) != names.end()) return true;
  }
  return false;
}

// ---- workspace 名称/目录校验 ----
[[nodiscard]] inline bool valid_workspace_name(std::string const& n) {
  if (n.empty() || n.size() > 32) return false;
  if (!std::isalnum(static_cast<unsigned char>(n[0]))) return false;
  for (char c : n)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
  return true;
}

// 校验并规范化工作区目录：非空、非绝对路径、任一分量不得为 `..`、lexically_normal 后仍以 root 为前缀。
// 返回规范化后的相对路径（root 相对）；非法 → nullopt。
[[nodiscard]] inline std::optional<std::string> sanitize_workspace_dir(
    std::filesystem::path const& root, std::string const& d) {
  if (d.empty()) return std::nullopt;
  std::filesystem::path p(d);
  if (p.is_absolute()) return std::nullopt;
  for (auto const& seg : p)
    if (seg == "..") return std::nullopt;
  std::filesystem::path const norm = p.lexically_normal();
  if (norm.empty() || norm.is_absolute()) return std::nullopt;
  // 规范化后仍以 root 为前缀（防 `tmp/../evil` 这类绕路）
  std::filesystem::path const full = (root / norm).lexically_normal();
  std::filesystem::path const root_n = root.lexically_normal();
  auto const fr = full.string(), rn = root_n.string();
  if (fr.size() < rn.size() || fr.compare(0, rn.size(), rn) != 0) return std::nullopt;
  return norm.string();
}

// ---- workspaces.json 读写 ----
// 读 `<repo>/web/workspaces.json`（{workspaces:[{name,dir}]}），坏/缺 → 空数组不抛。
[[nodiscard]] inline Json read_workspaces_file(std::filesystem::path const& cfg) {
  Json out = Json::array();
  if (std::ifstream in(cfg); in) {
    Json j;
    try { in >> j; } catch (...) { j = Json::object(); }
    if (j.contains("workspaces") && j["workspaces"].is_array())
      for (auto const& w : j["workspaces"])
        if (w.is_object()) out.push_back(w);
  }
  return out;
}

// 向 workspaces.json 追加一个 workspace。原子写（tmp + rename）。
// 已存在同名 name 或规范化后 dir → false（不写）。写失败 → false。
[[nodiscard]] inline bool append_workspace(std::filesystem::path const& cfg,
                                           std::string const& name, std::string const& dir) {
  auto const list = read_workspaces_file(cfg);
  for (auto const& w : list)
    if (w.is_object() && (w.value("name", "") == name || w.value("dir", "") == dir))
      return false;
  Json j = Json{{"workspaces", list}};
  j["workspaces"].push_back(Json{{"name", name}, {"dir", dir}});
  std::filesystem::path const tmp = cfg.string() + ".tmp";
  if (std::ofstream out(tmp); out) out << j.dump(2) << "\n";
  else return false;
  std::error_code ec;
  std::filesystem::rename(tmp, cfg, ec);
  return !ec;
}

// ---- per-workflow 默认 `--set` 映射 ----
// 返回固定键（如 chat_cgi 的 compress_*）与从当前输入文本派生的键（如 mch 的 goal）。
// 未知 workflow → 空（wfr 对缺键 fail 于运行时，不做无为注入）。
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> default_sets_for(
    std::string const& wf, std::string const& query) {
  std::vector<std::pair<std::string, std::string>> out;
  if (wf == "chat_cgi") {
    out.emplace_back("compress_when", "messages");
    out.emplace_back("compress_max_msgs", "40");
    out.emplace_back("compress_keep_last", "6");
  } else if (wf == "chat2") {
    out.emplace_back("history_mode", "full");
  } else if (wf == "mch" || wf == "mch_par") {
    // 任务类：当前输入文本既作为 @config.goal 驱动计划，又作为对话输入（--stream 位置参）
    out.emplace_back("goal", query);
    out.emplace_back("mch_criteria", "整批验收：产物真实落盘且有依据");
  } else if (wf == "lot") {
    out.emplace_back("task", query);
    out.emplace_back("approve_policy", "allow");  // 长程批处理既有语义（cron 同款）
  }
  return out;
}

}  // namespace cppchain::cgi
