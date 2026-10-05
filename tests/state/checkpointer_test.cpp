// M5 测试：Checkpointer（快照链 / 恢复 / 线程与命名空间隔离 / 细粒度 writes）
#include <gtest/gtest.h>

#include "cppchain/state/checkpointer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
// ---------------- FileCheckpointer：跨进程续跑的核心验证 ----------------
// 下面这组用例用"两个互相独立的对象、只共享一个文件"来模拟**两次进程调用** ——
// 这正是"跨命令连续对话"要的语义：进程退出后历史还在，下一次调用能续上。

namespace {

// 每个用例一个独立临时目录，析构时清理
struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() /
          ("cppchain-fcp-" + std::to_string(::getpid()) + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);  // 否则 ofstream 打不开、文件根本写不出来
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string file(std::string const& name = "history.json") const {
    return (dir / name).string();
  }
};

}  // namespace

TEST(FileCheckpointer, survives_across_independent_instances) {
  TempDir tmp;
  std::string ckpt_id;

  {  // ---- 第一次"进程调用"：写一轮 ----
    FileCheckpointer cp(tmp.file());
    EXPECT_EQ(cp.saves(), 0u);  // 首次使用：文件还不存在是正常的，不算落盘
    ckpt_id = cp.put("cli", "", std::nullopt,
                     Json{{"messages", Json::array({"你好", "在吗"})}}, {"model"},
                     Json{{"step", 0}});
    EXPECT_EQ(cp.saves(), 1u);
  }  // 析构 = 进程退出

  ASSERT_TRUE(std::filesystem::exists(tmp.file()));

  {  // ---- 第二次"进程调用"：全新对象，只共享文件 ----
    FileCheckpointer cp(tmp.file());
    ASSERT_EQ(cp.thread_count(), 1u);
    auto snap = cp.get_tuple("cli", "", std::nullopt);
    ASSERT_TRUE(snap.has_value()) << "第二次调用没能从文件恢复历史";
    EXPECT_EQ(snap->values.at("messages").size(), 2u);  // ← 上一次留下的消息还在
    EXPECT_EQ(snap->values.at("messages")[0].get<std::string>(), "你好");
    EXPECT_EQ(snap->next, std::vector<std::string>{"model"});
    EXPECT_EQ(snap->config.value("checkpoint_id", ""), ckpt_id);

    // 续写一轮：链要接上（parent = 上一次的 id），而不是另起炉灶
    auto const id2 = cp.put("cli", "", ckpt_id,
                            Json{{"messages", Json::array({"你好", "在吗", "在的"})}}, {},
                            Json{{"step", 1}});
    auto const latest = cp.get_tuple("cli", "", std::nullopt);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->values.at("messages").size(), 3u);
    EXPECT_EQ(latest->config.value("checkpoint_id", ""), id2);
    ASSERT_TRUE(latest->parent_checkpoint_id.has_value());
    EXPECT_EQ(*latest->parent_checkpoint_id, ckpt_id);  // 跨进程的快照链是接续的
  }

  {  // ---- 第三次：链已经在文件里，list 能按新→旧读出两代 ----
    FileCheckpointer cp(tmp.file());
    auto all = cp.list("cli", "", 0);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].values.at("messages").size(), 3u);  // 新的在前
    EXPECT_EQ(all[1].values.at("messages").size(), 2u);
    // parent 链也必须**跨进程存活**（engaged 的 optional 走文件往返）
    ASSERT_TRUE(all[0].parent_checkpoint_id.has_value()) << "parent 在落盘/回读中丢了";
    EXPECT_EQ(*all[0].parent_checkpoint_id, ckpt_id);
    EXPECT_FALSE(all[1].parent_checkpoint_id.has_value());
  }

  // 原子落盘：不留临时文件
  EXPECT_FALSE(std::filesystem::exists(tmp.file() + ".tmp"));
}

TEST(FileCheckpointer, writes_and_delete_thread_are_persisted) {
  TempDir tmp;
  {
    FileCheckpointer cp(tmp.file());
    cp.put("t1", "", std::nullopt, Json{{"v", 1}}, {}, Json::object());
    cp.put("t2", "", std::nullopt, Json{{"v", 2}}, {}, Json::object());
    cp.put_writes("t1", "", "ck-1", {WriteRecord{"task", "messages", Json{{"a", 1}}}});
  }
  {
    FileCheckpointer cp(tmp.file());
    auto w = cp.get_writes("t1", "", "ck-1");
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].channel, "messages");
    EXPECT_EQ(w[0].value.at("a").get<int>(), 1);
    EXPECT_EQ(cp.thread_count(), 2u);

    cp.delete_thread("t1");  // 删除也要落盘
    EXPECT_EQ(cp.thread_count(), 1u);
  }
  {
    FileCheckpointer cp(tmp.file());
    EXPECT_EQ(cp.thread_count(), 1u) << "删除没有落盘";
    EXPECT_FALSE(cp.get_tuple("t1", "", std::nullopt).has_value());
    EXPECT_TRUE(cp.get_tuple("t2", "", std::nullopt).has_value());
  }
}

TEST(FileCheckpointer, corrupt_file_is_fail_closed_by_default) {
  TempDir tmp;
  {
    std::ofstream out(tmp.file());
    ASSERT_TRUE(out) << "测试自身的文件没写出来";
    out << "{ 这不是合法 JSON";
  }
  ASSERT_TRUE(std::filesystem::exists(tmp.file()));

  EXPECT_THROW(FileCheckpointer cp(tmp.file()), std::runtime_error);  // 不静默丢历史

  // 显式选择才允许从空开始
  FileCheckpointer cp(tmp.file(), FileCheckpointer::OnCorrupt::StartEmpty);
  EXPECT_EQ(cp.thread_count(), 0u);
}

TEST(FileCheckpointer, creates_missing_parent_directory) {
  TempDir tmp;
  std::string const nested = (tmp.dir / ".cppchain" / "sub" / "history.json").string();
  FileCheckpointer cp(nested);
  cp.put("cli", "", std::nullopt, Json{{"v", 1}}, {}, Json::object());
  EXPECT_TRUE(std::filesystem::exists(nested));
}
