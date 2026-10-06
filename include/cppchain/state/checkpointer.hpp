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

// 待执行单元（M10 起 next 从「节点名列表」升级为「包队列」）：
//   · 普通包（全局状态包）：state 为空，节点读主 state；
//   · Send 扇出包：state 为该包独立的局部输入（节点读 *state），结果仍经 reducer 写回主 state。
struct Packet {
  std::string node;
  std::optional<Json> state;

  Packet() = default;
  Packet(std::string n) : node(std::move(n)) {}
  Packet(std::string n, std::optional<Json> s) : node(std::move(n)), state(std::move(s)) {}

  friend bool operator==(Packet const&, Packet const&) = default;
};

struct StateSnapshot {
  Json values;                                  // 通道值（含 messages）
  std::vector<Packet> next;                     // 下一批待执行包（M6 为节点名，M10 为包队列）
  Json config = Json::object();                 // thread_id / checkpoint_id / checkpoint_ns
  Json metadata = Json::object();               // source(input/loop/update) / step
  std::optional<std::string> parent_checkpoint_id;
};

class Checkpointer {
 public:
  virtual ~Checkpointer() = default;

  // 写入一个快照，返回新 checkpoint_id（并记录 parent 形成链）。
  // next = 该步之后待执行的包队列（恢复语义："回到该步边界继续"）
  virtual std::string put(std::string const& thread_id, std::string const& checkpoint_ns,
                          std::optional<std::string> parent_checkpoint_id, Json values,
                          std::vector<Packet> next, Json metadata) = 0;

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
                  std::vector<Packet> next, Json metadata) override;

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

  // 持久化（对标 agent::MemoryJournal 的同名方法）：FileCheckpointer 复用它们，
  // 也可自行落盘 / 回读。
  [[nodiscard]] nlohmann::json dump() const;
  void load(nlohmann::json const& j);

 private:
  static std::string key_of(std::string const& thread_id, std::string const& ns);
  std::unordered_map<std::string, std::vector<StateSnapshot>> snapshots_;  // key → 按时间正序
  std::unordered_map<std::string, std::vector<WriteRecord>> writes_;       // key|checkpoint_id
  long counter_ = 0;
};

// ---- 落盘实现：把快照链按**工作路径**持久化 ----
//
// 为什么需要它：MemoryCheckpointer 一退出进程就没了，"跨命令连续对话"随之失效。
// 形态与 agent::FileJournal 完全一致：内嵌一个 MemoryCheckpointer 承载数据结构，
// 构造时载入、每次变更后**原子落盘**（写临时文件 → rename，同目录 rename 在 POSIX 上原子）。
//
// 安全默认 **fail-closed**：文件存在但损坏时抛异常，而不是"静默从空历史开始"——
// 后者等于悄悄把用户的会话历史丢掉。
class FileCheckpointer final : public Checkpointer {
 public:
  enum class OnCorrupt { Throw, StartEmpty };

  explicit FileCheckpointer(std::string path, OnCorrupt on_corrupt = OnCorrupt::Throw);

  std::string put(std::string const& thread_id, std::string const& checkpoint_ns,
                  std::optional<std::string> parent_checkpoint_id, Json values,
                  std::vector<Packet> next, Json metadata) override;

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

  [[nodiscard]] std::string const& path() const { return path_; }
  [[nodiscard]] std::size_t saves() const { return saves_; }  // 落盘次数（便于观测）
  [[nodiscard]] std::size_t thread_count() const { return inner_.thread_count(); }

 private:
  void persist();  // 原子写盘

  std::string path_;
  MemoryCheckpointer inner_;
  std::size_t saves_ = 0;
};

}  // namespace cppchain::state