// ⑤ 动态库导入（docs/02-arch-design/12 §5，A0 按名 dlsym）E2E 与 fail-closed 收口。
//
// 被测 .so：tests/plugin_demo/node_demo_plugin.cpp（CMake 构建，路径经
// CPPCHAIN_NODE_DEMO_SO bake 进来）。它导出三个 creator：
//   sentiment —— 好路径（名实相符 / version 匹配）
//   misnamed  —— 符号 misnamed 但 descriptor.name="sentiment" → 改名漏检自校验
//   ancient   —— version=0 ≠ kNodeApiVersion → ABI 版本校验
//
// 副带证明（这也是把 ①②③ 的设计串起来的关节点）：
//   · 动态节点的 edges() 喂给 VG1（目标存在）→ 与内置节点共用通用图规则
//   · 动态节点的 ref_texts() 喂给 VR1/VR4（config 契约）→ 引用收集不因节点类型漏检
//   · 动态节点经 LoggingStateGraph 自动埋结构事件（编译走 ③ 的 add_node 封装）
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/plugin_node.hpp"
#include "cppchain/workflow/refs.hpp"  // kMessagesKey
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

// sentiment 好路径 IR：imports + 一个 sentiment 节点（score/query/next）。
// 注意：nodes 元素必须显式构造成 Json 对象，否则 `Json::array({{...k:v...}})` 会被
// nlohmann 误解成"多个单键对象组成的数组"。
Json sentiment_ir() {
  Json node = Json::object();
  node["id"] = "s";
  node["kind"] = "sentiment";
  node["score"] = 3;
  node["query"] = "@config.q";
  node["next"] = "_turn_end";

  Json import = Json::object();
  import["lib"] = CPPCHAIN_NODE_DEMO_SO;
  import["nodes"] = Json::array({"sentiment"});

  return Json{{"id", "wf_plugin"},
              {"trigger", Json{{"source", "time"},
                               {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
              {"imports", Json::array({std::move(import)})},
              {"nodes", Json::array({std::move(node)})}};
}

// 确保 sentiment 已注册（load_dynamic_node 幂等：重复加载按成功处理）。
void ensure_sentiment() {
  auto r = load_dynamic_node(CPPCHAIN_NODE_DEMO_SO, "sentiment");
  ASSERT_TRUE(r) << r.error();
}

}  // namespace

// ---- E2E：import → parse → validate → compile → fire ----

TEST(WfPlugin, import_and_run) {
  Json const raw = sentiment_ir();

  // 1) 加载 imports（fail-closed 驱动，含 host_log 于此处不带日志的普通版）
  auto loaded = load_workflow_imports(raw, /*allow_import=*/true, nullptr);
  ASSERT_TRUE(loaded) << loaded.error();
  EXPECT_EQ(*loaded, 1u);

  // 2) parse：判别不再报"未知 kind"—— kind:"sentiment" 走导入表
  auto ir = WorkflowIR::parse(raw);
  ASSERT_TRUE(ir) << ir.error();
  EXPECT_EQ(ir->nodes[0].kind, NodeKind::Dynamic);

  // 3) validate：动态节点自己的校验 + 通用图规则 + config 契约
  ValidatorOptions vo;
  vo.config_keys = std::vector<std::string>{"q", "workspace", "session"};
  auto vr = validate(*ir, vo);
  ASSERT_TRUE(vr.ok()) << vr.describe();

  // 4) compile + fire：sentiment 节点向 messages 追加一条消息（编译走注册表分派）
  tool::ToolRegistry const reg;
  auto wf = compile_workflow(*ir, reg, nullptr, CompileOptions{});
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, Json{{"q", "any"}});
  ASSERT_TRUE(r) << r.error().message;
  ASSERT_TRUE(r->contains(kMessagesKey));
  EXPECT_EQ((*r)[std::string(kMessagesKey)].size(), 1u);
  EXPECT_EQ((*r)[std::string(kMessagesKey)][0]["content"][0]["text"].get<std::string>(),
            "sentiment scored 3")
      << "sentiment 节点的 compile 应真正跑起来，消息内容应含 score";
  EXPECT_EQ(wf->registered_nodes(), 1u);

  // 5) to_json 往返：动态节点的字段（kind/score/query/next）不丢
  auto j = ir->to_json();
  ASSERT_TRUE(j.contains("imports"));
  EXPECT_EQ(j["nodes"][0]["score"], 3);
  EXPECT_EQ(j["nodes"][0]["query"], "@config.q");
  EXPECT_EQ(j["nodes"][0]["kind"], "sentiment");
}

// kind 名不 import 就用不起来（"kind 就是它的名字"，但名字必须先注册）
TEST(WfPlugin, unknown_kind_requires_import) {
  auto raw = sentiment_ir();
  raw["nodes"][0]["kind"] = "no_such_plugin";
  auto ir = WorkflowIR::parse(raw);
  ASSERT_FALSE(ir) << "未导入的 kind 名不应被接受";
  EXPECT_NE(ir.error().find("no_such_plugin"), std::string::npos) << ir.error();
}

// ---- fail-closed：加载的每个拒绝分支 ----

TEST(WfPlugin, import_rejected_without_allow) {
  auto r = load_workflow_imports(sentiment_ir(), /*allow_import=*/false, nullptr);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("--allow-import"), std::string::npos) << r.error();
}

TEST(WfPlugin, dlsym_miss_rejected_fail_closed) {
  auto raw = sentiment_ir();
  raw["imports"][0]["nodes"] = Json::array({"ghost"});  // .so 里没有 graph_node_ghost_creator
  auto r = load_workflow_imports(raw, true, nullptr);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("graph_node_ghost_creator"), std::string::npos) << r.error();
}

TEST(WfPlugin, name_mismatch_rejected) {
  // 符号 misnamed 但 descriptor.name="sentiment" → 按名加载时立刻被发现（§5.1 name 的存在理由）
  auto r = load_dynamic_node(CPPCHAIN_NODE_DEMO_SO, "misnamed");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("descriptor.name"), std::string::npos) << r.error();
}

TEST(WfPlugin, version_mismatch_rejected) {
  // ancient 的 version=0 ≠ kNodeApiVersion → ABI 校验拒（§5.1 version 的存在理由）
  auto r = load_dynamic_node(CPPCHAIN_NODE_DEMO_SO, "ancient");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("版本不匹配"), std::string::npos) << r.error();
}

// 重复导入幂等：同一库反复 import 不炸（多 workflow 共享进程的场景）
TEST(WfPlugin, reimport_is_idempotent) {
  ASSERT_TRUE(load_dynamic_node(CPPCHAIN_NODE_DEMO_SO, "sentiment"));
  auto r = load_workflow_imports(sentiment_ir(), true, nullptr);
  ASSERT_TRUE(r) << r.error();
}

// ---- 动态节点的 edges() / ref_texts() 也走通用规则（①② 的成果对插件同样生效）----

TEST(WfPlugin, plugin_edges_feed_vg_rules) {
  ensure_sentiment();
  auto raw = sentiment_ir();
  raw["nodes"][0]["next"] = "nope";  // 不存在的目标

  auto ir = WorkflowIR::parse(raw);
  ASSERT_TRUE(ir) << ir.error();
  auto r = validate(*ir, {});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VG1"), std::string::npos)
      << "动态节点的 edges() 应喂给 VG1（目标存在）" << r.describe();
}

TEST(WfPlugin, plugin_ref_texts_feed_vr_contract) {
  ensure_sentiment();
  auto raw = sentiment_ir();
  raw["nodes"][0]["query"] = "@config.wrokspace";  // 故意拼错

  auto ir = WorkflowIR::parse(raw);
  ASSERT_TRUE(ir) << ir.error();
  ValidatorOptions vo;
  vo.config_keys = std::vector<std::string>{"q"};  // 契约里没有 wrokspace
  auto r = validate(*ir, vo);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VR4"), std::string::npos)
      << "动态节点的 ref_texts() 应喂给 VR4（config 契约）" << r.describe();
}

// ---- imports 形状（parse 层 + validator 的 VI1 兜底）----

TEST(WfPlugin, imports_empty_nodes_rejected_by_loader) {
  // 空数组 ≠ 省略（省略走 A1 枚举）；空数组是类型错误，loader 拒
  auto raw = sentiment_ir();
  raw["imports"][0]["nodes"] = Json::array();
  auto r = load_workflow_imports(raw, true, nullptr);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("空数组"), std::string::npos) << r.error();
}

TEST(WfPlugin, imports_shape_violates_vi1) {
  // 程序化构造的 IR（不经 parse）→ validator 的 VI1 兜底形状（缺 lib）
  WorkflowIR ir;
  ir.id = "x";
  TriggerIR trig;
  trig.source = "time";
  trig.kind = "schedule";
  trig.schedule = "0 8 * * *";
  ir.trigger = trig;
  ir.imports.push_back(ImportIR{/*lib=*/"", /*nodes=*/{"ultra"}});
  NodeIR node;  // 逐字段赋值（designated 初始化会触发 -Wmissing-field-initializers）
  node.id = "L";
  node.kind = NodeKind::Label;
  node.raw = Json{{"id", "L"}, {"kind", "label"}};
  ir.nodes.push_back(std::move(node));

  auto r = validate(ir, {});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VI1"), std::string::npos) << r.describe();
}

// ---- ⑤b（A1）：枚举本库自带符号 + 省略 nodes 走整库加载 ----
// 用"全好"库 tests/plugin_demo/node_enum_plugin.cpp（blink / nudge）：
// 枚举的每个节点都能干净加载；带坏 creator 的库（node_demo_plugin）省略加载会整体失败，
// 这种库只能显式列名 —— 这正是"库是信任单元"的本意。

TEST(WfPlugin, enumerate_lists_defined_creators) {
  // A1 只扫**本库定义**的 graph_node_*_creator（不看进程里其它库的同名符号）
  auto e = enumerate_plugin_nodes(CPPCHAIN_NODE_DEMO_SO);
  ASSERT_TRUE(e) << e.error();
  std::vector<std::string> const expect{"ancient", "misnamed", "sentiment"};
  EXPECT_EQ(*e, expect);
}

TEST(WfPlugin, enumerate_reads_file_not_executes) {
  // 枚举是读 ELF 文件：即使库里掺坏 creator，也不执行任何库代码 → --list-nodes 无需放行
  auto e = enumerate_plugin_nodes(CPPCHAIN_NODE_DEMO_SO);
  ASSERT_TRUE(e) << e.error();
  EXPECT_EQ(e->size(), 3u);
}

TEST(WfPlugin, import_without_nodes_enumerates_and_runs) {
  Json node = Json::object();
  node["id"] = "b";
  node["kind"] = "blink";
  node["next"] = "_turn_end";
  Json import = Json::object();
  import["lib"] = CPPCHAIN_NODE_ENUM_SO;  // 省略 nodes → A1 枚举整库
  Json const raw = Json{{"id", "wf_a1"},
                        {"trigger", Json{{"source", "time"},
                                         {"timing", Json{{"kind", "schedule"},
                                                         {"schedule", "0 8 * * *"}}}}},
                        {"imports", Json::array({std::move(import)})},
                        {"nodes", Json::array({std::move(node)})}};

  auto loaded = load_workflow_imports(raw, /*allow_import=*/true, nullptr);
  ASSERT_TRUE(loaded) << loaded.error();
  EXPECT_GE(*loaded, 2u);  // blink + nudge
  EXPECT_NE(node_type_of_name("blink"), nullptr);
  EXPECT_NE(node_type_of_name("nudge"), nullptr);

  auto ir = WorkflowIR::parse(raw);
  ASSERT_TRUE(ir) << ir.error();
  EXPECT_EQ(ir->nodes[0].kind, NodeKind::Dynamic);
  auto vr = validate(*ir, {});
  ASSERT_TRUE(vr.ok()) << vr.describe();

  tool::ToolRegistry const reg;
  auto wf = compile_workflow(*ir, reg, nullptr, CompileOptions{});
  ASSERT_TRUE(wf) << wf.error().message;
  auto r = fire_once(*wf, Json::object());
  ASSERT_TRUE(r) << r.error().message;
  ASSERT_TRUE(r->contains(kMessagesKey));
  EXPECT_EQ((*r)[std::string(kMessagesKey)][0]["content"][0]["text"].get<std::string>(), "blink");
}

TEST(WfPlugin, enumeration_failure_is_fail_closed) {
  Json import = Json::object();
  import["lib"] = "/nonexistent/lib_plugin.so";  // 省略 nodes，但库文件读不了
  Json const raw = Json{{"imports", Json::array({std::move(import)})}};

  auto r = load_workflow_imports(raw, true, nullptr);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("无法打开库文件"), std::string::npos) << r.error();
}