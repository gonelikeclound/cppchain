// tests/wf —— IR 解析与校验器（workflow/ir + workflow/validator）
#include <gtest/gtest.h>

#include <string>

#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::tool::ToolRegistry;

namespace {

ToolRegistry const& tools() {
  // 静态注册表：validator/compiler 只读它（find），返回稳定 lvalue 供取地址
  static FakeToolSinks sinks;
  static ToolRegistry reg = make_fake_tools(sinks);
  return reg;
}

Json node(std::string id, std::string expr, std::string then, std::string els) {
  return Json{{"id", id}, {"expr", expr}, {"then", then}, {"else", els}};
}

// 一份最小合法 IR（线性价格检查）
Json minimal_price_ir() {
  return Json{
      {"id", "wf_t"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8,20 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "fetch"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"price", "number"}, {"in_stock", "boolean"}}},
                          {"next", "check"}},
                     node("check", "$_.result.price != null && $_.result.price < 800", "notify",
                          "_turn_end"),
                     Json{{"id", "notify"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "降价了"}}},
                          {"output_schema", Json{{"ok", "boolean"}}},
                          {"next", "_turn_end"}}})}};
}

}  // namespace

// ---- parse / classify ----

TEST(WfIr, parse_minimal) {
  auto ir = WorkflowIR::parse(minimal_price_ir());
  ASSERT_TRUE(ir) << ir.error();
  EXPECT_EQ(ir->id, "wf_t");
  EXPECT_EQ(ir->trigger.source, "time");
  EXPECT_EQ(ir->trigger.schedule, "0 8,20 * * *");
  ASSERT_EQ(ir->nodes.size(), 3u);
  EXPECT_EQ(ir->nodes[0].kind, NodeKind::Tool);
  EXPECT_EQ(ir->nodes[1].kind, NodeKind::Expr);
  EXPECT_EQ(ir->nodes[1].then, "notify");
}

TEST(WfIr, parse_errors) {
  EXPECT_FALSE(WorkflowIR::parse(Json{{"trigger", Json{{"source", "time"}}},
                                     {"nodes", Json::array()}})
                   .has_value());  // 缺 id
  EXPECT_FALSE(WorkflowIR::parse(Json{{"id", "x"}}).has_value());  // 缺 trigger/nodes
}

TEST(WfIr, classify_precedence) {
  EXPECT_EQ(WorkflowIR::classify(Json{{"kind", "label"}}), NodeKind::Label);
  EXPECT_EQ(WorkflowIR::classify(Json{{"goto", "l"}}), NodeKind::Goto);
  EXPECT_EQ(WorkflowIR::classify(Json{{"expr", "a"}}), NodeKind::Expr);
  EXPECT_EQ(WorkflowIR::classify(Json{{"tool", "t"}}), NodeKind::Tool);
  EXPECT_FALSE(WorkflowIR::classify(Json{{"expr", "a"}, {"tool", "t"}}).has_value());  // 歧义
  EXPECT_FALSE(WorkflowIR::classify(Json{{"kind", "label"}, {"tool", "t"}}).has_value());
  EXPECT_FALSE(WorkflowIR::classify(Json{{"id", "x"}}).has_value());  // 无决定性键
}

TEST(WfIr, label_and_goto_parse) {
  auto ir = WorkflowIR::parse(Json{
      {"id", "wf_l"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 * * * *"}}}}},
      {"nodes", Json::array({Json{{"id", "l0"}, {"kind", "label"}},
                             Json{{"id", "gp"}, {"goto", "l0"}, {"max", 4}}})}});
  ASSERT_TRUE(ir);
  EXPECT_EQ(ir->nodes[0].kind, NodeKind::Label);
  EXPECT_EQ(ir->nodes[1].kind, NodeKind::Goto);
  EXPECT_EQ(ir->nodes[1].goto_target, "l0");
  EXPECT_EQ(ir->nodes[1].max, 4);
}

// ---- validator：正向 ----

TEST(WfValidator, minimal_passes) {
  ValidationResult r = validate_json(minimal_price_ir(), ValidatorOptions{&tools()});
  EXPECT_TRUE(r.ok()) << r.describe();
}

TEST(WfValidator, spec_example_passes) {
  // 11-ir-syntax.md §9 的完整真实产物
  Json spec = Json::parse(R"json({
    "id": "wf_demo_s3a_os",
    "trigger": {"source":"time","timing":{"kind":"schedule","schedule":"0 20 * * *"}},
    "nodes": [
      {"id":"fetch_hot","tool":"web_search","parameters":{"query":"内容平台 今日热门视频 榜单 Top3"},
       "output_schema":{"html":"string"},"next":"extract_top3",
       "on_error":{"retry":2,"fallback":"fetch_hot_backup"}},
      {"id":"fetch_hot_backup","tool":"fetch_price","parameters":{"item_id":"hot_video_ranking"},
       "output_schema":{"price":"number","in_stock":"boolean"},"next":"extract_top3"},
      {"id":"extract_top3","tool":"llm_extract",
       "parameters":{"prompt":"从榜单数据中提取当天热门视频 Top3，输出标题与链接",
                     "text":"$fetch_hot.result.html"},
       "output_schema":{"nl":"*"},"next":"check_result"},
      {"id":"check_result","expr":"$extract_top3.result.nl != null && $extract_top3.result.nl != \"\"",
       "then":"notify","else":"notify_fail"},
      {"id":"notify","tool":"send_notification","parameters":{"title":"🔥 今日热门视频 Top3",
       "body":"今天内容平台热门视频 Top3 已整理好，点击查看：$extract_top3.result.nl"},
       "output_schema":{"ok":"boolean"},"next":"_turn_end"},
      {"id":"notify_fail","tool":"send_notification","parameters":{"title":"⚠️ 今日热门视频获取失败",
       "body":"今天未能成功获取内容平台热门视频 Top3，请稍后手动查看。"},
       "output_schema":{"ok":"boolean"},"next":"_turn_end"}
    ]
  })json");
  ValidationResult r = validate_json(spec, ValidatorOptions{&tools()});
  EXPECT_TRUE(r.ok()) << r.describe();
}

// ---- validator：负向（按规则编号） ----

TEST(WfValidator, trigger_rules) {
  auto with_trigger = [](Json trig) {
    Json j = minimal_price_ir();
    j["trigger"] = std::move(trig);
    return j;
  };
  // VT3：trigger 带 condition
  auto c3 = validate_json(with_trigger(Json{{"source", "time"},
                                            {"timing", Json{{"kind", "schedule"},
                                                            {"schedule", "0 8 * * *"}}},
                                            {"condition", "$x > 1"}}),
                          ValidatorOptions{&tools()});
  EXPECT_FALSE(c3.ok());
  EXPECT_NE(c3.describe().find("VT3"), std::string::npos);

  // VT5：6 字段 cron / @ 宏 / 非法字段
  for (std::string const bad : {"0 8 * * * *", "@daily", "0 8 L * *", "0 8 * * * 8 9", "0 60 * * *"}) {
    auto v = validate_json(with_trigger(Json{{"source", "time"},
                                             {"timing", Json{{"kind", "schedule"},
                                                             {"schedule", bad}}}}),
                           ValidatorOptions{&tools()});
    EXPECT_FALSE(v.ok()) << "bad cron 应拒绝: " << bad;
    EXPECT_NE(v.describe().find("VT5"), std::string::npos) << bad;
  }
  // schedule 允许逗号列表与区间
  auto ok_cron = validate_json(with_trigger(Json{{"source", "time"},
                                                 {"timing", Json{{"kind", "schedule"},
                                                                 {"schedule", "0 8,20 1-3 * *"}}}}),
                               ValidatorOptions{&tools()});
  EXPECT_TRUE(ok_cron.ok()) << ok_cron.describe();

  // VT1：非 time 源 → 拒绝
  auto vt1 = validate_json(with_trigger(Json{{"source", "push"},
                                             {"timing", Json{{"kind", "schedule"},
                                                             {"schedule", "0 8 * * *"}}}}),
                           ValidatorOptions{&tools()});
  EXPECT_FALSE(vt1.ok());
}

TEST(WfValidator, node_name_rules) {
  // VN1：前导下划线 / 点 不行
  auto j = minimal_price_ir();
  j["nodes"][0]["id"] = "_fetch";
  auto r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VN1"), std::string::npos);

  // VN2：重复 id
  auto j2 = minimal_price_ir();
  j2["nodes"][2]["id"] = "fetch";
  auto r2 = validate_json(j2, ValidatorOptions{&tools()});
  EXPECT_FALSE(r2.ok());
  EXPECT_NE(r2.describe().find("VN2"), std::string::npos);

  // VN6：未知工具（D4 白名单：os 级工具不在 fakes 注册表）
  auto j3 = minimal_price_ir();
  j3["nodes"][0]["tool"] = "setting_get";
  auto r3 = validate_json(j3, ValidatorOptions{&tools()});
  EXPECT_FALSE(r3.ok());
  EXPECT_NE(r3.describe().find("VN6"), std::string::npos);
}

TEST(WfValidator, ref_rules) {
  // VR2：引用下游节点字段 → 拒绝
  auto j = minimal_price_ir();
  j["nodes"][0]["parameters"] = Json{{"item_id", "$notify.result.ok"}};
  auto r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VR2"), std::string::npos);

  // VR2：引用未声明字段 → 拒绝
  auto j2 = minimal_price_ir();
  j2["nodes"][0]["parameters"] = Json{{"item_id", "$fetch.result.nope"}};
  auto r2 = validate_json(j2, ValidatorOptions{&tools()});
  EXPECT_FALSE(r2.ok());

  // VR1：损坏引用片段 → 拒绝（"$fetch.result." 缺字段 / "@config" 缺前缀点）
  auto j3 = minimal_price_ir();
  j3["nodes"][0]["parameters"] = Json{{"item_id", "$fetch.result."}};
  auto r3 = validate_json(j3, ValidatorOptions{&tools()});
  EXPECT_FALSE(r3.ok());
  EXPECT_NE(r3.describe().find("VR1"), std::string::npos);

  auto j3b = minimal_price_ir();
  j3b["nodes"][0]["parameters"] = Json{{"item_id", "@config"}};
  auto r3b = validate_json(j3b, ValidatorOptions{&tools()});
  EXPECT_FALSE(r3b.ok());
}

TEST(WfValidator, graph_rules) {
  // VG1：next 指向不存在节点
  auto j = minimal_price_ir();
  j["nodes"][0]["next"] = "ghost";
  auto r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VG1"), std::string::npos);

  // VG2：expr 自环（then == 自身）
  auto j2 = minimal_price_ir();
  j2["nodes"][1]["then"] = "check";
  auto r2 = validate_json(j2, ValidatorOptions{&tools()});
  EXPECT_FALSE(r2.ok());
  EXPECT_NE(r2.describe().find("VG2"), std::string::npos);

  // VG7：死节点（无人可达、也不是 fallback 目标）
  auto j3 = minimal_price_ir();
  j3["nodes"].push_back(Json{{"id", "orphan"}, {"tool", "echo"},
                             {"output_schema", Json{{"value", "string"}}},
                             {"next", "_turn_end"}});
  auto r3 = validate_json(j3, ValidatorOptions{&tools()});
  EXPECT_FALSE(r3.ok());
  EXPECT_NE(r3.describe().find("VG7"), std::string::npos);

  // VG8：死 label
  auto j4 = minimal_price_ir();
  j4["nodes"].push_back(Json{{"id", "dead_l"}, {"kind", "label"}});
  auto r4 = validate_json(j4, ValidatorOptions{&tools()});
  EXPECT_FALSE(r4.ok());
  EXPECT_NE(r4.describe().find("VG8"), std::string::npos);

  // VG3：label 在末尾没有续接
  auto j5 = Json{{"id", "wf_l"},
                 {"trigger", Json{{"source", "time"},
                                  {"timing", Json{{"kind", "schedule"}, {"schedule", "0 * * * *"}}}}},
                 {"nodes", Json::array({Json{{"id", "only"}, {"kind", "label"}}})}};
  auto r5 = validate_json(j5, ValidatorOptions{&tools()});
  EXPECT_FALSE(r5.ok());
  EXPECT_NE(r5.describe().find("VG3"), std::string::npos);
}

// ---- label 消解（D2）：整体计算，不要求 label 先于 goto ----

TEST(WfValidator, label_loop_header) {
  // label 在开头、goto 在末尾（规范形态：入口经过 label 进循环体）
  Json j = Json{
      {"id", "wf_poll"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 * * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "l0"}, {"kind", "label"}},
                     Json{{"id", "fetch"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"in_stock", "boolean"}}}, {"next", "check"}},
                     node("check", "$fetch.result.in_stock == true", "notify", "gp"),
                     Json{{"id", "gp"}, {"goto", "l0"}, {"max", 4}},
                     Json{{"id", "notify"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "到货"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
  ValidationResult r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_TRUE(r.ok()) << r.describe();
}

TEST(WfValidator, label_as_join_target) {
  // 汇合：两条分支都指向 label，label 之后才是公共续接（labels 仅作汇合目标）
  Json j = Json{
      {"id", "wf_join"},
      {"trigger", Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"},
                                                           {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {node("start", "true", "a", "b"),
                     Json{{"id", "a"}, {"tool", "echo"}, {"output_schema", Json{{"value", "string"}}},
                          {"next", "join"}},
                     Json{{"id", "b"}, {"tool", "echo"}, {"output_schema", Json{{"value", "string"}}},
                          {"next", "join"}},
                     Json{{"id", "join"}, {"kind", "label"}},
                     Json{{"id", "after"}, {"tool", "send_notification"},
                          {"parameters", Json{{"title", "done"}}},
                          {"output_schema", Json{{"ok", "boolean"}}}, {"next", "_turn_end"}}})}};
  ValidationResult r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_TRUE(r.ok()) << r.describe();
}
// 回归：on_error.retry 必须有上界。
// retry 是在**一次节点调用内**同步循环的（compiler.cpp），graph 的 max_steps 只数
// super-step、管不到这里 —— 没有上界就等于能把进程挂死。
// 注意判定要回读 raw：ir.cpp 的 get<int>() 对超 int 范围的数字是静默截断。
TEST(WfValidator, on_error_retry_is_bounded) {
  auto with_retry = [](Json const& retry) {
    Json j = minimal_price_ir();
    j["nodes"][0]["on_error"] = Json{{"retry", retry}};
    return validate_json(j, ValidatorOptions{&tools()});
  };

  EXPECT_TRUE(with_retry(0).ok());
  EXPECT_TRUE(with_retry(2).ok());
  EXPECT_TRUE(with_retry(10).ok());

  // 越界 / 非整数 / 超 int 范围（会静默截断，旧实现漏判）
  for (Json const& bad : {Json(-1), Json(11), Json(2.5), Json("2"), Json(2000000000),
                          Json(10000000000000000000ULL)}) {
    ValidationResult r = with_retry(bad);
    EXPECT_FALSE(r.ok()) << "应被拒绝但通过了: " << bad.dump();
    EXPECT_NE(r.describe().find("VH1"), std::string::npos) << bad.dump();
  }
}

// 回归：on_error.fallback 的**下游**也必须是可达的。
// 早先只把 fallback 目标标记为 seen、不展开它的后继 → "只能经 fallback 到达"的节点
// 被误判成死节点，合法 IR 被拒（逼生成器空转修复轮）。
TEST(WfValidator, fallback_successors_count_as_reachable) {
  Json j = Json{
      {"id", "wf_fb_chain"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"}, {"schedule", "0 * * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "main"}, {"tool", "fetch_price"},
                          {"parameters", Json{{"item_id", "@config.product_id"}}},
                          {"output_schema", Json{{"price", "number"}}}, {"next", "_turn_end"},
                          {"on_error", Json{{"retry", 1}, {"fallback", "backup"}}}},
                     Json{{"id", "backup"}, {"tool", "echo"}, {"parameters", Json{{"v", "b"}}},
                          {"output_schema", Json{{"value", "string"}}}, {"next", "after"}},
                     Json{{"id", "after"}, {"tool", "echo"}, {"parameters", Json{{"v", "a"}}},
                          {"output_schema", Json{{"value", "string"}}}, {"next", "_turn_end"}}})}};

  ValidationResult r = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_TRUE(r.ok()) << r.describe();  // 修复前：VG7 误报 'after' 为死节点

  // 对照：真正没人到达的节点仍要被判死（别把护栏一起修没了）
  j["nodes"].push_back(Json{{"id", "orphan"}, {"tool", "echo"},
                            {"output_schema", Json{{"value", "string"}}}, {"next", "_turn_end"}});
  ValidationResult r2 = validate_json(j, ValidatorOptions{&tools()});
  EXPECT_FALSE(r2.ok());
  EXPECT_NE(r2.describe().find("VG7"), std::string::npos);
}

// B2 回归：宿主声明 config 契约后，IR 里的 @config.<键> 必须在其列。
// 此前这个引用**毫无约束** —— 写一个宿主不会提供的键能过校验，运行期静默解析成
// null，直到工具读到 null 才炸（校验期 fail-closed，运行期 fail-open）。
TEST(WfValidator, config_keys_contract_is_enforced) {
  auto with_keys = [](std::optional<std::vector<std::string>> keys) {
    ValidatorOptions o{&tools()};
    o.config_keys = std::move(keys);
    return validate_json(minimal_price_ir(), o);
  };

  // 未声明契约 → 不检查（向后兼容，minimal_price_ir 里用的是 @config.product_id）
  EXPECT_TRUE(with_keys(std::nullopt).ok());

  // 声明了且命中 → 通过
  EXPECT_TRUE(with_keys(std::vector<std::string>{"product_id"}).ok());

  // 声明了但不含该键 → VR4 拒绝，且报出实际引用的键
  auto bad = with_keys(std::vector<std::string>{"item_id", "threshold"});
  EXPECT_FALSE(bad.ok());
  EXPECT_NE(bad.describe().find("VR4"), std::string::npos);
  EXPECT_NE(bad.describe().find("product_id"), std::string::npos) << bad.describe();

  // 空契约 = 不允许任何 @config 引用
  auto none = with_keys(std::vector<std::string>{});
  EXPECT_FALSE(none.ok());
  EXPECT_NE(none.describe().find("VR4"), std::string::npos);
}
