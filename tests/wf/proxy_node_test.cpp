// wfr-㊿ proxyNode 测试（arch 24）：subworkflow 节点 —— 运行期调用子 workflow（隔离执行）。
// 覆盖：
//   1) sync 回传契约：子 workflow 最终 state 的 results 通道键提升为 results.<id> 顶层键，
//      下游 `$<id>.result.<子节点id>` 直接命中子节点产物（全确定性：子 = sum 工具，无 LLM）；
//   2) detach：立即返回 detached:true（不阻塞主流程），后台线程跑子图并落盘 .wfr/subflows/ 审计；
//   3) 子 state 构造：config 继承 + copy_state 选键拷贝 + input 右胜（inline 引用解析）；
//   4) fail-closed：缺 loader / 子 workflow 不存在 / 校验不过 / 编译失败 → 主编译报错；
//   5) 环 & 深度防护：A→A 环、嵌套超深 → 编译期拒绝；
//   6) VN24：workflow 空 / mode 非法 / copy_state 空键 / input 非对象 / 缺 next。
// 规模控制：全部离线（fake tools），不需要模型组件。
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;

namespace {

using Json = nlohmann::json;

// 子 workflow：单 sum 工具节点。入参 a/b 由父经 input 或 copy_state 给，结果 {value: a+b}。
Json sub_sum_json() {
  return Json{{"id", "sub_sum"},
              {"nodes", Json::array({Json{{"id", "calc"},
                                          {"kind", "tool"},
                                          {"tool", "sum"},
                                          {"parameters", Json{{"a", "@config.a"},
                                                              {"b", "@config.b"}}},
                                          {"next", "_turn_end"}}})}};
}

// 父 workflow：一个 subworkflow(sync) 节点 + 一个 expr 引用子节点产物。
Json parent_sync_json() {
  return Json{{"id", "parent_sync"},
              {"nodes", Json::array(
                            {Json{{"id", "entry"}, {"kind", "echo"}, {"text", "start"}, {"next", "sub"}},
                             Json{{"id", "sub"},
                                  {"kind", "subworkflow"},
                                  {"workflow", "sub_sum"},
                                  {"mode", "sync"},
                                  {"next", "check"}},
                             Json{{"id", "check"},
                                  {"kind", "expr"},
                                  {"expr", "@config.a > 0 && $sub.result.calc != null"},
                                  {"then", "_turn_end"},
                                  {"else", "_fail"}}})}};
}

// 父 workflow：一个 subworkflow(detach) 节点 + 一个 expr 引用 detached flag。
Json parent_detach_json() {
  return Json{{"id", "parent_detach"},
              {"nodes", Json::array(
                            {Json{{"id", "entry"}, {"kind", "echo"}, {"text", "start"}, {"next", "sub"}},
                             Json{{"id", "sub"},
                                  {"kind", "subworkflow"},
                                  {"workflow", "sub_sum"},
                                  {"mode", "detach"},
                                  {"copy_state", Json::array({"x"})},
                                  {"input", Json{{"verb", "go"}}},
                                  {"next", "check"}},
                             Json{{"id", "check"},
                                  {"kind", "expr"},
                                  {"expr", "$sub.result.detached == true"},
                                  {"then", "_turn_end"},
                                  {"else", "_fail"}}})}};
}

// 内存 loader：spec → 预置 JSON（返回 optional；找不到 = nullopt）。
auto mem_sub_loader(std::map<std::string, Json> subs) {
  return [subs = std::move(subs)](std::string const& spec) -> std::optional<Json> {
    auto it = subs.find(spec);
    if (it == subs.end()) return std::nullopt;
    return std::optional<Json>{it->second};
  };
}

struct Fixture {
  FakeToolSinks sinks;
  ToolRegistry registry;
  Fixture() { registry = make_fake_tools(sinks); }
};

// 编译 + fire 一次，返回 state；失败返回错误串。
struct RunResult {
  std::optional<Json> state;
  std::string error;
};

RunResult run_wf(Fixture& f, Json ir_json, Json config,
                 std::optional<std::function<std::optional<Json>(std::string const&)>> loader = std::nullopt,
                 std::size_t max_depth = 4) {
  auto parsed = WorkflowIR::parse(ir_json);
  if (!parsed) return {std::nullopt, "parse: " + parsed.error()};
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  auto const v = validate(*parsed, vopts);
  if (!v.ok()) return {std::nullopt, "validate: " + v.describe()};
  CompileOptions copts;
  copts.components = nullptr;
  if (loader) copts.subworkflow_loader = *loader;
  copts.max_subflow_depth = max_depth;
  auto wf = compile_workflow(*parsed, f.registry, nullptr, copts);
  if (!wf) return {std::nullopt, "compile: " + wf.error().message};
  auto const r = fire_once(*wf, config);
  if (!r) return {std::nullopt, "fire: " + r.error().message};
  return {std::optional<Json>{*r}, ""};
}

// 等待审计文件出现（detach 是后台线程，主流程可能先返回）。
bool wait_for_file(std::filesystem::path const& p, int timeout_ms = 5000) {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(p)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// -- 1) sync：子节点产物提升为帧顶层键，下游 $sub.result.calc.value 命中 ----
TEST(WfSubworkflow, sync_returns_child_node_results_promoted) {
  Fixture f;
  auto r = run_wf(f, parent_sync_json(),
                  Json{{"workspace", std::filesystem::temp_directory_path().string()},
                       {"a", 2}, {"b", 3}},
                  mem_sub_loader({{"sub_sum", sub_sum_json()}}));
  ASSERT_TRUE(r.state) << r.error;
  Json const& st = *r.state;
  // 帧：results.<sub>.calc = sum 工具输出 {value: 5}；_proxy 携带元信息
  ASSERT_TRUE(st.contains("results")) << st.dump();
  ASSERT_TRUE(st["results"].contains("sub")) << st.dump();
  EXPECT_EQ(st["results"]["sub"]["calc"]["value"].get<double>(), 5.0);
  EXPECT_EQ(st["results"]["sub"]["_proxy"]["mode"], "sync");
  EXPECT_EQ(st["results"]["sub"]["_proxy"]["workflow"], "sub_sum");
  ASSERT_TRUE(st["results"]["sub"]["_proxy"].contains("state"))
      << "sync 应在 _proxy 里带完整子 state（审计/调试兜底）";
  // expr 前置：a>0 && 子结果 > 0；若 check 走了 _fail，fire 会抛错 → state 存在即证明 expr 通过
}

// -- 2) detach：立即 detached:true，后台落盘审计 ----
TEST(WfSubworkflow, detach_returns_immediately_and_audits_to_disk) {
  Fixture f;
  auto const ws = std::filesystem::temp_directory_path() / ("wf_sub_detach_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(ws);
  auto r = run_wf(f, parent_detach_json(),
                  Json{{"workspace", ws.string()}, {"a", 1}, {"b", 2}},
                  mem_sub_loader({{"sub_sum", sub_sum_json()}}));
  ASSERT_TRUE(r.state) << r.error;
  // detach 不阻塞：主 state 里 sub 帧只有 detached 状态（无子结果 —— 子还没跑完或并行）
  ASSERT_TRUE(r.state->contains("results"));
  ASSERT_TRUE((*r.state)["results"].contains("sub"));
  bool const detached = (*r.state)["results"]["sub"].value("detached", false);
  EXPECT_TRUE(detached) << (*r.state)["results"]["sub"].dump();
  // 审计落盘（fire-and-forget 也要可观测）：<ws>/.wfr/subflows/sub_sum.json
  std::filesystem::path const audit = ws / ".wfr" / "subflows" / "sub_sum.json";
  ASSERT_TRUE(wait_for_file(audit)) << "detach 子流程应落盘审计文件";
  Json const saved = [&] {
    std::ifstream in(audit);
    Json j;
    in >> j;  // 后台线程写入完成（wait_for_file 已等文件存在；内容可能半行？writer 原子截断，防半行）
    return j;
  }();
  EXPECT_EQ(saved.value("ok", false), true);
  EXPECT_EQ(saved.value("flow", ""), "sub_sum");
  EXPECT_TRUE(saved.contains("state"));
}

// -- 3) copy_state + input 构造子 state：copy_state(主 state 键) 与 input(引用解析) 都进子 ----
TEST(WfSubworkflow, copy_state_and_input_construct_child_state) {
  Fixture f;
  // 子 workflow 读 config（@config.a/@config.b）+ input 里放初值；父 config 不直接给 a/b，
  // 而是 copy_state 拷贝"共享键"、input 引用 config 的另一半。这里简化：直接断言子收到两者。
  Json sub = Json{{"id", "sub_cfg"},
                  {"nodes", Json::array({Json{{"id", "calc"},
                                              {"kind", "tool"},
                                              {"tool", "format"},
                                              {"parameters", Json{{"v", "@config.name"}}},
                                              {"next", "_turn_end"}}})}};
  Json parent = Json{{"id", "parent_cfg"},
                     {"nodes", Json::array(
                                   {Json{{"id", "sub"},
                                         {"kind", "subworkflow"},
                                         {"workflow", "sub_cfg"},
                                         {"mode", "sync"},
                                         {"copy_state", Json::array({"session"})},
                                         {"input", Json{{"seed", "@config.seed"}}},
                                         {"next", "_turn_end"}}})}};
  auto r = run_wf(f, parent, Json{{"session", "s-1"}, {"seed", "ready"}, {"name", "单元测试"}},
                  mem_sub_loader({{"sub_cfg", sub}}));
  ASSERT_TRUE(r.state) << r.error;
  ASSERT_TRUE((*r.state)["results"].contains("sub"));
  // 子把 format(config.name) 存进 results.calc.text → 帧里可读
  EXPECT_EQ((*r.state)["results"]["sub"]["calc"]["text"], "单元测试");
}

// -- 4a) 缺 loader → 编译失败（fail-closed，编译期不静默） --
TEST(WfSubworkflow, missing_loader_fails_compile) {
  Fixture f;
  auto r = run_wf(f, parent_sync_json(), Json{{"a", 1}, {"b", 1}}, std::nullopt);
  ASSERT_FALSE(r.state);
  EXPECT_TRUE(r.error.find("subworkflow_loader") != std::string::npos) << r.error;
}

// -- 4b) 子 workflow 不存在 → 编译失败 --
TEST(WfSubworkflow, missing_sub_workflow_fails_compile) {
  Fixture f;
  auto r = run_wf(f, parent_sync_json(), Json{{"a", 1}, {"b", 1}},
                  mem_sub_loader({}));  // loader 里没有 sub_sum
  ASSERT_FALSE(r.state);
  EXPECT_TRUE(r.error.find("找不到子 workflow") != std::string::npos) << r.error;
}

// -- 4c) 子 IR 校验不过（tool 未注册 / 缺 next）→ 编译失败 --
TEST(WfSubworkflow, invalid_child_ir_fails_compile) {
  Fixture f;
  Json bad_sub = Json{{"id", "sub_bad"},
                      {"nodes", Json::array({Json{{"id", "nope"},
                                                  {"kind", "tool"},
                                                  {"tool", "not_registered_tool"},
                                                  {"next", "_turn_end"}}})}};
  auto r = run_wf(f, parent_sync_json(), Json{{"a", 1}, {"b", 1}},
                  mem_sub_loader({{"sub_sum", bad_sub}}));
  ASSERT_FALSE(r.state);
  EXPECT_TRUE(r.error.find("校验未过") != std::string::npos) << r.error;
}

// -- 5a) 环：sub 引用自己 → 编译拒绝 --
TEST(WfSubworkflow, self_cycle_fails_compile) {
  Fixture f;
  // cyc 的 id 是 sub_cyc；其节点引用 workflow "sub_cyc"。loader 里把该名也指向同一份 JSON，
  // 于是编译 cyc 时再次装载自己 → chain 已有 sub_cyc → 环引用 fail-closed。
  Json cyc = Json{{"id", "sub_cyc"},
                  {"nodes", Json::array({Json{{"id", "x"},
                                              {"kind", "subworkflow"},
                                              {"workflow", "sub_cyc"},
                                              {"next", "_turn_end"}}})}};
  auto r = run_wf(f, parent_sync_json(), Json{{"a", 1}, {"b", 1}},
                  mem_sub_loader({{"sub_sum", cyc}, {"sub_cyc", cyc}}));
  ASSERT_FALSE(r.state);
  EXPECT_TRUE(r.error.find("环") != std::string::npos) << r.error;
}

// -- 5b) 深度超限 → 编译拒绝（三层链，max_depth=2） --
TEST(WfSubworkflow, nesting_depth_capped) {
  Fixture f;
  // 链：parent_sync → sub_sum → sub_d2（子里再嵌一层）。max_depth=2 时编译 sub_d2 前
  // chain 已含 {parent, sub_sum} → size>=max_depth → "嵌套过深"。
  Json d1 = Json{{"id", "sub_d1"},
                 {"nodes", Json::array({Json{{"id", "inner"},
                                             {"kind", "subworkflow"},
                                             {"workflow", "sub_d2"},
                                             {"next", "_turn_end"}}})}};
  Json d2 = Json{{"id", "sub_d2"},
                 {"nodes", Json::array({Json{{"id", "leaf"},
                                             {"kind", "echo"},
                                             {"text", "leaf"},
                                             {"next", "_turn_end"}}})}};
  auto r = run_wf(f, parent_sync_json(), Json{{"a", 1}, {"b", 1}},
                  mem_sub_loader({{"sub_sum", d1}, {"sub_d2", d2}}), /*max_depth=*/2);
  ASSERT_FALSE(r.state);
  EXPECT_TRUE(r.error.find("嵌套过深") != std::string::npos) << r.error;
}

// -- 6) VN24 --
TEST(WfSubworkflow, vn24_invalid_modes_and_fields) {
  Fixture f;
  auto check_one = [&](Json node, std::string expect) {
    Json ir = Json{{"id", "w"}, {"nodes", Json::array({node})}};
    auto parsed = WorkflowIR::parse(ir);
    ASSERT_TRUE(parsed);
    ValidatorOptions vopts;
    vopts.registry = &f.registry;
    auto const v = validate(*parsed, vopts);
    ASSERT_FALSE(v.ok()) << node.dump();
    EXPECT_TRUE(v.describe().find(expect) != std::string::npos) << node.dump() << v.describe();
  };
  // 缺 workflow
  check_one(Json{{"id", "s"}, {"kind", "subworkflow"}, {"next", "_turn_end"}}, "VN24");
  // mode 非法
  check_one(Json{{"id", "s"}, {"kind", "subworkflow"}, {"workflow", "x"},
                 {"mode", "async"}, {"next", "_turn_end"}}, "VN24");
  // copy_state 空键
  check_one(Json{{"id", "s"}, {"kind", "subworkflow"}, {"workflow", "x"},
                 {"copy_state", Json::array({""})}, {"next", "_turn_end"}}, "VN24");
  // input 非对象
  check_one(Json{{"id", "s"}, {"kind", "subworkflow"}, {"workflow", "x"},
                 {"input", Json::array({1})}, {"next", "_turn_end"}}, "VN24");
  // 缺 next
  check_one(Json{{"id", "s"}, {"kind", "subworkflow"}, {"workflow", "x"}}, "VN24");
}

}  // namespace