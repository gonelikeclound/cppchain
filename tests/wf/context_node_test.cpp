// tests/wf —— context 节点（wfr-context，⑤：环境/skill 上下文注入）：
//   env 键值对 → 一条「【当前环境】key: value」system 消息写入 state[system_context]；
//   system[].from（文本引用）/ system[].file（读文件内容）→ 各自一条 system 消息；
//   model 节点装配请求时把它们统一拼在系统提示之后、历史之前，不进 messages/会话日志。
#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

struct TempDir {
  static std::atomic<unsigned> counter;
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_ctx_" + std::to_string(counter.fetch_add(1)));
  TempDir() { std::filesystem::create_directories(dir); }
  ~TempDir() { std::filesystem::remove_all(dir); }
};
std::atomic<unsigned> TempDir::counter{0};

// 记录每次 invoke 收到的消息（role + 文本），便于断言上下文的拼装顺序。
class RecordingModel final : public model::ChatModel {
 public:
  struct Seen {
    std::string role;
    std::string text;
  };
  std::vector<Seen> seen;
  std::deque<Message> replies;  // 预置回复（空则抛错）

  static std::string role_name(Message const& m) {
    using R = cppchain::message::Role;
    switch (m.type) {
      case R::System: return "system";
      case R::Human: return "human";
      case R::Ai: return "ai";
      case R::Tool: return "tool";
      case R::Chat: return m.custom_role.value_or("chat");
    }
    return "";
  }

  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    seen.clear();
    for (Message const& m : msgs) seen.push_back({role_name(m), m.text()});
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
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
  model::Task<std::expected<Message, model::ModelError>> invoke_async(
      std::span<Message const>, model::CallOptions) override {
    return {};
  }
};

// 编排：ctx(context) → think(model m) → route → _turn_end；模型回 text 即结束。
Json wf_ctx(Json env, Json sys) {
  Json ctx = Json{{"id", "c0"}, {"kind", "context"}, {"next", "think"}};
  if (!env.is_null()) ctx["env"] = std::move(env);
  if (!sys.is_null()) ctx["system"] = std::move(sys);
  return Json{{"id", "wf_ctx"},
              {"nodes", Json::array(
                            {std::move(ctx),
                             Json{{"id", "think"}, {"kind", "model"}, {"model", "m"},
                                  {"next", "route"}},
                             Json{{"id", "route"},
                                  {"kind", "route"},
                                  {"on", "last_ai_outcome"},
                                  {"cases", Json{{"text", "_turn_end"},
                                                 {"invalid_tool_calls", "_fail"}}},
                                  {"default", "_fail"}}})}};
}

struct Fixture {
  tool::ToolRegistry registry;
  RecordingModel model;
  ComponentRegistry components;
  Fixture() { components.models["m"] = &model; }
  std::expected<Json, WorkflowError> run(Json ir, Json const& config) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    CompileOptions opts;
    opts.components = &components;
    auto wf = compile_workflow(*parsed, registry, nullptr, opts);
    if (!wf) return std::unexpected(wf.error());
    return fire_once(*wf, config);
  }
};

}  // namespace

TEST(WfContext, malformed_system_entry_rejected) {
  Json ir = wf_ctx(
      Json::object(),
      Json::array({Json{{"bogus", 1}}}));  // system 条目必须是 {from|file}
  Fixture f;
  auto r = f.run(ir, Json::object());
  EXPECT_FALSE(r);
  EXPECT_TRUE(r.error().message.find("from（字符串）或 file（字符串）") != std::string::npos)
      << r.error().message;
}

TEST(WfContext, env_and_file_injected_as_system_messages) {
  TempDir tmp;
  std::filesystem::path const skill = tmp.dir / "skills" / "hmi-skill.md";
  std::filesystem::create_directories(skill.parent_path());
  std::ofstream(skill) << "# HMI Skill\n\n- 文件必须 <page> 置顶\n- meta 必须有 default 主题\n";

  Json sys = Json::array();
  sys.push_back(Json{{"file", "@config.skill_dir/hmi-skill.md"}});
  Json ir = wf_ctx(
      Json{{"project", "@config.env_project"}, {"page", "@config.env_page"}}, sys);
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("明白"));

  Json const config =
      Json{{"env_project", "liveproj"}, {"env_page", "pages/home.hmi.xml"},
           {"skill_dir", (tmp.dir / "skills").string()}};
  auto st = f.run(ir, config);
  ASSERT_TRUE(st) << st.error().message;

  // ① context 把环境块 + skill 各写成一条 system 消息 → state[system_context]
  ASSERT_TRUE((*st).contains("system_context"));
  auto const& ctx_arr = (*st)["system_context"];
  ASSERT_TRUE(ctx_arr.is_array() && ctx_arr.size() >= 2);
  std::string const env_text = ctx_arr[0]["content"][0].value("text", "");
  EXPECT_NE(env_text.find("【当前环境】"), std::string::npos);
  EXPECT_NE(env_text.find("liveproj"), std::string::npos);
  EXPECT_NE(env_text.find("pages/home.hmi.xml"), std::string::npos);
  std::string const skill_text = ctx_arr[1]["content"][0].value("text", "");
  EXPECT_NE(skill_text.find("skill: hmi-skill.md"), std::string::npos);
  EXPECT_NE(skill_text.find("<page> 置顶"), std::string::npos);

  // ② model 收到的消息顺序：系统提示(空 → 无) → context system 消息（env、skill 各一）
  ASSERT_FALSE(f.model.seen.empty());
  ASSERT_GE(f.model.seen.size(), 2u);
  EXPECT_EQ(f.model.seen[0].role, "system");
  EXPECT_EQ(f.model.seen[1].role, "system");
  EXPECT_NE(f.model.seen[0].text.find("【当前环境】"), std::string::npos);
  EXPECT_NE(f.model.seen[1].text.find("<page> 置顶"), std::string::npos);

  // ③ 上下文不进 messages 通道（会话日志/replay 不会带上注入内容）：messages 里只有
  //    模型的最终回复（ai），没有任何 system 消息。
  auto const& msgs = (*st)["messages"];
  ASSERT_TRUE(msgs.is_array());
  for (Json const& m : msgs)
    EXPECT_NE(m.value("type", ""), "system") << "context 注入不应进入 messages 通道";
}

TEST(WfContext, missing_skill_file_warns_and_fires) {
  Json ir = wf_ctx(Json::object(),
                   Json::array({Json{{"file", "@config.skill_dir/不存在.md"}}}));
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("没事"));
  auto st = f.run(ir, Json{{"skill_dir", "/tmp/definitely_missing_dir_ctx_test"}});
  ASSERT_TRUE(st) << st.error().message;  // 缺文件只是少注入，不影响流程
  EXPECT_FALSE((*st).contains("system_context"));
}