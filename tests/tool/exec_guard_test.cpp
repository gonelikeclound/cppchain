// wfr-㊷ 测试：ExecutionGuard 执行点防护（防重放 / 幂等 / probe 兜底 / 只读免审 / 事务边界）
// 纪律（problems/03 §5）：断言被测路径真的被走到 —— 用计数 + report 内容，不靠"看起来对"。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/probe.hpp"
#include "cppchain/tool/tool.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using Prepared = ExecutionGuard::Prepared;

namespace {

using Json = nlohmann::json;
using Res = std::expected<nlohmann::json, ToolError>;

// 计数器工具：真实副作用（模拟"写盘"，executions 必须真的 +1 才说明 handler 被调用）
struct CounterTool {
  int calls = 0;
  ToolSpec spec;
  CounterTool() {
    spec = make_tool("counter", "自增计数（有副作用）",
                     SchemaBuilder().prop("x", "integer", "步长", /*required=*/false).build(),
                     [this](Json const& args, ToolContext&) -> Res {
                       ++calls;
                       int step = 1;
                       if (auto s = arg<int>(args, "x", /*required=*/false); s) step = *s;
                       return Json{{"calls", calls}, {"step", step}};
                     },
                     /*read_only=*/false);
  }
};

// 假"会话日志"：宿主在 wfr 里的 LogReader 由 .wfr/sessions 重建，这里用内存 map 等价模拟。
//   intent[true] + completed[false]  → Intent 残留
//   completed[true]                  → 有 ToolMessage（success/error），content 即记录
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

ToolCall call(std::string name, std::string args) {
  ToolCall c;
  c.id = "call_" + name;
  c.name = name;
  c.args = Json::parse(args);
  return c;
}

}  // namespace

// ---- 防重放：未开始 → 执行；已完成 → 重放（不执行） ----

TEST(ExecGuard, not_started_executes_and_replay_does_not) {
  CounterTool t;
  FakeLog log;  // 默认 completed=false, intent=false → 未开始
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};

  Prepared p1 = guard.prepare(call("counter", R"({"x":2})"), &t.spec);
  EXPECT_TRUE(p1.execute);
  Message m1 = guard.run(call("counter", R"({"x":2})"), &t.spec, Json{{"x", 2}});
  EXPECT_EQ(t.calls, 1);

  // 这次"已完成"：日志给出记录 → 重放，counter 不再 +1（事务边界：handler 不被第二次调用）
  FakeLog done;
  done.completed = true;
  done.content = R"({"calls":1,"step":2})";
  ExecutionGuard guard2{ExecutionGuard::Options{.log = [&done](std::string const& k) { return done(k); }}};
  Message m2 = guard2.execute(call("counter", R"({"x":2})"), &t.spec, Json{{"x", 2}});
  EXPECT_EQ(t.calls, 1);                                     // handler 没被调用
  EXPECT_EQ(m2.text(), R"({"calls":1,"step":2})");           // 重放日志记录
  EXPECT_EQ(guard2.replays(), 1u);
  EXPECT_EQ(guard2.executions(), 0u);
}

// ---- 幂等键：参数指纹。同工具同参数 = 同键；键序不同 ≠ 不同键 ----

TEST(ExecGuard, key_fingerprint_is_argument_order_invariant) {
  ToolCall a = call("file_write", R"({"path":"a.txt","content":"x","append":false})");
  ToolCall b = call("file_write", R"({"append":false,"content":"x","path":"a.txt"})");
  EXPECT_EQ(ExecutionGuard::key_for(a, ExecKeyMode::NameAndArgs),
            ExecutionGuard::key_for(b, ExecKeyMode::NameAndArgs));
  // 参数不同 → 键不同
  ToolCall c = call("file_write", R"({"path":"a.txt","content":"y","append":false})");
  EXPECT_NE(ExecutionGuard::key_for(a, ExecKeyMode::NameAndArgs),
            ExecutionGuard::key_for(c, ExecKeyMode::NameAndArgs));
}

// ---- probe 兜底：Intent 残留 + 产物状态四格 ----

// 构造一个带 probe 的工具：probe 决策由外部注入（拟磁盘状态），配合真实磁盘 probe 单测互补
struct ProbeTool {
  ProbeResult preset = ProbeResult::Unknown;
  int calls = 0;
  ToolSpec spec;
  ProbeTool() {
    spec = make_tool("probed", "带探测的写工具",
                     SchemaBuilder().prop("v", "string", "值").build(),
                     [this](Json const& args, ToolContext&) -> Res {
                       ++calls;
                       auto v = arg<std::string>(args, "v");
                       return Json{{"wrote", v ? *v : "?"}};
                     },
                     /*read_only=*/false);
    spec.probe = [this](Json const&) -> ProbeResult { return preset; };
  }
};

TEST(ExecGuard, probe_absent_allows_execution) {
  ProbeTool t;
  t.preset = ProbeResult::Absent;
  FakeLog log; log.intent = true;  // Intent 残留，但产物不存在
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("probed", R"({"v":"a"})"), &t.spec);
  EXPECT_TRUE(p.execute);
  EXPECT_EQ(t.calls, 0);
  Message m = guard.run(call("probed", R"({"v":"a"})"), &t.spec, Json{{"v", "a"}});
  EXPECT_EQ(t.calls, 1);  // 安全重跑：产物不存在 → 视为未发生
  EXPECT_EQ(guard.audit_lines(), 1u);
}

TEST(ExecGuard, probe_complete_reports_and_does_not_rerun) {
  ProbeTool t;
  t.preset = ProbeResult::Complete;
  FakeLog log; log.intent = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("probed", R"({"v":"a"})"), &t.spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(t.calls, 0);
  EXPECT_EQ(guard.replays(), 1u);  // 探测到完成 → 计入重放（不执行）
  EXPECT_EQ(guard.executions(), 0u);
  EXPECT_NE(p.report.text().find("已执行"), std::string::npos);
}

TEST(ExecGuard, probe_partial_fail_closed) {
  ProbeTool t;
  t.preset = ProbeResult::Partial;
  FakeLog log; log.intent = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("probed", R"({"v":"a"})"), &t.spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(t.calls, 0);
  EXPECT_NE(p.report.text().find("不完整"), std::string::npos);  // fail-closed，如实说"部分写入"
  EXPECT_EQ(guard.blocked(), 1u);
}

TEST(ExecGuard, probe_unknown_fail_closed) {
  ProbeTool t;
  t.preset = ProbeResult::Unknown;
  FakeLog log; log.intent = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("probed", R"({"v":"a"})"), &t.spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(t.calls, 0);
  EXPECT_EQ(guard.blocked(), 1u);
}

// ---- probe 铁律：probe 抛异常 → 当 Unknown（fail-closed，绝不当作 Absent） ----

TEST(ExecGuard, probe_throwing_is_treated_as_unknown) {
  ToolSpec spec = make_tool("throwprobe", "probe 会抛", Json::object(),
                            [](Json const&, ToolContext&) -> Res { return Json{}; },
                            /*read_only=*/false);
  spec.probe = [](Json const&) -> ProbeResult { throw std::runtime_error("probe broke"); };
  FakeLog log; log.intent = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("throwprobe", R"({})"), &spec);
  EXPECT_FALSE(p.execute);  // 抛异常也 fail-closed，不允许执行
  EXPECT_EQ(guard.blocked(), 1u);
}

// ---- 无 probe → Intent 一律 fail-closed（安全默认） ----

TEST(ExecGuard, intent_without_probe_fail_closed) {
  CounterTool t;  // 无 probe
  FakeLog log; log.intent = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("counter", R"({})"), &t.spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(t.calls, 0);  // 宁可请人，不可重跑
  EXPECT_EQ(guard.blocked(), 1u);
}

// ---- 只读免审：不查日志、不审计 ----

TEST(ExecGuard, read_only_bypasses_guard) {
  ToolSpec ro = make_tool("read_only_probe", "只读", Json::object(),
                          [](Json const&, ToolContext&) -> Res { return Json{{"ok", true}}; },
                          /*read_only=*/true);
  // 即使日志说"已完成"，只读工具也不该重放（它没有副作用可重复）—— 直接执行
  FakeLog done; done.completed = true;
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&done](std::string const& k) { return done(k); }}};
  Prepared p = guard.prepare(call("read_only_probe", R"({})"), &ro);
  EXPECT_TRUE(p.execute);  // read_only → 走不到判定树 → 直接执行
  Message m = guard.execute(call("read_only_probe", R"({})"), &ro, Json::object());
  EXPECT_EQ(m.text(), R"({"ok":true})");
  EXPECT_EQ(guard.audit_lines(), 0u);  // 只读不审计
}

// ---- Failed(有 error 结果)：默认拒跑；retry_on_failure 才放行 ----

TEST(ExecGuard, failed_result_blocks_without_retry_flag) {
  CounterTool t;
  FakeLog log; log.completed = true; log.status = "error";
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("counter", R"({})"), &t.spec);
  EXPECT_FALSE(p.execute);
  EXPECT_EQ(guard.blocked(), 1u);
  EXPECT_EQ(t.calls, 0);
}

TEST(ExecGuard, failed_result_reruns_with_retry_flag) {
  CounterTool t;
  FakeLog log; log.completed = true; log.status = "error";
  ExecutionGuard::Options opts;
  opts.log = [&log](std::string const&) { return log(""); };
  opts.retry_on_failure = true;
  ExecutionGuard guard{std::move(opts)};
  Prepared p = guard.prepare(call("counter", R"({})"), &t.spec);
  EXPECT_TRUE(p.execute);  // 配置允许重试失败 → 放行
}

// ---- 事务边界：崩溃恢复语义 ~ replay 不会二次执行，Handler 只在 execute==true 被调 ----

TEST(ExecGuard, handler_only_called_when_execute_is_true) {
  CounterTool t;
  FakeLog log;  // 未开始 → 执行；之后清理日志 → 同一键变成"已完成"（等价崩溃后恢复重跑 prepare）
  ExecutionGuard guard{ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }}};
  Prepared p = guard.prepare(call("counter", R"({})"), &t.spec);
  ASSERT_TRUE(p.execute);
  Message m = guard.execute(call("counter", R"({})"), &t.spec, Json::object());
  EXPECT_EQ(t.calls, 1);

  // 模拟日志此时已记录"完成"（Host 落盘），同一参数再次 prepare —— 与崩溃重跑等价的时序：
  // 第二次调用必须重放而不是再执行（否则副作用×2）
  FakeLog done; done.completed = true; done.content = "completed-record";
  ExecutionGuard guard2{ExecutionGuard::Options{.log = [&done](std::string const& k) { return done(k); }}};
  Prepared p2 = guard2.prepare(call("counter", R"({})"), &t.spec);
  EXPECT_FALSE(p2.execute);
  EXPECT_EQ(t.calls, 1);  // 没有第二次执行
}

// ---- 审计：decision 按路径分类 + 行数 == 真实判定发生次数 ----

TEST(ExecGuard, audit_records_decisions) {
  ProbeTool t;
  FakeLog log; log.intent = true;
  std::vector<std::string> decisions;
  ExecutionGuard::Options opts;
  opts.log = [&log](std::string const& k) { return log(k); };
  opts.audit = [&decisions](AuditRecord const& rec) { decisions.push_back(rec.decision); };
  ExecutionGuard guard{std::move(opts)};
  (void)guard.prepare(call("probed", R"({"v":"a"})"), &t.spec);
  EXPECT_EQ(decisions.size(), 1u);
  EXPECT_EQ(decisions[0], "blocked");  // Unknown 无 probe → blocked
  EXPECT_EQ(guard.audit_lines(), 1u);
}

// ---- probe_file：真实磁盘判定（离线确定性） ----

namespace {
// 独立临时目录（测试自管，不污染仓库）
struct TmpDir {
  std::filesystem::path dir;
  // 每次测试一个固定名 → remove_all 幂等
  explicit TmpDir(std::string tag) : dir(std::filesystem::temp_directory_path() / tag) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~TmpDir() { std::filesystem::remove_all(dir); }
  std::filesystem::path file(std::string name) const { return dir / name; }
  void write(std::filesystem::path p, std::string content) const {
    std::ofstream(p, std::ios::binary).write(content.data(),
                                             static_cast<std::streamsize>(content.size()));
  }
};
}  // namespace

TEST(ExecGuardProbeFile, missing_complete_partial_primitives) {
  TmpDir tmp("cppchain_probe_test_absent");
  std::filesystem::path const p = tmp.file("x.txt");
  // 文件不存在 → Absent
  EXPECT_EQ(probe_file(p, "hello", 5), ProbeResult::Absent);

  // 写入完整内容 → Complete
  tmp.write(p, "hello");
  EXPECT_EQ(probe_file(p, "hello", 5), ProbeResult::Complete);
  // 字节数签名不同 → Partial
  EXPECT_EQ(probe_file(p, "hello", 6), ProbeResult::Partial);
  // 内容不同 → Partial（字节数同但内容不同 = 被改过/写一半凑巧同长）
  tmp.write(p, "world");  // 同是 5 字节
  EXPECT_EQ(probe_file(p, "hello", 5), ProbeResult::Partial);

  // 残缺：只有前 3 字节 → Partial
  tmp.write(p, "hel");
  EXPECT_EQ(probe_file(p, "hello", 5), ProbeResult::Partial);
}

TEST(ExecGuardProbeFile, directory_is_partial) {
  TmpDir tmp("cppchain_probe_test_dir");
  std::filesystem::path const d = tmp.file("sub");
  std::filesystem::create_directories(d);
  // 目标是目录：不能算"写完的文件" → Partial（fail-closed）
  EXPECT_EQ(probe_file(d, "hello", 5), ProbeResult::Partial);
}