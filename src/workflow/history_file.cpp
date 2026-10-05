// cppchain —— src/workflow/history_file.cpp
#include "cppchain/workflow/history_file.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "cppchain/message/serializer.hpp"

namespace cppchain::workflow {

namespace {
constexpr int kVersion = 1;
}  // namespace

std::string session_path(std::string const& workspace, std::string const& session) {
  std::string const name = session.empty() ? "session" : "session-" + session;
  return (std::filesystem::path(workspace) / ".cppchain" / (name + ".json")).string();
}

std::vector<message::Message> load_history_file(std::string const& path,
                                                bool start_empty_on_corrupt) {
  std::vector<message::Message> out;
  std::ifstream in(path);
  if (!in) return out;  // 首次使用：文件不存在是正常的

  nlohmann::json j;
  try {
    in >> j;
  } catch (std::exception const& e) {
    if (start_empty_on_corrupt) return out;
    throw std::runtime_error("history 文件损坏，拒绝以空历史启动（fail-closed，"
                             "否则会悄悄丢掉会话历史）：" + path + " —— " + e.what());
  }
  if (!j.is_object() || !j.contains("messages") || !j["messages"].is_array()) {
    if (start_empty_on_corrupt) return out;
    throw std::runtime_error("history 文件形状不对（应为 {version, session, messages[]}）：" + path);
  }
  for (nlohmann::json const& raw : j["messages"]) out.push_back(raw.get<message::Message>());
  return out;
}

void save_history_file(std::string const& path, std::span<message::Message const> msgs,
                       std::string const& session) {
  nlohmann::json arr = nlohmann::json::array();
  for (message::Message const& m : msgs) arr.push_back(m);
  nlohmann::json doc{{"version", kVersion}, {"session", session}, {"messages", std::move(arr)}};

  if (auto const parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
      throw std::runtime_error("history: 无法创建目录 " + parent.string() + ": " + ec.message());
  }

  // 原子写：临时文件 → rename（同目录 rename 在 POSIX 上原子）
  std::string const tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("history: 无法写入 " + tmp);
    out << doc.dump(2);
    out.flush();
    if (!out) throw std::runtime_error("history: 写入失败 " + tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec)
    throw std::runtime_error("history: rename 失败 " + tmp + " → " + path + ": " + ec.message());
}

}  // namespace cppchain::workflow
