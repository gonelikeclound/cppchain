// demo: session_resume —— 落盘历史：**跨命令**的连续对话（架构验证 A，真机）
//
// 验证的命题：Agent 的会话历史落到**工作路径**下的一个文件里，于是**两次互相独立的
// 进程调用**（而不是同一个进程里的多轮）能续上同一段对话。
//
// 与 examples/history_agent 的区别：那个用 MemoryCheckpointer，进程一退历史就没了；
// 这个用 FileCheckpointer，历史在磁盘上。两者的 Agent 代码完全一样 —— 换的只是
// Checkpointer 实现（这正是"分层可插拔"要证明的事）。
//
// 用法：
//   ./session_resume --workdir DIR "问题"     # 跑一轮（历史自动落盘/续读）
//   ./session_resume --workdir DIR --show     # 只看已存的历史（不起模型，可离线）
//   ./session_resume --workdir DIR --reset    # 清掉历史文件
//
// 核心证明不依赖模型发挥：打印"本次真正发往模型的 messages 条数"（探针中间件）与
// "从文件恢复到的条数"。第二次调用若显示恢复了上一轮的消息，跨命令续跑即成立。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（企业代理见 LLM_CA_BUNDLE）
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/state/state.hpp>  // StateSchema：与 Agent 恢复历史用同一条归一化路径

#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using cppchain::message::Message;

namespace {

constexpr char const* kThreadId = "cli";          // 同一个工作路径 = 同一段会话
constexpr char const* kHistoryRel = ".cppchain/history.json";

// 探针：记录每次模型调用前真正发过去的 messages 条数（跨命令对比的机械证据）
struct HistoryProbe final : agent::Middleware {
  std::vector<std::size_t> sent_sizes;
  void before_model(std::vector<Message>& msgs) override { sent_sizes.push_back(msgs.size()); }
};

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

std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

// 从已存快照里读出 messages（--show 用；顺带演示"从工作路径提取历史消息"）
std::vector<Message> load_history(state::FileCheckpointer& cp) {
  std::vector<Message> out;
  auto snap = cp.get_tuple(kThreadId, "", std::nullopt);
  if (!snap) return out;
  auto parsed = state::StateSchema::messages_state().apply(state::Json::object(), snap->values);
  if (parsed.contains("messages"))
    for (auto const& raw : parsed["messages"]) out.push_back(raw.get<Message>());
  return out;
}

void print_history(std::vector<Message> const& msgs) {
  for (Message const& m : msgs)
    std::println("    [{}] {}", std::string(message::role_str(m.type)), clip(m.text(), 160));
}

}  // namespace

int main(int argc, char** argv) {
  std::string workdir = ".";
  std::string prompt;
  bool reset = false, show = false;

  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == "--workdir" && i + 1 < argc) {
      workdir = argv[++i];
    } else if (a == "--reset") {
      reset = true;
    } else if (a == "--show") {
      show = true;
    } else if (a == "--help" || a == "-h") {
      std::println("用法: session_resume [--workdir DIR] [--reset] [--show] \"你的问题\"");
      return 0;
    } else {
      prompt = prompt.empty() ? a : prompt + " " + a;
    }
  }

  std::string const history_path = (std::filesystem::path(workdir) / kHistoryRel).string();

  if (reset) {
    std::error_code ec;
    std::filesystem::remove(history_path, ec);
    std::println("已清除历史：{}", history_path);
  }

  // 落盘 Checkpointer：必须有文件；生命周期覆盖整个 Agent
  state::FileCheckpointer cp(history_path);

  if (show) {  // 离线：只把已存的历史打印出来
    auto const hist = load_history(cp);
    std::println("工作路径: {}", std::filesystem::absolute(workdir).string());
    std::println("历史文件: {}（{} 字节）", history_path,
                 std::filesystem::exists(history_path)
                     ? std::filesystem::file_size(history_path)
                     : 0);
    std::println("已存会话消息: {} 条", hist.size());
    print_history(hist);
    return 0;
  }

  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）；只想看历史请用 --show");
    return 1;
  }

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  cfg.model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  cfg.ca_info = ca_bundle_from_env();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  tool::ToolRegistry const no_tools;

  // 开场先亮证据：这次调用从文件里恢复到了什么
  auto const restored = load_history(cp);
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());
  std::println("历史文件: {}", history_path);
  std::println("本次启动从文件恢复: {} 条消息", restored.size());
  if (!restored.empty()) {
    std::println("  （即：上一次进程留下的历史）");
    print_history(restored);
  }

  if (prompt.empty()) {
    std::println("\n（未给问题；历史已展示。加一个问题即可续跑）");
    return 0;
  }

  agent::AgentOptions opts;
  opts.system_prompt = "你是命令行助手。回答简短直接；记住用户此前说过的事。";
  opts.thread_id = kThreadId;
  opts.checkpointer = &cp;  // ← 唯一的区别：落盘而非内存

  agent::Agent agent(model, no_tools, opts);
  auto probe = std::make_shared<HistoryProbe>();
  agent.add_middleware(probe);

  std::println("\n本轮输入: {}", prompt);
  auto r = agent.invoke({message::human_message(prompt)});
  if (!r) {
    std::println("调用失败: {}", r.error().message);
    return 1;
  }

  std::println("本轮发往模型的 messages: {} 条（含上面恢复的历史）",
               probe->sent_sizes.empty() ? 0 : probe->sent_sizes.back());
  std::println("回答: {}", clip(r->text(), 400));
  std::println("\n历史已落盘: {}（本次落盘 {} 次，累计 {} 条消息）", history_path, cp.saves(),
               load_history(cp).size());
  return 0;
}
