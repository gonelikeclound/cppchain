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

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <generator>
#include <map>
#include <print>
#include <span>
#include <string>
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

}  // namespace

int main() {
  std::println("SystemAgent 骨架 demo（problems/64 · S1 · 全离线）\n");
  std::println("SA ≠ 单个 agent：核心 agent（编排·只读观测） + 电话域 agent + 消息域 agent；");
  std::println("假世界全部来自独立 so：tests/plugin_demo/samock.cpp → libsamock.so\n");

  std::filesystem::path const so = find_samock_so();
  std::println("samock 库：{}", so.string());

  // ---- ① samock 装载（fail-closed 加载器；wfr 动态工具库同一路径）----
  std::vector<std::string> names(std::begin(kSamockTools), std::end(kSamockTools));
  auto specs = load_dynamic_tools(so.string(), names);
  if (!specs) {
    std::println("[✗] samock 加载失败：{}", specs.error());
    return 1;
  }
  if (specs->size() != names.size()) {
    std::println("[✗] 加载工具数不符：期望 {} 实际 {}", names.size(), specs->size());
    return 1;
  }
  ToolRegistry samock;
  for (ToolSpec& s : *specs) samock.add(std::move(s));

  // 启动自检（wfr-㊹ 治理惯例）：有副作用却未声明效果分类 → fail-fast。
  auto vio = effect_class_violations(samock);
  if (!vio.empty()) {
    std::println("[✗] samock 效果分类违规（read_only=false 必须声明 Idempotent/Irreversible）：");
    for (std::string const& t : vio) std::println("     {}", t);
    return 1;
  }

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
  auto bind_agent_tool = [&trace](std::string const& agent, ToolSpec spec) {
    std::string const name = spec.name;
    ToolSpec::Handler base = spec.handler;
    spec.handler = [&trace, agent, name, base](Json const& args, tool::ToolContext& ctx) {
      trace.push_back(sa::Step{agent, name, args});
      return base(args, ctx);
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

  // (1) 核心咨询①：初始分诊 → 电话域
  model_core.replies = {message::ai_message("→电话域：给妈妈打电话问她晚上来不来吃饭。")};
  auto core1 = core_agent.invoke(one_human("给妈妈打电话问她晚上来不来吃饭。"));
  rec(core1.has_value() && core1->text().find("电话域") != std::string::npos,
      "核心咨询①（初始分诊）→ 路由到电话域");

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

  // (3) 用户改口 → 控制平面 barge_in（导演代用户直灌，不经核心）
  Json barge = run_tool(samock, "samock_barge_in",
                        Json{{"call_id", "c1"}, {"intent", "redirect_to_messaging"}}, e2);
  rec(!barge.empty() && barge.value("status", "") == "redirected",
      "用户改口「算了，发个微信就行」→ barge_in 直灌控制平面（仅通话中有效）");

  // (4) 电话域咨询②：观察 ended{user_redirected} → 上报核心改道
  model_phone.replies = {
      ai_call("samock_milestones", Json{{"call_id", "c1"}, {"since", 6}}),
      message::ai_message("电话域：妈妈改口让发微信，通话已改道结束（ended user_redirected），请核心编排收尾。"),
  };
  auto phone2 =
      phone_agent.invoke(one_human("通话有了新进展，查一下里程碑并汇报。"));
  rec(phone2.has_value() && phone2->text().find("微信") != std::string::npos,
      "电话域咨询②：读到 ended{user_redirected} → 上报核心改道");

  // (5) 核心咨询②：改道编排 → 消息域
  model_core.replies = {message::ai_message("→消息域：妈妈改口，让发微信。")};
  auto core2 =
      core_agent.invoke(one_human("电话域上报：妈妈改口要发微信，不再电话问。请你编排收尾。"));
  rec(core2.has_value() && core2->text().find("消息") != std::string::npos,
      "核心咨询②（改道编排）→ 路由到消息域");

  // (6) 消息域咨询：发消息 → 送达回执
  model_msg.replies = {
      ai_call("samock_send_message", Json{{"msg_id", "m5"}, {"to", "妈妈"},
                                          {"text", "你晚上来不来吃饭？我提前准备。"}}),
      message::ai_message("消息域：已发送到妈妈并确认送达。"),
  };
  auto msg1 = msg_agent.invoke(one_human("域内任务：给妈妈发微信问晚上来不来吃饭。"));
  rec(msg1.has_value() && msg1->text().find("送达") != std::string::npos,
      "消息域咨询：send_message → 送达回执");

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

  // ---- 汇总 ----
  std::println("");
  if (all && i1_core && s2.ok && s3.ok) {
    std::println("验收结论：S1 + S2 + S3 全部通过 —— samock 世界 ✓ · C7 简版闭环 ✓ · M0 验证器纯函数 + 对抗集 ✓");
    return 0;
  }
  std::println("验收结论：存在失败项，见上 [✗]。");
  return 1;
}