// wfr-㊸ @① 测试：bash 工具（ToolSpec 契约 + 词面门禁 + 执行器 + guard 集成）
// 验收：首条 bash 命令过 guard —— 只读命令经 ExecutionGuard 放行并真执行，
// 写/危险/未知在"未开始"分支就被词面门禁阻断（fail-closed），无执行器时 handler 拒跑。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/tool/bash_tool.hpp"
#include "cppchain/tool/command_classify.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/executor.hpp"
#include "cppchain/tool/tool.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using Prepared = ExecutionGuard::Prepared;
using Json = nlohmann::json;

namespace {

ToolCall bash_call(std::string command) {
  ToolCall c;
  c.id = "call_bash";
  c.name = "bash";
  c.args = Json{{"command", command}};
  return c;
}

// 假"会话日志"（与 exec_guard_test 同款：intent/completed 三分）
struct FakeLog {
  bool intent = false;
  bool completed = false;
  std::string content;
  std::string status = "success";
  ExecutionGuard::LogState operator()(std::string const&) const {
    ExecutionGuard::LogState st;
    if (completed) { st.completed = true; st.content = content; st.status = status; }
    if (intent) st.intent = true;
    return st;
  }
};

// 包一层，方便三条直接用 make_plain_spawn_executor 的产物
struct PlainSpawnExecutorWrap {
  std::shared_ptr<ToolExecutor> ptr = make_plain_spawn_executor();
  ToolExecutor* operator->() { return ptr.get(); }
};

}  // namespace

// ---- ToolSpec 契约：bash 出厂字段 ----

TEST(BashTool, contract_fields) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  EXPECT_EQ(spec.name, "bash");
  EXPECT_FALSE(spec.read_only);                     // 副作用任意
  EXPECT_EQ(spec.category, "action");
  EXPECT_EQ(spec.approval, ToolApprovalLevel::NeedHuman);  // 越过静态策略，直接要求人审
  ASSERT_TRUE(spec.timeout_ms.has_value());
  EXPECT_EQ(spec.timeout_ms.value(), std::chrono::milliseconds(120000));
  EXPECT_EQ(spec.executor, exec.get());
  ASSERT_NE(spec.command_classify, nullptr);        // 词面分类器已绑定
  // nlohmann 的 contains() 只查对象键；required 是数组，用下标断言
  ASSERT_TRUE(spec.parameters["required"].is_array());
  ASSERT_EQ(spec.parameters["required"].size(), 1u);
  EXPECT_EQ(spec.parameters["required"][0], "command");
}

// ---- 验收：首条 bash 命令过 guard ----
// 只读命令 → 分类 ReadOnly → prepare 放行 → 真执行

TEST(BashTool, readonly_command_passes_guard_and_runs) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog log;  // 未开始
  std::vector<std::string> decisions;
  ExecutionGuard::Options opts;
  opts.log = [&log](std::string const& k) { return log(k); };
  opts.audit = [&decisions](AuditRecord const& rec) { decisions.push_back(rec.decision); };
  ExecutionGuard guard{std::move(opts)};

  Prepared p = guard.prepare(bash_call("echo hi"), &spec);
  EXPECT_TRUE(p.execute);                            // 词面可证只读 → 放行
  EXPECT_EQ(guard.blocked(), 0u);

  Message m = guard.run(bash_call("echo hi"), &spec, Json{{"command", "echo hi"}});
  EXPECT_EQ(m.status, "success");
  EXPECT_NE(m.text().find("hi"), std::string::npos); // 真的派生出子进程执行
  EXPECT_NE(m.text().find("exit_code: 0"), std::string::npos);
  EXPECT_EQ(guard.executions(), 1u);
  ASSERT_EQ(decisions.size(), 1u);
  EXPECT_EQ(decisions[0], "command_readonly");       // 审计词面门禁放行
}

// ---- 写命令 → 词面门禁阻断（fail-closed，批② 前无放行通道）----

TEST(BashTool, write_command_blocked) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog log;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};

  Prepared p = guard.prepare(bash_call("echo hi > /tmp/cppchain_bash_f"), &spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(guard.blocked(), 1u);                     // intent? 未开始 → 被门禁拦下
  EXPECT_NE(p.report.text().find("写副作用"), std::string::npos);
  EXPECT_EQ(guard.executions(), 0u);                 // handler 没被调用
}

// ---- 危险命令 → 阻断 ----

TEST(BashTool, risky_command_blocked) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog log;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};

  Prepared p = guard.prepare(bash_call("rm -rf /tmp/cppchain_bash_x"), &spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(guard.blocked(), 1u);
  EXPECT_NE(p.report.text().find("危险"), std::string::npos);
}

// ---- 未知命令 → fail-closed ----

TEST(BashTool, unknown_command_blocked) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog log;
  std::vector<std::string> decisions;
  ExecutionGuard::Options opts;
  opts.log = [&log](std::string const& k) { return log(k); };
  opts.audit = [&decisions](AuditRecord const& rec) { decisions.push_back(rec.decision); };
  ExecutionGuard guard{std::move(opts)};

  Prepared p = guard.prepare(bash_call("some_mystery_tool x"), &spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(guard.blocked(), 1u);
  EXPECT_NE(p.report.text().find("无法静态判断"), std::string::npos);
  ASSERT_EQ(decisions.size(), 1u);
  EXPECT_EQ(decisions[0], "command_unknown");
}

// ---- 缺 command 参数 → 分类 Unknown → fail-closed ----

TEST(BashTool, missing_command_arg_fail_closed) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog log;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};

  ToolCall c;
  c.id = "call_bash_missing";
  c.name = "bash";
  c.args = Json{{"cwd", "/tmp"}};   // 没有 command
  Prepared p = guard.prepare(c, &spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(guard.blocked(), 1u);
}

// ---- 铁律：无执行器 → handler 拒跑（即使词面只读）----

TEST(BashTool, no_executor_refuses_to_run) {
  ToolSpec spec = make_bash_tool(/*executor=*/nullptr);
  FakeLog log;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};

  // 只读命令过门禁（execute=true），但 handler 因无执行器拒跑
  Prepared p = guard.prepare(bash_call("echo hi"), &spec);
  EXPECT_TRUE(p.execute);
  Message m = guard.run(bash_call("echo hi"), &spec, Json{{"command", "echo hi"}});
  EXPECT_EQ(m.status, "error");
  EXPECT_NE(m.text().find("未配置执行器"), std::string::npos);
  EXPECT_EQ(guard.executions(), 1u);  // handler 被调了，但以拒跑结果收手
}

// ---- 已完成（日志重放）不经过词面门禁：replay 安全 ----

TEST(BashTool, completed_replays_without_classifier) {
  auto exec = make_plain_spawn_executor();
  ToolSpec spec = make_bash_tool(exec.get());
  FakeLog done;  // 已完成
  done.completed = true;
  done.content = "run-logs-say-so";
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&done](std::string const& k) { return done(k); }}};

  // 即使是危险命令：已完成 → 重放日志结果，不执行、不拦
  Prepared p = guard.prepare(bash_call("rm -rf /x"), &spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(p.report.text(), "run-logs-say-so");
  EXPECT_EQ(guard.replays(), 1u);
  EXPECT_EQ(guard.blocked(), 0u);
}

// ---- PlainSpawnExecutor：真实子进程 + 输出 + 退出码 + 超时强杀 ----

TEST(PlainSpawn, runs_and_captures_output) {
  PlainSpawnExecutorWrap exec;
  ExecResult r = exec->run("echo hello", std::chrono::milliseconds(2000));
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_NE(r.output.find("hello"), std::string::npos);
  EXPECT_FALSE(r.timed_out);
  EXPECT_TRUE(r.note.empty());
}

TEST(PlainSpawn, exit_code_and_stderr_merged) {
  PlainSpawnExecutorWrap exec;
  ExecResult r = exec->run("sh -c 'echo out; echo err >&2; exit 3'", std::chrono::milliseconds(2000));
  EXPECT_EQ(r.exit_code, 3);
  EXPECT_NE(r.output.find("out"), std::string::npos);
  EXPECT_NE(r.output.find("err"), std::string::npos);  // stderr 合流保留
  EXPECT_FALSE(r.timed_out);
}

TEST(PlainSpawn, timeout_kills_process_group) {
  PlainSpawnExecutorWrap exec;
  ExecResult r = exec->run("sleep 2", std::chrono::milliseconds(300));
  EXPECT_TRUE(r.timed_out);
  EXPECT_NE(r.note.find("强杀"), std::string::npos);
}