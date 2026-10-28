// 测试用动态工具库（libslow_tools.so）：唯一的 slow_sleep(ms) 只读工具。
// 用途：problems/07 §9.4 的真实端到端取消用例——让 workflow 有一个"跑得够久"的节点，
// 好在运行中调用 wfr cancel / CGI cancel，验证节点边界取消。
#include "cppchain/tool/tool_plugin.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

using Json = nlohmann::json;

int ok(char** out, Json const& j) {
  *out = ::strdup(j.dump().c_str());
  return 0;
}

int fail(char** err, std::string const& why) {
  *err = ::strdup(why.c_str());
  return 1;
}

int slow_sleep_run(char const* args, char** out, char** err) {
  Json a = Json::parse(args == nullptr ? "{}" : args, nullptr, false);
  if (a.is_discarded()) return fail(err, "slow_sleep: 参数不是合法 JSON");
  long ms = a.value("ms", 0L);
  if (ms < 0) return fail(err, "slow_sleep: ms 必须 ≥0");
  if (ms > 30000) ms = 30000;  // 防测试把 CI 拖死
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return ok(out, Json{{"slept_ms", ms}});
}

}  // namespace

extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_slow_sleep_creator(
    void* /*host_ctx*/) {
  static cppchain_tool_descriptor d{};
  d = cppchain_tool_descriptor{};
  d.name = "slow_sleep";
  d.version = kToolApiVersion;
  d.context = nullptr;
  d.description = "测试用：sleep ms 毫秒后返回（只读；用于取消/超时类端到端用例）";
  d.parameters_json = R"({"type":"object","properties":{"ms":{"type":"integer","description":"毫秒（0–30000）"}}})";
  d.read_only = 1;
  d.effect_class = kToolEffectRead;
  d.approval = kToolApprovalNever;
  d.ttl_ms = 0;
  d.handler = slow_sleep_run;
  d.probe = nullptr;
  return &d;
}
