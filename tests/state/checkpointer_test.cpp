// M5 测试：Checkpointer（快照链 / 恢复 / 线程与命名空间隔离 / 细粒度 writes）
#include <gtest/gtest.h>

#include "cppchain/state/checkpointer.hpp"

using namespace cppchain;
using namespace cppchain::state;

TEST(Checkpointer, put_returns_id_and_records_parent_chain) {
  MemoryCheckpointer cp;
  auto const id1 = cp.put("t1", "", std::nullopt, Json{{"step", 1}}, {}, Json{{"source", "input"}});
  auto const id2 = cp.put("t1", "", id1, Json{{"step", 2}}, {}, Json{{"source", "loop"}});

  EXPECT_NE(id1, id2);

  auto const snap2 = cp.get_tuple("t1", "", std::nullopt);  // 最新
  ASSERT_TRUE(snap2.has_value());
  EXPECT_EQ(snap2->values.at("step").get<int>(), 2);
  ASSERT_TRUE(snap2->parent_checkpoint_id.has_value());
  EXPECT_EQ(*snap2->parent_checkpoint_id, id1);
  EXPECT_EQ(snap2->config.at("checkpoint_id").get<std::string>(), id2);
}

TEST(Checkpointer, get_tuple_by_id_and_unknown_id) {
  MemoryCheckpointer cp;
  auto const id1 = cp.put("t1", "", std::nullopt, Json{{"step", 1}}, {}, Json::object());
  (void)cp.put("t1", "", id1, Json{{"step", 2}}, {}, Json::object());

  auto const first = cp.get_tuple("t1", "", id1);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->values.at("step").get<int>(), 1);

  EXPECT_FALSE(cp.get_tuple("t1", "", std::string("nope")).has_value());
  EXPECT_FALSE(cp.get_tuple("no-such-thread", "", std::nullopt).has_value());
}

TEST(Checkpointer, list_is_newest_first_with_limit) {
  MemoryCheckpointer cp;
  for (int i = 1; i <= 3; ++i)
    cp.put("t1", "", std::nullopt, Json{{"step", i}}, {}, Json::object());

  auto const all = cp.list("t1", "", 0);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].values.at("step").get<int>(), 3);  // 新的在前
  EXPECT_EQ(all[2].values.at("step").get<int>(), 1);

  auto const top2 = cp.list("t1", "", 2);
  ASSERT_EQ(top2.size(), 2u);
  EXPECT_EQ(top2[0].values.at("step").get<int>(), 3);
}

TEST(Checkpointer, threads_and_namespaces_are_isolated) {
  MemoryCheckpointer cp;
  cp.put("t1", "", std::nullopt, Json{{"who", "t1-root"}}, {}, Json::object());
  cp.put("t2", "", std::nullopt, Json{{"who", "t2-root"}}, {}, Json::object());
  cp.put("t1", "sub:node-1", std::nullopt, Json{{"who", "t1-sub"}}, {}, Json::object());

  EXPECT_EQ(cp.get_tuple("t1", "", std::nullopt)->values.at("who").get<std::string>(), "t1-root");
  EXPECT_EQ(cp.get_tuple("t2", "", std::nullopt)->values.at("who").get<std::string>(), "t2-root");
  // 线程相同、命名空间不同 → 互不可见（子图/子代理隔离的机制）
  EXPECT_EQ(cp.get_tuple("t1", "sub:node-1", std::nullopt)->values.at("who").get<std::string>(),
            "t1-sub");
}

TEST(Checkpointer, delete_thread_removes_all_namespaces_of_that_thread_only) {
  MemoryCheckpointer cp;
  cp.put("t1", "", std::nullopt, Json{{"v", 1}}, {}, Json::object());
  cp.put("t1", "sub", std::nullopt, Json{{"v", 2}}, {}, Json::object());
  cp.put("t2", "", std::nullopt, Json{{"v", 3}}, {}, Json::object());

  cp.delete_thread("t1");
  EXPECT_FALSE(cp.get_tuple("t1", "", std::nullopt).has_value());
  EXPECT_FALSE(cp.get_tuple("t1", "sub", std::nullopt).has_value());
  EXPECT_TRUE(cp.get_tuple("t2", "", std::nullopt).has_value());
}

TEST(Checkpointer, writes_are_stored_per_checkpoint) {
  MemoryCheckpointer cp;
  auto const id = cp.put("t1", "", std::nullopt, Json::object(), {}, Json::object());

  cp.put_writes("t1", "", id,
                {WriteRecord{"task-1", "messages", Json::array({Json("m1")})},
                 WriteRecord{"task-1", "step", 1}});

  auto const writes = cp.get_writes("t1", "", id);
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0].channel, "messages");
  EXPECT_EQ(writes[1].channel, "step");
  EXPECT_TRUE(cp.get_writes("t1", "", "other").empty());
}