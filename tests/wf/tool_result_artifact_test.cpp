// tests/wf —— problems/02 追加 S3：工具结果**执行时**自动句柄化。
// @config.tool_result_artifact={enabled,max_inline_tokens} 开启后，run_tools 把超预算的
// 工具结果全文落盘（缺省 <workspace>/.wfr/artifacts/tool），inline 换成「头部 + 引用行」，
// 并设 message.artifact；agent 可用 file_read/grep 回读。默认关 → 内容原样（零回归）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/file_tools.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

using Json = nlohmann::json;

namespace {

class ScriptModel final : public model::ChatModel {
 public:
  std::deque<message::Message> replies;
  std::expected<message::Message, model::ModelError> invoke(
      std::span<message::Message const>, model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
    message::Message m = replies.front();
    replies.pop_front();
    return m;
  }
  std::generator<message::MessageChunk> stream(std::span<message::Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<message::Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<message::Message, model::ModelError>> invoke_async(
      std::span<message::Message const>, model::CallOptions) override {
    return {};
  }
};

message::Message ai_call(std::vector<message::ToolCall> tcs, std::string const& id = "ai1") {
  message::Message m = message::ai_message("");
  for (message::ToolCall& tc : tcs) m.tool_calls.push_back(std::move(tc));
  m.id = id;
  return m;
}

// 最小循环：think → route → act(run_tools)；文本 → 退出。
Json loop_ir() {
  return Json::parse(R"json({
    "id": "wf_s3",
    "nodes": [
      {"id": "loop", "kind": "loop", "max": 8, "next": "_turn_end",
       "body": [
         {"id": "think", "kind": "model", "model": "m", "tools": "ts", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"tool_calls": "act", "text": "_break"}, "default": "_break"},
         {"id": "act", "kind": "run_tools", "next": "_continue"}
       ]}
    ]
  })json");
}

struct Workspace {
  std::filesystem::path dir;
  Workspace()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_s3_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~Workspace() { std::filesystem::remove_all(dir); }
};

tool::FileToolsConfig ft_config(std::filesystem::path ws) {
  tool::FileToolsConfig c;
  c.root_dir = std::move(ws);
  return c;
}

bool write_text(std::filesystem::path const& p, std::string const& s) {
  std::ofstream out(p, std::ios::binary);
  out << s;
  return static_cast<bool>(out);
}

std::string read_text(std::filesystem::path const& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
}

struct Fixture {
  Workspace ws;
  tool::ToolRegistry registry;
  ScriptModel model;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;

  Fixture() {
    components.models["m"] = &model;
    components.toolsets["ts"] = {"file_read"};
    registry.add(tool::make_file_read_tool(ft_config(ws.dir)));
  }

  std::expected<Json, WorkflowError> run(Json state_in) {
    auto parsed = WorkflowIR::parse(loop_ir());
    if (!parsed)
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    CompileOptions c;
    c.components = &components;
    auto wf = compile_workflow(*parsed, registry, &cp, c);
    if (!wf) return std::unexpected(wf.error());
    state_in["workspace"] = ws.dir.string();
    FireOptions fo;
    fo.thread_id = "t1";
    return fire_once(*wf, state_in, fo);
  }
};

// 取指定 tool_call_id 的 ToolMessage JSON（state.messages 里的对象）
Json const* find_tool_msg(Json const& st, std::string const& id) {
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return nullptr;
  for (auto const& m : *it)
    if (m.value("type", "") == "tool" && m.value("tool_call_id", "") == id) return &m;
  return nullptr;
}

std::string message_text(Json const& m) {
  std::string out;
  if (m.contains("content") && m["content"].is_array())
    for (Json const& blk : m["content"])
      if (blk.is_object() && blk.contains("text")) out += blk["text"].get<std::string>();
  return out;
}

}  // namespace

// 1) 开启 + 大结果 → 落盘、引用行、message.artifact；落盘文件是全文；file_read 可回读
TEST(WfToolResultArtifact, enabled_spills_large_result_and_redeems) {
  Fixture f;
  std::string const big = [] {
    std::string s;
    for (int i = 0; i < 300; ++i) s += "0123456789";  // 3000 字节，远超 10 token 预算
    return s;
  }();
  ASSERT_TRUE(write_text(f.ws.dir / "big.txt", big));

  f.model.replies.push_back(ai_call({message::ToolCall{"c1", "file_read",
                                                       Json{{"path", "big.txt"}}}}));
  f.model.replies.push_back(message::ai_message("done"));

  auto st = f.run(Json{{"tool_result_artifact",
                        Json{{"enabled", true}, {"max_inline_tokens", 10}}}});
  ASSERT_TRUE(st) << st.error().message;

  Json const* tm = find_tool_msg(*st, "c1");
  ASSERT_NE(tm, nullptr);
  std::string const inline_text = message_text(*tm);
  EXPECT_NE(inline_text.find("完整结果已落盘"), std::string::npos);
  EXPECT_EQ(inline_text.find(big), std::string::npos);  // 全文不再内联

  ASSERT_TRUE(tm->contains("artifact"));
  ASSERT_FALSE((*tm)["artifact"].is_null());
  std::string const shown = (*tm)["artifact"].get<std::string>();
  EXPECT_EQ(shown.rfind("/", 0), std::string::npos);      // 沙箱相对路径
  EXPECT_NE(shown.find(".wfr/artifacts/tool/artifact_"), std::string::npos);

  // 落盘文件 = 全文（工具结果文本含 JSON 外壳，原始内容作为子串在其中）
  std::string const spilled = read_text(f.ws.dir / shown);
  EXPECT_NE(spilled.find(big), std::string::npos);

  // 回读通道：把 artifact 相对路径直接喂给 file_read（沙箱根内）
  auto spec = tool::make_file_read_tool(ft_config(f.ws.dir));
  tool::ToolContext ctx;
  auto rr = spec.handler(Json{{"path", shown}, {"max_chars", 200}}, ctx);
  ASSERT_TRUE(rr.has_value()) << rr.error().message;
  EXPECT_NE(rr->at("content").get<std::string>().find(big.substr(0, 100)), std::string::npos);
}

// 2) 默认关（未配置）→ 内容原样、无 artifact（旧路径零回归）
TEST(WfToolResultArtifact, disabled_default_passthrough) {
  Fixture f;
  std::string const big = std::string(3000, 'x');
  ASSERT_TRUE(write_text(f.ws.dir / "big.txt", big));

  f.model.replies.push_back(ai_call({message::ToolCall{"c1", "file_read",
                                                       Json{{"path", "big.txt"}}}}));
  f.model.replies.push_back(message::ai_message("done"));

  auto st = f.run(Json::object());
  ASSERT_TRUE(st) << st.error().message;

  Json const* tm = find_tool_msg(*st, "c1");
  ASSERT_NE(tm, nullptr);
  std::string const inline_text = message_text(*tm);
  EXPECT_EQ(inline_text.find("完整结果已落盘"), std::string::npos);
  EXPECT_NE(inline_text.find(big.substr(0, 200)), std::string::npos);  // 全文内联
  ASSERT_TRUE(tm->contains("artifact"));
  EXPECT_TRUE((*tm)["artifact"].is_null());
}

// 2b) wfr --set 形态：平铺字符串键（tool_result_artifact=on + _budget=10）同样生效
TEST(WfToolResultArtifact, flat_config_keys_enable) {
  Fixture f;
  ASSERT_TRUE(write_text(f.ws.dir / "big.txt", std::string(3000, 'y')));

  f.model.replies.push_back(ai_call({message::ToolCall{"c1", "file_read",
                                                       Json{{"path", "big.txt"}}}}));
  f.model.replies.push_back(message::ai_message("done"));

  auto st = f.run(Json{{"tool_result_artifact", "on"},
                       {"tool_result_artifact_budget", "10"}});
  ASSERT_TRUE(st) << st.error().message;

  Json const* tm = find_tool_msg(*st, "c1");
  ASSERT_NE(tm, nullptr);
  EXPECT_NE(message_text(*tm).find("完整结果已落盘"), std::string::npos);
  ASSERT_TRUE(tm->contains("artifact"));
  ASSERT_FALSE((*tm)["artifact"].is_null());
  EXPECT_NE((*tm)["artifact"].get<std::string>().find(".wfr/artifacts/tool/artifact_"),
            std::string::npos);
}

// 3) 开启但结果在预算内 → 不落盘（短结果不制造文件）
TEST(WfToolResultArtifact, small_result_not_spilled) {
  Fixture f;
  ASSERT_TRUE(write_text(f.ws.dir / "small.txt", "hello"));

  f.model.replies.push_back(ai_call({message::ToolCall{"c1", "file_read",
                                                       Json{{"path", "small.txt"}}}}));
  f.model.replies.push_back(message::ai_message("done"));

  auto st = f.run(Json{{"tool_result_artifact",
                        Json{{"enabled", true}, {"max_inline_tokens", 1000}}}});
  ASSERT_TRUE(st) << st.error().message;

  Json const* tm = find_tool_msg(*st, "c1");
  ASSERT_NE(tm, nullptr);
  EXPECT_EQ(message_text(*tm).find("完整结果已落盘"), std::string::npos);
  ASSERT_TRUE(tm->contains("artifact"));
  EXPECT_TRUE((*tm)["artifact"].is_null());
  EXPECT_FALSE(std::filesystem::exists(f.ws.dir / ".wfr" / "artifacts" / "tool"));
}
