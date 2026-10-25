// tests/wf —— 项目级完成标准自动判定（arch 26 第二刀）
//   覆盖：verify review(project=<p>) 项目级终审 → 项目简报喂独立 Reviewer（只读事实）→
//   verdict 写 state.project_review → project end 收口 project.json.completion
//   （done/fail）、criteria 空/“null”用 completion_criteria 兜底、缺模型 fail-closed、
//   无项目评审时不写 completion（与第一刀回归一致）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

// 确定性裁判模型：按脚本队列吐 {pass,feedback}，并把每次最后 human 文本记下来（简报可见性审计）。
class ScriptedReview final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::vector<std::string> last_humans;
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    std::string h;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Human) {
        h = it->text();
        break;
      }
    last_humans.push_back(h);
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
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
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

struct ReviewFixture {
  std::filesystem::path ws;
  std::filesystem::path proj_dir;
  ReviewFixture()
      : ws(std::filesystem::temp_directory_path() /
           ("wf_proj_rev_" + std::to_string(static_cast<unsigned>(std::random_device{}())))),
        proj_dir(ws / ".wfr" / "projects" / "monthly_ops") {
    std::filesystem::create_directories(proj_dir / "tasks");
    write("project.json",
          R"({"id":"monthly_ops","goal":"产出本月运营综述",
              "completion_criteria":"周报与月报产物落盘、数字前后一致，StrictDoD",
              "discipline":["数字一致性"],
              "tasks":[{"id":"weekly","name":"周报"},{"id":"monthly","name":"月报"}]})");
    write("lessons.json", "[]");
  }
  ~ReviewFixture() {
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
  }
  void write(std::string const& rel, std::string const& text) const {
    std::ofstream out(proj_dir / rel);
    out << text;
  }
};

// 项目 workflow：project begin → task begin → task end → 项目级终审 review → project end
Json review_ir() {
  return Json::parse(R"json({
    "id": "wf_proj_review",
    "nodes": [
      {"id": "p_begin", "kind": "project", "project": "monthly_ops", "phase": "begin", "next": "t_begin"},
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "t_end"},
      {"id": "t_end",   "kind": "task", "task": "@config.task", "phase": "end",   "next": "p_review"},
      {"id": "p_review", "kind": "verify", "phase": "review", "project": "monthly_ops",
       "model": "m", "role": "reviewer", "criteria": "@config.project_criteria", "next": "p_end"},
      {"id": "p_end",   "kind": "project", "project": "monthly_ops", "phase": "end", "next": "_turn_end"}
    ]
  })json");
}

struct Run {
  std::optional<Json> state;
  std::string error;
};

Run fire(Json ir, Json config, ScriptedReview& mdl,
         std::vector<std::string> const& replies) {
  for (std::string const& r : replies) mdl.replies.push_back(message::ai_message(r));
  auto parsed = WorkflowIR::parse(ir);
  if (!parsed) return {std::nullopt, "parse: " + parsed.error()};
  auto v = validate(*parsed, ValidatorOptions{});
  if (!v.ok()) return {std::nullopt, "validate: " + v.describe()};
  tool::ToolRegistry reg;
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto wf = compile_workflow(*parsed, reg, nullptr, copts);
  if (!wf) return {std::nullopt, "compile: " + wf.error().message};
  auto r = fire_once(*wf, config);
  if (!r) return {std::nullopt, "fire: " + r.error().message};
  return {std::optional<Json>{*r}, ""};
}

Json read_file(std::filesystem::path const& p) {
  std::ifstream in(p);
  Json j;
  in >> j;
  return j;
}

}  // namespace

// pass → project.json.completion.verdict=done + why 带 Reviewer feedback；简报喂给 Reviewer
TEST(WfProjectReview, pass_writes_completion_done) {
  ReviewFixture fx;
  ScriptedReview mdl;
  auto r = fire(review_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"},
                     {"task", "weekly"}, {"goal", "周报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写周报"}] )"},
                     {"project_criteria", "自定义验收标准 ABC"}},
                mdl, {R"({"pass":true,"feedback":"产物齐全、数字一致"})"});
  ASSERT_TRUE(r.state) << r.error;
  // state 槽
  EXPECT_EQ((*r.state)["project_review"]["verdict"], "done");
  EXPECT_EQ((*r.state)["results"]["p_end"]["project_review"], "done");
  // project.json.completion 落盘
  Json const po = read_file(fx.proj_dir / "project.json");
  ASSERT_EQ(po["completion"]["verdict"], "done");
  EXPECT_NE(po["completion"]["why"].get<std::string>().find("产物齐全"), std::string::npos);
  EXPECT_FALSE(po["completion"]["when"].get<std::string>().empty());
  // 简报确实喂给了 Reviewer：目标/验收标准(自定义)/任务进展
  ASSERT_FALSE(mdl.last_humans.empty());
  std::string const brief = mdl.last_humans.back();
  EXPECT_NE(brief.find("本月运营综述"), std::string::npos) << brief;
  EXPECT_NE(brief.find("自定义验收标准 ABC"), std::string::npos) << brief;  // 判据用 review 节点传的
  EXPECT_NE(brief.find("weekly"), std::string::npos) << brief;
}

// fail → completion.verdict=fail；progress[task] 仍是任务级状态（两层不混）
TEST(WfProjectReview, fail_writes_completion_fail) {
  ReviewFixture fx;
  ScriptedReview mdl;
  auto r = fire(review_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"},
                     {"task", "weekly"}, {"goal", "周报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写周报"}] )"},
                     {"project_criteria", "产物必须落盘"}},
                mdl, {R"({"pass":false,"feedback":"周报产物没找到"})"});
  ASSERT_TRUE(r.state) << r.error;
  Json const po = read_file(fx.proj_dir / "project.json");
  EXPECT_EQ(po["completion"]["verdict"], "fail");
  EXPECT_NE(po["completion"]["why"].get<std::string>().find("周报产物没找到"), std::string::npos);
  EXPECT_EQ(po["progress"]["weekly"], "in_progress");  // 任务级照旧
}

// 判据缺省（@config.project_criteria 缺失/“null”）→ 用 project.json.completion_criteria 兜底
TEST(WfProjectReview, criteria_falls_back_to_completion_criteria) {
  ReviewFixture fx;
  ScriptedReview mdl;
  auto r = fire(review_ir(),
                Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"},
                     {"task", "weekly"}, {"goal", "周报"},
                     {"plan_steps", R"([{"id":"s1","goal":"写周报"}] )"}},
                mdl, {R"({"pass":true,"feedback":"ok"})"});
  ASSERT_TRUE(r.state) << r.error;
  ASSERT_FALSE(mdl.last_humans.empty());
  EXPECT_NE(mdl.last_humans.back().find("StrictDoD"), std::string::npos) << mdl.last_humans.back();
}

// 缺裁判模型 → fail-closed（verdict=fail，why 提示未配置），不伪造 pass
TEST(WfProjectReview, missing_model_fails_closed) {
  ReviewFixture fx;
  // 不给 components（无 m 组件）→ judge==nullptr
  auto parsed = WorkflowIR::parse(review_ir());
  ASSERT_TRUE(parsed);
  tool::ToolRegistry reg;
  ComponentRegistry comps;  // 空：models 无 "m"
  CompileOptions copts;
  copts.components = &comps;
  auto wf = compile_workflow(*parsed, reg, nullptr, copts);
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"},
                               {"task", "weekly"}, {"goal", "周报"},
                               {"plan_steps", R"([{"id":"s1","goal":"x"}] )"},
                               {"project_criteria", "C"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ((*r)["project_review"]["verdict"], "fail");
  EXPECT_NE((*r)["project_review"]["why"].get<std::string>().find("未配置"), std::string::npos);
  Json const po = read_file(fx.proj_dir / "project.json");
  EXPECT_EQ(po["completion"]["verdict"], "fail");
}

// 无项目评审节点 → completion 不写（第一刀回归：progress 照常）
TEST(WfProjectReview, no_review_no_completion) {
  ReviewFixture fx;
  // 与 project_test 的 chain 一致：project begin→task begin→task end→project end，无 verify
  auto chain = Json::parse(R"json({
    "id": "wf_proj_chain",
    "nodes": [
      {"id": "p_begin", "kind": "project", "project": "monthly_ops", "phase": "begin", "next": "t_begin"},
      {"id": "t_begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "t_end"},
      {"id": "t_end",   "kind": "task", "task": "@config.task", "phase": "end",   "next": "p_end"},
      {"id": "p_end",   "kind": "project", "project": "monthly_ops", "phase": "end", "next": "_turn_end"}
    ]
  })json");
  auto parsed = WorkflowIR::parse(chain);
  ASSERT_TRUE(parsed);
  tool::ToolRegistry reg;
  ComponentRegistry comps;
  auto wf = compile_workflow(*parsed, reg, nullptr, CompileOptions{});
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, Json{{"workspace", fx.ws.string()}, {"project", "monthly_ops"},
                               {"task", "weekly"}, {"goal", "周报"},
                               {"plan_steps", R"([{"id":"s1","goal":"x"}] )"}});
  ASSERT_TRUE(r) << r.error().message;
  Json const po = read_file(fx.proj_dir / "project.json");
  EXPECT_FALSE(po.contains("completion"));            // 没评审就不写 completion
  EXPECT_EQ((*r)["results"]["p_end"]["status"], "in_progress");
}