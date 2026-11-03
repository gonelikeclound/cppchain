// demo: sa_demo —— SystemAgent（SA）骨架 demo（S1 + S2，纯离线）
//
// SA 不是一个 agent，而是一组 agent（problems/64 §5.1）：
//   核心 agent（分诊/编排，只有只读观测工具 = I1 最小形态）
//   + 电话域 agent（samock 的 make_call/FSM 里程碑/时钟）
//   + 消息域 agent（samock 的 send_message）
// 假世界全部来自独立 so：tests/plugin_demo/samock.cpp → libsamock.so。
//
// S1 里程碑内容（problems/64 §5.5 S1）：
//   ① samock 装载：tool::load_dynamic_tools（wfr 动态工具库同一加载器，fail-closed）；
//   ② 世界空跑六场景：断言**查世界不查返回值**（机制测试不经自然语言，工具直灌）；
//   ③ SA 组装配：核心 + 电话域 + 消息域三实例构造 + I1（核心工具集全只读）。
//
// S2 里程碑内容（problems/64 §5.5 S2，C7 简版组织闭环）：
//   ① samock 新增**平台侧电话 FSM**：每次拨号 = 一个 Session，状态 = fold(剧本, dialed_at, now)
//      （纯派生、无后台线程、逐字节确定）；剧本 = attempt1 振铃→拒接 / attempt2 振铃→接通；
//   ② **里程碑事件**（dialing/ringing/rejected/answered/ended）相位跳变时原子发一次（幂等），
//      按 seq 游标增量读（samock_milestones = 观测归约）；**samock_barge_in** 控制平面注入
//      （仅 Talking 有效，平台挂断发 ended{user_redirected}）；
//   ③ **导演编排**（离线脚本桩弹 tool_calls，Agent 循环真跑工具）：核心 2 次咨询（初始分诊+改道），
//      电话域域内拒接→重拨 attempt2→接通，barge_in 直灌，消息域 send_message 收尾；
//      24 项断言 = 世界终态 + 里程碑全序 7 条 + 唤醒账（核心≤3）。
//
// 运行：./run-demo.sh sa_demo    （全离线；CI 纪律 R5：不碰真模型）
#include <cppchain/agent/agent.hpp>
#include <cppchain/message/message.hpp>
#include <cppchain/model/chat_model.hpp>
#include <cppchain/model/task.hpp>
#include <cppchain/tool/registry.hpp>

#include "sa_guard.hpp"  // S3（M0）：SA 语义层校验纯函数
#include "sa_plan.hpp"   // S7：一句话 → DAG（规划契约 + 静态校验）

#ifdef SA_PLAN_LIVE
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <memory>

#include "support/dotenv.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <generator>
#include <map>
#include <print>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cppchain;
using namespace cppchain::agent;
using namespace cppchain::tool;

using Json = nlohmann::json;

#ifndef SA_SAMOCK_DIR
#define SA_SAMOCK_DIR "build/dev/examples"
#endif

namespace {

// samock 导出的工具清单（与 tests/plugin_demo/samock.cpp 的 tool_<name>_creator 一一对应）。
// 注意：名字必须是干净 C 标识符（loader is_ident 校验）。
constexpr char const* kSamockTools[] = {
    "samock_world_state",    // 只读：世界快照（断言唯一入口，永远说真话）
    "samock_clock_forward",  // 测试控制：推进虚拟钟 N 秒
    "samock_set_honesty",    // 测试控制：honest / lying / partial
    "samock_world_reset",    // 测试控制：清空世界 + 钟 + 诚实度（跨用例隔离）
    "samock_make_call",      // 能力（S2）：假电话 FSM（每次拨号=一个 Session，attempt 显式）
    "samock_barge_in",       // 控制平面（S2）：用户改口直灌（仅通话中有效）
    "samock_send_message",   // 能力（v0）：发送消息
    "samock_milestones",     // 只读（S2）：通话里程碑增量（观测归约，按 seq 游标）
};

// 脚本化模型（纯离线桩，照 phantasma_demo 的 ScriptModel）。
class ScriptModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::size_t invoked = 0;

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
    ++invoked;
    return m;
  }
  std::generator<message::MessageChunk> stream(std::span<Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

// 找 libsamock.so：CPPCHAIN_SAMOCK_DIR env → 编译期 SA_SAMOCK_DIR（examples 构建目录）→ 仓库根兜底。
std::filesystem::path find_samock_so() {
  std::string dir;
  if (char const* env = std::getenv("CPPCHAIN_SAMOCK_DIR"); env != nullptr && *env != '\0')
    dir = env;
  else
    dir = SA_SAMOCK_DIR;
  std::filesystem::path p = std::filesystem::path(dir) / "libsamock.so";
  if (!std::filesystem::exists(p))
    p = std::filesystem::path("build/dev/examples") / "libsamock.so";
  return p;
}

// 直灌执行一把工具（不经过自然语言）：找 spec → handler(args)。失败即记入 fail。
Json run_tool(ToolRegistry const& reg, std::string const& name, Json args,
              std::vector<std::string>& fail) {
  ToolSpec const* spec = reg.find(name);
  if (spec == nullptr) {
    fail.push_back("找不到工具 " + name);
    return Json::object();
  }
  ToolContext ctx;
  ctx.tool_call_id = name;
  auto out = spec->handler(args, ctx);
  if (!out) {
    fail.push_back(name + ": " + out.error().message);
    return Json::object();
  }
  return *out;
}

// 一键取世界快照（断言入口）。
Json world(ToolRegistry const& reg, std::vector<std::string>& fail) {
  return run_tool(reg, "samock_world_state", Json::object(), fail);
}

// ---- S6c · 运行中实时进度（JSONL）----
// 目的：mock 执行本身毫秒级结束，页面只能看到最终结果；这里让 sa_demo **边跑边写**
// 结构化事件并可按 --pace-ms 放慢，页面轮询后能看见节点点亮/消息逐条出现。
// 事件（每行一条，逐行 flush，append-only）：
//   {"t":"run","state":"start","pace_ms":N} / {"t":"run","state":"done"}
//   {"t":"task","state":"start","id":"t1","title":"…"} / {"t":"task","state":"done","id":"t1"}
//   {"t":"dag","task":"t1","dag":{nodes,edges}}
//   {"t":"node","task":"t1","id":"n_call","state":"active|done"}
//   {"t":"msg","task":"t1","msg":{at_sec,kind,from,node,text[,tool]}}
// 纪律：进度是**观测面**，任务日志（tasklog）仍是唯一真相；事件内容与 tasklog 消息同口径。
class Progress {
 public:
  Progress(std::string const& path, long pace_ms) : pace_ms_(pace_ms) {
    if (!path.empty()) {
      os_.open(path, std::ios::trunc);
      if (!os_) std::println("[!] 进度文件打开失败：{}（继续跑，页面无实时进度）", path);
    }
  }
  bool enabled() const { return os_.is_open(); }
  void emit(Json const& e) {
    if (!os_.is_open()) return;
    os_ << e.dump() << '\n';
    os_.flush();
  }
  void node(std::string const& task, std::string const& id, std::string const& state) {
    emit(Json{{"t", "node"}, {"task", task}, {"id", id}, {"state", state}});
  }
  void msg(Json m) {
    emit(Json{{"t", "msg"}, {"task", task_}, {"msg", std::move(m)}});
    pace();
  }
  void pace() {
    if (pace_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms_));
  }
  void set_task(std::string t) { task_ = std::move(t); }
  std::string const& task() const { return task_; }
  void set_clock(std::int64_t c) { clock_ = c; }
  std::int64_t clock() const { return clock_; }
  long pace_ms() const { return pace_ms_; }

 private:
  std::ofstream os_;
  long pace_ms_ = 0;
  std::string task_ = "t1";
  std::int64_t clock_ = 0;
};

// 工具调用的"人话"行：与 tasklog 的 t1 工具行同一口径（页面实时与最终一致）。
std::string tool_msg_text(std::string const& name, Json const& args, Json const& out) {
  if (name == "samock_make_call") {
    std::string s = "make_call{" + args.value("call_id", std::string()) + ", " +
                    args.value("number", std::string()) + ", attempt:" +
                    std::to_string(args.value("attempt", 0)) + "}";
    if (args.value("attempt", 0) > 1) s += " （retry_policy 重拨）";
    return s;
  }
  if (name == "samock_clock_forward")
    return "clock_forward{" + std::to_string(args.value("seconds", 0)) + "}";
  if (name == "samock_send_message")
    return "send_message{" + args.value("msg_id", std::string()) + ", " +
           args.value("to", std::string()) + ", " + args.value("text", std::string()) + "}";
  if (name == "samock_milestones") {
    std::string s = "milestones{since:" + std::to_string(args.value("since", 0)) + "} → [";
    bool first = true, answered = false;
    for (Json const& m : out.value("milestones", Json::array())) {
      if (!first) s += ", ";
      first = false;
      s += m.value("kind", "?") + ":" + std::to_string(m.value("attempt", 0));
      if (m.value("kind", "") == "answered") answered = true;
    }
    s += "]";
    if (answered) s += " —— 已接通";
    return s;
  }
  return name;
}

// 哪些工具调用进实时进度：电话域/消息域白名单；里程碑读只在含非 ended 事件时展示
// （ended 由导演显式 milestone 行承担，避免与最终 tasklog 重复）。
bool live_tool_visible(std::string const& name, Json const& out) {
  if (name == "samock_make_call" || name == "samock_clock_forward" ||
      name == "samock_send_message")
    return true;
  if (name == "samock_milestones") {
    for (Json const& m : out.value("milestones", Json::array()))
      if (m.value("kind", "") != "ended") return true;
    return false;
  }
  return false;
}

// 一个脚本桩的"人类输入"（每次咨询=独立上下文）。
std::vector<Message> one_human(std::string text) {
  return {message::human_message(std::move(text))};
}

// 脚本桩的"带 tool_calls 的 AI 消息"：给 ToolCall 造唯一 id（Agent 循环按 id 回填 ToolMessage）。
Message ai_call(std::string name, Json args, std::string text = "…") {
  static std::uint64_t g = 0;
  Message m = message::ai_message(std::move(text));
  m.tool_calls.push_back(
      message::ToolCall{"sa_tc_" + std::to_string(++g), std::move(name), std::move(args)});
  return m;
}

// 一个场景的核对条目。
struct Verdict {
  bool ok = true;
  std::vector<std::string> lines;
  void c(bool b, std::string what) {
    lines.push_back(std::string("  ") + (b ? "[✓] " : "[✗] ") + std::move(what));
    ok = ok && b;
  }
  void note(std::string s) { lines.push_back("    · " + std::move(s)); }
};

// 场景 A：空世界的基线。
Verdict stage_empty_world(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;
  Json w = world(reg, fail);
  if (!fail.empty()) {
    v.c(false, fail.front());
    return v;
  }
  v.c(w.value("now_sec", -1) == 0, "虚拟钟起步 0（确定性：无系统时钟依赖）");
  v.c(w.value("honesty", "") == "honest", "诚实度缺省 honest");
  v.c(w["calls"].empty(), "通话表空");
  v.c(w.contains("messages") && w["messages"].empty(), "消息表空");
  v.c(w.contains("side_effects") && w["side_effects"].empty(), "副作用台账空");
  v.c(w.value("seq", -1) == 0, "世界改动序号 seq 归零");
  return v;
}

// 场景 B：honest——能力真做，世界真变 + 台账全记 + 幂等。
Verdict stage_honest_ops(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;

  Json call = run_tool(reg, "samock_make_call",
                       Json{{"call_id", "c1"}, {"number", "13800000000"}}, fail);
  v.c(call.value("status", "") == "dialing", "make_call 返回 status=dialing");
  v.note("返回值：" + call.dump());

  Json w = world(reg, fail);
  v.c(!fail.empty() ? false : w["calls"].size() == 1, "世界通话表新增 1 笔");
  v.c(!fail.empty() ? false
                    : w["calls"]["c1"].value("state", "") == "dialing",
      "c1 状态 = dialing");
  v.c(!fail.empty() ? false
                    : w["side_effects"].size() == 1,
      "台账记录拨号笔（make_call 入账）");

  Json msg = run_tool(reg, "samock_send_message",
                      Json{{"msg_id", "m1"}, {"to", "妈妈"}, {"text", "明天来吃饭吗"}}, fail);
  v.c(msg.value("delivered", false), "send_message 返回 delivered=true");

  w = world(reg, fail);
  v.c(!fail.empty() ? false : w["messages"].size() == 1, "世界消息表新增 1 条");
  v.c(!fail.empty() ? false : w["messages"][0]["delivered"] == true, "消息已送达（delivered=true）");
  v.c(!fail.empty() ? false : w["side_effects"].size() == 2,
      "台账新增送信笔（send_message 入账）");

  // 幂等：同 call_id 重拨 → existing，世界不再多一页。
  Json w2 = world(reg, fail);
  std::int64_t const seq_before_dup = fail.empty() ? w2.value("seq", -1) : -1;
  Json dup = run_tool(reg, "samock_make_call",
                      Json{{"call_id", "c1"}, {"number", "13800000000"}}, fail);
  w2 = world(reg, fail);
  v.c(dup.value("status", "") == "existing", "同 call_id 重拨 → status=existing（幂等）");
  v.c(!fail.empty() ? false : w2["calls"].size() == 1, "重拨不新增世界实体");
  v.c(!fail.empty() ? false : w2.value("seq", -1) == seq_before_dup, "幂等命中不推进 seq");
  return v;
}

// 场景 C：lying——能力说谎，断言查世界抓住它（R4 的取证）。
Verdict stage_lying(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;

  Json seth = run_tool(reg, "samock_set_honesty", Json{{"mode", "lying"}}, fail);
  v.c(seth.value("honesty", "") == "lying", "切到 lying 模式");

  Json msg = run_tool(reg, "samock_send_message",
                      Json{{"msg_id", "m2"}, {"to", "妈妈"}, {"text", "天气不错"}}, fail);
  v.c(msg.value("status", "") == "sent" && msg.value("delivered", false),
      "说谎的能力返回「已发送 · delivered=true」");

  Json w = world(reg, fail);
  v.c(!fail.empty() ? false : w["messages"].size() == 1,
      "但世界消息表仍是 1 条（没有 m2）—— 说谎被抓");
  v.c(!fail.empty() ? false : w["side_effects"].size() == 2,
      "台账仍 2 笔（无新笔）");
  v.note("核心教训：断言只查世界、不查返回值（07 R4）—— 返回值说已送达，世界说没有。");
  return v;
}

// 场景 D：partial——世界变一半（实体今天、台账漏记 / 在途未送达），对账 miss 可测。
Verdict stage_partial(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;

  Json seth = run_tool(reg, "samock_set_honesty", Json{{"mode", "partial"}}, fail);
  v.c(seth.value("honesty", "") == "partial", "切到 partial 模式");

  Json call = run_tool(reg, "samock_make_call",
                       Json{{"call_id", "c2"}, {"number", "13900000000"}}, fail);
  v.c(call.value("status", "") == "dialing", "partial 下 make_call 仍返回成功");

  Json msg = run_tool(reg, "samock_send_message",
                      Json{{"msg_id", "m3"}, {"to", "小李"}, {"text", "明天老地方见"}}, fail);
  v.c(msg.contains("delivered") && msg["delivered"] == false,
      "partial 下 send_message 返回 delivered=false（在途）");

  Json w = world(reg, fail);
  v.c(!fail.empty() ? false : w["calls"].size() == 2, "世界通话表 2 笔（c2 入水）");
  v.c(!fail.empty() ? false : w["side_effects"].size() == 2,
      "台账仍 2 笔（c2 漏记账 —— 实体进了、账没记）");
  v.c(!fail.empty() ? false : w["messages"].size() == 2, "世界消息表 2 条（m3 入水）");
  v.c(!fail.empty() ? false : w["messages"][1]["delivered"] == false,
      "m3 在途（delivered=false）—— 世界写了一半");
  v.note("partial = 对账/善后（02 §8.7）的素材：实体 vs 台账差集就是 orphan_resources。");
  return v;
}

// 场景 E：时钟——虚拟钟推进（工具推进的模拟钟，N1-v1）。
Verdict stage_clock(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;
  Json clk = run_tool(reg, "samock_clock_forward", Json{{"seconds", 120}}, fail);
  v.c(clk.value("now_sec", -1) == 120, "clock_forward(120) → now_sec=120");
  Json w = world(reg, fail);
  v.c(!fail.empty() ? false : w.value("now_sec", -1) == 120, "世界快照 now_sec=120");
  v.note("虚拟钟推进 = 通话 120 秒瞬时化（C6 的「一夜 8 小时」靠它才能在 CI 里秒跑）。");
  return v;
}

// 场景 F：重置——跨用例隔离。
Verdict stage_reset(ToolRegistry const& reg) {
  Verdict v;
  std::vector<std::string> fail;
  Json r = run_tool(reg, "samock_world_reset", Json::object(), fail);
  v.c(r.value("reset", false), "world_reset 执行");
  Json w = world(reg, fail);
  v.c(!fail.empty() ? false : w.value("now_sec", -1) == 0, "钟归零");
  v.c(!fail.empty() ? false : w["calls"].empty() && w["messages"].empty(),
      "通话/消息清空");
  v.c(!fail.empty() ? false : w.value("honesty", "") == "honest", "诚实度回 honest");
  return v;
}

bool all_read_only(ToolRegistry const& reg) {
  for (ToolSpec const& s : reg.all())
    if (!s.read_only) return false;
  return true;
}

// S3：转录里是否出现过"对某工具的 status=error 回喂"（运行时拒绝证据）。
bool has_tool_error(agent::Agent const& a, std::string const& tool) {
  for (Message const& m : a.transcript()) {
    if (m.type == message::Role::Tool && m.status == "error" &&
        m.text().find(tool) != std::string::npos)
      return true;
  }
  return false;
}

/* ======================== S7 · 一句话 → DAG → 执行 ========================
   与 S1–S6c 的分工：那些是「手写 DAG + 导演段」证明执行/观测；S7 把 DAG 变成**输入**——
   LLM 规划器生成受限 IR → sa_plan.hpp 静态校验 → 通用解释器按拓扑序调度域能力。
   LLM 只出现在规划 gate；域执行沿用 samock 的确定性 FSM/工具（可回放）。 */

std::expected<ToolRegistry, std::string> load_samock_registry() {
  std::filesystem::path const so = find_samock_so();
  std::vector<std::string> names(std::begin(kSamockTools), std::end(kSamockTools));
  auto specs = load_dynamic_tools(so.string(), names);
  if (!specs) return std::unexpected("samock 加载失败：" + specs.error());
  if (specs->size() != names.size())
    return std::unexpected("加载工具数不符：期望 " + std::to_string(names.size()) + " 实际 " +
                           std::to_string(specs->size()));
  ToolRegistry reg;
  for (ToolSpec& s : *specs) reg.add(std::move(s));
  auto vio = effect_class_violations(reg);
  if (!vio.empty()) {
    std::string msg = "samock 效果分类违规（read_only=false 必须声明效果分类）：";
    for (std::string const& v : vio) msg += "\n  " + v;
    return std::unexpected(std::move(msg));
  }
  return reg;
}

/* ---- S8 · 任务历史（web/sa/sa_tasklog.json 即历史库）----
   过去历史要保留、可删除（删除即持久删除）。合并规则：
   · 同 id 的新结果覆盖旧结果（脚本 demo 的 t1/t2 稳定 id，不会越跑越多）；
   · 新结果在最前（最近优先），历史上限 60 条；
   · 原子写（tmp → rename），避免页面读到半截 JSON。 */
Json load_history(std::string const& path) {
  std::ifstream in(path);
  if (!in) return Json{{"schema", "sa-tasklog/v1"}, {"tasks", Json::array()}};
  Json j = Json::parse(in, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object())
    return Json{{"schema", "sa-tasklog/v1"}, {"tasks", Json::array()}};
  if (!j.contains("tasks") || !j["tasks"].is_array()) j["tasks"] = Json::array();
  return j;
}

Json merge_history(Json const& existing_tasks, Json const& new_tasks,
                   std::size_t cap = 60) {
  Json merged = Json::array();
  std::set<std::string> new_ids;
  for (Json const& t : new_tasks) {
    merged.push_back(t);
    new_ids.insert(t.value("id", ""));
  }
  for (Json const& t : existing_tasks)
    if (new_ids.count(t.value("id", "")) == 0) merged.push_back(t);
  while (merged.size() > cap) merged.erase(merged.end() - 1);
  return merged;
}

bool write_json_atomic(std::string const& path, Json const& j) {
  std::string const tmp = path + ".tmp";
  {
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) return false;
    o << j.dump(2) << "\n";
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::ofstream o(path, std::ios::trunc);
    if (!o) return false;
    o << j.dump(2) << "\n";
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
  }
  return true;
}

std::string next_plan_id(Json const& existing_tasks) {
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  std::string const base = "p" + std::to_string(ms);
  std::set<std::string> ids;
  for (Json const& t : existing_tasks) ids.insert(t.value("id", ""));
  std::string id = base;
  for (int n = 2; ids.count(id) != 0; ++n) id = base + "-" + std::to_string(n);
  return id;
}

Json find_plan_node(Json const& plan, std::string const& id) {
  for (Json const& n : plan.value("nodes", Json::array()))
    if (n.value("id", "") == id) return n;
  return Json::object();
}

std::string plan_task_text(Json const& n) {
  return n.value("task", std::string());
}

// 节点 args 的 id 缺省值：按节点 id 派生，避免多个域节点共用 c1/m1 互相踩踏。
std::string plan_arg_id(Json const& n, char const* key, char const* prefix) {
  std::string v = n.value("args", Json::object()).value(key, std::string());
  return v.empty() ? std::string(prefix) + n.value("id", "x") : v;
}

bool has_control_edge(Json const& plan, std::string const& phone_id) {
  for (Json const& e : plan.value("edges", Json::array()))
    if (e.value("kind", "") == "control" && e.value("to", "") == phone_id) return true;
  return false;
}

// 实时事件 + tasklog 消息双写（一次 L.msg 同时进进度流和最终任务日志）。
struct LiveRec {
  Progress* prog = nullptr;
  Json msgs = Json::array();
  void msg(std::string const& node, std::string const& kind, std::string const& from,
           std::string const& text, std::string const& tool = {}) {
    Json m{{"at_sec", prog->clock()}, {"kind", kind}, {"from", from},
           {"node", node},           {"text", text}};
    if (!tool.empty()) m["tool"] = tool;
    msgs.push_back(m);
    prog->msg(m);
  }
  void node(std::string const& id, char const* state) { prog->node(prog->task(), id, state); }
  void sync_clock(Json const& out) { prog->set_clock(out.value("now_sec", prog->clock())); }
};

struct ExecOutcome {
  bool ok = true;
  std::vector<std::string> fails;
};

// 电话域执行器：拨号 → 拒接重拨一次 → 接通；有 control 边则注入 barge_in 改道。
void exec_phone(Json const& n, Json const& plan, ToolRegistry& samock, LiveRec& L,
                ExecOutcome& o) {
  std::string const id = n.value("id", "");
  Json const args = n.value("args", Json::object());
  std::string const cid = plan_arg_id(n, "call_id", "c_");
  std::string const number = args.value("number", "13800000000");
  bool const redirect = has_control_edge(plan, id);

  L.node(id, "active");
  auto call = [&](std::string const& tool, Json const& a) -> Json {
    Json out = run_tool(samock, tool, a, o.fails);
    L.msg(id, "tool", "电话域", tool_msg_text(tool, a, out), tool);
    return out;
  };
  call("samock_make_call", Json{{"call_id", cid}, {"number", number}, {"attempt", 1}});
  Json const c1_out = call("samock_clock_forward", Json{{"seconds", 15}});
  L.sync_clock(c1_out);
  Json const m1 = call("samock_milestones", Json{{"call_id", cid}, {"since", 0}});

  bool rejected = false;
  for (Json const& m : m1.value("milestones", Json::array()))
    if (m.value("kind", "") == "rejected") rejected = true;
  if (rejected) {
    call("samock_make_call", Json{{"call_id", cid}, {"number", number}, {"attempt", 2}});
    Json const c2_out = call("samock_clock_forward", Json{{"seconds", 12}});
    L.sync_clock(c2_out);
    call("samock_milestones", Json{{"call_id", cid}, {"since", 3}});
  }
  L.msg(id, "ai", "电话域",
        rejected ? "attempt1 拒接 → 按重拨策略重试 attempt2 → 已接通。" : "已接通。");

  if (redirect) {
    run_tool(samock, "samock_barge_in",
             Json{{"call_id", cid}, {"intent", "redirect_to_messaging"}}, o.fails);
    L.msg(id, "control", "用户",
          "barge_in{intent: redirect_to_messaging} —— 直灌控制平面（不经核心，仅通话中有效）");
    L.msg(id, "milestone", "电话域",
          "call.ended{attempt:2, reason:user_redirected, disposition:redirect_to_messaging}");
    L.msg(id, "ai", "电话域", "妈妈改口让发微信，通话已改道结束。请核心编排收尾。");
  }
  L.node(id, "done");
}

// 消息域执行器：send_message(args.to, args.text) → 送达回执。
void exec_msg(Json const& n, ToolRegistry& samock, LiveRec& L, ExecOutcome& o) {
  std::string const id = n.value("id", "");
  Json const args = n.value("args", Json::object());
  std::string const mid = plan_arg_id(n, "msg_id", "m_");
  std::string const to = args.value("to", "妈妈");
  std::string text = args.value("text", std::string());
  if (text.empty()) text = plan_task_text(n);

  L.node(id, "active");
  Json const a{{"msg_id", mid}, {"to", to}, {"text", text}};
  Json const out = run_tool(samock, "samock_send_message", a, o.fails);
  L.msg(id, "tool", "消息域", tool_msg_text("samock_send_message", a, out),
        "samock_send_message");
  L.msg(id, "ai", "消息域", "已发送到" + to + "并确认送达。");
  L.node(id, "done");
}

// 世界断言：按计划里的域效果逐项核对（查世界，不查工具返回值）。
void exec_world(Json const& n, Json const& plan, ToolRegistry& samock, LiveRec& L,
                ExecOutcome& o) {
  std::string const id = n.value("id", "");
  std::vector<std::string> wf;
  Json const w = world(samock, wf);
  bool ok = wf.empty();
  std::string clock_txt;
  std::vector<std::string> parts;

  for (Json const& pn : plan.value("nodes", Json::array())) {
    std::string const role = pn.value("role", "");
    Json const args = pn.value("args", Json::object());
    if (role == "phone") {
      std::string const cid = plan_arg_id(pn, "call_id", "c_");
      bool const redirect = has_control_edge(plan, pn.value("id", ""));
      if (!w.contains("calls") || !w["calls"].contains(cid)) {
        ok = false;
        parts.push_back("缺通话 " + cid);
        continue;
      }
      Json const& c = w["calls"][cid];
      std::string const st = c.value("state", "?");
      if (redirect) {
        bool const good = (st == "ended" && c.value("ended_reason", "") == "user_redirected");
        ok = ok && good;
        parts.push_back(cid + " state=" + st + " · reason=" +
                        c.value("ended_reason", "-"));
      } else {
        ok = ok && (st == "talking");
        parts.push_back(cid + " state=" + st);
      }
    } else if (role == "msg") {
      std::string const mid = plan_arg_id(pn, "msg_id", "m_");
      bool found = false, delivered = false;
      std::string to;
      for (Json const& m : w.value("messages", Json::array()))
        if (m.value("id", "") == mid) {
          found = true;
          delivered = m.value("delivered", false);
          to = m.value("to", "");
        }
      ok = ok && found && delivered;
      parts.push_back("消息=[" + to + ", delivered=" + (delivered ? "true" : "false") + "]");
    }
  }
  clock_txt = "钟=" + std::to_string(w.value("now_sec", 0)) + "s";
  parts.push_back(clock_txt);

  std::string text = "世界断言：";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) text += " · ";
    text += parts[i];
  }
  L.msg(id, "assert", "世界", text);
  L.node(id, "done");
  o.ok = o.ok && ok;
}

ExecOutcome execute_plan(Json const& plan, std::string const& query, ToolRegistry& samock,
                         LiveRec& L) {
  ExecOutcome o;
  run_tool(samock, "samock_world_reset", Json::object(), o.fails);  // 跨任务隔离
  auto const order = sa::plan::topo_order(plan);
  for (std::string const& id : order) {
    Json const n = find_plan_node(plan, id);
    std::string const role = n.value("role", "");
    if (role == "user") {
      L.node(id, "active");
      std::string text = plan_task_text(n);
      if (text.empty()) text = query;
      L.msg(id, "user", "用户", text);
      L.node(id, "done");
    } else if (role == "core") {
      L.node(id, "active");
      bool dispatched = false;
      for (Json const& e : plan.value("edges", Json::array())) {
        if (e.value("from", "") != id || e.value("kind", "") != "dispatch") continue;
        Json const target = find_plan_node(plan, e.value("to", ""));
        std::string text = plan_task_text(n);
        if (text.empty())
          text = e.value("label", "→" + target.value("label", "")) + "：" + plan_task_text(target);
        L.msg(id, "dispatch", "核心", text);
        dispatched = true;
      }
      if (!dispatched) L.msg(id, "ai", "核心", plan_task_text(n));
      L.node(id, "done");
    } else if (role == "phone") {
      exec_phone(n, plan, samock, L, o);
    } else if (role == "msg") {
      exec_msg(n, samock, L, o);
    } else if (role == "world") {
      exec_world(n, plan, samock, L, o);
    }
  }
  return o;
}

Json gather_plan_milestones(Json const& plan, ToolRegistry& samock) {
  Json arr = Json::array();
  std::vector<std::string> fail;
  for (Json const& n : plan.value("nodes", Json::array())) {
    if (n.value("role", "") != "phone") continue;
    std::string const cid = plan_arg_id(n, "call_id", "c_");
    Json const m =
        run_tool(samock, "samock_milestones", Json{{"call_id", cid}, {"since", 0}}, fail);
    for (Json const& ev : m.value("milestones", Json::array())) {
      Json row{{"seq", ev.value("seq", 0)},
               {"kind", ev.value("kind", "?") + ":" + std::to_string(ev.value("attempt", 0))},
               {"at", ev.value("at_sec", 0)}};
      if (ev.contains("reason")) row["reason"] = ev["reason"];
      arr.push_back(std::move(row));
    }
  }
  return arr;
}

Json build_plan_task(Json const& plan, std::string const& query, LiveRec const& L,
                     ToolRegistry& samock, bool ok, std::string const& source,
                     std::string const& model_name) {
  Json t;
  t["id"] = "p1";
  t["title"] = plan.value("title", query);
  t["status"] = ok ? "Completed" : "Failed";
  t["query"] = query;
  t["user_supplements"] = Json::array();

  Json replies = Json::array();
  for (Json const& m : L.msgs) {
    std::string const kind = m.value("kind", "");
    if (kind != "dispatch" && kind != "ai" && kind != "milestone") continue;
    replies.push_back(Json{{"at_sec", m.value("at_sec", 0)},
                           {"from", m.value("from", "")},
                           {"text", m.value("text", "")}});
  }
  t["sa_replies"] = std::move(replies);

  Json const miles = gather_plan_milestones(plan, samock);
  std::vector<std::string> fail;
  Json const w = world(samock, fail);
  t["facts"] = Json{{"planner", source},
                    {"model", model_name},
                    {"model_calls_core", 0},
                    {"milestones", static_cast<std::int64_t>(miles.size())},
                    {"clock_end_sec", w.value("now_sec", 0)},
                    {"verdict", ok ? (source == "llm" ? "Completed · LLM 规划 → 校验 → 执行"
                                                      : "Completed · 计划文件 → 校验 → 执行")
                                   : "Failed · 世界断言/工具失败"}};
  t["dag"] = plan;
  t["messages"] = L.msgs;
  t["milestones_summary"] = miles;
  return t;
}

#ifdef SA_PLAN_LIVE
struct PlannerResult {
  bool ok = false;
  Json plan;
  std::string error;
  std::string model;
};

PlannerResult plan_with_llm(std::string const& query) {
  auto const dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const api_key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const model_name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (api_key.empty()) return {false, {}, "缺少 LLM_API_KEY（请在 .env 提供）", model_name};

  std::string ca_bundle = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca_bundle.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const candidate = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(candidate)) ca_bundle = candidate;
    }
  }
  model::ModelConfig cfg;
  cfg.api_key = api_key;
  cfg.base_url = base_url;
  cfg.model = model_name;
  cfg.timeout_ms = 90000;
  cfg.ca_info = ca_bundle;

  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel llm(cfg, transport);

  std::vector<Message> msgs{message::system_message(sa::plan::planner_system_prompt()),
                            message::human_message("用户一句话：" + query)};
  std::string last_raw;
  std::vector<std::string> issues;
  for (int attempt = 0; attempt < 3; ++attempt) {
    model::CallOptions opt;
    opt.temperature = 0.0;
    opt.response_format = Json{{"type", "json_object"}};
    auto resp = llm.invoke(msgs, opt);
    if (!resp) return {false, {}, "模型调用失败：" + resp.error().message, model_name};
    last_raw = resp->text();
    Json p = Json::parse(sa::plan::extract_json_text(last_raw), nullptr, /*allow_exceptions=*/false);
    if (p.is_discarded()) {
      issues = {"P00: 输出不是合法 JSON"};
    } else {
      auto const v = sa::plan::validate(p);
      if (v.empty()) return {true, sa::plan::layout(std::move(p)), {}, model_name};
      issues.clear();
      for (auto const& i : v) issues.push_back(i.code + ": " + i.message);
    }
    msgs.push_back(message::ai_message(last_raw));
    std::string feedback = "上面的计划不合法：\n";
    for (std::string const& s : issues) feedback += "- " + s + "\n";
    feedback += "请只输出修正后的 JSON（不要解释、不要 Markdown）。";
    msgs.push_back(message::human_message(feedback));
  }
  std::string err = "规划重试 3 次仍不合法：";
  for (std::string const& s : issues) err += "\n  - " + s;
  return {false, {}, std::move(err), model_name};
}
#endif

int run_plan_mode(std::string const& query, std::string const& plan_json_path,
                  std::string const& tl_path, Progress& prog, bool dry_run) {
  std::string source = "fixture";
  std::string model_name = "(fixture)";
  Json plan;

  if (!plan_json_path.empty()) {
    std::ifstream in(plan_json_path);
    if (!in) {
      std::println("[✗] 读不到计划文件：{}", plan_json_path);
      return 2;
    }
    Json p = Json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (p.is_discarded()) {
      std::println("[✗] 计划文件不是合法 JSON：{}", plan_json_path);
      return 2;
    }
    plan = std::move(p);
  } else {
#ifdef SA_PLAN_LIVE
    std::println("—— S7 · LLM 规划（一句话 → DAG）——\n用户：{}", query);
    PlannerResult const pr = plan_with_llm(query);
    model_name = pr.model;
    if (!pr.ok) {
      std::println("[✗] 规划失败：{}", pr.error);
      return 2;
    }
    source = "llm";
    plan = pr.plan;
#else
    std::println("[✗] 本构建未启用 HTTP/模型，无法真机规划（可传 --plan-json=<路径> 离线执行）");
    return 2;
#endif
  }

  auto const issues = sa::plan::validate(plan);
  if (!issues.empty()) {
    std::println("[✗] 计划未通过静态校验（fail-closed，不执行）：");
    for (auto const& i : issues) std::println("  - {}: {}", i.code, i.message);
    return 2;
  }
  plan = sa::plan::layout(std::move(plan));

  if (dry_run) {
    std::println("{}", plan.dump(2));
    return 0;
  }

  auto loaded = load_samock_registry();
  if (!loaded) {
    std::println("[✗] {}", loaded.error());
    return 1;
  }
  ToolRegistry samock = std::move(*loaded);

  Json const history = load_history(tl_path);
  std::string const plan_id = next_plan_id(history.value("tasks", Json::array()));
  prog.set_task(plan_id);
  prog.set_clock(0);
  prog.emit(Json{{"t", "run"}, {"state", "start"}, {"pace_ms", prog.pace_ms()}});
  prog.emit(Json{{"t", "task"}, {"state", "start"}, {"id", plan_id},
                 {"title", plan.value("title", query)}});
  prog.emit(Json{{"t", "dag"}, {"task", plan_id}, {"dag", plan}});

  LiveRec L;
  L.prog = &prog;
  ExecOutcome const out = execute_plan(plan, query, samock, L);
  prog.emit(Json{{"t", "task"}, {"state", "done"}, {"id", plan_id}});

  Json task = build_plan_task(plan, query, L, samock, out.ok, source, model_name);
  task["id"] = plan_id;
  Json tl = history;
  tl["schema"] = "sa-tasklog/v1";
  tl["deterministic"] = (source == "fixture");
  tl["source"] = "sa_demo S7/S8 · 一句话规划（" + source + "）+ 历史合并";
  tl["tasks"] = merge_history(history.value("tasks", Json::array()), Json::array({task}));
  if (!write_json_atomic(tl_path, tl)) {
    std::println("[✗] 写 {} 失败", tl_path);
    prog.emit(Json{{"t", "run"}, {"state", "done"}});
    return 1;
  }
  prog.emit(Json{{"t", "run"}, {"state", "done"}});

  std::println("\n计划：{} · 节点 {} · 边 {}", plan.value("title", query),
               plan["nodes"].size(), plan["edges"].size());
  std::println("执行：{} · 消息 {} 条 · 世界钟 {}s", out.ok ? "Completed" : "Failed",
               L.msgs.size(), task["facts"].value("clock_end_sec", 0));
  for (std::string const& f : out.fails) std::println("  - 工具失败：{}", f);
  std::println("已写 {}", tl_path);
  return out.ok ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
  // 参数：--tasklog=<path>（默认 web/sa/sa_tasklog.json；sa_cgi 传绝对路径）
  //       --progress=<path>（实时进度 JSONL；空 = 不写，直跑/测试默认）
  //       --pace-ms=<N>（每条进度消息后 sleep N ms；0 = 全速，页面运行默认 300）
  //       --query=<一句话>（S7：LLM 规划 → DAG → 执行；也可经 env SA_QUERY 传入）
  //       --plan-json=<path>（S7：跳过 LLM，直接执行计划文件；离线可测）
  //       --dry-run（只规划/校验并打印 DAG，不执行）
  std::string tl_path = "web/sa/sa_tasklog.json";
  std::string prog_path;
  std::string query;
  std::string plan_json_path;
  bool dry_run = false;
  long pace_ms = 0;
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a.starts_with("--tasklog=")) {
      tl_path = a.substr(std::string("--tasklog=").size());
    } else if (a.starts_with("--progress=")) {
      prog_path = a.substr(std::string("--progress=").size());
    } else if (a.starts_with("--query=")) {
      query = a.substr(std::string("--query=").size());
    } else if (a.starts_with("--plan-json=")) {
      plan_json_path = a.substr(std::string("--plan-json=").size());
    } else if (a == "--dry-run") {
      dry_run = true;
    } else if (a.starts_with("--pace-ms=")) {
      std::string const v = a.substr(std::string("--pace-ms=").size());
      try {
        pace_ms = std::stol(v);
      } catch (...) {
        std::println("[✗] --pace-ms 需为非负整数：{}", v);
        return 2;
      }
      if (pace_ms < 0) pace_ms = 0;
      if (pace_ms > 5000) pace_ms = 5000;
    } else if (a == "--help" || a == "-h") {
      std::println(
          "用法：sa_demo [--tasklog=<路径>] [--progress=<JSONL 路径>] [--pace-ms=<0..5000>]\n"
          "              [--query=<一句话> | --plan-json=<路径>] [--dry-run]");
      return 0;
    }
  }
  if (query.empty()) {
    if (char const* q = std::getenv("SA_QUERY"); q != nullptr && *q != '\0') query = q;
  }
  Progress prog(prog_path, pace_ms);

  // S7：一句话 / 计划文件模式（不跑 S1–S4 手写导演段）。
  if (!query.empty() || !plan_json_path.empty())
    return run_plan_mode(query, plan_json_path, tl_path, prog, dry_run);

  std::println("SystemAgent 骨架 demo（problems/64 · S1–S4 · 全离线）\n");
  std::println("SA ≠ 单个 agent：核心 agent（编排·只读观测） + 电话域 agent + 消息域 agent；");
  std::println("假世界全部来自独立 so：tests/plugin_demo/samock.cpp → libsamock.so\n");

  std::filesystem::path const so = find_samock_so();
  std::println("samock 库：{}", so.string());

  // ---- ① samock 装载（fail-closed 加载器；wfr 动态工具库同一路径）----
  auto loaded = load_samock_registry();
  if (!loaded) {
    std::println("[✗] {}", loaded.error());
    return 1;
  }
  ToolRegistry samock = std::move(*loaded);

  std::println("\n已装载 {} 把 samock 工具（read / effect / approval）：", samock.size());
  for (ToolSpec const& s : samock.all())
    std::println("  · {:<26} ro={}  effect={}  approval={}", s.name, s.read_only ? 1 : 0,
                 kEffectClassNames[static_cast<unsigned>(s.effect_class)],
                 static_cast<int>(s.approval));
  std::println("\n工具描述器示例（make_call）：{}", samock.find("samock_make_call")->description);

  // ---- ② 世界空跑：全部断言查世界、不查返回值 ----
  std::vector<std::pair<std::string, Verdict (*)(ToolRegistry const&)>> const script = {
      {"A 空世界基线", stage_empty_world},
      {"B honest · 能力真做（世界真变 + 台账 + 幂等）", stage_honest_ops},
      {"C lying · 说谎被抓（断言只查世界）", stage_lying},
      {"D partial · 世界变一半（对账 miss 素材）", stage_partial},
      {"E 时钟 · 虚拟钟推进", stage_clock},
      {"F 重置 · 跨用例隔离", stage_reset},
  };

  bool const all = [&] {
    bool ok = true;
    for (auto const& [name, fn] : script) {
      std::println("\n场景：{}", name);
      Verdict v = fn(samock);
      for (std::string const& line : v.lines) std::println("{}", line);
      ok = ok && v.ok;
    }
    return ok;
  }();

  // ---- ③ SA 组装配（S1 骨架：构造三实例，不跑对话）----
  std::println("\n———— SA 组装配（S1 骨架） ————");
  ToolRegistry core_reg, phone_reg, msg_reg;
  auto copy_spec = [&samock](std::string const& n) {
    ToolSpec const* s = samock.find(n);
    return *s;
  };
  std::vector<sa::Step> trace;  // S3：编排轨迹（域 agent 每把工具调用全录，供验证器过检）
  // 录迹包装：不改工具语义，只在域注册表里包一层 recorder，
  // 让每个域 agent 的工具调用落进 trace（M0 集成用的运行时事实）。
  auto bind_agent_tool = [&trace, &prog](std::string const& agent, ToolSpec spec) {
    std::string const name = spec.name;
    ToolSpec::Handler base = spec.handler;
    spec.handler = [&trace, &prog, agent, name, base](Json const& args,
                                                      tool::ToolContext& ctx) {
      trace.push_back(sa::Step{agent, name, args});
      auto out = base(args, ctx);
      // S6c 实时进度：域 agent 的工具调用 → 页面节点点亮 + 工具消息（核心只读观测不展示）
      if (prog.enabled() && out.has_value() && agent != "core" &&
          live_tool_visible(name, *out)) {
        std::string const node = agent == "phone" ? "n_call" : "n_msg";
        std::string const from = agent == "phone" ? "电话域" : "消息域";
        prog.node(prog.task(), node, "active");
        prog.msg(Json{{"at_sec", prog.clock()},
                      {"kind", "tool"},
                      {"from", from},
                      {"node", node},
                      {"tool", name},
                      {"text", tool_msg_text(name, args, *out)}});
        // 时钟事件本身记在推进前（与 tasklog 口径一致），后续消息才看到新时刻。
        if (name == "samock_clock_forward") prog.set_clock((*out).value("now_sec", 0));
      }
      return out;
    };
    return spec;
  };
  core_reg.add(bind_agent_tool("core", copy_spec("samock_world_state")));
  phone_reg.add(bind_agent_tool("phone", copy_spec("samock_make_call")));
  phone_reg.add(bind_agent_tool("phone", copy_spec("samock_world_state")));
  phone_reg.add(bind_agent_tool("phone", copy_spec("samock_clock_forward")));
  phone_reg.add(bind_agent_tool("phone", copy_spec("samock_milestones")));  // 电话域读里程碑
  msg_reg.add(bind_agent_tool("msg", copy_spec("samock_send_message")));
  msg_reg.add(bind_agent_tool("msg", copy_spec("samock_world_state")));

  ScriptModel model_core, model_phone, model_msg;

  Agent core_agent(model_core, core_reg,
                   AgentOptions{.system_prompt =
                                    "你是 SystemAgent 核心编排者：对请求做分诊，决定交给哪个领域 agent；"
                                    "你只看到任务表与观测，**不直接执行任何领域动作**（世界只由领域节点改变）。",
                                .max_iterations = 8, .thread_id = "", .checkpointer = nullptr});
  Agent phone_agent(model_phone, phone_reg,
                    AgentOptions{.system_prompt =
                                     "你是 SA 电话域 agent：你的域内能力是打电话（samock_make_call，通话由假电话 FSM 驱动）；"
                                     "你只上报里程碑（拨号/拒接/接通/挂断），不碰其他域。",
                                 .max_iterations = 12, .thread_id = "", .checkpointer = nullptr});
  Agent msg_agent(model_msg, msg_reg,
                  AgentOptions{.system_prompt =
                                   "你是 SA 消息域 agent：你的域内能力是发送消息（samock_send_message）；"
                                   "只在被核心 agent 指派时执行，完成后回里程碑。",
                               .max_iterations = 8, .thread_id = "", .checkpointer = nullptr});

  std::println("核心 agent  : 分诊/编排 —— 工具：");
  for (ToolSpec const& s : core_reg.all()) std::println("    · {}（只读观测）", s.name);
  std::println("电话域 agent: 域内=通话（S2 假电话 FSM）—— 工具：");
  for (ToolSpec const& s : phone_reg.all()) std::println("    · {}", s.name);
  std::println("消息域 agent: 域内=消息 —— 工具：");
  for (ToolSpec const& s : msg_reg.all()) std::println("    · {}", s.name);

  bool const i1_core = all_read_only(core_reg);  // I1：SA 的动作只作用于图（工具集无改写世界的动词）
  std::println("\nI1 检查（核心 agent 工具集全部只读 → SA 只观测不执行）: {}",
               i1_core ? "通过" : "未通过");
  for (Agent const* a : {&core_agent, &phone_agent, &msg_agent}) (void)a;  // 证明已构造

  // ================= S2 · C7 简版：打电话 → 改口 → 转消息（组织闭环，离线脚本桩） =================
  std::println("\n———— S2 · C7 简版：打电话 → 改口 → 转消息 ————");
  std::println("组织形态：核心 agent 分诊/改道（2 次咨询）· 电话域 agent 域内处理拒接重试 · 消息域 agent 收尾\n");

  Verdict s2;
  auto rec = [&](bool b, std::string what) { s2.c(b, std::move(what)); };
  std::vector<std::string> e2;  // 导演层 run_tool 失败收集

  // —— t1 DAG 定义（先给页面画图；S4 任务日志复用同一份，防漂移）——
  Json const t1_dag = Json{
      {"nodes", Json::array({
            Json{{"id", "n_user"}, {"label", "用户"}, {"role", "user"}, {"layer", 0}, {"row", 0}},
            Json{{"id", "n_core1"}, {"label", "核心"}, {"role", "core"}, {"layer", 1}, {"row", 1},
                 {"detail", "分诊：→电话域（核心第 1 次咨询）"}},
            Json{{"id", "n_call"}, {"label", "电话域"}, {"role", "phone"}, {"layer", 2}, {"row", 1},
                 {"detail", "FSM 域内：attempt1 拒接@10s → 按 retry_policy 重拨 attempt2 → 接通；"
                             "拒接/振铃/接通均不打扰核心（04 §5 职责切分）"}},
            Json{{"id", "n_core2"}, {"label", "核心"}, {"role", "core"}, {"layer", 3}, {"row", 1},
                 {"detail", "改道编排（核心第 2 次咨询）：收到 ended{user_redirected} → 改派消息域"}},
            Json{{"id", "n_msg"}, {"label", "消息域"}, {"role", "msg"}, {"layer", 4}, {"row", 1},
                 {"detail", "send_message{m5, 妈妈} → delivered=true"}},
            Json{{"id", "n_world"}, {"label", "世界"}, {"role", "world"}, {"layer", 5}, {"row", 1},
                 {"detail", "c1 state=ended · reason=user_redirected · disposition=redirect_to_messaging · "
                             "消息=[妈妈, delivered] · 钟=27s"}},
        })},
      {"edges", Json::array({
            Json{{"from", "n_user"}, {"to", "n_core1"}, {"label", "query"}, {"kind", "query"}},
            Json{{"from", "n_core1"}, {"to", "n_call"}, {"label", "→电话域"}, {"kind", "dispatch"}},
            Json{{"from", "n_user"}, {"to", "n_call"}, {"label", "改口(barge_in)"}, {"kind", "control"}},
            Json{{"from", "n_call"}, {"to", "n_core2"}, {"label", "ended 上报"}, {"kind", "report"}},
            Json{{"from", "n_core2"}, {"to", "n_msg"}, {"label", "→消息域"}, {"kind", "dispatch"}},
            Json{{"from", "n_msg"}, {"to", "n_world"}, {"label", "写世界"}, {"kind", "world"}},
        })}};

  // —— S6c 实时进度：t1 开跑（图先到，随后 node/msg 逐步推）——
  auto live_msg = [&prog](std::string const& node, std::string const& kind,
                          std::string const& from, std::string const& text) {
    prog.msg(Json{{"at_sec", prog.clock()},
                  {"kind", kind},
                  {"from", from},
                  {"node", node},
                  {"text", text}});
  };
  prog.set_task("t1");
  prog.set_clock(0);
  prog.emit(Json{{"t", "run"}, {"state", "start"}, {"pace_ms", pace_ms}});
  prog.emit(Json{{"t", "task"}, {"state", "start"}, {"id", "t1"},
                 {"title", "给妈妈打电话问她晚上来不来吃饭"}});
  prog.emit(Json{{"t", "dag"}, {"task", "t1"}, {"dag", t1_dag}});
  prog.node("t1", "n_user", "active");
  live_msg("n_user", "user", "用户", "给妈妈打电话问她晚上来不来吃饭。");
  prog.node("t1", "n_user", "done");

  // (1) 核心咨询①：初始分诊 → 电话域
  model_core.replies = {message::ai_message("→电话域：给妈妈打电话问她晚上来不来吃饭。")};
  auto core1 = core_agent.invoke(one_human("给妈妈打电话问她晚上来不来吃饭。"));
  rec(core1.has_value() && core1->text().find("电话域") != std::string::npos,
      "核心咨询①（初始分诊）→ 路由到电话域");
  prog.node("t1", "n_core1", "active");
  live_msg("n_core1", "dispatch", "核心", "→电话域：给妈妈打电话问她晚上来不来吃饭。");
  prog.node("t1", "n_core1", "done");

  // (2) 电话域咨询①：拒接 → 按 retry_policy 重拨 attempt2 → 接通（域内闭环，核心不醒）
  model_phone.replies = {
      ai_call("samock_make_call", Json{{"call_id", "c1"}, {"number", "13800000000"}, {"attempt", 1}}),
      ai_call("samock_clock_forward", Json{{"seconds", 15}}),
      ai_call("samock_milestones", Json{{"call_id", "c1"}, {"since", 0}}),
      ai_call("samock_make_call", Json{{"call_id", "c1"}, {"number", "13800000000"}, {"attempt", 2}}),
      ai_call("samock_clock_forward", Json{{"seconds", 12}}),
      ai_call("samock_milestones", Json{{"call_id", "c1"}, {"since", 3}}),
      message::ai_message("电话域：attempt1 拒接 → 按重拨策略重试 attempt2 → 已接通。"),
  };
  auto phone1 = phone_agent.invoke(
      one_human("域内任务：给妈妈打电话问她晚上来不来吃饭。拒接就按你的重拨策略处理，不要打扰核心。"));
  rec(phone1.has_value(), "电话域咨询①：拒接 → 重拨 attempt2 → 接通（retry_policy 在域内，不打扰核心）");
  live_msg("n_call", "ai", "电话域", "attempt1 拒接 → 按重拨策略重试 attempt2 → 已接通。");

  // (3) 用户改口 → 控制平面 barge_in（导演代用户直灌，不经核心）
  Json barge = run_tool(samock, "samock_barge_in",
                        Json{{"call_id", "c1"}, {"intent", "redirect_to_messaging"}}, e2);
  rec(!barge.empty() && barge.value("status", "") == "redirected",
      "用户改口「算了，发个微信就行」→ barge_in 直灌控制平面（仅通话中有效）");
  live_msg("n_call", "control", "用户",
           "barge_in{intent: redirect_to_messaging} —— 直灌控制平面（不经核心，仅通话中有效）");
  live_msg("n_call", "milestone", "电话域",
           "call.ended{attempt:2, reason:user_redirected, disposition:redirect_to_messaging}");

  // (4) 电话域咨询②：观察 ended{user_redirected} → 上报核心改道
  model_phone.replies = {
      ai_call("samock_milestones", Json{{"call_id", "c1"}, {"since", 6}}),
      message::ai_message("电话域：妈妈改口让发微信，通话已改道结束（ended user_redirected），请核心编排收尾。"),
  };
  auto phone2 =
      phone_agent.invoke(one_human("通话有了新进展，查一下里程碑并汇报。"));
  rec(phone2.has_value() && phone2->text().find("微信") != std::string::npos,
      "电话域咨询②：读到 ended{user_redirected} → 上报核心改道");
  live_msg("n_call", "ai", "电话域", "妈妈改口让发微信，通话已改道结束。请核心编排收尾。");
  prog.node("t1", "n_call", "done");

  // (5) 核心咨询②：改道编排 → 消息域
  model_core.replies = {message::ai_message("→消息域：妈妈改口，让发微信。")};
  auto core2 =
      core_agent.invoke(one_human("电话域上报：妈妈改口要发微信，不再电话问。请你编排收尾。"));
  rec(core2.has_value() && core2->text().find("消息") != std::string::npos,
      "核心咨询②（改道编排）→ 路由到消息域");
  prog.node("t1", "n_core2", "active");
  live_msg("n_core2", "dispatch", "核心", "→消息域：妈妈改口，让发微信。");
  prog.node("t1", "n_core2", "done");

  // (6) 消息域咨询：发消息 → 送达回执
  model_msg.replies = {
      ai_call("samock_send_message", Json{{"msg_id", "m5"}, {"to", "妈妈"},
                                          {"text", "你晚上来不来吃饭？我提前准备。"}}),
      message::ai_message("消息域：已发送到妈妈并确认送达。"),
  };
  auto msg1 = msg_agent.invoke(one_human("域内任务：给妈妈发微信问晚上来不来吃饭。"));
  rec(msg1.has_value() && msg1->text().find("送达") != std::string::npos,
      "消息域咨询：send_message → 送达回执");
  live_msg("n_msg", "ai", "消息域", "已发送到妈妈并确认送达。");
  prog.node("t1", "n_msg", "done");

  // —— 世界终态断言（查世界、不查返回值）——
  e2.clear();
  Json wf = world(samock, e2);
  if (e2.empty()) {
    rec(wf["calls"]["c1"].value("state", "") == "ended", "世界终态：c1.state == ended");
    rec(wf["calls"]["c1"].value("ended_reason", "") == "user_redirected",
        "ended_reason == user_redirected（用户改道，非失败）");
    rec(wf["calls"]["c1"].value("ended_disposition", "") == "redirect_to_messaging",
        "ended_disposition == redirect_to_messaging");
    rec(wf.value("now_sec", -1) == 27, "虚拟钟 now_sec == 27（全程无真实等待）");
    rec(wf["messages"].size() == 1 && wf["messages"][0].value("to", "") == "妈妈" &&
            wf["messages"][0].value("delivered", false) == true,
        "世界消息表：1 条 · 收件人=妈妈 · delivered=true（查询结果真实落在世界）");
  } else {
    rec(false, "世界读取失败：" + e2.front());
  }
  prog.node("t1", "n_world", "active");
  live_msg("n_world", "assert", "世界",
           "世界断言：c1 state=ended · ended_reason=user_redirected · messages=[妈妈, delivered=true] · 钟=27s");
  prog.node("t1", "n_world", "done");

  // —— 里程碑全序断言（C7 唯一感官：7 条事件，零音频/零逐句转写）——
  e2.clear();
  Json miles = run_tool(samock, "samock_milestones", Json{{"call_id", "c1"}, {"since", 0}}, e2);
  if (!miles.empty()) {
    std::string got;
    bool first = true;
    for (Json const& m : miles["milestones"]) {
      if (!first) got += ",";
      first = false;
      got += m.value("kind", "?") + ":" + std::to_string(m.value("attempt", 0));
    }
    std::string const exp = "dialing:1,ringing:1,rejected:1,dialing:2,ringing:2,answered:2,ended:2";
    rec(got == exp, "里程碑全序 == 「" + exp + "」 (实测：" + got + ")");
    rec(miles.value("seq", -1) == 7, "里程碑 seq == 7（7 条事件全程可回放）");
    if (!miles["milestones"].empty()) {
      auto const ended = miles["milestones"].back();
      rec(ended.value("reason", "") == "user_redirected",
          "ended 里程碑 reason == user_redirected");
      rec(ended.value("disposition", "") == "redirect_to_messaging",
          "ended 里程碑 disposition == redirect_to_messaging");
    }
  } else {
    rec(false, "里程碑读取失败：seq 断言跳过");
  }

  // —— 唤醒账（C7 口径：SA 唤醒从 7 降到 3 的方向在此兑现）——
  rec(model_core.invoked == 2,
      "核心模型咨询共 2 次（初始分派 + 改道编排）≤ 3 —— 拒接/振铃/重拨全程在电话域内部，未打扰核心");
  rec(model_phone.invoked == 9, "电话域模型消耗 9 轮（拒接→重拨→接通→观察改道全在域内）");
  rec(model_msg.invoked == 2, "消息域模型消耗 2 轮（发送 + 收尾汇报）");

  s2.note("职责切分（04 §5 精缩）：拒接→重拨 = 电话域 retry_policy（域内契约）；挂断后做什么 = 核心（改道编排）；"
          "到没到 = 世界断言（delivered）。");
  s2.note("观测归约：SA 全程只看到 7 条里程碑 —— 音频/逐句转写不存在于任何上下文（04 §6 词汇表）。");
  s2.note("Barge-in 走控制平面（不经核心也不经业务体）：平台侧约束=仅通话中（Talking）有效，"
          "负路径（振铃时打断应被拒）留给后续故障注入 F 系列，本 demo 只走有效路径。");

  for (std::string const& line : s2.lines) std::println("{}", line);
  prog.emit(Json{{"t", "task"}, {"state", "done"}, {"id", "t1"}});

  // ================= S3 · M0 化：SA 语义层校验纯函数 + 坏编排对抗集 ================
  std::println("\n———— S3 · M0 化：SA 语义层校验纯函数 + 坏编排对抗集 ————");
  std::println("规则：不需要 LLM/世界/时钟，纯函数即判（07 §0）—— 单元自检直灌坏样本，集成过检真实轨迹\n");

  Verdict s3;
  auto r3 = [&](bool b, std::string what) { s3.c(b, std::move(what)); };
  auto expect_clean = [&](std::vector<sa::Issue> issues, std::string what) {
    bool const ok = issues.empty();
    r3(ok, std::move(what) + (ok ? std::string()
                                 : std::string("（违规=" + issues.front().message + "）")));
  };
  auto expect_flagged = [&](std::vector<sa::Issue> issues, std::string const& code,
                            std::string what) {
    bool hit = false;
    for (sa::Issue const& i : issues)
      if (i.code == code) hit = true;
    r3(hit, std::move(what));
  };
  std::vector<std::string> e3;  // S3 run_tool 失败收集

  // 编排常量：白名单 = 实际发给各 agent 的注册表（防漂移 —— 验证器看的是部署真相）
  auto names_of = [](ToolRegistry const& r) {
    std::vector<std::string> n;
    for (ToolSpec const& s : r.all()) n.push_back(s.name);
    return n;
  };
  std::map<std::string, std::vector<std::string>> const whitelist = {
      {"core", names_of(core_reg)}, {"phone", names_of(phone_reg)}, {"msg", names_of(msg_reg)}};
  std::vector<std::string> read_only_tools;
  for (ToolSpec const& s : samock.all())
    if (s.read_only) read_only_tools.push_back(s.name);
  std::vector<std::string> const vocabulary = {"dialing", "ringing", "rejected", "answered", "ended"};
  std::vector<std::string> const known_domains = {"电话域", "消息域"};
  Json const limits = Json{{"core_consultations", 3}, {"phone_tool_rounds", 8},
                           {"msg_tool_rounds", 4},    {"call_retry_attempts", 2}};
  std::vector<sa::TaskSpec> const tasks = {
      {"电话域", "就晚饭安排达成答复", "已获得答复 ∨ 已切换渠道并送达"},  // 04 §5 N_assert 语义
      {"消息域", "向妈妈确认晚饭", "消息送达（delivered=true）"},
  };

  // ① 单元自检：直灌好/坏样本（M0「oracle 直灌」的组编排版）—— 合法全过、对抗全拒
  std::vector<sa::Step> const good = {
      {"phone", "samock_make_call", Json{{"call_id", "x"}, {"number", "1"}, {"attempt", 1}}},
      {"phone", "samock_clock_forward", Json{{"seconds", 15}}},
      {"phone", "samock_make_call", Json{{"call_id", "x"}, {"number", "1"}, {"attempt", 2}}},
  };
  expect_clean(sa::check_unknown_tools(good, whitelist), "合法轨迹①：域白名单全过");
  expect_clean(sa::check_dedup(good, 2), "合法轨迹①：attempt1/attempt2 不同参不算循环");

  expect_flagged(sa::check_unknown_tools({{"phone", "samock_send_sms", Json::object()}}, whitelist),
                 "I01", "对抗①：电话域幻觉出不存在的工具 samock_send_sms → I01 拒");
  expect_flagged(sa::check_unknown_tools({{"msg", "samock_make_call", Json::object()}}, whitelist),
                 "I01", "对抗②：消息域去打电话（越域）→ I01 拒");
  expect_flagged(sa::check_unknown_tools({{"typo", "samock_world_state", Json::object()}}, whitelist),
                 "I00", "对抗③：未知 agent（拼写错误）→ I00 拒");
  expect_flagged(sa::check_core_tools_read_only({"samock_make_call"}, read_only_tools), "I02",
                 "对抗④：把写工具塞给核心 → I02 拒（I1）");
  expect_flagged(sa::check_dispatch({"总经办"}, known_domains), "I03",
                 "对抗⑤：分诊路由到不存在的域「总经办」→ I03 拒");
  expect_flagged(sa::check_vocabulary({"audio.frame"}, vocabulary), "I04",
                 "对抗⑥：词汇表外里程碑 audio.frame → I04 拒");
  expect_flagged(sa::check_budgets(Json{{"core_consultations", 4}}, limits), "I05",
                 "对抗⑦：核心咨询 4 > 3（V4 预算）→ I05 拒");
  expect_flagged(sa::check_budgets(Json{{"call_retry_attempts", 3}}, limits), "I05",
                 "对抗⑦b：电话重试 3 > 2（V4 预算）→ I05 拒");
  std::vector<sa::Step> const loop = {
      {"phone", "samock_make_call", Json{{"call_id", "x"}, {"number", "1"}, {"attempt", 1}}},
      {"phone", "samock_make_call", Json{{"call_id", "x"}, {"number", "1"}, {"attempt", 1}}},
      {"phone", "samock_make_call", Json{{"call_id", "x"}, {"number", "1"}, {"attempt", 1}}},
  };
  expect_flagged(sa::check_dedup(loop, 2), "I06",
                 "对抗⑧：同工具同参 3 次（S4 循环迹象）→ I06 拒");
  expect_flagged(sa::check_task_templates({{"档案", "整理文件", ""}}), "I07",
                 "对抗⑨：任务模板缺 goal_assertion → I07 拒");
  expect_clean(sa::check_task_templates(tasks), "任务模板：电话/消息两模板 goal + assertion 齐备 ✓");

  // ② 集成：S2 真实编排轨迹 + samock 里程碑全量过检（消费运行时事实，非宿主口报）
  std::vector<sa::Issue> all_issues;
  for (auto const& chunk : {sa::check_unknown_tools(trace, whitelist),
                            sa::check_dispatch({"电话域", "消息域"}, known_domains),
                            sa::check_dedup(trace, 2)})
    all_issues.insert(all_issues.end(), chunk.begin(), chunk.end());
  for (auto const& c : sa::check_core_tools_read_only(names_of(core_reg), read_only_tools))
    all_issues.push_back(c);
  for (auto const& c : sa::check_task_templates(tasks)) all_issues.push_back(c);

  Json used = sa::usage_from_trace(trace);
  used["core_consultations"] = static_cast<std::int64_t>(model_core.invoked);

  std::vector<std::string> milestone_kinds;
  e3.clear();
  Json mi = run_tool(samock, "samock_milestones", Json{{"call_id", "c1"}, {"since", 0}}, e3);
  bool const mi_ok = !mi.empty() && mi.contains("milestones");
  r3(mi_ok, "集成：读回 c1 全集里程碑（7 条）供词汇表过检");
  if (mi_ok)
    for (Json const& m : mi["milestones"]) milestone_kinds.push_back(m.value("kind", ""));
  for (auto const& c : sa::check_vocabulary(milestone_kinds, vocabulary)) all_issues.push_back(c);
  for (auto const& c : sa::check_budgets(used, limits)) all_issues.push_back(c);

  expect_clean(all_issues, "集成：S2 全量轨迹过检（白名单/只读/分诊/词汇表/预算/去重/任务断言）");
  std::println("  用量账（轨迹自证）：{}", used.dump());
  std::string kinds;
  for (std::string const& k : milestone_kinds) {
    if (!kinds.empty()) kinds += ",";
    kinds += k;
  }
  std::println("  里程碑 kinds（{} 条）：{}", milestone_kinds.size(), kinds);
  s3.note("V1–V8 落地（agent-group 取舍，64 §5.5 S3）：V4 预算→I05 · V6 断言→I07；"
          "I01/I03/I04/I06 = 组编排层新增不变式（域白名单/分诊/词汇表/循环）；"
          "V1/V2/V3/V5/V7/V8 依赖 DAG/waits/pivot，本形态无 → 标注 N/A 不硬凑。");

  // ③ 运行时对抗：脚本桩弹幻觉工具 → ToolNode 当场拒绝（library 既有失败关闭）
  model_phone.replies = {
      ai_call("samock_send_sms", Json{{"to", "妈妈"}, {"text", "喂"}}),
      message::ai_message("哦，我没有 samock_send_sms 这个工具，改用已有工具继续。"),
  };
  auto adv = phone_agent.invoke(one_human("对抗：尝试用不存在的工具 samock_send_sms 发短信。"));
  r3(adv.has_value(), "运行时对抗：幻觉工具被拒后 agent 咨询仍正常收尾（错误回喂模型）");
  r3(has_tool_error(phone_agent, "samock_send_sms"),
     "运行时对抗：转录出现 status=error 的「unknown tool: samock_send_sms」（ToolNode 拒）");
  e3.clear();
  Json wa = world(samock, e3);
  bool const clean_world = e3.empty() && wa["messages"].size() == 1 && wa["side_effects"].size() == 3;
  r3(clean_world, "运行时对抗：世界零污染 —— 消息仍 1 条、台账仍 3 笔（幻觉副作用从未发生）");
  s3.note("分工：验证器 = 审（对轨迹静态判）· ToolNode = 挡（未知工具回喂 error 不中断）——两层各自归位。");

  for (std::string const& line : s3.lines) std::println("{}", line);

  // ================= S4 · SA 专用 UI 页 —— 任务日志产出（web/sa/index.html） =================
  //
  // 数据契约 sa-tasklog/v1（web/sa/index.html 消费）：
  //   左栏任务列表 = query + SA 回传消息 + 用户补充信息（一个完整任务）；
  //   右栏详情 = 上 DAG 节点图 + 下对话/消息列表（50% 可拖动）。
  // v1 组装法：确定性编排 → 结束时用「实时读世界/里程碑 + 编排常量」拼两条任务记录
  //   （t1 C7 简版 / t2 消息直路），写 web/sa/sa_tasklog.json 供 run-webui.sh 静态服务。
  std::println("\n———— S4 · SA 专用 UI 页 —— 任务日志产出 ————");
  Verdict s4;
  auto r4 = [&](bool b, std::string what) { s4.c(b, std::move(what)); };
  std::vector<std::string> e4;

  // —— t1 · C7 简版：实时事实（世界 + 里程碑；唤醒账 S2 断言已锁：core 2 / phone 9 / msg 2）——
  Json t1_world = world(samock, e4);
  Json t1_ms = run_tool(samock, "samock_milestones", Json{{"call_id", "c1"}, {"since", 0}}, e4);
  r4(e4.empty(), "t1 事实抓取：世界 + 里程碑实时读成功");

  Json t1;
  t1["id"] = "t1";
  t1["title"] = "给妈妈打电话问她晚上来不来吃饭";
  t1["status"] = "Completed";
  t1["query"] = "给妈妈打电话问她晚上来不来吃饭。";
  t1["user_supplements"] = Json::array(
      {Json{{"at_sec", 27}, {"text", "（用户改口）算了，发个微信就行 —— barge_in 直灌控制平面"}}});
  t1["sa_replies"] = Json::array(
      {Json{{"at_sec", 0}, {"from", "核心"}, {"text", "→电话域：打电话问妈妈晚饭。"}},
       Json{{"at_sec", 0}, {"from", "电话域"}, {"text", "attempt1 拒接 → 重试 attempt2 → 已接通。"}},
       Json{{"at_sec", 27}, {"from", "电话域"}, {"text", "妈妈改口让发微信，通话已改道结束。"}},
       Json{{"at_sec", 27}, {"from", "核心"}, {"text", "→消息域：发微信。"}},
       Json{{"at_sec", 27}, {"from", "消息域"}, {"text", "已发送到妈妈并确认送达。"}}});
  t1["facts"] =
      Json{{"model_calls_core", 2},   // S2 断言已锁（≤3，拒接/重试全程不打扰核心）
           {"milestones", t1_ms.value("seq", -1)},
           {"clock_end_sec", t1_world.value("now_sec", -1)},
           {"verdict", "Completed · ended(user_redirected) · 消息=妈妈 delivered"}};
  t1["dag"] = t1_dag;   // 与实时进度共用同一份定义（S2 开头已构造）
  t1["messages"] = Json::array(
      {Json{{"at_sec", 0}, {"kind", "user"}, {"from", "用户"}, {"node", "n_user"},
            {"text", "给妈妈打电话问她晚上来不来吃饭。"}},
       Json{{"at_sec", 0}, {"kind", "dispatch"}, {"from", "核心"}, {"node", "n_core1"},
            {"text", "→电话域：给妈妈打电话问她晚上来不来吃饭。"}},
       Json{{"at_sec", 0}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_make_call"}, {"text", "make_call{c1, 13800000000, attempt:1}"}},
       Json{{"at_sec", 0}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_clock_forward"}, {"text", "clock_forward{15}"}},
       Json{{"at_sec", 15}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_milestones"},
            {"text", "milestones{since:0} → [dialing:1, ringing:1, rejected:1]"}},
       Json{{"at_sec", 15}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_make_call"}, {"text", "make_call{c1, 13800000000, attempt:2} （retry_policy 重拨）"}},
       Json{{"at_sec", 15}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_clock_forward"}, {"text", "clock_forward{12}"}},
       Json{{"at_sec", 27}, {"kind", "tool"}, {"from", "电话域"}, {"node", "n_call"},
            {"tool", "samock_milestones"},
            {"text", "milestones{since:3} → [dialing:2, ringing:2, answered:2] —— 已接通"}},
       Json{{"at_sec", 27}, {"kind", "ai"}, {"from", "电话域"}, {"node", "n_call"},
            {"text", "attempt1 拒接 → 按重拨策略重试 attempt2 → 已接通。"}},
       Json{{"at_sec", 27}, {"kind", "control"}, {"from", "用户"}, {"node", "n_call"},
            {"text", "barge_in{intent: redirect_to_messaging} —— 直灌控制平面（不经核心，仅通话中有效）"}},
       Json{{"at_sec", 27}, {"kind", "milestone"}, {"from", "电话域"}, {"node", "n_call"},
            {"text", "call.ended{attempt:2, reason:user_redirected, disposition:redirect_to_messaging}"}},
       Json{{"at_sec", 27}, {"kind", "ai"}, {"from", "电话域"}, {"node", "n_call"},
            {"text", "妈妈改口让发微信，通话已改道结束。请核心编排收尾。"}},
       Json{{"at_sec", 27}, {"kind", "dispatch"}, {"from", "核心"}, {"node", "n_core2"},
            {"text", "→消息域：妈妈改口，让发微信。"}},
       Json{{"at_sec", 27}, {"kind", "tool"}, {"from", "消息域"}, {"node", "n_msg"},
            {"tool", "samock_send_message"},
            {"text", "send_message{m5, 妈妈, 你晚上来不来吃饭？我提前准备。}"}},
       Json{{"at_sec", 27}, {"kind", "ai"}, {"from", "消息域"}, {"node", "n_msg"},
            {"text", "已发送到妈妈并确认送达。"}},
       Json{{"at_sec", 27}, {"kind", "assert"}, {"from", "世界"}, {"node", "n_world"},
            {"text", "世界断言：c1 state=ended · ended_reason=user_redirected · messages=[妈妈, delivered=true] · 钟=27s"}}});
  Json ms_sum = Json::array();
  if (t1_ms.contains("milestones"))
    for (Json const& m : t1_ms["milestones"]) {
      Json row{{"seq", m.value("seq", 0)},
               {"kind", m.value("kind", "?") + ":" + std::to_string(m.value("attempt", 0))},
               {"at", m.value("at_sec", 0)}};
      if (m.contains("reason")) row["reason"] = m["reason"];
      ms_sum.push_back(std::move(row));
    }
  t1["milestones_summary"] = std::move(ms_sum);

  // —— t2 · 消息直路（单域快路 = 分诊的退化结论）：重置世界（多任务隔离）后跑 ——
  Json t2_reset = run_tool(samock, "samock_world_reset", Json::object(), e4);
  r4(t2_reset.value("reset", false), "t2：世界重置（任务间隔离；L1 语义）");

  Json const t2_dag = Json{
      {"nodes", Json::array({
            Json{{"id", "n_user"}, {"label", "用户"}, {"role", "user"}, {"layer", 0}, {"row", 0}},
            Json{{"id", "n_core"}, {"label", "核心"}, {"role", "core"}, {"layer", 1}, {"row", 1},
                 {"detail", "分诊：直接 →消息域（单域快路 = 分诊的退化结论，05 §2）"}},
            Json{{"id", "n_msg"}, {"label", "消息域"}, {"role", "msg"}, {"layer", 2}, {"row", 1},
                 {"detail", "send_message{m6, 妈妈} → delivered=true（其他域零介入）"}},
            Json{{"id", "n_world"}, {"label", "世界"}, {"role", "world"}, {"layer", 3}, {"row", 1},
                 {"detail", "messages=[妈妈, delivered=true] · calls 空 · 钟=0s"}},
        })},
      {"edges", Json::array({
            Json{{"from", "n_user"}, {"to", "n_core"}, {"label", "query"}, {"kind", "query"}},
            Json{{"from", "n_core"}, {"to", "n_msg"}, {"label", "→消息域"}, {"kind", "dispatch"}},
            Json{{"from", "n_msg"}, {"to", "n_world"}, {"label", "写世界"}, {"kind", "world"}},
        })}};
  prog.set_task("t2");
  prog.set_clock(0);
  prog.emit(Json{{"t", "task"}, {"state", "start"}, {"id", "t2"},
                 {"title", "给妈妈发微信问她晚上来不来吃饭"}});
  prog.emit(Json{{"t", "dag"}, {"task", "t2"}, {"dag", t2_dag}});
  prog.node("t2", "n_user", "active");
  live_msg("n_user", "user", "用户", "给妈妈发微信问她晚上来不来吃饭。");
  prog.node("t2", "n_user", "done");

  model_core.replies = {message::ai_message("→消息域：给妈妈发微信问她晚上来不来吃饭。")};
  auto tc2 = core_agent.invoke(one_human("给妈妈发微信问她晚上来不来吃饭。"));
  r4(tc2.has_value() && tc2->text().find("消息") != std::string::npos,
     "t2·核心咨询：直接 →消息域（1 次咨询 = 单域快路）");
  prog.node("t2", "n_core", "active");
  live_msg("n_core", "dispatch", "核心", "→消息域：给妈妈发微信问她晚上来不来吃饭。");
  prog.node("t2", "n_core", "done");
  model_msg.replies = {
      ai_call("samock_send_message", Json{{"msg_id", "m6"}, {"to", "妈妈"}, {"text", "你晚上来不来吃饭？"}}),
      message::ai_message("已发送到妈妈并确认送达。"),
  };
  auto tm2 = msg_agent.invoke(one_human("域内任务：给妈妈发微信问她晚上来不来吃饭。"));
  r4(tm2.has_value() && tm2->text().find("送达") != std::string::npos, "t2·消息域：send_message → 送达回执");
  live_msg("n_msg", "ai", "消息域", "已发送到妈妈并确认送达。");
  prog.node("t2", "n_msg", "done");
  e4.clear();
  Json t2_world = world(samock, e4);
  bool const t2_world_ok =
      e4.empty() && t2_world["messages"].size() == 1 &&
      t2_world["messages"][0].value("to", "") == "妈妈" &&
      t2_world["messages"][0].value("delivered", false) == true && t2_world["calls"].empty() &&
      t2_world.value("now_sec", -1) == 0;
  r4(t2_world_ok, "t2·世界断言：1 条消息=妈妈 delivered · 无通话 · 钟回 0");
  prog.node("t2", "n_world", "active");
  live_msg("n_world", "assert", "世界",
           "世界断言：messages=[妈妈, delivered=true] · calls 空 · 钟=0s");
  prog.node("t2", "n_world", "done");
  prog.emit(Json{{"t", "task"}, {"state", "done"}, {"id", "t2"}});

  Json t2;
  t2["id"] = "t2";
  t2["title"] = "给妈妈发微信问她晚上来不来吃饭";
  t2["status"] = "Completed";
  t2["query"] = "给妈妈发微信问她晚上来不来吃饭。";
  t2["user_supplements"] = Json::array();
  t2["sa_replies"] = Json::array(
      {Json{{"at_sec", 0}, {"from", "核心"}, {"text", "→消息域：发微信。"}},
       Json{{"at_sec", 0}, {"from", "消息域"}, {"text", "已发送到妈妈并确认送达。"}}});
  t2["facts"] =
      Json{{"model_calls_core", 1},   // t2：核心仅 1 次咨询（单域快路）
           {"milestones", 0},         // 无通话 → 无里程碑
           {"clock_end_sec", t2_world.value("now_sec", -1)},
           {"verdict", "Completed · 消息=妈妈 delivered · 单域快路（核心 1 咨询）"}};
  t2["dag"] = t2_dag;   // 与实时进度共用同一份定义（t2 开跑前已构造）
  t2["messages"] = Json::array(
      {Json{{"at_sec", 0}, {"kind", "user"}, {"from", "用户"}, {"node", "n_user"},
            {"text", "给妈妈发微信问她晚上来不来吃饭。"}},
       Json{{"at_sec", 0}, {"kind", "dispatch"}, {"from", "核心"}, {"node", "n_core"},
            {"text", "→消息域：给妈妈发微信问她晚上来不来吃饭。"}},
       Json{{"at_sec", 0}, {"kind", "tool"}, {"from", "消息域"}, {"node", "n_msg"},
            {"tool", "samock_send_message"}, {"text", "send_message{m6, 妈妈, 你晚上来不来吃饭？}"}},
       Json{{"at_sec", 0}, {"kind", "ai"}, {"from", "消息域"}, {"node", "n_msg"},
            {"text", "已发送到妈妈并确认送达。"}},
       Json{{"at_sec", 0}, {"kind", "assert"}, {"from", "世界"}, {"node", "n_world"},
            {"text", "世界断言：messages=[妈妈, delivered=true] · calls 空 · 钟=0s"}}});
  t2["milestones_summary"] = Json::array();

  // —— 组装任务日志并与历史合并落盘（web/sa/index.html 的数据源）——
  // S8：scripts 的 t1/t2 用稳定 id 覆盖更新；一句话规划产生的任务保留在历史里（删除才消失）。
  Json tl = load_history(tl_path);
  tl["schema"] = "sa-tasklog/v1";
  tl["deterministic"] = true;
  tl["source"] = "sa_demo S1–S4 脚本（t1 C7 简版 / t2 消息直路）+ S7/S8 一句话规划历史";
  tl["tasks"] = merge_history(tl.value("tasks", Json::array()), Json::array({t1, t2}));

  if (!write_json_atomic(tl_path, tl)) {
    r4(false, "写 " + tl_path + " 失败（页面将退到内置 sample-tasklog.json）");
  } else {
    r4(true, "已写 " + tl_path + " —— ./run-webui.sh 起服后开 http://localhost:8000/sa/ 查看");
  }
  prog.emit(Json{{"t", "run"}, {"state", "done"}});
  s4.note("左=任务列表（query + SA 回传 + 用户补充组成完整任务）· 右=详情（上 DAG 节点图 / 下消息列表，50% 可拖动）。");

  for (std::string const& line : s4.lines) std::println("{}", line);

  // ---- 汇总 ----
  std::println("");
  if (all && i1_core && s2.ok && s3.ok && s4.ok) {
    std::println("验收结论：S1 + S2 + S3 + S4 全部通过 —— samock 世界 ✓ · C7 简版闭环 ✓ · M0 验证器+对抗 ✓ · SA UI 任务日志 ✓");
    return 0;
  }
  std::println("验收结论：存在失败项，见上 [✗]。");
  return 1;
}