// demo: sa_demo —— SystemAgent（SA）骨架 demo（S1，纯离线）
//
// SA 不是一个 agent，而是一组 agent（problems/64 §5.1）：
//   核心 agent（分诊/编排，只有只读观测工具 = I1 最小形态）
//   + 电话域 agent（samock 的 make_call/时钟）
//   + 消息域 agent（samock 的 send_message）
// 假世界全部来自独立 so：tests/plugin_demo/samock.cpp → libsamock.so。
//
// S1 里程碑内容（problems/64 §5.5 S1）：
//   ① samock 装载：tool::load_dynamic_tools（wfr 动态工具库同一加载器，
//      fail-closed：dlopen/符号/版本/名字/缺 handler 任一失败整体失败）；
//   ② 世界空跑：samock_world_state 序列化整个世界，所有断言**查世界不查返回值**
//      （机制测试不经自然语言，工具直灌 —— 07 R2 / oracle-patch 精神）；
//      脚本覆盖：空世界 → 真实写（honest）→ 说谎被抓（lying）→ 半写（partial）
//      → 虚拟钟推进 → 世界重置；
//   ③ SA 组装配：核心 + 电话域 + 消息域三实例构造（模型=脚本桩，S1 不发起对话；
//      组织闭环 + 假电话 FSM 在 S2 / C7 简版）。
//
// 运行：./run-demo.sh sa_demo    （全离线；CI 纪律 R5：不碰真模型）
#include <cppchain/agent/agent.hpp>
#include <cppchain/message/message.hpp>
#include <cppchain/model/chat_model.hpp>
#include <cppchain/model/task.hpp>
#include <cppchain/tool/registry.hpp>

#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <generator>
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
    "samock_make_call",      // 能力（v0）：拨打电话（S2 换完整假电话 FSM）
    "samock_send_message",   // 能力（v0）：发送消息
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
  core_reg.add(copy_spec("samock_world_state"));
  phone_reg.add(copy_spec("samock_make_call"));
  phone_reg.add(copy_spec("samock_world_state"));
  phone_reg.add(copy_spec("samock_clock_forward"));
  msg_reg.add(copy_spec("samock_send_message"));
  msg_reg.add(copy_spec("samock_world_state"));

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
                                 .max_iterations = 8, .thread_id = "", .checkpointer = nullptr});
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

  std::println("\nS2 预告：假电话 FSM（Dialing→Ringing→Connected→Ended|Failed）+ barge_in"
               " 进 samock，跑 C7 简版（打电话→改口→转消息）。");

  // ---- 汇总 ----
  std::println("");
  if (all && i1_core) {
    std::println("验收结论：S1 全部通过 —— samock 装载 ✓ · 世界空跑六场景 ✓ · SA 组装配 + I1 ✓");
    return 0;
  }
  std::println("验收结论：存在失败项，见上 [✗]。");
  return 1;
}