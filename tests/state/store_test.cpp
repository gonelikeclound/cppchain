// M5 测试：Store（跨线程长期记忆 / 命名空间 / 子串检索）
#include <gtest/gtest.h>

#include <thread>

#include "cppchain/state/store.hpp"

using namespace cppchain;
using namespace cppchain::state;

TEST(Store, put_get_del_roundtrip) {
  MemoryStore store;
  EXPECT_FALSE(store.get({"user", "u1"}, "profile").has_value());

  store.put({"user", "u1"}, "profile", Json{{"name", "董"}, {"lang", "zh"}});
  auto const item = store.get({"user", "u1"}, "profile");
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->value.at("name").get<std::string>(), "董");
  EXPECT_EQ(item->ns, (Namespace{"user", "u1"}));
  EXPECT_FALSE(item->created_at.empty());

  EXPECT_TRUE(store.del({"user", "u1"}, "profile"));
  EXPECT_FALSE(store.get({"user", "u1"}, "profile").has_value());
  EXPECT_FALSE(store.del({"user", "u1"}, "profile"));  // 幂等：不存在返回 false
}

TEST(Store, update_keeps_created_at_and_bumps_updated_at) {
  MemoryStore store;
  store.put({"ns"}, "k", Json{{"v", 1}});
  auto const first = store.get({"ns"}, "k");
  ASSERT_TRUE(first.has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // 跨越秒边界（时间戳精度为秒）
  store.put({"ns"}, "k", Json{{"v", 2}});
  auto const second = store.get({"ns"}, "k");
  ASSERT_TRUE(second.has_value());

  EXPECT_EQ(second->value.at("v").get<int>(), 2);
  EXPECT_EQ(second->created_at, first->created_at);  // 创建时间不变
  EXPECT_GE(second->updated_at, first->updated_at);
}

TEST(Store, namespaces_are_isolated) {
  MemoryStore store;
  store.put({"user", "u1"}, "note", Json("u1 的笔记"));
  store.put({"user", "u2"}, "note", Json("u2 的笔记"));

  EXPECT_EQ(store.get({"user", "u1"}, "note")->value.get<std::string>(), "u1 的笔记");
  EXPECT_EQ(store.get({"user", "u2"}, "note")->value.get<std::string>(), "u2 的笔记");
}

TEST(Store, search_scopes_by_prefix_and_matches_substring) {
  MemoryStore store;
  store.put({"user", "u1", "facts"}, "city", Json("住在北京"));
  store.put({"user", "u1", "facts"}, "job", Json("做系统工程"));
  store.put({"user", "u2", "facts"}, "city", Json("住在上海"));

  // 前缀限定 u1
  auto const u1_all = store.search({"user", "u1"}, "", 0);
  ASSERT_EQ(u1_all.size(), 2u);

  // 子串命中 key
  auto const by_key = store.search({"user", "u1"}, "cit", 0);
  ASSERT_EQ(by_key.size(), 1u);
  EXPECT_EQ(by_key[0].key, "city");

  // 子串命中 value
  auto const by_value = store.search({"user", "u1"}, "系统工程", 0);
  ASSERT_EQ(by_value.size(), 1u);
  EXPECT_EQ(by_value[0].key, "job");

  // 跨用户搜索（前缀放宽到 user）
  auto const across = store.search({"user"}, "住在", 0);
  EXPECT_EQ(across.size(), 2u);
}

TEST(Store, search_honours_limit) {
  MemoryStore store;
  for (int i = 0; i < 5; ++i)
    store.put({"ns"}, "k" + std::to_string(i), Json{{"i", i}});
  EXPECT_EQ(store.search({"ns"}, "", 2).size(), 2u);
}

TEST(Store, list_namespaces_returns_descendants) {
  MemoryStore store;
  store.put({"user", "u1", "facts"}, "a", Json(1));
  store.put({"user", "u1", "prefs"}, "b", Json(2));
  store.put({"user", "u2", "facts"}, "c", Json(3));

  auto const under_u1 = store.list_namespaces({"user", "u1"});
  EXPECT_EQ(under_u1.size(), 2u);

  auto const under_user = store.list_namespaces({"user"});
  EXPECT_EQ(under_user.size(), 3u);

  EXPECT_TRUE(store.list_namespaces({"nope"}).empty());
}

TEST(Store, survives_within_process_and_reports_size) {
  MemoryStore store;
  store.put({"a"}, "1", Json(1));
  store.put({"a"}, "2", Json(2));
  store.put({"b"}, "3", Json(3));
  EXPECT_EQ(store.size(), 3u);
}