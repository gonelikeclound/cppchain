// cppchain —— src/wfr/builtin_workflows.hpp
// wfr 内置 workflow（无需 .wfr/workflows/ 文件即可跑）。
//
// chat（最简单聊天）：load_history（从本会话日志取历史）→ model 回复。
//   多轮即"继续聊"：同一 --session 下第二次 fire 自动带上上轮对话（load_history
//   对日志 replay），真正连续。无 trigger（manual，wfr 直接拉起）。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cppchain::wfr {

using Json = nlohmann::json;

inline Json builtin_chat_ir() {
  return Json{
      {"id", "builtin_chat"},
      {"nodes", Json::array(
                    {// 从本会话日志把历史 replay 回来（@config.workspace/@config.session 由 wfr 提供）
                     Json{{"id", "recall"},
                          {"kind", "load_history"},
                          {"path", "@config.workspace"},
                          {"session", "@config.session"},
                          {"next", "think"}},
                     // 一句回复：model 组件 "main"（wfr 注入实时模型）
                     Json{{"id", "think"}, {"kind", "model"}, {"model", "main"},
                          {"next", "_turn_end"}}})}};
}

// 内置 workflow 名（命中即用它，不读 .wfr/workflows/）
inline std::vector<std::string> builtin_names() { return {"chat"}; }

[[nodiscard]] inline std::optional<Json> resolve_builtin(std::string const& name) {
  if (name == "chat") return builtin_chat_ir();
  return std::nullopt;
}

}  // namespace cppchain::wfr