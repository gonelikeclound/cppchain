// wfr-㊵ 测试：历史消息处理的 **op 语义在会话日志上的落地**（replay 一致性）。
//
// 本专题的地基不变量：**日志永远 append-only 存原文；op 只改写"派生视图"**。
// 于是 replay 能重建任意时刻的视图，而裁剪/折叠不破坏原始数据。
//
// 这个文件盯的是最高危的一条纪律（risks R1）：新增的 summarize / refine_history 节点
// **绝不能**被加进 `compiler.cpp` 那份 `kind=="load_history"` 的回声抑制名单 —— 一旦加进去，
// 运行期的 op 就不落日志，replay 会得到未折叠的视图，**静默分叉**。
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/wfr.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

struct TempDir {
  static std::atomic<unsigned> counter;
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_histop_" + std::to_string(counter.fetch_add(1)));
  TempDir() { std::filesystem::create_directories(dir); }
  ~TempDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string file(std::string const& sess = "cli") const {
    return session_path(dir.string(), sess);
  }
};
std::atomic<unsigned> TempDir::counter{0};

Json ai(std::string const& text) { return Json(message::ai_message(text)); }
Json human(std::string const& text) { return Json(message::human_message(text)); }

Json summary(std::string const& text) {
  Message m = message::system_message(text);
  m.additional_kwargs = Json{{"cppchain_summary", true}};
  return Json(m);
}

void exit_record(SessionLog& log, std::string const& node, Json update) {
  log.engine_enter("r-1", node, "node");
  log.engine_exit("r-1", node, "node", true, "ok", 0, std::move(update));
}

std::string read_file(std::string const& path) {
  std::ifstream in(path);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

}  // namespace

TEST(WfHistoryOps, replay_applies_fold_ops_like_the_runtime_does) {
  TempDir tmp;
  std::string const path = tmp.file();

  {
    SessionLog log(path);
    exit_record(log, "__input__", Json{{"messages", Json::array({human("北京天气怎么样？")})}});
    exit_record(log, "think", Json{{"messages", Json::array({ai("我查一下")})}});
    // 摘要节点：op 落日志（**不能被回声抑制**）
    exit_record(log, "sum", Json{{"messages", Json{{"op", "fold"}, {"keep_last", 1},
                                                   {"base_size", 2},
                                                   {"prepend", Json::array({summary("用户问北京天气")})}}}});
    exit_record(log, "think", Json{{"messages", Json::array({ai("北京今天 25°C")})}});
  }

  auto const msgs = replay_session_messages(path);
  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[0].text(), "用户问北京天气");            // 折叠成摘要
  EXPECT_EQ(msgs[1].text(), "我查一下");                  // keep_last=1 保住的那条
  EXPECT_EQ(msgs[2].text(), "北京今天 25°C");             // 折叠之后新增的照常追加
  EXPECT_TRUE(msgs[0].additional_kwargs.contains("cppchain_summary"));
  // 序列协议不能被折叠破坏（真实端点会因孤儿 ToolMessage 直接 400）
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
}

TEST(WfHistoryOps, op_only_rewrites_the_view_never_the_original_log) {
  // "op 只改写派生视图"的哨兵：被折掉的原文必须仍然躺在日志里
  TempDir tmp;
  std::string const path = tmp.file();

  {
    SessionLog log(path);
    exit_record(log, "__input__", Json{{"messages", Json::array({human("被折掉的那句话")})}});
    exit_record(log, "think", Json{{"messages", Json::array({ai("a1")})}});
    exit_record(log, "sum", Json{{"messages", Json{{"op", "fold"}, {"keep_last", 0},
                                                   {"base_size", 2},
                                                   {"prepend", Json::array({summary("摘要")})}}}});
  }

  auto const msgs = replay_session_messages(path);
  ASSERT_EQ(msgs.size(), 1u);  // 视图里只剩摘要
  EXPECT_EQ(msgs[0].text(), "摘要");

  std::string const raw = read_file(path);
  EXPECT_NE(raw.find("被折掉的那句话"), std::string::npos)
      << "原文必须仍在日志里（日志是唯一事实来源，op 只改派生视图）";
  EXPECT_NE(raw.find("\"op\":\"fold\""), std::string::npos) << "op 本身也要落盘，否则 replay 分叉";
}

TEST(WfHistoryOps, node_diagnostics_carrying_ops_do_not_participate_in_replay) {
  // replay 铁律：只取 source=engine && phase=exit && status=ok。节点诊断（source=node）
  // 即使带 op 也不参与 —— 这正是"完整统计走 source=node、op 才进 update"这条分工的依据。
  TempDir tmp;
  std::string const path = tmp.file();

  {
    SessionLog log(path);
    exit_record(log, "__input__", Json{{"messages", Json::array({human("q")})}});
    log.node_log("r-1", "sum", "info", "摘要统计",
                 Json{{"messages", Json{{"op", "fold"}, {"keep_last", 0}, {"base_size", 1},
                                        {"prepend", Json::array({summary("不该出现的摘要")})}}}});
  }

  auto const msgs = replay_session_messages(path);
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].text(), "q");  // 诊断里的 op 被忽略
}

TEST(WfHistoryOps, replay_is_stable_across_repeated_calls) {
  // 同一个日志重放两次必须逐条相同（reducer 是纯函数，无隐藏状态）
  TempDir tmp;
  std::string const path = tmp.file();
  {
    SessionLog log(path);
    exit_record(log, "__input__", Json{{"messages", Json::array({human("q1")})}});
    exit_record(log, "think", Json{{"messages", Json::array({ai("a1")})}});
    exit_record(log, "sum", Json{{"messages", Json{{"op", "fold"}, {"keep_last", 1},
                                                   {"base_size", 2},
                                                   {"prepend", Json::array({summary("S1")})}}}});
    exit_record(log, "think", Json{{"messages", Json::array({ai("a2")})}});
    exit_record(log, "sum", Json{{"messages", Json{{"op", "fold"}, {"keep_last", 1},
                                                   {"base_size", 2},
                                                   {"prepend", Json::array({summary("S2")})}}}});
  }

  auto const a = replay_session_messages(path);
  auto const b = replay_session_messages(path);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]);

  // 两次折叠后摘要不堆叠（世代交替），只剩一条
  std::size_t summaries = 0;
  for (auto const& m : a)
    if (m.additional_kwargs.value("cppchain_summary", false)) ++summaries;
  EXPECT_EQ(summaries, 1u);
  EXPECT_EQ(a[0].text(), "S2");
}
