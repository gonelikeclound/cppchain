// cppchain —— state/checkpointer.hpp
// 短时记忆：线程内的状态快照（M5）。参照 langchain Checkpointer 五方法。
// 内存实现先行；SQLite 等只需换实现、接口不变。
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cppchain::state {

using Json = nlohmann::json;

// 细粒度写入（per task/node）—— M6 才有真实语义，M5 只做存取
struct WriteRecord {
  std::string task_id;
  std::string channel;
  Json value;
};

struct StateSnapshot {
  Json values;                                  // 通道值（含 messages）
  std::vector<std::string> next;                // 下一批节点（M6 用）
  Json config = Json::object();                 // thread_id / checkpoint_id / checkpoint_ns
  Json metadata = Json::object();               // source(input/loop/update) / step
  std::optional<std::string> parent_checkpoint_id;
};

class Checkpointer {
 public:
  virtual ~Checkpointer() = default;

  // 写入一个快照，返回新 checkpoint_id（并记录 parent 形成链）。
  // next = 该步之后待执行的节点（恢复语义："回到该步边界继续"）
  virtual std::string put(std::string const& thread_id, std::string const& checkpoint_ns,
                          std::optional<std::string> parent_checkpoint_id, Json values,
                          std::vector<std::string> next, Json metadata) = 0;

  virtual void put_writes(std::string const& thread_id, std::string const& checkpoint_ns,
                          std::string const& checkpoint_id, std::vector<WriteRecord> writes) = 0;
  virtual std::vector<WriteRecord> get_writes(std::string const& thread_id,
                                              std::string const& checkpoint_ns,
                                              std::string const& checkpoint_id) = 0;

  // checkpoint_id 为空 → 取最新
  virtual std::optional<StateSnapshot> get_tuple(std::string const& thread_id,
                                                 std::string const& checkpoint_ns,
                                                 std::optional<std::string> checkpoint_id) = 0;

  // 新的在前；limit=0 表示不限
  virtual std::vector<StateSnapshot> list(std::string const& thread_id,
                                          std::string const& checkpoint_ns, std::size_t limit) = 0;

  virtual void delete_thread(std::string const& thread_id) = 0;
};

// ---- 内存实现 ----
class MemoryCheckpointer final : public Checkpointer {
 public:
  std::string put(std::string const& thread_id, std::string const& checkpoint_ns,
                  std::optional<std::string> parent_checkpoint_id, Json values,
                  std::vector<std::string> next, Json metadata) override;

  void put_writes(std::string const& thread_id, std::string const& checkpoint_ns,
                  std::string const& checkpoint_id, std::vector<WriteRecord> writes) override;
  std::vector<WriteRecord> get_writes(std::string const& thread_id, std::string const& checkpoint_ns,
                                      std::string const& checkpoint_id) override;

  std::optional<StateSnapshot> get_tuple(std::string const& thread_id,
                                         std::string const& checkpoint_ns,
                                         std::optional<std::string> checkpoint_id) override;

  std::vector<StateSnapshot> list(std::string const& thread_id, std::string const& checkpoint_ns,
                                  std::size_t limit) override;

  void delete_thread(std::string const& thread_id) override;

  [[nodiscard]] std::size_t thread_count() const { return snapshots_.size(); }

 private:
  static std::string key_of(std::string const& thread_id, std::string const& ns);
  std::unordered_map<std::string, std::vector<StateSnapshot>> snapshots_;  // key → 按时间正序
  std::unordered_map<std::string, std::vector<WriteRecord>> writes_;       // key|checkpoint_id
  long counter_ = 0;
};

}  // namespace cppchain::state