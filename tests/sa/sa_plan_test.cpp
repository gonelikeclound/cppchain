// tests/sa/sa_plan_test.cpp
// S7（problems/64）：「一句话 → DAG」的静态校验 / 布局 / JSON 抽取纯函数测试。
// Planner 本身走真机模型，不在这里；这里锁死的是**执行前必须过的门槛**（fail-closed）。
#include "sa_demo/sa_plan.hpp"

#include <gtest/gtest.h>

using cppchain::sa::plan::extract_json_text;
using cppchain::sa::plan::layout;
using cppchain::sa::plan::validate;
using Json = nlohmann::json;

namespace {

Json t1_plan() {
  return Json::parse(R"JSON({
  "title": "给妈妈打电话；改口后发微信",
  "nodes": [
    {"id": "n_user", "role": "user", "label": "用户", "task": "给妈妈打电话"},
    {"id": "n_core1", "role": "core", "label": "核心", "task": "→电话域：打电话。"},
    {"id": "n_call", "role": "phone", "label": "电话域", "task": "打电话", "args": {"call_id": "c1", "number": "13800000000"}},
    {"id": "n_core2", "role": "core", "label": "核心", "task": "→消息域：改口发微信。"},
    {"id": "n_msg", "role": "msg", "label": "消息域", "task": "发微信", "args": {"msg_id": "m1", "to": "妈妈", "text": "来不来吃饭？"}},
    {"id": "n_world", "role": "world", "label": "世界", "task": "核对"}
  ],
  "edges": [
    {"from": "n_user", "to": "n_core1", "kind": "query"},
    {"from": "n_core1", "to": "n_call", "kind": "dispatch"},
    {"from": "n_user", "to": "n_call", "kind": "control"},
    {"from": "n_call", "to": "n_core2", "kind": "report"},
    {"from": "n_core2", "to": "n_msg", "kind": "dispatch"},
    {"from": "n_msg", "to": "n_world", "kind": "world"}
  ]
})JSON");
}

bool has_issue(std::vector<cppchain::sa::plan::Issue> const& v, std::string const& code) {
  for (auto const& i : v)
    if (i.code == code) return true;
  return false;
}

}  // namespace

TEST(SaPlan, AcceptsValidPlan) {
  auto const issues = validate(t1_plan());
  EXPECT_TRUE(issues.empty()) << (issues.empty() ? "" : issues.front().message);
}

TEST(SaPlan, RejectsMissingWorld) {
  Json p = t1_plan();
  p["nodes"].erase(p["nodes"].begin() + 5);
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P08"));
}

TEST(SaPlan, RejectsCycle) {
  Json p = t1_plan();
  p["edges"].push_back(Json{{"from", "n_world"}, {"to", "n_core1"}, {"kind", "report"}});
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P23"));
}

TEST(SaPlan, RejectsUnknownRole) {
  Json p = t1_plan();
  p["nodes"][3]["role"] = "ops";
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P05"));
}

TEST(SaPlan, RejectsDomainWithoutDispatch) {
  Json p = t1_plan();
  // 去掉 core1 → phone 的 dispatch 入边
  Json edges = Json::array();
  for (Json const& e : p["edges"])
    if (!(e.value("from", "") == "n_core1" && e.value("kind", "") == "dispatch"))
      edges.push_back(e);
  p["edges"] = std::move(edges);
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P21"));
}

TEST(SaPlan, RejectsControlToNonPhone) {
  Json p = t1_plan();
  p["edges"].push_back(Json{{"from", "n_user"}, {"to", "n_msg"}, {"kind", "control"}});
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P19"));
}

TEST(SaPlan, RejectsCoreWithoutDispatch) {
  Json p = t1_plan();
  p["nodes"].push_back(Json{{"id", "n_core9"}, {"role", "core"}, {"label", "核心"}});
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P22"));
}

TEST(SaPlan, RejectsUnknownEdgeKind) {
  Json p = t1_plan();
  p["edges"][0]["kind"] = "jump";
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P15"));
}

TEST(SaPlan, RejectsControlWithoutMsgNode) {
  Json p = t1_plan();
  p["nodes"].erase(p["nodes"].begin() + 4);   // n_msg
  Json edges = Json::array();
  for (Json const& e : p["edges"])
    if (e.value("to", "") != "n_msg" && e.value("from", "") != "n_msg") edges.push_back(e);
  p["edges"] = std::move(edges);
  auto const issues = validate(p);
  EXPECT_TRUE(has_issue(issues, "P29"));
}

TEST(SaPlan, AcceptsPhoneOnlyWithoutControl) {
  Json p = Json::parse(R"JSON({
    "title":"打电话",
    "nodes":[
      {"id":"u","role":"user","task":"打电话"},
      {"id":"c","role":"core","task":"→电话域：打电话。"},
      {"id":"p","role":"phone","task":"打电话","args":{"call_id":"c1","number":"1"}},
      {"id":"w","role":"world","task":"核对"}
    ],
    "edges":[
      {"from":"u","to":"c","kind":"query"},
      {"from":"c","to":"p","kind":"dispatch"},
      {"from":"p","to":"w","kind":"world"}
    ]
  })JSON");
  EXPECT_TRUE(validate(p).empty());
}

Json tool_plan(std::string const& tool_name) {
  Json p = Json::parse(R"JSON({
    "title":"计算",
    "nodes":[
      {"id":"u","role":"user","task":"算一下"},
      {"id":"c","role":"core","task":"→工具：计算。"},
      {"id":"t","role":"tool","tool":"calculator","args":{"expression":"1+1"},"task":"计算"},
      {"id":"w","role":"world","task":"核对"}
    ],
    "edges":[
      {"from":"u","to":"c","kind":"query"},
      {"from":"c","to":"t","kind":"dispatch"},
      {"from":"t","to":"w","kind":"world"}
    ]
  })JSON");
  for (Json& n : p["nodes"])
    if (n.value("role", "") == "tool") n["tool"] = tool_name;
  return p;
}

TEST(SaPlan, AcceptsToolNodeWithAllowedTool) {
  Json p = tool_plan("calculator");
  EXPECT_TRUE(validate(p, {"calculator", "file_read"}).empty());
}

TEST(SaPlan, RejectsUnknownTool) {
  Json p = tool_plan("rm_rf");
  auto const issues = validate(p, {"calculator"});
  EXPECT_TRUE(has_issue(issues, "P30"));
}

TEST(SaPlan, RejectsToolNodeWithoutDispatch) {
  Json p = tool_plan("calculator");
  Json edges = Json::array();
  for (Json const& e : p["edges"])
    if (!(e.value("kind", "") == "dispatch")) edges.push_back(e);
  p["edges"] = std::move(edges);
  auto const issues = validate(p, {"calculator"});
  EXPECT_TRUE(has_issue(issues, "P21"));
}

Json synthesis_plan() {
  return Json::parse(R"JSON({
    "title":"检索并摘要",
    "nodes":[
      {"id":"u","role":"user","task":"查最新论文并摘要"},
      {"id":"c","role":"core","task":"→工具：检索。"},
      {"id":"t","role":"tool","tool":"arxiv_search","args":{"search_query":"x"},"task":"检索"},
      {"id":"s","role":"synthesis","task":"用中文写摘要"},
      {"id":"w","role":"world","task":"核对"}
    ],
    "edges":[
      {"from":"u","to":"c","kind":"query"},
      {"from":"c","to":"t","kind":"dispatch"},
      {"from":"t","to":"s","kind":"synthesize"},
      {"from":"s","to":"w","kind":"world"}
    ]
  })JSON");
}

TEST(SaPlan, AcceptsSynthesisNode) {
  EXPECT_TRUE(validate(synthesis_plan(), {"arxiv_search"}).empty());
}

TEST(SaPlan, RejectsSynthesisWithoutInput) {
  Json p = synthesis_plan();
  Json edges = Json::array();
  for (Json const& e : p["edges"])
    if (e.value("kind", "") != "synthesize") edges.push_back(e);
  p["edges"] = std::move(edges);
  auto const issues = validate(p, {"arxiv_search"});
  EXPECT_TRUE(has_issue(issues, "P32"));
}

TEST(SaPlan, RejectsSynthesizeEdgeFromUser) {
  Json p = synthesis_plan();
  p["edges"].push_back(Json{{"from", "u"}, {"to", "s"}, {"kind", "synthesize"}});
  auto const issues = validate(p, {"arxiv_search"});
  EXPECT_TRUE(has_issue(issues, "P33"));
}

TEST(SaPlan, RejectsSynthesisWithoutTask) {
  Json p = synthesis_plan();
  p["nodes"][3].erase("task");
  auto const issues = validate(p, {"arxiv_search"});
  EXPECT_TRUE(has_issue(issues, "P35"));
}

TEST(SaPlan, RejectsMultipleSynthesisNodes) {
  Json p = synthesis_plan();
  p["nodes"].push_back(Json{{"id", "s2"}, {"role", "synthesis"}, {"task", "再摘要"}});
  p["edges"].push_back(Json{{"from", "t"}, {"to", "s2"}, {"kind", "synthesize"}});
  p["edges"].push_back(Json{{"from", "s2"}, {"to", "w"}, {"kind", "world"}});
  auto const issues = validate(p, {"arxiv_search"});
  EXPECT_TRUE(has_issue(issues, "P34"));
}

TEST(SaPlan, LayoutStacksMultipleNodesInSameLayer) {
  Json p = tool_plan("calculator");
  p["nodes"].push_back(Json{{"id", "t2"}, {"role", "tool"}, {"tool", "get_datetime"}});
  p["edges"].push_back(Json{{"from", "c"}, {"to", "t2"}, {"kind", "dispatch"}});
  p["edges"].push_back(Json{{"from", "t2"}, {"to", "w"}, {"kind", "world"}});
  auto const issues = validate(p, {"calculator", "get_datetime"});
  EXPECT_TRUE(issues.empty()) << (issues.empty() ? "" : issues.front().message);
  Json const laid = layout(p);
  // 同层两个工具节点应分行，而不是重叠
  std::vector<int> rows;
  for (Json const& n : laid["nodes"])
    if (n.value("role", "") == "tool") rows.push_back(n["row"].get<int>());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_NE(rows[0], rows[1]);
}

TEST(SaPlan, LayoutUsesLongestPath) {
  Json const p = layout(t1_plan());
  EXPECT_EQ(p["nodes"][0]["layer"].get<int>(), 0);  // user
  EXPECT_EQ(p["nodes"][1]["layer"].get<int>(), 1);  // core1
  EXPECT_EQ(p["nodes"][2]["layer"].get<int>(), 2);  // phone
  EXPECT_EQ(p["nodes"][3]["layer"].get<int>(), 3);  // core2
  EXPECT_EQ(p["nodes"][4]["layer"].get<int>(), 4);  // msg
  EXPECT_EQ(p["nodes"][5]["layer"].get<int>(), 5);  // world
  EXPECT_EQ(p["nodes"][0]["row"].get<int>(), 0);
  EXPECT_EQ(p["nodes"][2]["row"].get<int>(), 1);
}

TEST(SaPlan, ExtractsJsonFromFences) {
  std::string const s = "好的，计划如下：\n```json\n{\"nodes\":[],\"edges\":[]}\n```\n以上。";
  EXPECT_EQ(extract_json_text(s), "{\"nodes\":[],\"edges\":[]}");
}
