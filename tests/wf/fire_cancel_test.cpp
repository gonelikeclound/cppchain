// tests/wf —— problems/07 §9 P1：取消在 fire_once 层的行为（节点边界）。
// 验证：should_stop 第 N 次为 true → 后续节点不执行、返回 WorkflowError::Cancelled、
// 会话日志只含已完成节点（取消点之后没有 node_end）；不取消时三节点全跑。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cppchain/state/checkpointer.hpp"
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
            ("wf_cancel_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

Json three_echo_ir() {
  return Json{{"id", "wf_cancel"},
              {"nodes", Json::array({
                  Json{{"id", "e1"}, {"kind", "echo"}, {"text", "one"}, {"next", "e2"}},
                  Json{{"id", "e2"}, {"kind", "echo"}, {"text", "two"}, {"next", "e3"}},
                  Json{{"id", "e3"}, {"kind", "echo"}, {"text", "three"}, {"next", "_turn_end"}},
              })}};
}

struct Fixture {
  tool::ToolRegistry registry;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;

  std::expected<CompiledWorkflow, std::string> compile() {
    auto parsed = WorkflowIR::parse(three_echo_ir());
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
    return *wf;
  }
};

// 读会话日志里出现过的 node（enter 记录）
std::vector<std::string> nodes_in_log(std::filesystem::path const& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    Json const j = Json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    if (j.value("source", "") != "engine" || j.value("phase", "") != "enter") continue;
    std::string const n = j.value("node", "");
    if (!n.empty() && n != "__input__") out.push_back(n);
  }
  return out;
}

}  // namespace

TEST(WfFireCancel, stops_before_next_node_and_keeps_completed_log) {
  TempDir tmp;
  Fixture f;
  auto wf = f.compile();
  ASSERT_TRUE(wf) << wf.error();

  SessionLog log(tmp.dir / "session.log.jsonl");
  FireOptions fo;
  fo.log = &log;
  fo.thread_id = "t-cancel";
  int checks = 0;
  fo.should_stop = [&checks] { return ++checks > 1; };  // 第 2 次（e2 前）取消

  auto r = fire_once(*wf, Json::object(), fo);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, WorkflowError::Kind::Cancelled);
  EXPECT_EQ(checks, 2);

  auto const nodes = nodes_in_log(tmp.dir / "session.log.jsonl");
  ASSERT_FALSE(nodes.empty());
  EXPECT_EQ(nodes.front(), "e1");
  for (std::string const& n : nodes) EXPECT_NE(n, "e2") << "取消点之后不应再有节点";
  for (std::string const& n : nodes) EXPECT_NE(n, "e3");
}

TEST(WfFireCancel, no_cancel_runs_all_nodes) {
  TempDir tmp;
  Fixture f;
  auto wf = f.compile();
  ASSERT_TRUE(wf) << wf.error();

  SessionLog log(tmp.dir / "session.log.jsonl");
  FireOptions fo;
  fo.log = &log;
  fo.thread_id = "t-ok";
  fo.should_stop = [] { return false; };

  auto r = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r) << r.error().message;
  auto const nodes = nodes_in_log(tmp.dir / "session.log.jsonl");
  ASSERT_EQ(nodes.size(), 3u);
  EXPECT_EQ(nodes[0], "e1");
  EXPECT_EQ(nodes[1], "e2");
  EXPECT_EQ(nodes[2], "e3");
}
