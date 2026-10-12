// wfr-㊸ @③ 测试：SandboxManager（provider 解析 + bwrap 沙箱声明 + 受限子进程兜底 + fail-closed）。
// 验收「无 provider 时拒绝执行用例」：只给不可用 provider → SandboxManager::available()==false；
// wfr 装配语义 = 该情形下 bash 以空执行器注册 → handler 拒跑（绝不裸奔）。
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cppchain/tool/bash_tool.hpp"
#include "cppchain/tool/executor.hpp"
#include "cppchain/tool/sandbox.hpp"
#include "cppchain/tool/tool.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using Json = nlohmann::json;

namespace {

std::filesystem::path temp_ws(std::string tag) {
  static unsigned counter = 0;
  auto const dir = std::filesystem::temp_directory_path() /
                   ("wf_sandbox_" + tag + "_" + std::to_string(counter++));
  std::filesystem::create_directories(dir);
  return dir;
}

// 跑一条 bash 命令；spec 由调用方给定（后端来自 SandboxManager 或手工）
Json run_bash(ToolSpec& spec, std::string const& command) {
  ToolContext ctx;
  ctx.tool_call_id = "call_sbx";
  ctx.executor = spec.executor;  // 直跑路径：执行器从 spec 注入 ctx（ToolNode/Guard 也是这么干的）
  auto const r = spec.handler(Json{{"command", command}}, ctx);
  if (!r) ADD_FAILURE() << "handler 失败: " << r.error().message;
  return r ? *r : Json{};
}

// 断言 handler 明确失败（返回 error）
std::string fail_bash(ToolSpec& spec, std::string const& command) {
  ToolContext ctx;
  ctx.tool_call_id = "call_sbx";
  ctx.executor = spec.executor;
  auto const r = spec.handler(Json{{"command", command}}, ctx);
  EXPECT_FALSE(r.has_value());
  return r ? std::string{} : r.error().message;
}

}  // namespace

// ---- provider 解析与 fail-closed ----

TEST(SandboxManagement, only_unavailable_provider_fails_closed) {
  // 只给 "container"（预留未实现）→ 解析不到任何 provider → available()==false
  SandboxConfig c;
  c.workspace = temp_ws("none");
  c.providers = {"container"};
  SandboxManager const sbx(c);
  EXPECT_FALSE(sbx.available());
  EXPECT_EQ(sbx.provider_name(), "");
  EXPECT_NE(sbx.describe().find("无可用"), std::string::npos);
}

TEST(SandboxManagement, unknown_provider_is_skipped_fail_closed) {
  SandboxConfig c;
  c.workspace = temp_ws("unk");
  c.providers = {"no-such-provider"};
  SandboxManager const sbx(c);
  EXPECT_FALSE(sbx.available());
}

TEST(SandboxManagement, auto_resolves_some_provider_everywhere) {
  // 自动 = {bwrap, subprocess}：bwrap 可用→它；否则受限子进程兜底 —— 任何宿主机都非空
  SandboxConfig c;
  c.workspace = temp_ws("auto");
  SandboxManager const sbx(c);
  ASSERT_TRUE(sbx.available()) << sbx.describe();
  EXPECT_TRUE(sbx.provider_name() == "bwrap" || sbx.provider_name() == "subprocess");
}

TEST(SandboxManagement, forcing_bwrap_when_namespaces_unavailable_is_fail_closed) {
  if (SandboxManager::bwrap_usable()) {
    GTEST_SKIP() << "bwrap 可用环境：本用例只测『二进制在但建不了 namespace』的降级路径";
  }
  SandboxConfig c;
  c.workspace = temp_ws("bwrap");
  c.providers = {"bwrap"};  // 显式只给 bwrap，且它建不了 namespace → 拒绝，不回退
  SandboxManager const sbx(c);
  EXPECT_FALSE(sbx.available());
  EXPECT_NE(sbx.describe().find("无可用"), std::string::npos);
}

// ---- bwrap 沙箱声明（纯函数，无需真跑 bwrap）----

TEST(SandboxManagement, bwrap_argv_declares_the_isolation_contract) {
  SandboxConfig c;
  c.workspace = "/ws";
  auto const argv = SandboxManager::bwrap_argv(c, "echo hi");
  auto contains = [&argv](std::string const& s) {
    return std::find(argv.begin(), argv.end(), s) != argv.end();
  };
  auto pos_of = [&argv](std::string const& s) {
    auto const it = std::find(argv.begin(), argv.end(), s);
    return it == argv.end() ? std::numeric_limits<std::size_t>::max() : std::distance(argv.begin(), it);
  };
  EXPECT_TRUE(contains("--unshare-all"));   // 禁网 + 新 pid/ipc/uts
  EXPECT_TRUE(contains("--die-with-parent"));
  EXPECT_TRUE(contains("--ro-bind"));       // 根只读
  EXPECT_TRUE(contains("--tmpfs"));         // /tmp 独立
  EXPECT_TRUE(contains("--clearenv"));      // 不泄漏宿主环境
  // workspace 可写：--bind <ws> <ws> 成对出现（顺序要紧跟在 --bind 之后）
  auto const bind_pos = pos_of("--bind");
  ASSERT_NE(bind_pos, std::numeric_limits<std::size_t>::max());
  ASSERT_LT(bind_pos + 2, argv.size());
  EXPECT_EQ(argv[bind_pos + 1], "/ws");
  EXPECT_EQ(argv[bind_pos + 2], "/ws");
  // cwd = workspace：--chdir <ws>
  auto const chdir_pos = pos_of("--chdir");
  ASSERT_NE(chdir_pos, std::numeric_limits<std::size_t>::max());
  ASSERT_LT(chdir_pos + 1, argv.size());
  EXPECT_EQ(argv[chdir_pos + 1], "/ws");
  // 命令最终以 /bin/sh -c <cmd> 收尾
  EXPECT_GE(argv.size(), 8u);
  EXPECT_EQ(argv[argv.size() - 3], "/bin/sh");
  EXPECT_EQ(argv[argv.size() - 2], "-c");
  EXPECT_EQ(argv.back(), "echo hi");
}

// ---- 受限子进程 provider（兜底语义诚实标注）----

TEST(SandboxManagement, subprocess_runs_in_workspace_and_admits_it_is_not_a_sandbox) {
  auto const ws = std::filesystem::weakly_canonical(temp_ws("sub"));
  SandboxConfig c;
  c.workspace = ws;
  c.providers = {"subprocess"};
  SandboxManager const sbx(c);
  ASSERT_TRUE(sbx.available());
  EXPECT_NE(sbx.describe().find("非真沙箱"), std::string::npos) << "不得冒充沙箱";
  EXPECT_EQ(sbx.provider_name(), "subprocess");

  ToolSpec spec = make_bash_tool(sbx.executor());
  Json const out = run_bash(spec, "pwd");
  EXPECT_NE(out.dump().find(ws.string()), std::string::npos) << "应 chdir 到工作区: " << out.dump();

  // 兜底 provider 没有文件系统隔离：写 workspace 落在宿主 —— 诚实语义（非真沙箱）
  Json const w = run_bash(spec, "echo probe > x.txt");
  EXPECT_TRUE(std::filesystem::exists(ws / "x.txt"));
  std::filesystem::remove(ws / "x.txt");
}

TEST(SandboxManagement, subprocess_preserves_timeout_and_truncation) {
  auto const ws = std::filesystem::weakly_canonical(temp_ws("lim"));
  SandboxConfig c;
  c.workspace = ws;
  c.providers = {"subprocess"};
  SandboxManager const sbx(c);
  ASSERT_TRUE(sbx.available());

  // 超时（L5）：sleep 超出 60ms 预算 → 强杀
  ToolSpec fast_spec = make_bash_tool(sbx.executor(), std::chrono::milliseconds(60));
  EXPECT_NE(fail_bash(fast_spec, "sleep 5").find("超时"), std::string::npos);

  // 输出封顶：小 max_output 时输出截断并带 note
  auto const tiny = SandboxManager::make_subprocess(ws, /*max_output=*/64);
  ToolSpec cap_spec = make_bash_tool(tiny.get(), std::chrono::milliseconds(2000));
  Json const out = run_bash(cap_spec, "seq 1 1000");
  EXPECT_NE(out.dump().find("截断"), std::string::npos) << "printf 输出应含截断说明";
}

// ---- wfr 装配语义：无 provider → bash 空执行器 → handler 拒跑 ----

TEST(SandboxManagement, no_provider_bash_handler_refuses_like_wfr_wiring) {
  SandboxConfig c;
  c.workspace = temp_ws("rej");
  c.providers = {"container"};  // 预留未实现 → 解析不到
  SandboxManager const sbx(c);
  EXPECT_FALSE(sbx.available());
  // 镜像 wfr：把空执行器灌给 bash —— handler 必须 fail-closed，绝不裸奔
  ToolSpec spec = make_bash_tool(/*executor=*/nullptr);
  EXPECT_NE(fail_bash(spec, "echo should-not-run").find("拒绝执行"), std::string::npos);
  // 且事实成立：确实没有执行（无副作用可查 —— 此处以返回契约为准）
}