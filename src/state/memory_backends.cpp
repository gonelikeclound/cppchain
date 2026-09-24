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

}  // namespace

// ---------------- MemoryCheckpointer ----------------

std::string MemoryCheckpointer::key_of(std::string const& thread_id, std::string const& ns) {
  return thread_id + "|" + ns;
}

std::string MemoryCheckpointer::put(std::string const& thread_id, std::string const& checkpoint_ns,
                                    std::optional<std::string> parent_checkpoint_id, Json values,
                                    Json metadata) {
  std::string const id = "ckpt-" + std::to_string(++counter_);
  StateSnapshot snap;
  snap.values = std::move(values);
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