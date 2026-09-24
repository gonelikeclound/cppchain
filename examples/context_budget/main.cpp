// demo: context_budget —— 用真实 usage 量化两个卡位问题的代价（M7 收口）
//
// 实验 A：工具输出大小 → prompt token（"超长工具返回"值多少钱）
// 实验 B：多轮对话的历史增长 vs 压缩后（"上下文压缩"能省多少）
//
// 两个实验都读模型返回的 usage.input_tokens（真实计量，不估算）。
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

namespace {

std::string ca_bundle() {
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

std::string filler(std::size_t target_bytes) {
  std::string out;
  out.reserve(target_bytes + 64);
  int i = 0;
  while (out.size() < target_bytes)
    out += "第" + std::to_string(++i) + "条记录：项目文档中的一段正文，用于占据上下文空间。\n";
  return out;
}

long input_tokens_of(model::openai::OpenAIChatModel& model, std::vector<Message> const& msgs) {
  auto r = model.invoke(msgs, model::CallOptions{});
  if (!r) {
    std::println("  调用失败: {}", r.error().message);
    return -1;
  }
  return r->usage ? r->usage->input_tokens : -1;
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）");
    return 1;
  }

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base;
  cfg.model = name;
  cfg.ca_info = ca_bundle();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);
  std::println("模型: {}（以下数字均来自真实 usage）\n", name);

  // ---- 实验 A：工具输出大小 → 占用 token ----
  std::println("=== A. 工具输出大小 vs prompt token ===");
  std::println("{:>10}  {:>10}  {:>12}  {:>10}", "字节", "input_tok", "tok/字节", "对比空载");
  long baseline = -1;
  for (std::size_t bytes : {0u, 1000u, 4000u, 16000u}) {
    std::vector<Message> msgs{message::human_message("读取数据")};
    if (bytes > 0) {
      Message tool = message::tool_message(filler(bytes), "call_1");
      Message ai;
      ai.type = Role::Ai;
      ai.tool_calls.push_back(message::ToolCall{"call_1", "read_data", nlohmann::json::object()});
      msgs.push_back(ai);
      msgs.push_back(tool);
    }
    long const tok = input_tokens_of(model, msgs);
    if (bytes == 0) baseline = tok;
    std::printf("%10zu  %10ld  %12.3f  %10s\n", bytes, tok,
                bytes > 0 ? static_cast<double>(tok - baseline) / static_cast<double>(bytes) : 0.0,
                bytes > 0 ? ("+" + std::to_string(tok - baseline) + " tok").c_str() : "-");
  }
  std::println("  → 工具输出会 1:1 进入下一次请求的 prompt；一次 16KB 的输出 ≈ 数千 token，"
               "且在后续每一轮里**重复计费**。");

  // ---- 实验 B：多轮历史增长 vs 压缩 ----
  std::println("\n=== B. 多轮历史增长 vs TrimHistory 压缩 ===");
  tool::ToolRegistry const no_tools;  // 具名：Agent 持有引用，必须比 agent 活得久

  // 注意：Agent::invoke 每次会重置消息（与 langchain 一致，不带 checkpointer 即无状态）。
  // 要让历史累积，必须走 checkpointer + thread_id 的续跑路径。
  struct TurnResult {
    std::vector<long> tokens;
    std::size_t compressions = 0;
  };
  auto run_turns = [&](bool compress) {
    state::MemoryCheckpointer cp;
    AgentOptions opts;
    opts.system_prompt = "你是助手，每次回答不超过 15 个字。";
    opts.checkpointer = &cp;
    opts.thread_id = compress ? "trim" : "plain";
    Agent agent(model, no_tools, opts);
    std::shared_ptr<TrimHistory> trim;
    if (compress) {
      trim = std::make_shared<TrimHistory>(6);
      agent.add_middleware(trim);
    }
    TurnResult out;
    for (int i = 1; i <= 8; ++i) {
      auto turn = agent.invoke(
          {message::human_message("请记住：这是第 " + std::to_string(i) + " 轮对话。")});
      if (!turn) {
        std::println("  轮次 {} 失败: {}", i, turn.error().message);
        break;
      }
      out.tokens.push_back(input_tokens_of(model, agent.transcript()));
    }
    if (trim) out.compressions = trim->compressions();
    return out;
  };

  auto const plain = run_turns(false);
  auto const trimmed = run_turns(true);
  std::println("{:>6}  {:>14}  {:>14}", "轮次", "不压缩(tok)", "压缩后(tok)");
  for (std::size_t i = 0; i < plain.tokens.size(); ++i)
    std::printf("%6zu  %14ld  %14ld\n", i + 1, plain.tokens[i],
                i < trimmed.tokens.size() ? trimmed.tokens[i] : -1);
  std::println("  压缩触发 {} 次；末轮: {} → {} tok", trimmed.compressions,
               plain.tokens.empty() ? -1 : plain.tokens.back(),
               trimmed.tokens.empty() ? -1 : trimmed.tokens.back());

  std::println("\n结论（用于 04-problems/02、04）：");
  std::println("  · 工具输出按字节线性进入 prompt，且在被压缩前**每轮重复计费**");
  std::println("  · 截断省的是「每轮重复」的钱，不是一次性的钱 —— 这才是它真正的价值");
  std::println("  · 压缩把历史增长从线性压成有界，代价是摘要调用 + 信息损失（需权衡且可观测）");
  return 0;
}