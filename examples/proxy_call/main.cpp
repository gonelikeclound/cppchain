// demo: proxy_call —— 运行期调用子 workflow（wfr-㊿ proxyNode，arch 24，纯离线）
//
// 主 workflow（proxy_main）编排两个子 workflow：
//   ① sync   —— 子 workflow sub_calc（对两个数求和，工具 sum）同步跑完，
//               结果提升为 results.sub.calc → 主流程用 `$sub.result.calc`
//               引用（expr 门验证 a>0 且子结果>0，走正常路径）。
//   ② detach —— 子 workflow sub_audit（echo 一个备注）fire-and-forget：
//               主流程立即继续（expr 证实 detached==true），子流程后台跑、
//               结果落盘 `<workspace>/.wfr/subflows/sub_audit.json`（可观测）。
//
// 子 workflow 装载走**内存 loader**（spec → 预置 JSON），不碰文件系统 —— 全离线可复现。
// 关键语义（见 arch 24 §四）：
//   · 子 workflow 不进主会话日志（隔离）；sync 结果经 results.<id> 回传；
//   · config 同源：子 workflow 的 @config.* 用主 workflow 同一个 config 对象。
#include <cppchain/message/serializer.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/import.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/validator.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <thread>
#include <string>

#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

Json sub_calc_json() {  // 子 workflow：sum 工具（离线确定性）
  return Json{{"id", "sub_calc"},
              {"nodes", Json::array({Json{{"id", "calc"},
                                          {"kind", "tool"},
                                          {"tool", "sum"},
                                          {"parameters", Json{{"a", "@config.a"},
                                                              {"b", "@config.b"}}},
                                          {"next", "_turn_end"}}})}};
}

Json sub_audit_json() {  // 子 workflow：echo 备注（detach：结果不进主流程，落盘审计）
  return Json{{"id", "sub_audit"},
              {"nodes", Json::array({Json{{"id", "note"},
                                          {"kind", "tool"},
                                          {"tool", "echo"},
                                          {"parameters", Json{{"v", "@config.audit_note"}}},
                                          {"next", "_turn_end"}}})}};
}

Json proxy_main_json() {
  return Json{{"id", "proxy_main"},
              {"nodes",
               Json::array(
                   {Json{{"id", "sub"},
                         {"kind", "subworkflow"},
                         {"workflow", "sub_calc"},
                         {"mode", "sync"},
                         {"next", "out1"}},
                    Json{{"id", "out1"},
                         {"kind", "expr"},
                         {"expr", "@config.a > 0 && $sub.result.calc != null"},
                         {"then", "detach"},
                         {"else", "_fail"}},
                    Json{{"id", "detach"},
                         {"kind", "subworkflow"},
                         {"workflow", "sub_audit"},
                         {"mode", "detach"},
                         {"next", "out2"}},
                    Json{{"id", "out2"},
                         {"kind", "expr"},
                         {"expr", "$detach.result.detached == true"},
                         {"then", "_turn_end"},
                         {"else", "_fail"}}})}};
}

}  // namespace

int main() {
  // 离线假工具（sum / echo / …）：同 examples 评测设施，不使用模型。
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);

  // 子 workflow 装载器：spec → 内存 JSON（找不到 → nullopt → 编译失败，fail-closed）。
  auto loader = [](std::string const& spec) -> std::optional<Json> {
    if (spec == "sub_calc") return std::optional<Json>{sub_calc_json()};
    if (spec == "sub_audit") return std::optional<Json>{sub_audit_json()};
    return std::nullopt;
  };

  Json const ir_json = proxy_main_json();
  auto const parsed = WorkflowIR::parse(ir_json);
  if (!parsed) {
    std::println("IR 解析失败: {}", parsed.error());
    return 1;
  }

  auto const v = validate(*parsed, ValidatorOptions{&registry, std::nullopt, nullptr});
  if (!v.ok()) {
    std::println("校验失败: {}", v.describe());
    return 1;
  }

  CompileOptions copts;
  copts.components = nullptr;
  copts.subworkflow_loader = loader;   // wfr-㊿：子 workflow 装载（编译期解析子 IR + 编译）
  auto wf = compile_workflow(*parsed, registry, /*cp=*/nullptr, copts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 1;
  }

  // 主 workflow 的 config（子 workflow 同源共享）
  Json const config{{"workspace", "."}, {"session", "proxy_demo"},
                    {"a", 3}, {"b", 4}, {"audit_note", "proxyNode detach 落盘审计"}};

  auto fire = fire_once(*wf, config);
  if (!fire) {
    std::println("运行失败: {}", fire.error().message);
    return 1;
  }
  Json const& st = *fire;

  std::println("=== proxyNode demo（wfr-㊿）===");
  std::println("sync 回传: results.sub.calc.value = {}",
               st["results"]["sub"]["calc"].value("value", 0.0));
  Json const& proxy = st["results"]["sub"]["_proxy"];
  std::println("sync 元信息: mode={} workflow={} ok={}",
               proxy.value("mode", ""), proxy.value("workflow", ""),
               proxy.value("ok", false));
  std::println("detach 返回: results.detach.detached = {}",
               st["results"]["detach"].value("detached", false));

  // 等 detach 后台线程落盘审计（fire-and-forget 也要可观测）。
  std::filesystem::path const audit = ".wfr/subflows/sub_audit.json";
  for (int i = 0; i < 100 && !std::filesystem::exists(audit); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (std::filesystem::exists(audit)) {
    Json saved = Json{{"ok", false}};
    std::ifstream in(audit);
    try {
      in >> saved;
    } catch (...) {
    }
    std::println("detach 审计落盘: {}（ok={} flow={}）",
                 audit.string(), saved.value("ok", false), saved.value("flow", ""));
  } else {
    std::println("detach 审计落盘: <未等到 {}（后台线程可能仍在跑/权限不足）>", audit.string());
  }
  return 0;
}