// wfr-㊵ 测试：messages 的 **op 语义**（折叠 / 删改）。
//
// 背景：add_messages 只能"追加 / 同 id 覆盖"，而 workflow 产出的消息都不带 id，于是
// "折叠一段历史""删一批消息"在数组形态下表达不出来。op 把这类操作表达成"对累计消息施加
// 一次操作"，op 里固化了一切所需数据 → reducer 是 left 的纯函数 → 会话日志重放同一条 op
// 必然得到同样结果（运行期与 replay 不分叉）。
//
// 这个文件同时是**向后兼容的哨兵**：op 分支必须严格前置、严格判定，数组分支的现语义
// 一条都不能变。
#include <gtest/gtest.h>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/state/state.hpp"

using namespace cppchain;
using namespace cppchain::state;
using cppchain::message::Message;

namespace {

Json msg(std::string const& type, std::string const& text, std::string const& id = "") {
  Message m;
  if (type == "human") m = message::human_message(text);
  else if (type == "ai") m = message::ai_message(text);
  else if (type == "system") m = message::system_message(text);
  else m = message::tool_message(text, "c1");
  if (!id.empty()) m.id = id;
  return Json(m);
}

// 摘要消息：靠 additional_kwargs 的标记被识别（与 summarize 节点共用的契约）
Json summary_msg(std::string const& text, int summarized = 5) {
  Message m = message::system_message(text);
  m.additional_kwargs = Json{{"cppchain_summary", true}, {"summarized_msgs", summarized}};
  return Json(m);
}

Json chain(std::size_t n) {  // m0..m(n-1)，全是 human
  Json out = Json::array();
  for (std::size_t i = 0; i < n; ++i) out.push_back(msg("human", "m" + std::to_string(i)));
  return out;
}

std::vector<Message> as_messages(Json const& arr) {
  std::vector<Message> out;
  for (auto const& j : arr) out.push_back(j.get<Message>());
  return out;
}

}  // namespace

// ---- fold ----

TEST(MessagesOp, fold_keeps_the_tail_and_prepends_the_summary) {
  Json const left = chain(10);
  Json const op = Json{{"op", "fold"}, {"keep_last", 3}, {"base_size", 10},
                       {"prepend", Json::array({summary_msg("前 7 条的摘要")})}};
  Json const out = add_messages(left, op);

  ASSERT_EQ(out.size(), 4u);  // 摘要 + 3 条
  EXPECT_EQ(out[0]["additional_kwargs"]["cppchain_summary"], true);
  for (std::size_t i = 1; i < 4; ++i)
    EXPECT_EQ(out[i]["content"][0]["text"], "m" + std::to_string(6 + i));
}

TEST(MessagesOp, fold_strips_the_previous_summary_so_summaries_never_stack) {
  // 已有头部摘要 → 再折一次，结果里**只能有一条**摘要（世代交替）
  Json const left = Json::array({summary_msg("旧摘要"), msg("human", "a"), msg("ai", "b"),
                                 msg("human", "c")});
  Json const op = Json{{"op", "fold"}, {"keep_last", 2}, {"base_size", 3},
                       {"prepend", Json::array({summary_msg("新摘要")})}};
  Json const out = add_messages(left, op);

  ASSERT_EQ(out.size(), 3u);  // 新摘要 + 2 条
  EXPECT_EQ(out[0]["content"][0]["text"], "新摘要");
  EXPECT_EQ(out[1]["content"][0]["text"], "b");
  EXPECT_EQ(out[2]["content"][0]["text"], "c");
  EXPECT_EQ(out[0]["additional_kwargs"]["cppchain_summary"], true);
}

TEST(MessagesOp, fold_drops_leading_tool_messages_from_the_tail) {
  // 防孤儿兜底：保留区若**以 Tool 消息开头**（它的 AI(tool_calls) 被折掉了）→ 逐条剔除。
  // 真端点会因孤儿 ToolMessage 直接 400，所以这是硬约束；节点侧另有 safe_boundary 先挡，
  // 这里守的是"手写 op / 边界漂移"的兜底。keep_last=2 → 保留区正好从 tool 开始。
  Json const left = Json::array({msg("human", "a"), msg("ai", "b"),
                                 message::tool_message("结果", "c1"), msg("human", "c")});
  Json const op = Json{{"op", "fold"}, {"keep_last", 2}, {"base_size", 4},
                       {"prepend", Json::array({summary_msg("摘要")})}};
  Json const out = add_messages(left, op);

  ASSERT_EQ(out.size(), 2u);  // 摘要 + (tool 被剔除后只剩 human)
  EXPECT_EQ(out[1]["type"], "human");
  EXPECT_FALSE(message::validate_sequence(as_messages(out)).has_value());
}

TEST(MessagesOp, fold_with_base_size_mismatch_is_ignored) {
  // 守卫：op 与"发出它时的列表"绑定。条数不符（重复 apply / 被人手改过）→ 什么都不做，
  // 免得折叠到错误的位置。
  Json const left = chain(10);
  Json const op = Json{{"op", "fold"}, {"keep_last", 3}, {"base_size", 7},   // 实际是 10
                       {"prepend", Json::array({summary_msg("摘要")})}};
  EXPECT_EQ(add_messages(left, op), left);
}

TEST(MessagesOp, fold_is_bounded_when_applied_twice) {
  // 不追求严格幂等，但有界：第二次 apply 的 base 已不含被折掉的原文，结果稳定在 1+keep_last
  Json const left = chain(10);
  Json const op = Json{{"op", "fold"}, {"keep_last", 3}, {"base_size", 10},
                       {"prepend", Json::array({summary_msg("摘要")})}};
  Json const once = add_messages(left, op);
  Json const twice = add_messages(once, op);   // 无 base_size 守卫时也应"有界不炸"
  EXPECT_LE(twice.size(), 4u);
}

TEST(MessagesOp, fold_keeps_everything_when_keep_last_exceeds_size) {
  Json const left = chain(3);
  Json const op = Json{{"op", "fold"}, {"keep_last", 99}, {"base_size", 3},
                       {"prepend", Json::array()}};
  EXPECT_EQ(add_messages(left, op).size(), 3u);
}

// ---- transform ----

TEST(MessagesOp, transform_drops_and_rewrites_by_index) {
  Json const left = chain(5);
  Json const op = Json{{"op", "transform"}, {"base_size", 5},
                       {"drop", Json::array({1, 3})},
                       {"rewrite", Json::array({Json{{"index", 0},
                                                     {"message", msg("human", "改写后")}}})}};
  Json const out = add_messages(left, op);

  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0]["content"][0]["text"], "改写后");  // 改写
  EXPECT_EQ(out[1]["content"][0]["text"], "m2");      // m1 被删
  EXPECT_EQ(out[2]["content"][0]["text"], "m4");      // m3 被删
}

TEST(MessagesOp, transform_ignores_out_of_range_indices) {
  Json const left = chain(3);
  Json const op = Json{{"op", "transform"}, {"base_size", 3},
                       {"drop", Json::array({1, 99})},
                       {"rewrite", Json::array({Json{{"index", 42},
                                                     {"message", msg("ai", "x")}}})}};
  Json const out = add_messages(left, op);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0]["content"][0]["text"], "m0");
  EXPECT_EQ(out[1]["content"][0]["text"], "m2");
}

TEST(MessagesOp, transform_normalizes_rewritten_messages) {
  // rewrite 里的消息可能是字符串 / OpenAI dict → 落成规范 {"type":…}
  Json const left = chain(2);
  Json const op = Json{{"op", "transform"}, {"base_size", 2}, {"drop", Json::array()},
                       {"rewrite", Json::array({Json{{"index", 1}, {"message", "纯字符串"}}})}};
  Json const out = add_messages(left, op);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1]["type"], "human");
  EXPECT_EQ(out[1]["content"][0]["text"], "纯字符串");
}

TEST(MessagesOp, transform_with_base_size_mismatch_is_ignored) {
  Json const left = chain(3);
  Json const op = Json{{"op", "transform"}, {"base_size", 9}, {"drop", Json::array({0})}};
  EXPECT_EQ(add_messages(left, op), left);
}

// ---- 未知 / 非法 op ----

TEST(MessagesOp, unknown_op_is_ignored_not_thrown) {
  // add_messages 有三个读者（运行期 / replay / wfr 打印会话条数）—— 抛异常会把第三个带崩
  Json const left = chain(2);
  EXPECT_EQ(add_messages(left, Json{{"op", "编造的"}, {"x", 1}}), left);
  EXPECT_EQ(add_messages(left, Json{{"op", "fold"}}), left);  // 无 keep_last/prepend：合法但无操作
}

TEST(MessagesOp, non_string_op_is_not_treated_as_an_op) {
  // {"op": 123} 形状不对 → 走原路径（当普通对象处理，保持既有语义）
  Json const out = add_messages(Json::array(), Json{{"op", 123}, {"x", 1}});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["op"], 123);  // 原样追加（与改动前一致）
}

// ---- 向后兼容（这些是"数组分支一字不改"的哨兵）----

TEST(MessagesOp, legacy_append_and_same_id_overwrite_still_work) {
  Json const left = Json::array({Json{{"type", "human"}, {"content", Json::array()}, {"id", "m1"}}});
  Json const right = Json::array(
      {Json{{"type", "ai"}, {"content", Json::array()}, {"id", "m1"}},    // 同 id → 覆盖
       Json{{"type", "human"}, {"content", Json::array()}, {"id", "m2"}}});  // 新 id → 追加
  Json const out = add_messages(left, right);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0]["type"], "ai");
  EXPECT_EQ(out[0]["id"], "m1");
  EXPECT_EQ(out[1]["id"], "m2");
}

TEST(MessagesOp, legacy_plain_string_and_openai_dict_still_normalize) {
  Json out = add_messages(Json::array(), "你好");                     // 纯字符串 → human
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["type"], "human");
  EXPECT_EQ(out[0]["content"][0]["text"], "你好");

  out = add_messages(Json::array(), Json{{"role", "assistant"}, {"content", "hi"}});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["type"], "ai");
}

TEST(MessagesOp, legacy_null_left_and_unknown_object_behaviour_unchanged) {
  Json out = add_messages(Json(), chain(2));  // left=null → 空起步
  EXPECT_EQ(out.size(), 2u);
  // 无 type / role 的普通对象：仍按改动前的行为"原样追加"（不静默丢弃）
  Json const obj = Json{{"foo", "bar"}};
  out = add_messages(Json::array(), obj);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["foo"], "bar");
}

TEST(MessagesOp, schema_apply_routes_the_op_through_the_messages_reducer) {
  // 端到端：StateSchema::apply 把 {"messages": {op}} 送进 add_messages（compiler 与 replay
  // 用的就是这条路）
  StateSchema const s = StateSchema::messages_state();
  Json state = Json{{"messages", chain(6)}};
  Json const update = Json{{"messages", Json{{"op", "fold"}, {"keep_last", 2}, {"base_size", 6},
                                             {"prepend", Json::array({summary_msg("摘要")})}}}};
  state = s.apply(state, update);
  ASSERT_EQ(state["messages"].size(), 3u);
  EXPECT_EQ(state["messages"][0]["additional_kwargs"]["cppchain_summary"], true);
}
