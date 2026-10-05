// cppchain —— workflow/history_file.hpp
// 会话历史的**落盘**（M12，见 02-arch-design/11 §5.2 / §6）。
//
// 落盘单位是**对话消息列表**，不是整个 state（快照链）—— 因为 history 是 workflow 里的
// 普通节点，state 每次触发都是全新的（M11 的 D1）。
//
// 纪律（与 agent::FileJournal / state::FileCheckpointer 同一套）：
//   · 原子写：先写临时文件，再 rename（同目录 rename 在 POSIX 上是原子的）
//   · fail-closed：文件损坏默认抛异常，不静默以空历史启动 —— 否则等于悄悄丢掉用户的会话
//   · 文件不存在是正常的（首次使用），返回空历史
#pragma once

#include <span>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"

namespace cppchain::workflow {

// `<workspace>/.cppchain/session-<name>.json`；name 为空时退化为 `session.json`
[[nodiscard]] std::string session_path(std::string const& workspace, std::string const& session);

// 读历史；文件不存在 → 空。损坏 → 抛 std::runtime_error（除非 start_empty_on_corrupt）
[[nodiscard]] std::vector<message::Message> load_history_file(std::string const& path,
                                                              bool start_empty_on_corrupt = false);

// 原子写历史（父目录不存在则创建）
void save_history_file(std::string const& path,
                       std::span<message::Message const> msgs, std::string const& session);

}  // namespace cppchain::workflow
