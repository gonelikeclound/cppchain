// cppchain —— src/state/memory_backends.cpp
// MemoryCheckpointer + MemoryStore（两个内存实现放一起，便于对照维护）
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/state/store.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace cppchain::state {

namespace {

std::string now_iso8601() {
  auto const now = std::chrono::system_clock::now();
  std::time_t const t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

bool starts_with(std::vector<std::string> const& ns, std::vector<std::string> const& prefix) {
  if (prefix.size() > ns.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i)
    if (ns[i] != prefix[i]) return false;
  return true;
}

// 包队列的落盘表示（v2 起）：`{"node":..., "state":...}`；state 为 null = 全局状态包。
// optional 显式写 null —— 与下方 parent_checkpoint_id 同一纪律（nlohmann 对 optional 无内建支持）。
// `resume` 为中断重跑包标记（v1/v2 旧记录没有该字段 → 读回默认 false）。
nlohmann::json packet_to_json(Packet const& p) {
  return nlohmann::json{{"node", p.node},
                        {"state", p.state ? nlohmann::json(*p.state) : nlohmann::json(nullptr)},
                        {"resume", p.is_restart}};
}

Packet packet_from_json(nlohmann::json const& j) {
  // 兼容 v1：next 元素可能是纯字符串（旧版节点名数组）
  if (j.is_string()) return Packet{j.get<std::string>()};
  return Packet{j.value("node", ""),
                j.contains("state") && !j["state"].is_null()
                    ? std::optional<Json>{j["state"]}
                    : std::nullopt,
                j.value("resume", false)};
}

nlohmann::json packets_to_json(std::vector<Packet> const& packets) {
  nlohmann::json a = nlohmann::json::array();
  for (Packet const& p : packets) a.push_back(packet_to_json(p));
  return a;
}

}  // namespace

// ---------------- MemoryCheckpointer ----------------

std::string MemoryCheckpointer::key_of(std::string const& thread_id, std::string const& ns) {
  return thread_id + "|" + ns;
}

std::string MemoryCheckpointer::put(std::string const& thread_id, std::string const& checkpoint_ns,
                                    std::optional<std::string> parent_checkpoint_id, Json values,
                                    std::vector<Packet> next, Json metadata) {
  std::string const id = "ckpt-" + std::to_string(++counter_);
  StateSnapshot snap;
  snap.values = std::move(values);
  snap.next = std::move(next);
  snap.metadata = std::move(metadata);
  snap.parent_checkpoint_id = std::move(parent_checkpoint_id);
  snap.config = Json{{"thread_id", thread_id},
                     {"checkpoint_ns", checkpoint_ns},
                     {"checkpoint_id", id}};
  snapshots_[key_of(thread_id, checkpoint_ns)].push_back(std::move(snap));
  return id;
}

void MemoryCheckpointer::put_writes(std::string const& thread_id, std::string const& checkpoint_ns,
                                    std::string const& checkpoint_id,
                                    std::vector<WriteRecord> writes) {
  auto& slot = writes_[key_of(thread_id, checkpoint_ns) + "|" + checkpoint_id];
  for (auto& w : writes) slot.push_back(std::move(w));
}

std::vector<WriteRecord> MemoryCheckpointer::get_writes(std::string const& thread_id,
                                                        std::string const& checkpoint_ns,
                                                        std::string const& checkpoint_id) {
  auto const it = writes_.find(key_of(thread_id, checkpoint_ns) + "|" + checkpoint_id);
  return it == writes_.end() ? std::vector<WriteRecord>{} : it->second;
}

std::optional<StateSnapshot> MemoryCheckpointer::get_tuple(
    std::string const& thread_id, std::string const& checkpoint_ns,
    std::optional<std::string> checkpoint_id) {
  auto const it = snapshots_.find(key_of(thread_id, checkpoint_ns));
  if (it == snapshots_.end() || it->second.empty()) return std::nullopt;

  if (!checkpoint_id) return it->second.back();  // 最新

  for (auto const& snap : it->second)
    if (snap.config.value("checkpoint_id", "") == *checkpoint_id) return snap;
  return std::nullopt;
}

std::vector<StateSnapshot> MemoryCheckpointer::list(std::string const& thread_id,
                                                    std::string const& checkpoint_ns,
                                                    std::size_t limit) {
  auto const it = snapshots_.find(key_of(thread_id, checkpoint_ns));
  if (it == snapshots_.end()) return {};

  std::vector<StateSnapshot> out(it->second.rbegin(), it->second.rend());  // 新的在前
  if (limit > 0 && out.size() > limit) out.resize(limit);
  return out;
}

// ---- 持久化（供 FileCheckpointer 复用）----
// 格式：内部两张表按**复合键原样**存取（key_of(thread,ns) 与 key + "|" + checkpoint_id）。
// 不做键的解析/重组 —— 原样往返最不容易出错，且键里的 '|' 只造成歧义、不造成丢失。
nlohmann::json MemoryCheckpointer::dump() const {
  nlohmann::json snaps = nlohmann::json::object();
  for (auto const& [key, vec] : snapshots_) {
    nlohmann::json arr = nlohmann::json::array();
    for (StateSnapshot const& s : vec) {
      // parent_checkpoint_id 必须**显式**写成 string 或 null：
      // nlohmann 对 std::optional 没有内建支持，直接塞会走"数组构造路径"——
      // nullopt 与 engaged 都变成 `[]`，回读时按 is_string() 守卫就全丢了
      // （实测：跨文件往返后快照链的 parent 全部消失）。
      arr.push_back(nlohmann::json{
          {"values", s.values},
          {"next", packets_to_json(s.next)},
          {"config", s.config},
          {"metadata", s.metadata},
          {"parent_checkpoint_id",
           s.parent_checkpoint_id ? nlohmann::json(*s.parent_checkpoint_id) : nlohmann::json(nullptr)}});
    }
    snaps[key] = std::move(arr);
  }
  nlohmann::json wr = nlohmann::json::object();
  for (auto const& [key, vec] : writes_) {
    nlohmann::json arr = nlohmann::json::array();
    for (WriteRecord const& w : vec)
      arr.push_back(nlohmann::json{{"task_id", w.task_id},
                                   {"channel", w.channel},
                                   {"value", w.value}});
    wr[key] = std::move(arr);
  }
  return nlohmann::json{{"version", 2}, {"counter", counter_}, {"snapshots", std::move(snaps)},
                        {"writes", std::move(wr)}};
}

void MemoryCheckpointer::load(nlohmann::json const& j) {
  snapshots_.clear();
  writes_.clear();
  counter_ = j.value("counter", 0L);
  if (j.contains("snapshots") && j["snapshots"].is_object())
    for (auto it = j["snapshots"].begin(); it != j["snapshots"].end(); ++it) {
      std::vector<StateSnapshot> vec;
      for (nlohmann::json const& e : it.value()) {
        StateSnapshot s;
        s.values = e.value("values", nlohmann::json::object());
        if (e.contains("next") && e["next"].is_array())
          for (nlohmann::json const& n : e["next"]) s.next.push_back(packet_from_json(n));
        s.config = e.value("config", nlohmann::json::object());
        s.metadata = e.value("metadata", nlohmann::json::object());
        if (auto const& p = e.find("parent_checkpoint_id");
            p != e.end() && p->is_string())
          s.parent_checkpoint_id = p->get<std::string>();
        vec.push_back(std::move(s));
      }
      snapshots_[it.key()] = std::move(vec);
    }
  if (j.contains("writes") && j["writes"].is_object())
    for (auto it = j["writes"].begin(); it != j["writes"].end(); ++it) {
      std::vector<WriteRecord> vec;
      for (nlohmann::json const& e : it.value())
        vec.push_back(WriteRecord{e.value("task_id", ""), e.value("channel", ""),
                                  e.value("value", nlohmann::json())});
      writes_[it.key()] = std::move(vec);
    }
}

void MemoryCheckpointer::delete_thread(std::string const& thread_id) {
  std::string const prefix = thread_id + "|";
  for (auto it = snapshots_.begin(); it != snapshots_.end();)
    it = it->first.rfind(prefix, 0) == 0 ? snapshots_.erase(it) : std::next(it);
  for (auto it = writes_.begin(); it != writes_.end();)
    it = it->first.rfind(prefix, 0) == 0 ? writes_.erase(it) : std::next(it);
}

// ---------------- MemoryStore ----------------

std::string MemoryStore::join(std::vector<std::string> const& ns) {
  std::string out;
  for (std::size_t i = 0; i < ns.size(); ++i) {
    if (i) out += "/";
    out += ns[i];
  }
  return out;
}

void MemoryStore::put(Namespace const& ns, std::string const& key, Json value) {
  auto& bucket = items_[join(ns)];
  auto it = bucket.find(key);
  if (it != bucket.end()) {
    it->second.value = std::move(value);
    it->second.updated_at = now_iso8601();
    return;
  }
  StoreItem item;
  item.ns = ns;
  item.key = key;
  item.value = std::move(value);
  item.created_at = now_iso8601();
  item.updated_at = item.created_at;
  bucket.emplace(key, std::move(item));
}

std::optional<StoreItem> MemoryStore::get(Namespace const& ns, std::string const& key) {
  auto const bucket = items_.find(join(ns));
  if (bucket == items_.end()) return std::nullopt;
  auto const it = bucket->second.find(key);
  if (it == bucket->second.end()) return std::nullopt;
  return it->second;
}

bool MemoryStore::del(Namespace const& ns, std::string const& key) {
  auto const bucket = items_.find(join(ns));
  if (bucket == items_.end()) return false;
  return bucket->second.erase(key) > 0;
}

std::vector<StoreItem> MemoryStore::search(Namespace const& ns_prefix, std::string const& query,
                                           std::size_t limit) {
  std::vector<StoreItem> out;
  for (auto const& [ns_key, bucket] : items_) {
    (void)ns_key;
    for (auto const& [key, item] : bucket) {
      if (!starts_with(item.ns, ns_prefix)) continue;
      if (!query.empty()) {
        bool hit = key.find(query) != std::string::npos ||
                   item.value.dump().find(query) != std::string::npos;
        if (!hit) continue;
      }
      out.push_back(item);
    }
  }
  std::sort(out.begin(), out.end(), [](StoreItem const& a, StoreItem const& b) {
    if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;  // 新的在前
    return a.key < b.key;                                                 // 稳定
  });
  if (limit > 0 && out.size() > limit) out.resize(limit);
  return out;
}

std::vector<Namespace> MemoryStore::list_namespaces(Namespace const& ns_prefix) {
  std::vector<Namespace> out;
  for (auto const& [ns_key, bucket] : items_) {
    (void)ns_key;
    if (bucket.empty()) continue;
    Namespace const& ns = bucket.begin()->second.ns;
    if (!starts_with(ns, ns_prefix)) continue;
    if (std::find(out.begin(), out.end(), ns) == out.end()) out.push_back(ns);
  }
  return out;
}

}  // namespace cppchain::state