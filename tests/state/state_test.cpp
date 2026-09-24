// M5 测试：状态 schema 与 reducer（含 add_messages 的去重语义）
#include <gtest/gtest.h>

#include "cppchain/message/serializer.hpp"
#include "cppchain/state/state.hpp"

using namespace cppchain;
using namespace cppchain::state;

TEST(Reducer, overwrite_discards_left) {
  EXPECT_EQ(overwrite_reducer(Json{{"a", 1}}, Json{{"b", 2}}), Json({{"b", 2}}));
}

TEST(AddMessages, appends_new_and_replaces_same_id) {
  Json const left = Json::array({Json({{"type", "human"}, {"content", Json::array()}, {"id", "m1"}})});
  Json const right = Json::array(
      {Json({{"type", "ai"}, {"content", Json::array()}, {"id", "m1"}}),   // 同 id → 覆盖
       Json({{"type", "human"}, {"content", Json::array()}, {"id", "m2"}})  // 新 id → 追加
      });

  auto const merged = add_messages(left, right);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].at("type").get<std::string>(), "ai");   // m1 被覆盖
  EXPECT_EQ(merged[1].at("id").get<std::string>(), "m2");
}

TEST(AddMessages, messages_without_id_always_append) {
  Json const left = Json::array({Json({{"type", "human"}, {"content", Json::array()}})});
  Json const right = Json::array({Json({{"type", "human"}, {"content", Json::array()}})});
  EXPECT_EQ(add_messages(left, right).size(), 2u);
}

TEST(AddMessages, accepts_openai_dict_and_plain_string) {
  // OpenAI 风格 dict + 纯字符串 → 归一化为规范 JSON
  Json const right = Json::array({Json({{"role", "user"}, {"content", "你好"}}), Json("继续")});

  auto const merged = add_messages(Json::array(), right);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].at("type").get<std::string>(), "human");
  EXPECT_EQ(merged[0].at("content").at(0).at("text").get<std::string>(), "你好");
  EXPECT_EQ(merged[1].at("type").get<std::string>(), "human");
  EXPECT_EQ(merged[1].at("content").at(0).at("text").get<std::string>(), "继续");
}

TEST(AddMessages, handles_null_left) {
  auto const merged = add_messages(Json(), Json::array({Json("hi")}));
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].at("type").get<std::string>(), "human");
}

TEST(StateSchema, unregistered_keys_overwrite_registered_keys_reduce) {
  auto schema = StateSchema::messages_state();

  Json state = schema.apply(Json(), Json{{"messages", Json::array({Json("a")})}, {"step", 1}});
  state = schema.apply(state, Json{{"messages", Json::array({Json("b")})}, {"step", 2}});

  ASSERT_EQ(state.at("messages").size(), 2u);  // 归约：追加
  EXPECT_EQ(state.at("step").get<int>(), 2);   // 未注册：覆盖
}

TEST(StateSchema, has_reducer_reports_registration) {
  auto schema = StateSchema::messages_state();
  EXPECT_TRUE(schema.has_reducer("messages"));
  EXPECT_FALSE(schema.has_reducer("step"));
}

TEST(StateSchema, state_is_json_roundtrippable) {
  auto schema = StateSchema::messages_state();
  Json state = schema.apply(Json(), Json{{"messages", Json::array({Json("你好")})}, {"step", 3}});

  auto const dumped = state.dump();
  auto const restored = Json::parse(dumped);
  EXPECT_EQ(restored, state);

  // 继续在恢复后的状态上归约，语义不变
  auto const next = schema.apply(restored, Json{{"messages", Json::array({Json("世界")})}});
  EXPECT_EQ(next.at("messages").size(), 2u);
  EXPECT_EQ(next.at("step").get<int>(), 3);
}