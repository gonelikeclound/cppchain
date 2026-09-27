// tests/wf —— 生成端（workflow/generator），用 ScriptedChatModel 全离线驱动
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/store.hpp"
#include "cppchain/workflow/generator.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;
using cppchain::message::MessageChunk;
using cppchain::model::CallOptions;
using cppchain::model::ModelError;
using cppchain::model::Task;
using cppchain::message::UsageMetadata;

namespace {

// 按调用顺序吐罐头回复的模型 stub；invoke 时附加固定 usage。
class ScriptedChatModel final : public model::ChatModel {
 public:
  std::deque<std::string> replies;
  std::size_t invoked = 0;

  std::expected<Message, ModelError> invoke(std::span<Message const>,
                                            CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(ModelError{ModelError::Kind::Parse, 0, "no scripted reply", {}});
    Message m = cppchain::message::ai_message(replies.front());
    replies.pop_front();
    ++invoked;
    m.usage = UsageMetadata{10, 5, 15, 0};
    return m;
  }
  std::generator<MessageChunk> stream(std::span<Message const>, CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  Task<std::expected<Message, ModelError>> invoke_async(std::span<Message const>,
                                                        CallOptions) override {
    return {};  // 空任务（never awaited）
  }
};

const char* kPlanJson =
    R"({"plan":{"trigger":{"source":"time","schedule":"0 8 * * *"},
        "steps":[{"id":"fetch","action":"fetch_price","has_branch":true},
                 {"id":"notify","action":"send_notification"}],
        "assumptions":[]}})";

const char* kIrJson = R"({"id":"wf_t","trigger":{"source":"time",
 "timing":{"kind":"schedule","schedule":"0 8 * * *"}},
 "nodes":[
  {"id":"fetch","tool":"fetch_price","parameters":{"item_id":"@config.product_id"},
   "output_schema":{"price":"number","in_stock":"boolean"},"next":"check"},
  {"id":"check","expr":"$_.result.price != null && $_.result.price < 800",
   "then":"notify","else":"_turn_end"},
  {"id":"notify","tool":"send_notification","parameters":{"title":"降价了"},
   "output_schema":{"ok":"boolean"},"next":"_turn_end"}]})";

tool::ToolRegistry make_registry() {
  static FakeToolSinks sinks;
  return make_fake_tools(sinks);
}

}  // namespace

TEST(WfGenerator, plan_then_ir_generate) {
  ScriptedChatModel model;
  model.replies = {kPlanJson, kIrJson};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r = gen.run("每天 8 点查价格，低于 800 通知我");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Generated);
  ASSERT_TRUE(r.generated);
  EXPECT_EQ(r.generated->ir.id, "wf_t");
  EXPECT_EQ(r.generated->ir.trigger.schedule, "0 8 * * *");
  // 生成的 IR 必须过校验器（污防线：fail 上游也 fail 下游）
  EXPECT_TRUE(validate(r.generated->ir, ValidatorOptions{&reg}).ok());
  // 计量：各一次调用，总计 30 token
  EXPECT_EQ(gen.usage().plan_calls, 1);
  EXPECT_EQ(gen.usage().ir_calls, 1);
  EXPECT_EQ(gen.usage().plan_in + gen.usage().plan_out, 15);
  EXPECT_EQ(gen.usage().total_tokens(), 30);
}

TEST(WfGenerator, clarify_dispatch) {
  ScriptedChatModel model;
  model.replies = {R"({"questions":[{"id":"q1","text":"早上几点？"}]})"};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r = gen.run("早上提醒我看今天的新闻");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Clarify);
  ASSERT_TRUE(r.clarify);
  EXPECT_EQ(r.clarify->questions.size(), 1u);
  EXPECT_EQ(r.clarify->questions[0].id, "q1");
  EXPECT_FALSE(r.clarify->session_id.empty());
}

TEST(WfGenerator, clarify_options_roundtrip) {
  tool::ToolRegistry reg = make_registry();

  // 带 options 的澄清 → 读回；按选项文本作答 → 续跑至生成
  ScriptedChatModel model;
  model.replies = {R"({"questions":[{"id":"t","text":"几点？",
        "options":["早 8 点","晚 8 点","其他"]}]})",
                   kPlanJson, kIrJson};
  WorkflowGenerator gen(model, reg);
  auto r1 = gen.run("早上提醒我");
  ASSERT_EQ(r1.kind, WorkflowGenerationResult::Kind::Clarify);
  ASSERT_EQ(r1.clarify->questions.size(), 1u);
  ClarifyQuestion const& q = r1.clarify->questions[0];
  ASSERT_EQ(q.options.size(), 3u);
  EXPECT_EQ(q.options[0], "早 8 点");
  EXPECT_EQ(q.options[1], "晚 8 点");

  auto r2 = gen.submit_answers(r1.clarify->session_id, {{q.id, q.options[1]}});
  ASSERT_TRUE(r2.has_value());
  ASSERT_EQ(r2->kind, WorkflowGenerationResult::Kind::Generated);

  // 兼容旧回复：缺 options → 空向量（自由文本问答）
  ScriptedChatModel model2;
  model2.replies = {R"({"questions":[{"id":"q","text":"几点？"}]})"};
  WorkflowGenerator gen2(model2, reg);
  auto rc = gen2.run("x");
  ASSERT_EQ(rc.kind, WorkflowGenerationResult::Kind::Clarify);
  EXPECT_TRUE(rc.clarify->questions[0].options.empty());
}

TEST(WfGenerator, reject_dispatch) {
  ScriptedChatModel model;
  model.replies = {R"({"reject":{"reason":"被事件唤醒的触发语义，当前只有 time 触发"}})"};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r = gen.run("收到淘宝推送时提醒我比价");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Rejected);
  ASSERT_TRUE(r.rejected);
  EXPECT_NE(r.rejected->reason.find("time"), std::string::npos);
}

TEST(WfGenerator, clarify_session_then_answers) {
  ScriptedChatModel model;
  // 第一次：澄清；第二次（带答案）：plan；第三次：IR
  model.replies = {R"({"questions":[{"id":"t","text":"几点？"}]})", kPlanJson, kIrJson};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r1 = gen.run("早上提醒我");
  ASSERT_EQ(r1.kind, WorkflowGenerationResult::Kind::Clarify);
  std::string const sid = r1.clarify->session_id;

  // 错误答案组合 → 报错
  auto r_bad = gen.submit_answers(sid, {{"other", "8 点"}});
  ASSERT_FALSE(r_bad.has_value());
  EXPECT_NE(r_bad.error().find("不匹配"), std::string::npos);

  // 正确答案 → 续跑至生成
  auto r_good = gen.submit_answers(sid, {{"t", "8 点"}});
  ASSERT_TRUE(r_good.has_value());
  ASSERT_EQ(r_good->kind, WorkflowGenerationResult::Kind::Generated);
}

TEST(WfGenerator, plan_retries_exhausted) {
  ScriptedChatModel model;
  model.replies = {"not json", "not json", "not json", "not json"};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r = gen.run("任意意图");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Rejected);
  EXPECT_NE(r.rejected->reason.find("plan"), std::string::npos);
}

TEST(WfGenerator, ir_repair_once) {
  ScriptedChatModel model;
  // 首条 IR 带死节点（校验不过）→ 回喂整篇重写 → 第二条通过。
  // 注意 orphan 必须不是 nodes[0]（入口天然可达），否则就不是"死节点"。
  std::string const bad_ir = R"({"id":"wf_t","trigger":{"source":"time",
    "timing":{"kind":"schedule","schedule":"0 8 * * *"}},
    "nodes":[
      {"id":"start","tool":"echo","output_schema":{"value":"string"},"next":"_turn_end"},
      {"id":"orphan","tool":"echo","output_schema":{"value":"string"},"next":"_turn_end"}]})";
  model.replies = {kPlanJson, bad_ir, kIrJson};
  tool::ToolRegistry reg = make_registry();
  WorkflowGenerator gen(model, reg);

  auto r = gen.run("每天查价通知");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Generated) << gen.last_raw();
  EXPECT_EQ(gen.usage().ir_calls, 2);  // 修理了一轮
}

TEST(WfGenerator, budget_fail_closed) {
  ScriptedChatModel model;
  for (int i = 0; i < 10; ++i) model.replies.push_back("still not json");
  tool::ToolRegistry reg = make_registry();
  GeneratorOptions opts;
  opts.model_calls_budget = 2;  // 只允许两次调用
  opts.plan_max_retries = 9;
  WorkflowGenerator gen(model, reg, nullptr, opts);

  auto r = gen.run("任意意图");
  ASSERT_EQ(r.kind, WorkflowGenerationResult::Kind::Rejected);
  EXPECT_NE(r.rejected->reason.find("预算"), std::string::npos);
}