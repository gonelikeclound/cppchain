// tests/wf —— P1.5 声明式配置（config 三形态，arch 25）
//   覆盖：IR 声明区回环 / 命名空间点分（仅声明名导航，存量多级写法零回归）/
//   `${workdir}` 运行变量（config 树注入 + 参数内联）/ fire 合并优先级 /
//   编译期 fail-closed（缺 config_dir / 缺文件 / 坏 JSON / target 未命中 / 未声明引用）/
//   proxyNode 子 workflow 自带声明区的合并路径。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;

namespace {

// 测试配置：一个临时根目录，含标准 config 文件。
struct DeclFixture {
  std::filesystem::path root;
  DeclFixture() : root(std::filesystem::temp_directory_path() /
                       ("wf_cdecl_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(root / "configs");
    write("configs/llm.json", R"({"base_url":"https://llm.example","max_tokens":512})");
    write("configs/tools.json", R"({"hint_root":"${workdir}/data"})");
    write("configs/store.json", R"({"root":"${workdir}/store"})");
  }
  ~DeclFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  void write(std::string const& rel, std::string const& text) const {
    std::ofstream out(root / rel);
    out << text;
  }
};

// 回显工具：记录参数，证明 @config 引用已按声明解析。
struct EchoSink {
  Json last = Json::object();
};
auto make_echo(EchoSink& sink) {
  return make_tool("echo", "echo",
                   Json{{"type", "object"},
                        {"properties",
                         Json{{"url", Json{{"type", "string"}}},
                              {"hint", Json{{"type", "string"}}},
                              {"path", Json{{"type", "string"}}}}}},
                   [&sink](Json const& args, ToolContext&) -> std::expected<Json, ToolError> {
                     sink.last = args;
                     return Json{{"got", args.dump()}};
                   });
}

Json decl_ir() {
  return Json::parse(R"json({
    "id": "wf_cdecl",
    "declarations": {"configs": [
      {"name": "llm", "from": "configs/llm.json"},
      {"name": "tools", "from": "configs/tools.json", "target": "tool"},
      {"name": "store", "from": "configs/store.json"}
    ]},
    "nodes": [
      {"id": "use", "tool": "echo",
       "parameters": {"url": "@config.llm.base_url", "hint": "@config.hint_root"},
       "output_schema": {"got": "string"}, "next": "_turn_end"}
    ]
  })json");
}

}  // namespace

// ---------- refs 级 ----------

TEST(WfConfigDecl, declarations_parse_roundtrip) {
  auto ir = WorkflowIR::parse(decl_ir());
  ASSERT_TRUE(ir) << ir.error();
  ASSERT_EQ(ir->declarations.size(), 3u);
  EXPECT_EQ(ir->declarations[0].name, "llm");
  EXPECT_EQ(ir->declarations[0].from, "configs/llm.json");
  EXPECT_FALSE(ir->declarations[0].target.has_value());
  ASSERT_TRUE(ir->declarations[1].target);
  EXPECT_EQ(*ir->declarations[1].target, "tool");
  // to_json 回环
  auto back = WorkflowIR::parse(ir->to_json());
  ASSERT_TRUE(back) << back.error();
  ASSERT_EQ(back->declarations.size(), 3u);
  EXPECT_EQ(back->declarations[1].name, "tools");
  EXPECT_EQ(back->declarations[2].from, "configs/store.json");
}

TEST(WfConfigDecl, namespace_path_navigates_only_declared) {
  Json cfg = Json{{"llm", Json{{"base_url", "https://a"}, {"max_tokens", 1024}}}};
  cfg["_config_declared"] = Json::array({"llm"});
  Json st = Json{{"config", std::move(cfg)}};
  // 声明名 → 点分导航
  EXPECT_EQ(resolve_parameter(Json("@config.llm.base_url"), st).get<std::string>(),
            "https://a");
  EXPECT_EQ(resolve_parameter(Json("@config.llm.max_tokens"), st).get<int>(), 1024);
  // 整对象引用保留 JSON 类型
  EXPECT_EQ(resolve_parameter(Json("@config.llm"), st)["base_url"], "https://a");
  // 未声明名 + 多级 → 旧模板语义（存量 CGI 提示词零回归："current 的整值" + ".stem"）
  Json st2 = Json{{"config", Json{{"current", "{\"stem\":\"题\"}"}}}};
  EXPECT_EQ(resolve_parameter(Json("@config.current.stem 完全一致"), st2).get<std::string>(),
            "{\"stem\":\"题\"}.stem 完全一致");
  // 声明名下钻到非对象 → null（宽松）
  Json cfg3 = Json{{"llm", "i-am-string"}};
  cfg3["_config_declared"] = Json::array({"llm"});
  Json st3 = Json{{"config", std::move(cfg3)}};
  EXPECT_TRUE(resolve_parameter(Json("@config.llm.base_url"), st3).is_null());
}

TEST(WfConfigDecl, run_var_syntax_and_resolution) {
  // 语法：这三个合法；未知名 / 未闭合 → 语法损坏（编译期 VR1 兜住）
  EXPECT_FALSE(ref_syntax_problem("${workdir}").has_value());
  EXPECT_FALSE(ref_syntax_problem("/o/${sessionId}/x").has_value());
  EXPECT_TRUE(ref_syntax_problem("${wokrdir}").has_value());
  EXPECT_TRUE(ref_syntax_problem("${runId").has_value());
  // 解析：config.workspace / session / runId
  Json st = Json{{"config", Json{{"workspace", "/work/r1"}, {"session", "s9"}, {"runId", "r-3"}}}};
  EXPECT_EQ(resolve_parameter(Json("${workdir}"), st).get<std::string>(), "/work/r1");
  EXPECT_EQ(resolve_parameter(Json("${workdir}/out.md"), st).get<std::string>(), "/work/r1/out.md");
  EXPECT_EQ(resolve_parameter(Json("${sessionId}"), st).get<std::string>(), "s9");
  EXPECT_EQ(resolve_parameter(Json("${runId}"), st).get<std::string>(), "r-3");
  // config 缺该键 → 字面量保留（不合成 null）
  Json st2 = Json{{"config", Json{{"workspace", "/w"}}}};
  EXPECT_EQ(resolve_parameter(Json("${runId}"), st2).get<std::string>(), "${runId}");
}

TEST(WfConfigDecl, inject_run_vars_walks_tree) {
  Json tree = Json{{"dir", "${workdir}/sub"},
                   {"arr", Json::array({"${sessionId}-x"})},
                   {"obj", Json{{"p", "${runId}"}}},
                   {"keep", "${wokrdir}"}};  // 未知名 → 原样
  inject_run_vars(tree, Json{{"workspace", "/w"}, {"session", "s7"}, {"runId", "r-2"}});
  EXPECT_EQ(tree["dir"], "/w/sub");
  EXPECT_EQ(tree["arr"][0], "s7-x");
  EXPECT_EQ(tree["obj"]["p"], "r-2");
  EXPECT_EQ(tree["keep"], "${wokrdir}");
}

// ---------- compile / fire 端到端 ----------

// 声明值做缺省、宿主顶层键覆盖、${workdir} 从宿主 workspace 注入、target 平铺键全局可用。
TEST(WfConfigDecl, fire_merges_declared_and_host) {
  DeclFixture fx;
  EchoSink sink;
  tool::ToolRegistry reg;
  reg.add(make_echo(sink));

  auto ir = WorkflowIR::parse(decl_ir());
  ASSERT_TRUE(ir);
  CompileOptions copts;
  copts.config_dir = fx.root.string();
  auto wf = compile_workflow(*ir, reg, nullptr, copts);
  ASSERT_TRUE(wf) << wf.error().message;
  // 缺省合并：声明文件全部装入缺省 config
  EXPECT_EQ(wf->declared_config["llm"]["base_url"], "https://llm.example");
  EXPECT_EQ(wf->declared_names, (std::vector<std::string>{"llm", "tools", "store"}));

  std::filesystem::path const ws = fx.root / "ws1";
  std::filesystem::create_directories(ws);
  auto r = fire_once(*wf, Json{{"workspace", ws.string()}});
  ASSERT_TRUE(r) << r.error().message;
  // 参数：命名空间点分 url + target 平铺 hint（内含 ${workdir}，fire 已替换）
  EXPECT_EQ(sink.last["url"], "https://llm.example");
  EXPECT_EQ(sink.last["hint"], (ws / "data").string());
  // 最终 state 的 config 可见合并结果（命名空间全对象 + 平铺键 + 声明名单）
  auto const& cfg = (*r)["config"];
  EXPECT_EQ(cfg["llm"]["max_tokens"], 512);
  EXPECT_EQ(cfg["hint_root"], (ws / "data").string());
  EXPECT_EQ(cfg[kConfigDeclaredKey], Json::array({"llm", "tools", "store"}));
}

// 宿主顶层键覆盖声明缺省：命名空间整块覆盖、平铺键逐键覆盖。
TEST(WfConfigDecl, host_overrides_declared) {
  DeclFixture fx;
  EchoSink sink;
  tool::ToolRegistry reg;
  reg.add(make_echo(sink));
  auto ir = WorkflowIR::parse(decl_ir());
  ASSERT_TRUE(ir);
  CompileOptions copts;
  copts.config_dir = fx.root.string();
  auto wf = compile_workflow(*ir, reg, nullptr, copts);
  ASSERT_TRUE(wf);

  auto r = fire_once(*wf, Json{{"llm", Json{{"base_url", "https://host"}}},
                               {"hint_root", "host-hint"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(sink.last["url"], "https://host");   // 宿主整块覆盖命名空间
  EXPECT_EQ(sink.last["hint"], "host-hint");     // 宿主覆盖平铺键
  EXPECT_EQ((*r)["config"]["llm"]["base_url"], "https://host");
}

// ---------- 编译期 fail-closed ----------

TEST(WfConfigDecl, fail_closed_missing_config_dir) {
  EchoSink sink;
  tool::ToolRegistry reg;
  reg.add(make_echo(sink));
  auto ir = WorkflowIR::parse(decl_ir());
  ASSERT_TRUE(ir);
  // config_dir 空 + 带声明 → 编译报错
  auto wf = compile_workflow(*ir, reg);
  ASSERT_FALSE(wf);
  EXPECT_NE(wf.error().message.find("config_dir"), std::string::npos);
}

TEST(WfConfigDecl, fail_closed_bad_files) {
  tool::ToolRegistry reg;
  EchoSink sink;
  reg.add(make_echo(sink));

  // from 文件缺失
  DeclFixture fx1;
  auto ir_bad = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_bad", "declarations": {"configs": [{"name":"nope","from":"configs/nope.json"}]},
    "nodes": [{"id":"use","tool":"echo","parameters":{},"output_schema":{"got":"string"},"next":"_turn_end"}]
  })json"));
  CompileOptions bad1;
  bad1.config_dir = fx1.root.string();
  auto w1 = compile_workflow(*ir_bad, reg, nullptr, bad1);
  ASSERT_FALSE(w1);
  EXPECT_NE(w1.error().message.find("读取失败"), std::string::npos);

  // from 文件坏 JSON
  DeclFixture fx2;
  fx2.write("configs/bad.json", "{ not json !!");
  auto ir_bad2 = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_bad2", "declarations": {"configs": [{"name":"b","from":"configs/bad.json"}]},
    "nodes": [{"id":"use","tool":"echo","parameters":{},"output_schema":{"got":"string"},"next":"_turn_end"}]
  })json"));
  CompileOptions bad2;
  bad2.config_dir = fx2.root.string();
  auto w2 = compile_workflow(*ir_bad2, reg, nullptr, bad2);
  ASSERT_FALSE(w2);
  EXPECT_NE(w2.error().message.find("JSON 解析失败"), std::string::npos);

  // from 文件不是对象（数组）→ 拒绝
  DeclFixture fx3;
  fx3.write("configs/arr.json", "[1,2]");
  auto ir_bad3 = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_bad3", "declarations": {"configs": [{"name":"a","from":"configs/arr.json"}]},
    "nodes": [{"id":"use","tool":"echo","parameters":{},"output_schema":{"got":"string"},"next":"_turn_end"}]
  })json"));
  CompileOptions bad3;
  bad3.config_dir = fx3.root.string();
  auto w3 = compile_workflow(*ir_bad3, reg, nullptr, bad3);
  ASSERT_FALSE(w3);
  EXPECT_NE(w3.error().message.find("必须是 JSON 对象"), std::string::npos);
}

TEST(WfConfigDecl, fail_closed_target_no_hit_and_dup_name) {
  // target 未命中任何节点类型 → 校验 VC3
  auto ir_target = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_t", "declarations": {"configs": [{"name":"a","from":"c.json","target":"no_such_kind"}]},
    "nodes": [{"id":"use","kind":"expr","expr":"true","then":"_turn_end","else":"_turn_end"}]
  })json"));
  ASSERT_TRUE(ir_target);
  ValidationResult vr = validate(*ir_target, {});
  ASSERT_FALSE(vr.ok());
  EXPECT_NE(vr.describe().find("VC3"), std::string::npos);

  // name 重复 → VC2
  auto ir_dup = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_d", "declarations": {"configs": [
      {"name":"a","from":"x.json"},{"name":"a","from":"y.json"}]},
    "nodes": [{"id":"use","kind":"expr","expr":"true","then":"_turn_end","else":"_turn_end"}]
  })json"));
  ASSERT_TRUE(ir_dup);
  vr = validate(*ir_dup, {});
  ASSERT_FALSE(vr.ok());
  EXPECT_NE(vr.describe().find("VC2"), std::string::npos);
}

// 未声明命名空间 + 宿主 config_keys 白名单 → VR4 编译期拦（第一段校验）
TEST(WfConfigDecl, fail_closed_undeclared_first_segment) {
  EchoSink sink;
  tool::ToolRegistry reg;
  reg.add(make_echo(sink));
  DeclFixture fx;
  auto ir = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_u", "nodes": [
      {"id":"use","tool":"echo","parameters":{"path":"@config.nope.x"},
       "output_schema":{"got":"string"},"next":"_turn_end"}
    ]
  })json"));
  ASSERT_TRUE(ir);
  CompileOptions copts;
  copts.config_dir = fx.root.string();
  copts.config_keys = std::vector<std::string>{};  // 空白名单 + 空声明 → 任何 @config 都是未声明
  auto w = compile_workflow(*ir, reg, nullptr, copts);
  ASSERT_FALSE(w);
  EXPECT_NE(w.error().message.find("VR4"), std::string::npos);
}

// ---------- proxyNode 子 workflow 自声明 ----------

// 父调用子（sync），子自带声明区（与父共享 config_dir）；子参数按子声明解析。
TEST(WfConfigDecl, subworkflow_has_own_declarations) {
  DeclFixture fx;
  EchoSink sink;
  tool::ToolRegistry reg;
  reg.add(make_echo(sink));

  // 子 workflow：自己的声明区（sub_llm）+ 工具读 @config.sub_base 与 @config.llm.base_url（父命名空间）
  Json sub_json = Json::parse(R"json({
    "id": "wf_sub", "declarations": {"configs": [
      {"name": "sub_llm", "from": "configs/sub_llm.json"}
    ]},
    "nodes": [
      {"id": "sub_use", "tool": "echo",
       "parameters": {"url": "@config.sub_llm.base_url", "hint": "@config.llm.base_url"},
       "output_schema": {"got": "string"}, "next": "_turn_end"}
    ]
  })json");
  fx.write("configs/sub_llm.json", R"({"base_url":"https://sub.example"})");
  fx.write("sub_workflow.json", sub_json.dump());

  // 父 workflow：subworkflow 节点 sync 调子。父自身也声明 llm（与子共享同一 config_dir，
  // 验证"子链继承父命名空间 + 子自己的声明并存"）。
  Json parent_json = Json::parse(R"json({
    "id": "wf_parent", "declarations": {"configs": [
      {"name": "llm", "from": "configs/llm.json"}
    ]},
    "nodes": [
      {"id": "call_sub", "kind": "subworkflow", "workflow": "sub_workflow.json",
       "mode": "sync", "next": "_turn_end"}
    ]
  })json");

  CompileOptions copts;
  copts.config_dir = fx.root.string();
  copts.subworkflow_loader = [&fx](std::string const& spec) -> std::optional<Json> {
    if (spec == "sub_workflow.json") {
      std::ifstream in(fx.root / spec);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return Json::parse(text);
    }
    return std::nullopt;
  };

  auto ir = WorkflowIR::parse(parent_json);
  ASSERT_TRUE(ir);
  auto wf = compile_workflow(*ir, reg, nullptr, copts);
  // 注意：父没有声明区，config_dir 不强制；但 sub 编译沿用同一 config_dir → 父设置它即可。
  ASSERT_TRUE(wf) << wf.error().message;

  std::filesystem::path const ws = fx.root / "subws";
  std::filesystem::create_directories(ws);
  auto r = fire_once(*wf, Json{{"workspace", ws.string()}});
  ASSERT_TRUE(r) << r.error().message;
  // 子命名空间与父命名空间并存：sub_llm 来自子声明，llm 来自父声明（父已装入 merged config）
  EXPECT_EQ(sink.last["url"], "https://sub.example");
  EXPECT_EQ(sink.last["hint"], "https://llm.example");
}