// tests/wf —— echo 回显节点（wfr-㉗）：
//   缺失 next 拒 / 字面量 fire 并落节点诊断 / $节点.result 引用过校验（VR2 放开 RunTools）
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/tool/registry.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

struct TempDir {
  static std::atomic<unsigned> counter;
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_echo_" + std::to_string(counter.fetch_add(1)));
  TempDir() { std::filesystem::create_directories(dir); }
  ~TempDir() { std::filesystem::remove_all(dir); }
  std::string file() const { return (dir / "session.log.jsonl").string(); }
};
std::atomic<unsigned> TempDir::counter{0};

Json echo_ir(Json nodes) { return Json{{"id", "wf_echo"}, {"nodes", std::move(nodes)}}; }

}  // namespace

TEST(WfEcho, missing_next_rejected) {
  Json ir_json = echo_ir(Json::array({Json{{"id", "e0"}, {"kind", "echo"}, {"text", "hi"}}}));
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir);

  tool::ToolRegistry reg;
  auto wf = compile_workflow(*ir, reg);
  ASSERT_FALSE(wf);
  EXPECT_TRUE(wf.error().message.find("缺少 next") != std::string::npos) << wf.error().message;
}

TEST(WfEcho, literal_fires_and_logs_node_diag) {
  Json ir_json = echo_ir(Json::array(
      {Json{{"id", "e0"}, {"kind", "echo"}, {"text", "hello 回显"}, {"next", "_turn_end"}}}));
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir);

  tool::ToolRegistry reg;
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  TempDir tmp;
  SessionLog slog(tmp.file());
  FireOptions fo;
  fo.log = &slog;
  auto r = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r) << r.error().message;

  // echo 落一条 source=node 诊断：msg == 解析后文本（屏幕打印干同一串）
  bool seen = false;
  for (auto const& rec : slog.records())
    if (rec.value("source", "") == "node" && rec.value("node", "") == "e0") {
      seen = true;
      EXPECT_EQ(rec.value("msg", ""), "hello 回显");
    }
  EXPECT_TRUE(seen) << "未找到 echo 的节点诊断";
}

TEST(WfEcho, ref_to_run_tools_result_passes_vr2) {
  // wfr-㉗：run_tools 也可被 $..result 引用（VR2 放宽；模型节点同理）
  Json ir_json = echo_ir(Json::array(
      {Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "e0"}},
       Json{{"id", "e0"}, {"kind", "echo"}, {"text", "调用：$act.result.summary"},
            {"next", "_turn_end"}}}));
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir);

  tool::ToolRegistry reg;
  auto wf = compile_workflow(*ir, reg);
  ASSERT_TRUE(wf) << wf.error().message;

  // 回归：指向 expr 的 result 仍被 VR2 拒
  Json bad = echo_ir(Json::array(
      {Json{{"id", "x"}, {"kind", "expr"}, {"expr", "1==1"}, {"then", "e0"}, {"else", "_turn_end"}},
       Json{{"id", "e0"}, {"kind", "echo"}, {"text", "$x.result.price"}, {"next", "_turn_end"}}}));
  auto bir = WorkflowIR::parse(bad);
  ASSERT_TRUE(bir);
  auto bwf = compile_workflow(*bir, reg);
  ASSERT_FALSE(bwf);
  EXPECT_TRUE(bwf.error().message.find("非 result 节点") != std::string::npos)
      << bwf.error().message;
}