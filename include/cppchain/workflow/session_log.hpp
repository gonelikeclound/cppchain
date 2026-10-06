// cppchain —— workflow/session_log.hpp
// 统一日志 + 会话历史的事实来源（docs/02-arch-design/12 §6、§7）。
//
// 一个会话一个 `.log.jsonl`（append-only），多方写入、来源可区分：
//   · source=engine —— 引擎结构事件（enter/exit/error + 状态增量 `update`）。**replay 唯一读它**
//   · source=node   —— 节点自报诊断（召回几条、调了哪个模型…）。**不参与 replay**
//   · source=host   —— 宿主运行参数（命令行、加载了哪个库…）。不参与 replay
//
// replay 铁律见 §7.1：只取 source=engine && phase=exit && status=ok 的记录，
// 按 seq 依次 schema.apply 重建 messages —— 节点即使把 update 写进记录也不参与状态重建。
//
// 与旧 `history_file.*`（D 阶段全量重写的独立历史文件）的关系：历史即本日志的投影，
// 不再单独 save。save_history 节点删除，load_history 改为对日志的 replay。见 docs/12 §7。
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/message/message.hpp"

namespace cppchain::workflow {

// `<workspace>/.cppchain/session-<name>.log.jsonl`；name 为空时退化为 `session.log.jsonl`
// （session_path 与旧版同名同语义，仅扩展名从 .json 改为 .log.jsonl）。
[[nodiscard]] std::string session_path(std::string const& workspace, std::string const& session);

// 节点侧写日志的接口（docs/12 §6.4）：引擎实现、触发期注入给节点，不是全局变量。
struct NodeLog {
  virtual ~NodeLog() = default;
  virtual void log(std::string_view level, std::string_view msg,
                   nlohmann::json const& data = nlohmann::json::object()) = 0;
};

// 一个会话日志文件。线程安全：**写入互斥同时管 seq 分配与文件追加**（等价于"seq 原子分配 +
// 写锁"：文件内顺序 == seq 顺序，不会行交织）。节点日记（run_tools 里工具并行）也走同一把锁。
//
// 打开时（构造）：
//   · 建父目录 + `.cppchain/.gitignore`（历史不被误提交，与旧 history_file 同一套纪律）；
//   · 丢弃**末尾半行**（崩溃残迹）：若最后一行不构成合法 JSON，截断并计一条告警；
//   · seq 从已有记录继续（跨进程续写同一会话）。
//
// 生命周期：由宿主按工作路径开启并持有；fire_once 每触发期注入（见 FireOptions）。
// 构造失败（目录不可写等）抛异常 —— fail-closed，不静默丢会话。
class SessionLog : public NodeLog {
 public:
  explicit SessionLog(std::string path);
  ~SessionLog() override;
  SessionLog(SessionLog const&) = delete;
  SessionLog& operator=(SessionLog const&) = delete;

  // ---- 引擎结构事件（source=engine）----
  // enter 与 exit 成对；exit 的 update 是本次节点的状态增量（经 reducer 归约前的原始值）。
  // ok=false → status:error 且**无 update**（异常路径没有可 apply 的更新）；
  // update=nullopt（即使 ok）→ 不落 update 键 —— load_history 的回声 update 就借此省略。
  void engine_enter(std::string const& run, std::string const& node, std::string const& node_kind);
  void engine_exit(std::string const& run, std::string const& node, std::string const& node_kind,
                   bool ok, std::string_view status, std::int64_t ms,
                   std::optional<nlohmann::json> const& update = std::nullopt);
  // M10: 节点调 interrupt 悬挂时写一条 phase:"interrupt" 记录（与 enter 配对，不误记 error）。
  // replay 铁律只读 exit+ok，本记录天然不参与状态重建。
  void engine_interrupt(std::string const& run, std::string const& node,
                        std::string const& node_kind, std::int64_t ms);

  // ---- 节点自报（source=node，不参与 replay）----
  void log(std::string_view level, std::string_view msg,
           nlohmann::json const& data = nlohmann::json::object()) override;

  // ---- 宿主（source=host，不参与 replay）----
  void host_log(std::string_view msg, nlohmann::json const& data = {});

  // 全部记录（文件顺序即 seq 升序；末尾半行已在构造时清除）。
  [[nodiscard]] std::vector<nlohmann::json> records() const;

  [[nodiscard]] std::string const& path() const { return path_; }
  [[nodiscard]] std::size_t truncated_lines() const { return truncated_; }

 private:
  void write(nlohmann::json rec);

  std::string path_;
  std::ofstream out_;      // 追加句柄；records() 走独立只读路径（不互相影响）
  std::uint64_t seq_ = 0;  // 在 mu_ 内：seq = ++seq_ 后立即写盘 → 文件内顺序 == seq 顺序
  std::mutex mu_;
  std::size_t truncated_ = 0;
};

// 触发期注入的"会话日志句柄"：compiler 创建并随每个图节点 wrapper 捕获；fire_once 每触发周期
// 注入 `log` 与 `run`，触发结束清空。节点在运行期经 `nlog()` 拿注入的 logger（docs/12 §6.4）。
struct SessionSink {
  SessionLog* log = nullptr;  // 本次触发期的会话日志；null = 本次不落日志（引擎也不写 input）
  std::string run;            // 本次触发 id（如 "r-1"），fire_once 生成
  // M10: host 选择把引擎事件经 FireOptions.stream 走 graph 事件通道（由宿主投影为日志）时，
  // wrapper 让位不再直接写结构事件 —— 双源互斥，防止同一节点被记两遍。
  bool streaming = false;
  [[nodiscard]] NodeLog* nlog() const { return log; }  // SessionLog IS-A NodeLog
};

// ---- 读取端（replay，§7.1）----
// 读会话日志 → 按 seq 依次 apply（source=engine && phase=exit && status=ok 记录）→ 取 messages。
// 只读不写（replay 不该修复/变更文件）；文件缺失（首次使用）→ 空。
[[nodiscard]] std::vector<message::Message> replay_session_messages(std::string const& path);

}  // namespace cppchain::workflow