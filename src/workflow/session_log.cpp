// cppchain —— src/workflow/session_log.cpp
#include "cppchain/workflow/session_log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cppchain/message/utf8.hpp"  // utf8_safe_dump：外部文本坏字节也不让会话日志 dump 崩
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "cppchain/message/serializer.hpp"
#include "cppchain/state/state.hpp"
#include "cppchain/workflow/refs.hpp"  // kMessagesKey
#include "cppchain/workflow/wfr.hpp"   // .wfr 元数据目录约定

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

// 让 .wfr 根目录的**运行态**（sessions/checkpoints）不会被误提交；
// workflows/ 源要提交，因此忽略项是这两个目录而不是通配 `*`。
// 只在缺失时写、写不了就算了（便利不是正事）。
void ensure_dir_ignored(std::filesystem::path const& dir) {
  auto const gi = dir / ".gitignore";
  std::error_code ec;
  if (std::filesystem::exists(gi, ec)) return;
  std::ofstream out(gi, std::ios::trunc);
  if (!out) return;
  out << "sessions/\ncheckpoints/\n";
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
  // .cppchain → .wfr（wfr 工具统一元数据目录）：日志落在 <ws>/.wfr/sessions/。
  // 顺带建立 .wfr 根与 .gitignore（忽略运行态、保留 workflows/ 源）。
  std::filesystem::path const root = wfr_root(workspace);
  {  // 建根 + gitignore；失败不致命（目录由 SessionLog 构造兜底再建）
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (!ec) ensure_dir_ignored(root);
  }
  std::string const name = session.empty() ? "session" : "session-" + session;
  return (root / "sessions" / (name + ".log.jsonl")).string();
}

SessionLog::SessionLog(std::string path, std::string workflow_meta)
    : path_(std::move(path)), workflow_(std::move(workflow_meta)) {
  // 1) 父目录（.wfr 根与 .gitignore 已由 session_path 保证；这里兜底直接构造的场景）
  if (auto const parent = std::filesystem::path(path_).parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
      throw std::runtime_error("session log: 无法创建目录 " + parent.string() + ": " + ec.message());
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

  // 3) seq 从已有记录继续（跨进程续写同一会话）；并记下已有的最大 run 编号
  //   （"r-N" 的 N）—— CLI 每次起跑前把引擎 run 计数垫到该值，跨调用的轮次才不重号
  {
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      try {
        Json j = Json::parse(line);
        if (j.is_object() && j.contains("seq") && j["seq"].is_number_unsigned())
          seq_ = std::max(seq_, j["seq"].get<std::uint64_t>());
        if (j.is_object() && j.contains("run") && j["run"].is_string()) {
          std::string const r = j["run"].get<std::string>();
          if (r.size() >= 2 && r[0] == 'r' && r[1] == '-') {
            try {
              max_run_ = std::max(max_run_, static_cast<std::int64_t>(std::stoll(r.substr(2))));
            } catch (std::exception const&) {
            }
          }
        }
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
  if (!workflow_.empty()) rec["workflow"] = workflow_;
  rec["seq"] = ++seq_;
  rec["ts"] = now_iso();
  out_ << message::utf8_safe_dump(rec) << '\n';
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

void SessionLog::engine_interrupt(std::string const& run, std::string const& node,
                                  std::string const& node_kind, std::int64_t ms) {
  write(Json{{"run", run},
             {"source", "engine"},
             {"node", node},
             {"node_kind", node_kind},
             {"phase", "interrupt"},
             {"status", "interrupt"},
             {"ms", ms}});
}

void SessionLog::log(std::string_view level, std::string_view msg, Json const& data) {
  write(Json{{"source", "node"}, {"level", std::string(level)}, {"msg", std::string(msg)},
             {"data", data}});
}

void SessionLog::node_log(std::string const& run, std::string const& node,
                          std::string_view level, std::string_view msg, Json const& data) {
  write(Json{{"source", "node"},
             {"run", run},
             {"node", node},
             {"level", std::string(level)},
             {"msg", std::string(msg)},
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

tool::ExecutionGuard::LogReader replay_exec_log_reader(std::vector<message::Message> const& msgs) {
  // 扫描消息流，按"AI.tool_calls（发起）→ ToolMessage（完成）"配对建状态表。
  //   遍历到 Tool 消息时，其 tool_call_id 指向某次发起 —— 那次调用即完成。
  struct CallInfo {
    std::string key;   // 幂等键（NameAndArgs）
    bool done = false; // 是否见过对应 ToolMessage
  };
  std::unordered_map<std::string, CallInfo> calls;  // tool_call_id → 调用
  for (message::Message const& m : msgs) {
    if (m.type == message::Role::Ai) {
      for (message::ToolCall const& tc : m.tool_calls) {
        CallInfo& ci = calls[std::string{tc.id.empty() ? tc.name : tc.id}];
        ci.key = tool::ExecutionGuard::key_for(tc, tool::ExecKeyMode::NameAndArgs);
        // 同一 AI 消息里 id 与 name/args 一定成对；若 id 撞（不太可能），后写覆盖，可接受
      }
    } else if (m.type == message::Role::Tool && m.tool_call_id) {
      auto it = calls.find(*m.tool_call_id);
      if (it != calls.end()) it->second.done = true;
    }
  }

  // 汇总：同一 key 出现多次 → 取"最后一次"语义：展开到第一次完成之前算 intent，
  // 一旦出现过 ToolMessage → completed。sorted 靠输入序：calls 遍历顺序即消息序。
  // 简化：key 维度聚合 ——
  //   · key 有一个**已完成**调用 → completed（内容取最后一次完成的）
  //   · key 只有发起、无任何完成 → intent 残留
  struct KeyState {
    bool completed = false;
    bool intent = false;
    std::string content;
    std::string status = "success";
  };
  std::unordered_map<std::string, KeyState> states;
  // 重建 ToolMessage content 需要回查消息：这里直接在扫描时收集。
  // 上面只记了 done，没存 content —— 重扫一遍收集（消息不多，成本可忽略）。
  for (message::Message const& m : msgs) {
    if (m.type != message::Role::Tool || !m.tool_call_id) continue;
    auto const it = calls.find(*m.tool_call_id);
    if (it == calls.end() || !it->second.done) continue;
    KeyState& ks = states[it->second.key];
    ks.completed = true;
    ks.content = m.text();   // 最后一次完成的内容
    ks.status = m.status;
  }
  for (auto const& [id, ci] : calls)
    if (!ci.done && !ci.key.empty()) {
      auto& ks = states[ci.key];
      if (!ks.completed) ks.intent = true;  // 已完成过的不覆盖成 intent
    }

  // LogState 闭包：只读快照（msgs 副本），运行期可被并行/多次查 —— 安全。
  auto table = std::make_shared<std::unordered_map<std::string, KeyState>>(std::move(states));
  return [table](std::string const& key) -> tool::ExecutionGuard::LogState {
    auto const it = table->find(key);
    if (it == table->end()) return {};  // 未开始
    tool::ExecutionGuard::LogState st;
    if (it->second.completed) {
      st.completed = true;
      st.content = it->second.content;
      st.status = it->second.status;
    }
    if (it->second.intent) st.intent = true;
    return st;
  };
}

}  // namespace cppchain::workflow