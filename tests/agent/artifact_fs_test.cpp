// wfr-㊶ 跨半边集成测试：artifact 落盘「完整结果已落盘…用文件工具读取」的**兑现闭环**。
//
// 上半边（agent/history）：plan_history_rules 在 artifact_mode 下把超预算工具结果全文落盘，
//   引用行换成「头部 + 相对路径 + 规模标注」，`message.artifact` = 相对路径（display_root 起）。
// 下半边（tool/file_tools）：沙箱 file_read 只认**根内相对路径**。
// 这里把它们接起来：disp 是落盘时写给模型看的路径，直接喂 file_read（root=display_root）
// 必须读回**完整原文** —— 这条契约成立，wfr 里的"用文件工具读取"才是真的能兑现。
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/agent/history.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/tool/file_tools.hpp"

namespace {

using cppchain::message::Message;
using cppchain::message::ToolCall;
using Json = nlohmann::json;

std::atomic<unsigned> g_dir_counter{0};

// 临时"工作区"根（= 沙箱根），析构时整个清掉
struct Workspace {
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_artifact_fs_" + std::to_string(g_dir_counter.fetch_add(1)));
  Workspace() { std::filesystem::create_directories(dir); }
  ~Workspace() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string str() const { return dir.string(); }
};

Message human_msg(std::string const& t) { return cppchain::message::human_message(t); }
Message ai_calling(std::string const& id) {
  Message m = cppchain::message::ai_message("");
  m.tool_calls.push_back(ToolCall{id, "web_search", Json::object()});
  return m;
}
Message tool_result(std::string const& text, std::string const& id) {
  return cppchain::message::tool_message(text, id, "success");
}

// 把 plan 应用一遍（与 reducer transform 语义一致）
std::vector<Message> apply_plan(std::vector<Message> const& msgs,
                                cppchain::agent::HistoryRulePlan const& plan) {
  std::vector<Message> out;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (std::find(plan.drop.begin(), plan.drop.end(), i) != plan.drop.end()) continue;
    auto const it = std::find_if(plan.rewrite.begin(), plan.rewrite.end(),
                                 [i](auto const& p) { return p.first == i; });
    out.push_back(it != plan.rewrite.end() ? it->second : msgs[i]);
  }
  return out;
}

}  // namespace

TEST(ArtifactFsRedeem, file_read_reads_back_the_artifact_reference_line) {
  Workspace ws;  // sandbox root（= wfr 的工作区）
  cppchain::agent::HistoryRuleParams p;
  p.tool_budget = 20;
  p.artifact_mode = true;
  p.artifact_dir = (ws.dir / ".wfr" / "artifacts").string();
  p.artifact_display_root = ws.str();
  std::string const full_text(400, 'x');  // ≈100 token > 20 → 落盘

  std::vector<Message> const msgs{human_msg("q"), ai_calling("c1"), tool_result(full_text, "c1")};
  auto const plan = cppchain::agent::plan_history_rules(msgs, {"clip_tool_results"}, p);
  ASSERT_EQ(plan.stats.artifacts_written, 1u);
  auto const out = apply_plan(msgs, plan);

  ASSERT_TRUE(out[2].artifact.has_value()) << "落盘必须设 message.artifact";
  std::string const disp = *out[2].artifact;
  EXPECT_EQ(disp, ".wfr/artifacts/artifact_1_c1.txt") << "展示路径应对沙箱根相对";
  EXPECT_NE(out[2].text().find("完整结果已落盘"), std::string::npos) << out[2].text();

  // 下半边兑现：file_read 只有根内相对路径可用，喂 disp 必须读回完整原文
  cppchain::tool::FileToolsConfig cfg;
  cfg.root_dir = ws.str();
  auto const read_spec = cppchain::tool::make_file_read_tool(cfg);
  cppchain::tool::ToolContext ctx;
  ctx.tool_call_id = "call_f0";
  auto const r = read_spec.handler(Json{{"path", disp}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("content").get<std::string>(), full_text)
      << "file_read 应能兑现引用行：读到完整原文（不是截断版）";
  EXPECT_FALSE(r->at("truncated").get<bool>());
}

TEST(ArtifactFsRedeem, out_of_root_artifact_path_is_still_rejected_by_file_read) {
  // 反向护栏：若某条引用行给出了根外的路径（异盘 / 显式绝对 artifact_dir 落在根外），
  // file_read 必须拒绝 —— 沙箱纪律不因「这条路径是从 artifact 落盘来的」而放松。
  Workspace ws;
  cppchain::tool::FileToolsConfig cfg;
  cfg.root_dir = ws.str();
  auto const read_spec = cppchain::tool::make_file_read_tool(cfg);
  cppchain::tool::ToolContext ctx;
  ctx.tool_call_id = "call_f0";
  auto const r = read_spec.handler(Json{{"path", "../outside.txt"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos) << r.error().message;
}