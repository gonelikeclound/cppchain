// M7+ 测试：日志持久化后端（FileJournal）
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cppchain/agent/journal.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::ToolCall;

namespace {

// 每个用例一个独立临时文件，析构时清理
struct TempFile {
  std::filesystem::path path;

  explicit TempFile(std::string const& tag)
      : path(std::filesystem::temp_directory_path() /
             ("cppchain-journal-" + tag + "-" + std::to_string(::getpid()) + ".json")) {
    std::filesystem::remove(path);
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".tmp", ec);
  }
  [[nodiscard]] std::string str() const { return path.string(); }
};

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

http::FakeTransport::Script tool_script(std::string const& name, std::string const& args,
                                        std::string const& id) {
  http::FakeTransport::Script s;
  nlohmann::json delta = {
      {"tool_calls", nlohmann::json::array({{{"index", 0},
                                             {"id", id},
                                             {"function", {{"name", name}, {"arguments", args}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

JournalEntry entry_of(std::string key, JournalEntry::State st) {
  JournalEntry e;
  e.key = std::move(key);
  e.tool_name = "charge";
  e.state = st;
  return e;
}

}  // namespace

TEST(FileJournal, persists_across_instances) {
  TempFile f("durable");
  {
    FileJournal journal(f.str());
    journal.record_intent(entry_of("k1", JournalEntry::State::Intent));
    journal.record_result("k1", "charged", "success");
  }  // 析构 = "进程结束"

  FileJournal reopened(f.str());  // 新实例 = "重启"
  auto const found = reopened.find("k1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->state, JournalEntry::State::Completed);
  EXPECT_EQ(found->content, "charged");
}

TEST(FileJournal, intent_is_persisted_immediately) {
  // 关键：意图必须先落盘，否则"执行中崩溃"这条路径在重启后无法识别
  TempFile f("intent");
  {
    FileJournal journal(f.str());
    journal.record_intent(entry_of("k2", JournalEntry::State::Intent));
  }

  FileJournal reopened(f.str());
  auto const found = reopened.find("k2");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->state, JournalEntry::State::Intent);  // 未被误判为"已完成"
}

TEST(FileJournal, writes_are_valid_json_and_leave_no_temp_file) {
  TempFile f("atomic");
  FileJournal journal(f.str());
  for (int i = 0; i < 5; ++i) {
    journal.record_intent(entry_of("k" + std::to_string(i), JournalEntry::State::Intent));
    journal.record_result("k" + std::to_string(i), "ok", "success");
  }
  EXPECT_EQ(journal.size(), 5u);
  EXPECT_EQ(journal.saves(), 10u);  // 每次意图/结果各落盘一次

  std::ifstream in(f.str());
  nlohmann::json j;
  ASSERT_NO_THROW(in >> j);
  EXPECT_EQ(j.at("entries").size(), 5u);
  EXPECT_FALSE(std::filesystem::exists(f.str() + ".tmp"));  // 临时文件已 rename
}

TEST(FileJournal, corrupt_file_fails_closed_by_default) {
  TempFile f("corrupt");
  {
    std::ofstream out(f.str());
    out << "{ this is not json";
  }
  // 安全机制不能"静默从空开始"——那等于悄悄丢掉防重放能力
  EXPECT_THROW(FileJournal{f.str()}, std::runtime_error);
}

TEST(FileJournal, corrupt_file_can_opt_into_start_empty) {
  TempFile f("corrupt-ok");
  {
    std::ofstream out(f.str());
    out << "garbage";
  }
  FileJournal journal(f.str(), FileJournal::OnCorrupt::StartEmpty);
  EXPECT_EQ(journal.size(), 0u);
}

TEST(FileJournal, missing_file_is_a_normal_first_run) {
  TempFile f("missing");
  FileJournal journal(f.str());
  EXPECT_EQ(journal.size(), 0u);
  EXPECT_FALSE(std::filesystem::exists(f.str()));  // 只读不写
}

TEST(SideEffectGuard, key_prefix_isolates_tenants) {
  TempFile f("prefix");
  FileJournal journal(f.str());

  SideEffectGuard::Options a;
  a.key_prefix = "thread-A:";
  SideEffectGuard::Options b;
  b.key_prefix = "thread-B:";
  SideEffectGuard ga(journal, a);
  SideEffectGuard gb(journal, b);

  ToolCall const call{"call_shared", "charge", nlohmann::json{{"amount", 10}}};
  tool::ToolSpec spec = tool::make_tool("charge", "扣款", nlohmann::json::object(),
                                        [](nlohmann::json const&, tool::ToolContext&) {
                                          return std::expected<nlohmann::json, tool::ToolError>{
                                              nlohmann::json("ok")};
                                        },
                                        /*read_only=*/false);

  EXPECT_FALSE(ga.lookup_tool_result(call, &spec).has_value());  // A 首次 → 执行
  EXPECT_FALSE(gb.lookup_tool_result(call, &spec).has_value());  // B 首次 → 也执行（按前缀隔离）
  EXPECT_EQ(journal.size(), 2u);

  // 同前缀再来一次 → 命中日志（前提是记过结果）
  Message done = message::tool_message("charged", "call_shared");
  done.status = "success";
  ga.after_tool(done);
  EXPECT_TRUE(ga.lookup_tool_result(call, &spec).has_value());  // 这次被拦截/重放
}

TEST(FileJournal, agent_does_not_repeat_side_effect_after_restart) {
  TempFile f("e2e");
  auto transport = std::make_shared<http::FakeTransport>();
  model::ModelConfig cfg;
  cfg.api_key = "k";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "m";
  model::openai::OpenAIChatModel model(cfg, transport);

  int charges = 0;
  tool::ToolRegistry tools;
  tools.add(tool::make_tool("charge", "扣款", nlohmann::json::object(),
                            [&charges](nlohmann::json const&,
                                       tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                              ++charges;
                              return nlohmann::json("charged");
                            },
                            /*read_only=*/false));

  auto run = [&](ToolExecutionJournal& journal) {
    auto guard = std::make_shared<SideEffectGuard>(journal);
    Agent agent(model, tools, AgentOptions{});
    agent.add_middleware(guard);
    transport->push(tool_script("charge", "{}", "call_p"));
    transport->push(text_script("已处理"));
    auto r = agent.invoke({message::human_message("扣款")});
    EXPECT_TRUE(r.has_value());
    return guard->replays();
  };

  std::size_t replays = 0;
  {
    FileJournal journal{f.str()};
    (void)run(journal);        // 第一次：真实执行
    replays = run(journal);    // 同一进程内第二次：重放
  }
  EXPECT_EQ(charges, 1);
  EXPECT_EQ(replays, 1u);

  // "重启"：新 FileJournal 实例（从磁盘恢复）+ 新 Agent
  FileJournal journal2{f.str()};
  auto guard2 = std::make_shared<SideEffectGuard>(journal2);
  Agent agent2(model, tools, AgentOptions{});
  agent2.add_middleware(guard2);
  transport->push(tool_script("charge", "{}", "call_p"));
  transport->push(text_script("已处理"));
  ASSERT_TRUE(agent2.invoke({message::human_message("扣款")}).has_value());

  EXPECT_EQ(charges, 1);              // ← 重启后依然没有重复扣款
  EXPECT_EQ(guard2->replays(), 1u);
}