// cppchain —— src/state/file_checkpointer.cpp
// 落盘 Checkpointer（见 checkpointer.hpp 的类型注释）。
// 落盘纪律与 agent::FileJournal 完全一致：写临时文件 → rename 原子替换；
// 损坏时默认抛异常（fail-closed），不静默丢历史。
#include "cppchain/state/checkpointer.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace cppchain::state {

FileCheckpointer::FileCheckpointer(std::string path, OnCorrupt on_corrupt)
    : path_(std::move(path)) {
  std::ifstream in(path_);
  if (!in) return;  // 首次使用：文件不存在是正常的

  nlohmann::json j;
  try {
    in >> j;
  } catch (std::exception const& e) {
    if (on_corrupt == OnCorrupt::StartEmpty) return;
    throw std::runtime_error(
        "FileCheckpointer: 历史文件损坏，拒绝以空历史启动（fail-closed，"
        "否则会悄悄丢掉用户的会话历史）：" + path_ + " —— " + e.what());
  }
  inner_.load(j);
}

void FileCheckpointer::persist() {
  // 原子写：先写临时文件，再 rename（同目录 rename 在 POSIX 上是原子的）。
  // 父目录不存在则先建 —— 首次使用时 .cppchain/ 还没有。
  if (auto const parent = std::filesystem::path(path_).parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
      throw std::runtime_error("FileCheckpointer: 无法创建目录 " + parent.string() + ": " +
                              ec.message());
  }

  std::string const tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("FileCheckpointer: 无法写入 " + tmp);
    out << inner_.dump().dump(2);
    out.flush();
    if (!out) throw std::runtime_error("FileCheckpointer: 写入失败 " + tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec)
    throw std::runtime_error("FileCheckpointer: rename 失败 " + tmp + " → " + path_ + ": " +
                             ec.message());
  ++saves_;
}

std::string FileCheckpointer::put(std::string const& thread_id, std::string const& checkpoint_ns,
                                  std::optional<std::string> parent_checkpoint_id, Json values,
                                  std::vector<Packet> next, Json metadata) {
  std::string const id = inner_.put(thread_id, checkpoint_ns, std::move(parent_checkpoint_id),
                                    std::move(values), std::move(next), std::move(metadata));
  persist();
  return id;
}

void FileCheckpointer::put_writes(std::string const& thread_id, std::string const& checkpoint_ns,
                                  std::string const& checkpoint_id,
                                  std::vector<WriteRecord> writes) {
  inner_.put_writes(thread_id, checkpoint_ns, checkpoint_id, std::move(writes));
  persist();
}

std::vector<WriteRecord> FileCheckpointer::get_writes(std::string const& thread_id,
                                                      std::string const& checkpoint_ns,
                                                      std::string const& checkpoint_id) {
  return inner_.get_writes(thread_id, checkpoint_ns, checkpoint_id);
}

std::optional<StateSnapshot> FileCheckpointer::get_tuple(
    std::string const& thread_id, std::string const& checkpoint_ns,
    std::optional<std::string> checkpoint_id) {
  return inner_.get_tuple(thread_id, checkpoint_ns, std::move(checkpoint_id));
}

std::vector<StateSnapshot> FileCheckpointer::list(std::string const& thread_id,
                                                  std::string const& checkpoint_ns,
                                                  std::size_t limit) {
  return inner_.list(thread_id, checkpoint_ns, limit);
}

void FileCheckpointer::delete_thread(std::string const& thread_id) {
  inner_.delete_thread(thread_id);
  persist();
}

}  // namespace cppchain::state
