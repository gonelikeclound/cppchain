// cppchain —— src/agent/journal.cpp
#include "cppchain/agent/journal.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cppchain::agent {

using Json = nlohmann::json;

namespace {

std::string state_str(JournalEntry::State s) {
  switch (s) {
    case JournalEntry::State::Intent: return "intent";
    case JournalEntry::State::Completed: return "completed";
    case JournalEntry::State::Failed: return "failed";
  }
  return "intent";
}

JournalEntry::State state_from(std::string const& s) {
  if (s == "completed") return JournalEntry::State::Completed;
  if (s == "failed") return JournalEntry::State::Failed;
  return JournalEntry::State::Intent;
}

}  // namespace

nlohmann::json to_json(JournalEntry const& e) {
  return Json{{"key", e.key},         {"tool", e.tool_name}, {"args", e.args},
              {"state", state_str(e.state)}, {"content", e.content}, {"status", e.status}};
}

JournalEntry entry_from_json(nlohmann::json const& j) {
  JournalEntry e;
  e.key = j.value("key", "");
  e.tool_name = j.value("tool", "");
  if (j.contains("args")) e.args = j["args"];
  e.state = state_from(j.value("state", "intent"));
  e.content = j.value("content", "");
  e.status = j.value("status", "");
  return e;
}

// ---------------- MemoryJournal ----------------

std::optional<JournalEntry> MemoryJournal::find(std::string const& key) const {
  auto const it = entries_.find(key);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

void MemoryJournal::record_intent(JournalEntry entry) {
  entries_[entry.key] = std::move(entry);
}

void MemoryJournal::record_result(std::string const& key, std::string content,
                                   std::string status) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;  // 没登记意图就不记结果（不该发生）
  it->second.content = std::move(content);
  it->second.status = std::move(status);
  it->second.state =
      it->second.status == "success" ? JournalEntry::State::Completed : JournalEntry::State::Failed;
}

nlohmann::json MemoryJournal::dump() const {
  auto arr = Json::array();
  for (auto const& [key, entry] : entries_) {
    (void)key;
    arr.push_back(to_json(entry));
  }
  return Json{{"version", 1}, {"entries", std::move(arr)}};
}

void MemoryJournal::load(nlohmann::json const& j) {
  entries_.clear();
  if (!j.is_object() || !j.contains("entries")) return;
  for (auto const& raw : j["entries"]) entries_[raw.value("key", "")] = entry_from_json(raw);
}

// ---------------- FileJournal ----------------

FileJournal::FileJournal(std::string path, OnCorrupt on_corrupt) : path_(std::move(path)) {
  std::ifstream in(path_);
  if (!in) return;  // 首次使用：文件不存在是正常的

  nlohmann::json j;
  try {
    in >> j;
  } catch (std::exception const& e) {
    if (on_corrupt == OnCorrupt::StartEmpty) return;
    throw std::runtime_error("FileJournal: 日志文件损坏，拒绝以空日志启动（fail-closed）：" +
                             path_ + " —— " + e.what());
  }
  inner_.load(j);
}

void FileJournal::persist() {
  // 原子写：先写临时文件，再 rename（同目录 rename 在 POSIX 上是原子的）
  std::string const tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("FileJournal: 无法写入 " + tmp);
    out << inner_.dump().dump(2);
    out.flush();
    if (!out) throw std::runtime_error("FileJournal: 写入失败 " + tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec) throw std::runtime_error("FileJournal: rename 失败 " + tmp + " → " + path_ + ": " + ec.message());
  ++saves_;
}

std::optional<JournalEntry> FileJournal::find(std::string const& key) const {
  return inner_.find(key);
}

void FileJournal::record_intent(JournalEntry entry) {
  inner_.record_intent(std::move(entry));
  persist();  // 意图先落盘：崩溃后才知道"可能执行过"
}

void FileJournal::record_result(std::string const& key, std::string content, std::string status) {
  inner_.record_result(key, std::move(content), std::move(status));
  persist();
}

nlohmann::json FileJournal::dump() const { return inner_.dump(); }
void FileJournal::load(nlohmann::json const& j) {
  inner_.load(j);
  persist();
}

// ---------------- SideEffectGuard ----------------

std::string SideEffectGuard::key_for(message::ToolCall const& call, JournalKeyMode mode) {
  if (mode == JournalKeyMode::ToolCallId) return call.id.empty() ? call.name : call.id;
  // name + 参数（参数按键序规范化，保证同一参数得到同一键）
  return call.name + ":" + call.args.dump();
}

std::string SideEffectGuard::prefixed_key(message::ToolCall const& call) const {
  return opts_.key_prefix + key_for(call, opts_.key_mode);
}

std::optional<message::Message> SideEffectGuard::lookup_tool_result(message::ToolCall const& call,
                                                                   tool::ToolSpec const* spec) {
  // 只读工具无需防重放（重跑查询无害），也不该占用日志
  if (spec != nullptr && spec->read_only) return std::nullopt;

  std::string const key = prefixed_key(call);

  auto const found = journal_->find(key);
  if (!found) {
    JournalEntry e;
    e.key = key;
    e.tool_name = call.name;
    e.args = call.args;
    e.state = JournalEntry::State::Intent;
    journal_->record_intent(std::move(e));
    pending_key_[call.id] = key;  // 只有**真实执行**才登记待记录键
    ++executions_;
    return std::nullopt;  // 首次 → 正常执行
  }

  if (found->state == JournalEntry::State::Completed) {
    ++replays_;
    // 注意：不登记 pending_key_ —— 否则 after_tool 会把"[重放]"前缀再写回日志，
    // 多次重放会不断累加前缀（内容漂移）。
    return message::tool_message("[重放] " + found->content, call.id, found->status);
  }

  if (found->state == JournalEntry::State::Failed && opts_.retry_on_failure) {
    pending_key_[call.id] = key;
    ++executions_;
    return std::nullopt;
  }

  // 意图未完成 / 失败且不允许重试 → 结果未知，拒绝重跑（安全默认）
  ++blocked_;
  std::string const why = found->state == JournalEntry::State::Intent
                              ? "该副作用已开始执行但结果未知（可能是执行中崩溃）"
                              : "该副作用此前执行失败，可能已部分生效";
  return message::tool_message(
      "[拒绝重放] " + why + "：工具 " + found->tool_name +
          " 不会再次执行。请人工确认实际状态后再决定下一步。",
      call.id, "error");
}

void SideEffectGuard::after_tool(message::Message& tool_message) {
  if (!tool_message.tool_call_id) return;
  auto const it = pending_key_.find(*tool_message.tool_call_id);
  if (it == pending_key_.end()) return;
  journal_->record_result(it->second, tool_message.text(), tool_message.status);
  pending_key_.erase(it);
}

}  // namespace cppchain::agent