// tests/wf —— problems/07 §16：上一轮未完成（prev_round）内置到 env 层。
// 覆盖：日志尾扫描 helper / context 节点自动渲染 prev_round / file_context 跳过预取。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/serializer.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/file_tools.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

using Json = nlohmann::json;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_state_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

// 写一段会话日志：正常 run（可选）+ 取消 run（可选）
void seed_log(std::filesystem::path const& ws, std::string const& session, bool cancelled,
              std::string const& human = "") {
  SessionLog log(session_path(ws.string(), session));
  if (!human.empty()) {
    log.engine_enter("r-0", "__input__", "input");
    log.engine_exit("r-0", "__input__", "input", true, "ok", 0,
                    Json{{"messages", Json::array({Json(message::human_message(human))})}});
  }
  log.engine_enter("r-1", "think", "model");
  log.engine_exit("r-1", "think", "model", true, "ok", 0, Json::object());
  if (cancelled) {
    log.engine_enter("r-cancel-test", "__cancelled__", "cancel");
    log.engine_exit("r-cancel-test", "__cancelled__", "cancel", false, "cancelled", 0,
                    std::nullopt);
  }
}

Json context_ir() {
  return Json{{"id", "wf_ctx"},
              {"nodes", Json::array({Json{{"id", "env"}, {"kind", "context"},
                                          {"next", "_turn_end"}}})}};
}

Json files_ir() {
  return Json{{"id", "wf_files"},
              {"nodes", Json::array({
                  Json{{"id", "recall"}, {"kind", "load_history"},
                       {"path", "@config.workspace"}, {"session", "@config.session"},
                       {"next", "files"}},
                  Json{{"id", "files"}, {"kind", "file_context"}, {"next", "_turn_end"}},
              })}};
}

struct Fixture {
  tool::ToolRegistry registry;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;
  explicit Fixture(std::filesystem::path root) {
    for (auto& t : tool::make_file_tools(tool::FileToolsConfig{root})) registry.add(std::move(t));
  }

  std::expected<Json, std::string> run(Json ir, Json state_in) {
    auto parsed = WorkflowIR::parse(std::move(ir));
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vo;
    vo.registry = &registry;
    vo.components = &components;
    auto const v = validate(*parsed, vo);
    if (!v.ok()) return std::unexpected(v.describe());
    CompileOptions co;
    co.components = &components;
    auto wf = compile_workflow(*parsed, registry, &cp, co);
    if (!wf) return std::unexpected(wf.error().message);
    FireOptions fo;
    fo.thread_id = "t";
    auto r = fire_once(*wf, state_in, fo);
    if (!r) return std::unexpected(r.error().message);
    return *r;
  }
};

std::string sys_text(Json const& st) {
  auto const it = st.find("system_context");
  if (it == st.end() || !it->is_array()) return "";
  std::string out;
  for (Json const& m : *it)
    if (m.contains("content") && m["content"].is_array())
      for (Json const& b : m["content"])
        if (b.is_object() && b.contains("text")) out += b["text"].get<std::string>() + "\n";
  return out;
}

}  // namespace

TEST(WfSessionState, last_run_cancelled_detects_tail_terminal) {
  TempDir tmp;
  EXPECT_FALSE(session_last_run_cancelled(tmp.dir.string(), "s"));  // 无日志
  seed_log(tmp.dir, "s", false);
  EXPECT_FALSE(session_last_run_cancelled(tmp.dir.string(), "s"));
  seed_log(tmp.dir, "s2", true);
  EXPECT_TRUE(session_last_run_cancelled(tmp.dir.string(), "s2"));
}

TEST(WfSessionState, context_env_injects_prev_round_builtin) {
  TempDir tmp;
  seed_log(tmp.dir, "s", true);
  Fixture f(tmp.dir);
  auto const st = f.run(context_ir(), Json{{"workspace", tmp.dir.string()}, {"session", "s"}});
  ASSERT_TRUE(st) << st.error();
  std::string const text = sys_text(*st);
  EXPECT_NE(text.find("【当前环境】"), std::string::npos) << text;
  EXPECT_NE(text.find("prev_round"), std::string::npos) << text;
  EXPECT_NE(text.find("未完成（被新消息打断）"), std::string::npos) << text;
}

TEST(WfSessionState, context_env_omits_prev_round_when_normal) {
  TempDir tmp;
  seed_log(tmp.dir, "s", false);
  Fixture f(tmp.dir);
  auto const st = f.run(context_ir(), Json{{"workspace", tmp.dir.string()}, {"session", "s"}});
  ASSERT_TRUE(st) << st.error();
  EXPECT_EQ(sys_text(*st).find("prev_round"), std::string::npos);
}

TEST(WfSessionState, file_context_skips_prefetch_after_interrupted_round) {
  TempDir tmp;
  seed_log(tmp.dir, "s", true, "那明天呢？");  // 上一轮被取消，最后一条 Human 是追问碎片
  Fixture f(tmp.dir);
  auto const st = f.run(files_ir(), Json{{"workspace", tmp.dir.string()},
                                         {"session", "s"},
                                         {"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error();
  Json const& frame = st->at("results").at("files");
  EXPECT_EQ(frame.value("action", ""), "skipped");
  EXPECT_NE(frame.value("reason", "").find("上一轮未完成"), std::string::npos);
  EXPECT_FALSE(st->contains("system_context"));  // 没有预取片段
}

TEST(WfSessionState, file_context_runs_normally_without_interruption) {
  TempDir tmp;
  seed_log(tmp.dir, "s", false, "config.json 的 /a/b 是多少？");
  std::ofstream(tmp.dir / "config.json") << R"({"a": {"b": 42}})";
  Fixture f(tmp.dir);
  auto const st = f.run(files_ir(), Json{{"workspace", tmp.dir.string()},
                                         {"session", "s"},
                                         {"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error();
  EXPECT_EQ(st->at("results").at("files").value("action", ""), "route");
}
