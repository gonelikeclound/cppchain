// cppchain —— src/workflow/session_log.cpp
#include "cppchain/workflow/session_log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "cppchain/message/serializer.hpp"
#include "cppchain/state/state.hpp"
#include "cppchain/workflow/refs.hpp"  // kMessagesKey

namespace cppchain::workflow {

using Json = nlohmann::json;

namespace {

// UTC ISO-8601，毫秒精度（docs/12 §6.2 的 ts 字段示例：2026-10-05T17:05:01.456Z）
std::string now_iso() {
  using namespace std::chrono;
  auto const now = system_clock::now();
  std::time_t const t = system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  auto const ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long long>(ms.count()));
  return buf;
}

// 整文件读（文件不存在 → 空串）
std::string read_all(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_all(std::string const& path, std::string const& content) {
  std::ofstream out(path, std::ios::trunc | std::ios::binary);
  if (!out) throw std::runtime_error("session log: 无法重写 " + path);
  out << content;
  out.flush();
  if (!out) throw std::runtime_error("session log: 重写失败 " + path);
}

// 让会话目录不会被误提交：.cppchain/.gitignore 内容 `*` + `!.gitignore`。
// 只在缺失时写、写不了就算了（便利不是正事）。
void ensure_dir_ignored(std::filesystem::path const& dir) {
  auto const gi = dir / ".gitignore";
  std::error_code ec;
  if (std::filesystem::exists(gi, ec)) return;
  std::ofstream out(gi, std::ios::trunc);
  if (!out) return;
  out << "*\n!.gitignore\n";
}

std::vector<message::Message> messages_from_json(Json const& j) {
  std::vector<message::Message> out;
  if (j.is_null()) return out;
  for (Json const& raw : j)
    try {
      out.push_back(raw.get<message::Message>());
    } catch (std::exception const&) {
      // 坏消息跳过；形状由 validator + 本模块的一致性保证
    }
  return out;
}

}  // namespace

std::string session_path(std::string const& workspace, std::string const& session) {
  std::string const name = session.empty() ? "session" : "session-" + session;
  return (std::filesystem::path(workspace) / ".cppchain" / (name + ".log.jsonl")).string();
}

SessionLog::SessionLog(std::string path) : path_(std::move(path)) {
  // 1) 父目录 + .gitignore
  if (auto const parent = std::filesystem::path(path_).parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
      throw std::runtime_error("session log: 无法创建目录 " + parent.string() + ": " + ec.message());
    ensure_dir_ignored(parent);
  }

  // 2) 打开时修复：末尾半行（崩溃残迹）截掉；完整但缺换行的记录补上换行
  std::string content = read_all(path_);
  if (!content.empty() && content.back() != '\n') {
    std::size_t const nl = content.rfind('\n');
    std::string const tail = content.substr(nl == std::string::npos ? 0 : nl + 1);
    if (!tail.empty()) {
      bool parsed = false;
      try {
        parsed = Json::parse(tail).is_object();  // 半行（非法 JSON）→ 抛；完整记录 → 对象
      } catch (std::exception const&) {
      }
      if (parsed) {
        content += '\n';  // 完整一条但没换行（崩溃窗口）：补上，保留
      } else {
        content = nl == std::string::npos ? std::string{} : content.substr(0, nl + 1);
        ++truncated_;  // 半行残迹：丢弃并计数（docs/12 §6.6「末尾半行容忍」）
      }
      write_all(path_, content);
    }
  }

  // 3) seq 从已有记录继续（跨进程续写同一会话）
  {
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      try {
        Json j = Json::parse(line);
        if (j.is_object() && j.contains("seq") && j["seq"].is_number_unsigned())
          seq_ = std::max(seq_, j["seq"].get<std::uint64_t>());
      } catch (std::exception const&) {
      }
    }
  }

  // 4) 追加打开
  out_.open(path_, std::ios::app);
  if (!out_) throw std::runtime_error("session log: 无法打开 " + path_);
}

SessionLog::~SessionLog() {
  if (out_) out_.flush();
}

void SessionLog::write(Json rec) {
  std::lock_guard<std::mutex> lk(mu_);  // 同锁内：seq 分配 + 写盘 → 文件顺序 == seq 顺序
  if (!out_) throw std::runtime_error("session log: 未打开 " + path_);
  rec["seq"] = ++seq_;
  rec["ts"] = now_iso();
  out_ << rec.dump() << '\n';
  out_.flush();
  if (!out_) throw std::runtime_error("session log: 写入失败 " + path_);
}

void SessionLog::engine_enter(std::string const& run, std::string const& node,
                              std::string const& node_kind) {
  write(Json{{"run", run},
             {"source", "engine"},
             {"node", node},
             {"node_kind", node_kind},
             {"phase", "enter"}});
}

void SessionLog::engine_exit(std::string const& run, std::string const& node,
                             std::string const& node_kind, bool ok, std::string_view status,
                             std::int64_t ms, std::optional<Json> const& update) {
  Json rec{{"run", run},
           {"source", "engine"},
           {"node", node},
           {"node_kind", node_kind},
           {"phase", ok ? "exit" : "error"},  // errors 独立 phase，与 enter 配对（§6.3）
           {"status", std::string(status)},
           {"ms", ms}};
  if (ok && update.has_value()) rec["update"] = *update;  // 异常 / 回声 → 不落 update 键
  write(std::move(rec));
}

void SessionLog::log(std::string_view level, std::string_view msg, Json const& data) {
  write(Json{{"source", "node"}, {"level", std::string(level)}, {"msg", std::string(msg)},
             {"data", data}});
}

void SessionLog::host_log(std::string_view msg, Json const& data) {
  write(Json{{"source", "host"}, {"level", "info"}, {"msg", std::string(msg)}, {"data", data}});
}

std::vector<Json> SessionLog::records() const {
  std::vector<Json> out;
  std::ifstream in(path_);
  if (!in) return out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    try {
      out.push_back(Json::parse(line));
    } catch (std::exception const&) {
    }
  }
  return out;
}

std::vector<message::Message> replay_session_messages(std::string const& path) {
  std::vector<Json> recs;
  {
    std::ifstream in(path);
    if (!in) return {};  // 首次使用：文件不存在是正常的
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      try {
        recs.push_back(Json::parse(line));
      } catch (std::exception const&) {
        // 末尾半行已由 SessionLog 构造时清掉；这里再防御性地跳过（只读，不改文件）
      }
    }
  }

  // replay 铁律（docs/12 §7.1）：只取 source=engine && phase=exit && status=ok 的记录，
  // 按 seq 顺序经 reducer 重建 messages —— 节点的诊断/宿主记录即使带 update 也不参与。
  // 用固定归约器即可：messages 通道的契约就是 add_messages（编译期恒等设置）。
  state::StateSchema schema = state::StateSchema::messages_state();
  Json state = Json::object();
  for (Json const& r : recs)
    if (r.value("source", "") == "engine" && r.value("phase", "") == "exit" &&
        r.value("status", "") == "ok")
      if (auto it = r.find("update"); it != r.end() && it->is_object())
        state = schema.apply(state, *it);

  auto const mit = state.find(std::string(kMessagesKey));  // 见 workflow/refs.hpp
  if (mit == state.end() || !mit->is_array()) return {};
  return messages_from_json(*mit);
}

}  // namespace cppchain::workflow