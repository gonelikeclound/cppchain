// demo: m9_budget —— 压缩的成本/收益量化（真机）
//
//  看点：（extended）信息密度。用真实 LLM 跑多轮，TrimHistory（token 阈值触发 + LLM 摘要器）
//  每次压缩后打印：估算省下的 token（tokens_saved）与这次摘要调用花费的**真实 usage**。
//  两者对比 = "省下的输入 vs 多花的输出"，是压缩的净收益估量。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/agent.hpp>
#include <cppchain/agent/tokens.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <filesystem>
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
  cfg.ca_info = ca_bundle_from_env();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  // 摘要器：直接调模型，量到这次摘要调用的真实 usage（TrimHistory 只记次数与估算省量）
  long long summary_input = 0, summary_output = 0, summary_calls = 0;
  TrimHistory::Summarizer summarizer = [&](std::vector<Message> const& folded)
      -> std::expected<std::string, model::ModelError> {
    std::vector<Message> msgs;
    msgs.push_back(message::system_message(
        "把以下对话压成一段简洁摘要（事实清单式，保留人名、数字、关键偏好）："));
    msgs.insert(msgs.end(), folded.begin(), folded.end());
    auto r = model.invoke(msgs, model::CallOptions{});
    if (!r) return std::unexpected(r.error());
    ++summary_calls;
    if (r->usage) {
      summary_input += r->usage->input_tokens;
      summary_output += r->usage->output_tokens;
    }
    return r->text();
  };

  tool::ToolRegistry empty;
  AgentOptions opts;
  opts.system_prompt = "你是角色扮演助手：记住用户介绍的信息，回答保持简短。";
  TrimHistory::Options to;
  to.max_messages = 20;      // 条数阈值：不触发（消息少）
  to.max_tokens = 400;       // token 阈值：多轮后触发（口径为估算，见 tokens.hpp）
  to.summarizer = summarizer;
  auto trim = std::make_shared<TrimHistory>(to);

  Agent agent(model, empty, opts);
  agent.add_middleware(trim);

  std::println("模型: {} —— 压缩成本/收益量化（token 阈值 {} 触发）\n", name, *to.max_tokens);
  std::size_t prev_compressions = 0;
  std::vector<Message> convo;  // 无 checkpointer → 会话历史由调用方累积并作为每轮输入
  for (int round = 0; round < 10; ++round) {
    std::string const note =
        "我叫小明，正在学 C++ 与编译原理，目标是两年内做出一个解释器；"
        "我住在杭州，喜欢读费曼的《别闹了，费曼先生》，IDE 是 Neovim；"
        "第 " + std::to_string(round) + " 轮补充，请记住这条信息。";
    convo.push_back(message::human_message(note));
    auto r = agent.invoke(convo);  // 传整段历史 → TrimHistory 在 before_model 里见得到它
    if (!r) {
      std::println("失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }
    convo.push_back(*r);
    if (trim->compressions() > prev_compressions) {
      std::println("第 {} 轮发生压缩之后历史估算：{} tokens（累计省 ~{}）", round + 1,
                   tokens::estimate_msgs(agent.transcript()), trim->estimated_tokens_saved());
    }
    prev_compressions = trim->compressions();
  }
  std::println("\n压缩发生 {} 次；LLM 摘要 {} 次，摘要真实 usage 合计：输入 {} / 输出 {} tokens；",
               trim->compressions(), summary_calls, summary_input, summary_output);
  std::println("估算总省 ~{} tokens。净收益 ≈ 省下的输入 - 摘要输出的成本。",
               trim->estimated_tokens_saved());
}