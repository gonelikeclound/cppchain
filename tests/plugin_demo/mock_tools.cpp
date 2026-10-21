// wfr-㊹ 治理（problems/34）· 动态 mock 工具库（libmock_tools.so）。
//
// 纯 C descriptor + 裸函数指针（跨 DSO 安全），不链 cppchain（只 include tool_plugin.hpp + nlohmann）。
// C++ 零引用约束：`libmock_tools.so` 的名字、这些工具名只出现在 workflow JSON 的
// tool_registry 声明里（from:"mock_tools", load:[...]），宿主按名 dlsym。
//
// 每个工具一个 `tool_<name>_creator` 导出。handler 是裸 C fn（args JSON 字符串 →
// out 结果 JSON 字符串 / err 错误字符串），宿主包成 std::function。
#include "cppchain/tool/tool_plugin.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>

using Json = nlohmann::json;

namespace {

// ---- 通用助手：JSON 字符串 → 对象 ----
Json parse_args(char const* s) {
  if (s == nullptr) return Json::object();
  auto j = Json::parse(s, nullptr, /*allow_exceptions=*/false);
  return j.is_discarded() ? Json::object() : j;
}

// 写 out_json（宿主 free）；失败写 err_out 并返回非 0
int ok(char** out, Json const& j) {
  std::string const s = j.dump();
  *out = ::strdup(s.c_str());
  return 0;
}
int fail(char** err, std::string const& why) {
  *err = ::strdup(why.c_str());
  return 1;
}

// ---- mock_file_write：确定性写文件（Idempotent；可重放同参同果）----
int mock_file_write_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto path = a.value("path", "");
  auto content = a.value("content", "");
  if (path.empty()) return fail(err, "mock_file_write: 缺 path");
  std::ofstream f(path);
  if (!f) return fail(err, "mock_file_write: 写失败 " + path);
  f << content;
  return ok(out, Json{{"path", path}, {"created", true}});
}

// ---- mock_file_read：只读回读 ----
int mock_file_read_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto path = a.value("path", "");
  if (path.empty()) return fail(err, "mock_file_read: 缺 path");
  std::ifstream f(path);
  if (!f) return fail(err, "mock_file_read: 文件不存在 " + path);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return ok(out, Json{{"path", path}, {"content", content}});
}

// ---- mock_weather：固定天气 JSON（只读，无 TTL 时效 —— 确定性）----
int mock_weather_run(char const* args, char** out, char**) {
  Json const a = parse_args(args);
  return ok(out, Json{{"city", a.value("city", "")},
                      {"celsius", 23}, {"condition", "sunny"}, {"source", "mock"}});
}

// ---- mock_bash：回显命令（Idempotent；不执行 —— mock 只回显，无 executor fail-closed 由宿主兜底）----
int mock_bash_run(char const* args, char** out, char**) {
  Json const a = parse_args(args);
  return ok(out, Json{{"command", a.value("command", "")},
                      {"echo", a.value("command", "")}, {"mock", true}});
}

// ---- mock_probe_hit：恒真探针（模型"自以为完成"时的确定性 truth）----
int mock_probe_hit_run(char const* args, char** out, char**) {
  Json const a = parse_args(args);
  return ok(out, Json{{"hit", true}, {"checked", a.value("what", "")}});
}

// ---- mock_submit_result：写 verdict JSON（Idempotent）----
int mock_submit_result_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto path = a.value("path", "");
  if (path.empty()) return fail(err, "mock_submit_result: 缺 path");
  std::ofstream f(path);
  if (!f) return fail(err, "mock_submit_result: 写失败 " + path);
  f << a.value("verdict", "{}");
  return ok(out, Json{{"path", path}, {"submitted", true}});
}

// 探针函数：恒 Complete（0）。mock 工具写完文件，probe 恒真供 ExecutionGuard 兜底。
int probe_hit(char const*) { return 0; }

// ---- descriptor 工厂 ----
// 注意：必须给每个工具**稳定的 descriptor 指针**。不用 thread_local —— 批量 dlsym 时
// pending 阶段逐个调用 creator，thread_local 会让所有 creator 复用同一个 d（最后一次覆写），
// 导致全部工具串成同一 handler。用进程期 static map（加载阶段宿主串行 creator()，无并发）。
cppchain_tool_descriptor const* desc_for(char const* name, char const* desc,
                                        char const* params, int read_only, int effect,
                                        int approval, ToolRunFn handler, ToolProbeFn probe = nullptr) {
  static std::unordered_map<std::string, cppchain_tool_descriptor> by_name;
  cppchain_tool_descriptor& d = by_name[std::string(name)];
  d = cppchain_tool_descriptor{};
  d.name = name;
  d.version = kToolApiVersion;
  d.context = nullptr;
  d.description = desc;
  d.parameters_json = params;
  d.read_only = read_only;
  d.effect_class = effect;
  d.approval = approval;
  d.ttl_ms = 0;
  d.handler = handler;
  d.probe = probe;
  return &d;
}

}  // namespace

extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_file_write_creator(void*) {
  return desc_for("mock_file_write", "mock：确定性写文件（同参同果，可重放）",
                  R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy,
                  mock_file_write_run, probe_hit);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_file_read_creator(void*) {
  return desc_for("mock_file_read", "mock：只读回读文件",
                  R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
                  /*read_only=*/1, kToolEffectRead, kToolApprovalNever, mock_file_read_run);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_weather_creator(void*) {
  return desc_for("mock_weather", "mock：固定天气（确定性，无时效）",
                  R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})",
                  /*read_only=*/1, kToolEffectRead, kToolApprovalNever, mock_weather_run);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_bash_creator(void*) {
  return desc_for("mock_bash", "mock：回显命令（不执行，只回显 command）",
                  R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNeedHuman, mock_bash_run);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_probe_hit_creator(void*) {
  return desc_for("mock_probe_hit", "mock：恒真探针（模型自以为完成时的确定性 truth）",
                  R"({"type":"object","properties":{"what":{"type":"string"}},"required":["what"]})",
                  /*read_only=*/1, kToolEffectRead, kToolApprovalNever, mock_probe_hit_run);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_submit_result_creator(void*) {
  return desc_for("mock_submit_result", "mock：写 verdict JSON",
                  R"({"type":"object","properties":{"path":{"type":"string"},"verdict":{"type":"string"}},"required":["path","verdict"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy, mock_submit_result_run);
}
