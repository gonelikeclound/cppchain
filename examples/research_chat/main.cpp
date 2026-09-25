// demo: research_chat —— 单 Agent 连续聊天：意图二分（casual/research）+ 联网取数兜底保证（M7，真机）
//
// 架构参照 myplane.md 的「Single-Agent + Router + Skill」。
// 路由器只分两类（不做按工具的分类 —— 否则以后每加一个工具就要加一个意图标签）：
//   · casual   → 不挂任何工具（闲聊/记忆问答/常识答复）
//   · research → 挂上「全部外部数据源」工具集（现在 = web_search + get_weather +
//                arxiv_search + fetch_url；以后加新工具直接 add 进同一个注册表即可，
//                路由器与兜底判定都不用改——它们都遍历注册表整表，不针对工具名写死）
// 所有轮次共用 同一个 thread_id + MemoryCheckpointer → 「连续聊天」= 天然的多轮记忆。
//
// 核心保证（本 demo 的看点）：一旦意图被识别为 research（资料查询类），harness 必须确保
//   至少 1 次成功的外部取数 ——
//     · 模型自发调用了任意外源工具（web_search 通用检索，或 get_weather 这类专用实时源）
//       → 放行；
//     · 否则（模型凭记忆作答、没调任何外源工具）→ harness 直接执行 web_search（通用检索
//       兜底），再让模型仅凭检索结果重答。
//   兜底判定遍历注册表整表，不针对某个工具名写死 —— 加新工具不改判定口径。
//   注意：保证的是“联网取数真的发生”，成功与否单列——若 web_search 端点不可用（如 401），
//   本轮如实记为「未取到有效结果」。
//
// 需要 .env：
//   LLM_API_KEY / LLM_BASE_URL / LLM_MODEL        —— 对话与意图路由模型（如 DeepSeek）
//   WEB_SEARCH_API_KEY                            —— feedcoop 搜索 API Key（Bearer，参考 simple-agent）
//   缺 LLM 密钥时退化为「工具直连自检」（也可 RESEARCH_CHAT_DIRECT=1 强制直连）。
//   兜底路径可被 `RESEARCH_CHAT_FORCE_BACKSTOP=1` 强制走一遍（确定性演示兜底机制）。
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/model/structured.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/tool/arxiv.hpp>
#include <cppchain/tool/fetch_url.hpp>
#include <cppchain/tool/weather.hpp>
#include <cppchain/tool/web_search.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using namespace cppchain::model;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::TextBlock;

namespace {

// ---- 意图（只分两类；按工具的分类是反模式，见文件头注释）----
enum class Intent { Casual, Research };

std::string_view intent_name(Intent i) {
  switch (i) {
    case Intent::Casual: return "casual";
    case Intent::Research: return "research";
  }
  return "?";
}

// ---- 走廊/统计 ----
struct Stats {
  int turns = 0;
  int n_casual = 0, n_research = 0;
  std::map<std::string, int> tool_hits;  // 各外源工具成功执行累计（web_search 含 harness 兜底）
  int web_backstops = 0;   // harness 兜底直接 web_search 的尝试次数（含失败）
  int unretrieved = 0;     // research 轮中「零成功外源取数」的轮数
  std::vector<std::size_t> history_sent;  // 每轮首次发往模型的消息条数（记忆增长的机械证据）
};

// UTF-8 安全的字节截断（避免把中文切断）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

// 探针：记录每轮「首次模型调用前」的消息条数。
// 同一 thread 下它随轮次递增 = 历史在累积（连续聊天的机械证明，思路同 history_agent）。
struct TurnProbe final : agent::Middleware {
  bool captured = false;
  std::size_t msgs_sent = 0;
  void before_model(std::vector<Message>& msgs) override {
    if (!captured) {
      captured = true;
      msgs_sent = msgs.size();
    }
  }
};

// 数一数 transcript 里真实执行成功的指定工具调用次数（Ai(tool_calls) 与它的 ToolMessage 配对）
int count_executed(std::span<Message const> msgs, std::string_view tool_name) {
  int n = 0;
  for (auto const& m : msgs) {
    if (m.type != Role::Ai) continue;
    for (auto const& tc : m.tool_calls) {
      if (tc.name != tool_name) continue;
      for (auto const& t : msgs)
        if (t.type == Role::Tool && t.tool_call_id && *t.tool_call_id == tc.id &&
            t.status == "success") {
          ++n;
          break;
        }
    }
  }
  return n;
}

// 供意图路由器食用的「近况」：只保留角色 + 截断文本，剥掉工具协议字段，
// 避免推理时把整段检索结果和孤儿 Tool 消息塞给路由器（也省 token）。
std::vector<Message> compact_tail(std::span<Message const> thread, std::size_t n,
                                  std::size_t max_bytes) {
  std::size_t skip = (!thread.empty() && thread.front().type == Role::System) ? 1 : 0;
  std::size_t begin = thread.size();
  for (std::size_t want = 0; want < n && begin > skip;) {
    --begin;
    ++want;
  }
  std::vector<Message> out;
  for (std::size_t i = begin; i < thread.size(); ++i) {
    Message m;
    m.type = thread[i].type;
    if (m.type == Role::Tool) m.type = Role::System;  // 工具结果视作“系统给的资料”
    m.content.clear();
    m.content.push_back(TextBlock{clip(thread[i].text(), max_bytes), std::nullopt});
    out.push_back(std::move(m));
  }
  return out;
}

// 把注册表里的工具名列出来（展示本轮授予的工具集）
std::string tools_str(tool::ToolRegistry const& reg) {
  std::string s = "[";
  bool first = true;
  for (auto const& spec : reg.all()) {
    if (!first) s += ", ";
    first = false;
    s += spec.name;
  }
  s += "]";
  return s;
}

// 格式化成模型可见的检索结果文本（feedcoop：原始文档列表）
std::string render_search_result(nlohmann::json const& r) {
  std::string out;
  if (r.contains("query")) out += "  查询词: " + r["query"].get<std::string>() + "\n";
  auto const& docs = r.contains("documents") ? r["documents"] : nlohmann::json::array();
  for (auto const& d : docs) {
    if (!d.is_object()) continue;
    out += "  - " + d.value("title", "(无标题)") + "\n    " + d.value("url", "");
    std::string const host = d.value("hostname", "");
    if (!host.empty()) out += "（" + host + "）";
    out += "\n";
    if (d.contains("snippet") && d["snippet"].is_string())
      if (!d["snippet"].get<std::string>().empty())
        out += "    摘要: " + d["snippet"].get<std::string>() + "\n";
  }
  return out;
}

// ---- 单轮调度 ----
struct TurnOutcome {
  Intent intent = Intent::Casual;
  std::vector<std::string> used_tools;  // 本轮成功执行过的外源工具名（去重）
  int web_hits = 0;                     // 本轮成功执行的 web_search 次数（含兜底）
  bool backstopped = false;             // 本轮是否触发了 harness 兜底 web_search
  bool ok = true;                       // 本轮是否整体成功（含兜底成功）
  std::size_t sent = 0;                 // 本轮首次发往模型的消息条数
  std::string answer;
  std::vector<Message> thread;
};

// casual 轮：不挂任何工具，模型直接答
TurnOutcome run_casual_turn(ChatModel& model, tool::ToolRegistry const& reg_casual,
                            AgentOptions base, std::string const& user) {
  Agent agent(model, reg_casual, std::move(base));
  auto probe = std::make_shared<TurnProbe>();
  agent.add_middleware(probe);
  auto r = agent.invoke({message::human_message(user)});
  TurnOutcome out;
  out.intent = Intent::Casual;
  out.sent = probe->msgs_sent;
  out.thread = agent.transcript();
  out.ok = r.has_value();
  out.answer = r ? r->text() : ("(失败: " + r.error().message + ")");
  return out;
}

// 本轮新增消息的切片（从「本轮新输入」那条开始）：只看本轮实际发生的工具调用，
// 避免把**历史轮**的成功取数误算成本轮已联网（状态时效性：上一轮的旧天气算不了本轮的新资料）。
std::span<Message const> this_turn_slice(TurnOutcome const& out) {
  std::size_t start =
      (out.sent > 0 && out.sent - 1 <= out.thread.size()) ? out.sent - 1 : out.thread.size();
  return {out.thread.begin() + static_cast<std::ptrdiff_t>(start), out.thread.end()};
}

// research 轮：挂上「全部外部数据源」工具集（web_search + 专用实时源，如 get_weather），
// 模型自行选择。harness 保证：本轮必须有 ≥1 次成功的外部取数。
//   · 模型自发成功调了任一外源工具 → 放行；
//   · 零外源（凭记忆/编造作答）→ harness 直接 web_search 兜底，并让模型仅凭结果重答。
// 判定遍历注册表整表，不针对工具名写死 —— 未来 add 新工具，此处与路由器都不用改。
TurnOutcome run_research_turn(ChatModel& model, tool::ToolRegistry const& reg_research,
                              AgentOptions base, std::string const& user, Stats& st,
                              bool force_backstop) {
  AgentOptions opts = base;
  TurnOutcome out;
  out.intent = Intent::Research;
  bool direct_search_ok = false;  // 兜底那次直接 web_search 是否成功（不经过模型，thread 里没有）

  if (!force_backstop) {
    // 先让模型自由尝试：从轮首历史继续，看它会不会自己调任何外源工具
    Agent agent(model, reg_research, opts);
    auto probe = std::make_shared<TurnProbe>();
    agent.add_middleware(probe);
    auto r = agent.invoke({message::human_message(user)});
    out.sent = probe->msgs_sent;
    out.thread = agent.transcript();
    out.ok = r.has_value();
    out.answer = r ? r->text() : ("(失败: " + r.error().message + ")");
  } else {
    out.answer = "（FORCE_BACKSTOP：跳过自由尝试，直接走 harness 兜底检索）";
  }

  // 本轮是否有成功的「外部取数」？只看本轮新增切片（web_search 或任一专用实时源）
  for (auto const& spec : reg_research.all())
    if (count_executed(this_turn_slice(out), spec.name) > 0) out.used_tools.push_back(spec.name);

  if (out.used_tools.empty()) {
    // ---- 兜底：零外源 → harness 直接执行 web_search（通用检索兜底）----
    out.backstopped = true;
    ++st.web_backstops;
    auto* spec = reg_research.find("web_search");
    tool::ToolContext ctx;
    ctx.tool_call_id = "backstop_" + std::to_string(st.turns + 1);
    auto res = spec->handler(nlohmann::json{{"query", user}}, ctx);
    if (!res) {
      out.answer = "（harness 兜底 web_search 也失败：" + res.error().message + "）";
      out.ok = false;
    } else {
      // 同线程续跑：仅凭检索结果重答（上一条凭记忆的回答留在历史里，被本条显式纠正）
      direct_search_ok = true;
      Agent agent2(model, reg_research, opts);
      auto probe2 = std::make_shared<TurnProbe>();
      agent2.add_middleware(probe2);
      auto rr = agent2.invoke({message::human_message(
          "⚠️ 你上一条回答没有调用任何外部数据源（web_search / get_weather），"
          "但本问题需要联网取数。\n"
          "请你忽略上一条回答，仅依据下面的【联网检索结果】改写回答，尽量附上来源；"
          "资料不足就直说不足。\n"
          "用户问题：「" + user + "」\n\n【联网检索结果】\n" +
          render_search_result(*res))});
      out.thread = agent2.transcript();  // 以续跑后的完整历史为准
      out.ok = rr.has_value();
      out.sent = out.sent == 0 ? probe2->msgs_sent : out.sent;
      out.answer = rr ? rr->text() : ("(重答失败: " + rr.error().message + ")");
    }
  }

  // 汇总本轮真实发生的取数（以最终 thread 的本轮切片为准 + 兜底那次直接调用）
  out.used_tools.clear();
  for (auto const& spec : reg_research.all())
    if (count_executed(this_turn_slice(out), spec.name) > 0) out.used_tools.push_back(spec.name);
  out.web_hits = count_executed(this_turn_slice(out), "web_search");
  if (direct_search_ok &&
      std::find(out.used_tools.begin(), out.used_tools.end(), "web_search") ==
          out.used_tools.end()) {
    out.used_tools.push_back("web_search");
    ++out.web_hits;
  }
  return out;
}

std::string ca_bundle_from_env() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const c = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(c)) ca = c;
    }
  }
  return ca;
}

// 无 LLM 密钥时的退化路径：跳过意图路由与 Agent，直接自检两个真实工具
void run_direct_tools(tool::ToolRegistry const& reg_research) {
  auto probe = [](std::string name, tool::ToolSpec const* spec, nlohmann::json args) {
    if (!spec) {
      std::println("✗ {} 未注册", name);
      return;
    }
    tool::ToolContext ctx;
    ctx.tool_call_id = "direct_" + name;
    auto r = spec->handler(args, ctx);
    if (r)
      std::println("✓ {} → {}", name, r->dump(2));
    else
      std::println("✗ {} → {}", name, r.error().message);
  };
  std::println("→ get_weather（北京，1 天）：");
  probe("get_weather", reg_research.find("get_weather"), {{"city", "北京"}, {"days", 1}});
  std::println("\n→ web_search（2025 大模型进展）：");
  probe("web_search", reg_research.find("web_search"), {{"query", "2025 年大语言模型最重要进展"}});
  std::println("\n→ arxiv_search（2025 机器学习代表论文，走可过的查询）：");
  probe("arxiv_search", reg_research.find("arxiv_search"),
        {{"search_query", "all:transformer"}, {"max_results", 2}});
  std::println("\n→ fetch_url（arXiv 论文页正文）：");
  probe("fetch_url", reg_research.find("fetch_url"),
        {{"url", "https://arxiv.org/abs/1706.03762"}, {"max_chars", 600}});
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const llm_key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const llm_base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const llm_model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  std::string const ca = ca_bundle_from_env();

  auto transport = std::make_shared<http::CurlMultiTransport>();

  // 两个注册表：casual = ∅（一个工具都不带）；
  // research = 全部外部数据源（web_search 通用检索 + get_weather 专用实时源，将来继续 add）
  tool::ToolRegistry reg_casual;
  tool::ToolRegistry reg_research;

  tool::WeatherToolConfig wc;
  wc.ca_info = ca;
  reg_research.add(tool::make_weather_tool(transport, std::move(wc)));

  tool::WebSearchConfig sc;
  std::string const ws_url =
      demo::env_or(dotenv, "WEB_SEARCH_BASE_URL",
                   "https://open.feedcoopapi.com/search_api/global_search");
  std::string const ws_key = demo::env_or(dotenv, "WEB_SEARCH_API_KEY");
  sc.base_url = ws_url;
  sc.api_key = ws_key;
  sc.ca_info = ca;
  reg_research.add(tool::make_web_search_tool(transport, std::move(sc)));

  tool::ArxivConfig ac;
  ac.ca_info = ca;
  reg_research.add(tool::make_arxiv_search_tool(transport, std::move(ac)));

  tool::FetchUrlConfig fc;
  fc.ca_info = ca;
  reg_research.add(tool::make_fetch_url_tool(transport, std::move(fc)));

  std::println("research_chat：单 Agent 连续聊天 · 意图二分(casual/research) · 联网取数兜底保证");
  std::println("  对话模型 : {} @ {}", llm_model, llm_base);
  std::println("  web_search: {}（feedcoop）{}", ws_url, ws_key.empty() ? "（无密钥）" : "");
  std::println("  工具集   : casual=∅ | research={}（全部外部数据源，模型自选）", tools_str(reg_research));

  bool const direct = demo::env_or(dotenv, "RESEARCH_CHAT_DIRECT") == "1" || llm_key.empty();
  if (direct) {
    std::println("{}", llm_key.empty() ? "LLM_API_KEY 缺失 → 工具直连自检："
                                       : "RESEARCH_CHAT_DIRECT=1 → 工具直连自检：");
    run_direct_tools(reg_research);
    return 0;
  }

  model::ModelConfig cfg;
  cfg.api_key = llm_key;
  cfg.base_url = llm_base;
  cfg.model = llm_model;
  cfg.ca_info = ca;
  model::openai::OpenAIChatModel model(cfg, transport);

  // 意图路由器（强制结构化输出；分类本身是路由的第一步，不进入会话记忆）
  model::StructuredOutput router(model, nlohmann::json::object());

  bool const force_backstop = demo::env_or(dotenv, "RESEARCH_CHAT_FORCE_BACKSTOP") == "1";

  // 连续会话基础设施：同一 thread + checkpointer，所有轮次共用
  state::MemoryCheckpointer cp;
  AgentOptions base_opts;
  base_opts.system_prompt =
      "你是用户的日常助手。\n"
      "规则：\n"
      "  1. 闲聊/记忆问答直接回答，语气自然。\n"
      "  2. 资料查询必须调用合适的外部数据源，并基于结果回答、尽量附来源：\n"
      "     - 天气问题 → get_weather（专用实时源）；\n"
      "     - 其他需要最新/精确/外部资料的问题（新闻、数据、事实核实、进展等）→ web_search；\n"
      "     - 学术/论文调研（某方向是否有论文、找综述或代表工作）→ arxiv_search（只查论文）；\n"
      "     - 已经拿到一个网址、要读它的网页正文详情时 → fetch_url（会剥掉标签返回纯文本）。\n"
      "     - 不要拿 get_weather 答非天气问题，也不要拿 arxiv_search 当通用网络搜索。\n"
      "  3. 不确定或信息可能过时，宁可检索也不要编造。\n"
      "  4. 回答保持简短有力。";
  base_opts.checkpointer = &cp;
  base_opts.thread_id = "research-chat-u1";

  // 演示剧本：闲聊 → 天气(专用实时源) → 联网检索 → 记忆考核 → 知识题(触发兜底) →
  //            学术检索(arxiv_search) → 网页正文(fetch_url) → 收尾
  std::vector<std::string> const scenario{
      "嗨，跟我随便聊聊吧，今天想聊点轻松的话题。",
      "上海明天天气怎么样？我需要带伞或加衣服吗？",
      "用联网搜索查一下：2025 年大语言模型领域最重要的技术进展有哪些？",
      "我刚才问的是哪个城市明天的天气？",
      "珠穆朗玛峰最新的精确海拔是多少米？（数据需要外部核实）",
      "用 arXiv 学术检索帮我找两篇 Transformer 架构的代表性论文，"
      "给出标题、分类（如 cs.LG/cs.CL）和一段摘要。",
      "请用网页正文工具打开论文页面 https://arxiv.org/abs/1706.03762，"
      "把它的标题和摘要读给我。",
      "好的，今天聊得不错，谢谢你！",
  };

  Stats st;
  int failures = 0;
  std::vector<Message> thread;  // 本地镜像当前会话历史（每轮刷新）

  auto run_turn = [&](int idx, std::string const& user) {
    std::println("──────────────────────────────────────────────");
    std::println("[轮 {}/{}] 用户: {}", idx, scenario.size(), clip(user, 120));

    // ① 意图路由（二分）：结合近况（最近 6 条、每条截 220 字节）
    std::vector<Message> router_msgs;
    router_msgs.push_back(message::system_message(
        "你是意图路由器。结合最近对话上下文，把「用户最新一条消息」归为两类：\n"
        "  casual   —— 闲聊、寒暄、记忆问答、主观/常识答复，不需要外部最新资料；\n"
        "  research —— 资料查询：需要联网/最新/精确/外部数据来回答，"
        "如天气、新闻、行情数据、事实核实、最新进展等。\n"
        "只输出一个标签：casual 或 research。"));
    auto tail = compact_tail(thread, 6, 220);
    router_msgs.insert(router_msgs.end(), tail.begin(), tail.end());
    router_msgs.push_back(message::human_message(user));

    auto cr = router.classify(router_msgs, {"casual", "research"}, CallOptions{});
    Intent intent = Intent::Casual;
    if (cr)
      intent = (*cr == "research") ? Intent::Research : Intent::Casual;
    else
      std::println("  ⚠ 意图识别失败（{}），按 casual 处理", cr.error().message);
    std::println("  ① 意图识别 → {}", intent_name(intent));

    TurnOutcome out;
    if (intent == Intent::Research)
      out = run_research_turn(model, reg_research, base_opts, user, st, force_backstop);
    else
      out = run_casual_turn(model, reg_casual, base_opts, user);

    // ② 汇总本轮（工具计数只看本轮新增切片，避免把历史轮的取数算进来）
    thread = out.thread;
    ++st.turns;
    if (intent == Intent::Research) ++st.n_research; else ++st.n_casual;
    // 外源工具成功累计：web_search 用 out.web_hits（含 harness 兜底直查那一次）；
    // 其余工具只统计 thread 内的成功执行（都遍历注册表整表，新增工具自动纳入）
    st.tool_hits["web_search"] += out.web_hits;
    for (auto const& spec : reg_research.all())
      if (spec.name != "web_search")
        st.tool_hits[spec.name] += count_executed(this_turn_slice(out), spec.name);
    if (out.sent > 0) st.history_sent.push_back(out.sent);  // 无模型调用的轮次不进记忆链
    if (intent == Intent::Research && out.used_tools.empty()) ++st.unretrieved;

    std::println("  ② 工具集   → {}", intent == Intent::Casual ? "∅（不挂工具）" : tools_str(reg_research));
    if (intent == Intent::Research) {
      // 逐个工具打印本轮成功执行次数
      std::string hits;
      for (auto const& spec : reg_research.all()) {
        int h = count_executed(this_turn_slice(out), spec.name);
        if (!hits.empty()) hits += " | ";
        hits += spec.name + " ×" + std::to_string(h);
      }
      if (out.backstopped)
        std::println("  ③ 外部取数：{} → ⚠ 零外源 → harness 兜底执行 web_search（{}）", hits,
                     out.ok ? "成功" : "失败，见回答");
      else
        std::println("  ③ 外部取数：{}（模型自发）", hits);
    }
    if (out.sent > 0)
      std::println("  ④ 首轮发往模型 {} 条消息（同一 thread 历史累积）", out.sent);
    else
      std::println("  ④ 本轮无模型调用（harness 兜底直查失败，回答为错误说明）");
    std::println("  ⑤ 回答: {}", clip(out.answer, 260));
    if (!out.ok) ++failures;
    return out;
  };

  for (int i = 0; i < static_cast<int>(scenario.size()); ++i) run_turn(i + 1, scenario[i]);

  // ---- 汇总 ----
  std::println("\n──────────────── 汇总 ────────────────");
  std::println("意图分布   : casual {} 轮 | research {} 轮", st.n_casual, st.n_research);
  int total_hits = 0;
  std::string hits_line;
  for (auto const& spec : reg_research.all()) {
    int const h = st.tool_hits.count(spec.name) ? st.tool_hits.at(spec.name) : 0;
    total_hits += h;
    if (!hits_line.empty()) hits_line += " | ";
    hits_line += spec.name + " ×" + std::to_string(h);
  }
  std::println("工具执行   : {}（共 {} 次成功外源取数）", hits_line, total_hits);
  std::println("  harness 兜底尝试 ×{}", st.web_backstops);
  // research 轮的「检索保证」是构造性的：run_research_turn 要么放行模型自发的任意外源取数，
  // 要么兜底强制 web_search —— 判定遍历注册表整表（非 per-tool 硬编码），加新工具不改口径。
  // 但“取数发生”不等于“取数成功”：端点不可用（如 401）时本轮如实记为 unretrieved。
  std::println("检索保证   : research {} 轮全部确保调用外部数据源；"
               "其中取到有效结果 {} 轮，无有效结果 {} 轮",
               st.n_research, st.n_research - st.unretrieved, st.unretrieved);

  std::string hist;
  for (std::size_t i = 0; i < st.history_sent.size(); ++i)
    hist += (i ? "→" : "") + std::to_string(st.history_sent[i]);
  std::println("连续记忆   : 有模型调用的轮次，首轮消息数 {}（递增 = 同一 thread 历史累积）", hist);
  bool const memory_grows =
      st.history_sent.size() >= 2 && st.history_sent.front() < st.history_sent.back();
  bool const guarantee_met = st.unretrieved == 0;
  std::println("结构断言   : 连续记忆递增 {} | research 检索保证达成 {}",
               memory_grows ? "✓" : "✗", guarantee_met ? "✓" : "✗");

  std::println("\n=== 结果: {} ===",
               (failures == 0 && memory_grows && guarantee_met) ? "全部通过 ✓" : "有失败项 ✗");
  return (failures == 0 && memory_grows && guarantee_met) ? 0 : 1;
}