// demo: state_basics —— 状态归约 / 快照与恢复 / 跨线程长期记忆（M5，纯离线）
//
// langchain 参照：
//   State + reducer（每键独立归约）、add_messages（按 id 去重）
//   Checkpointer（thread_id + 快照，可回放）、Store（跨线程 namespace KV）
#include <cppchain/message/serializer.hpp>   // opt_string：安全读可选字段
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/state/state.hpp>
#include <cppchain/state/store.hpp>

#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::state;

namespace {

void dump_messages(char const* label, Json const& state) {
  std::print("{}: ", label);
  for (auto const& m : state.at("messages")) {
    std::string const id = message::opt_string(m, "id").value_or("-");
    std::string text;
    for (auto const& b : m.at("content"))
      if (b.value("type", "") == "text") text += b.at("text").get<std::string>();
    std::print("[{}:{}] ", id, text);
  }
  std::println("");
}

}  // namespace

int main() {
  // ---- 1. 状态归约：messages 追加 / 同 id 覆盖，其它键覆盖 ----
  std::println("=== 1. StateSchema 归约 ===");
  auto schema = StateSchema::messages_state();

  Json state = Json::object();
  // 带显式 id 的消息 —— 才可能被"同 id 覆盖"（纯字符串消息没有 id，只能追加）
  auto msg = [](std::string const& id, std::string const& text) {
    return Json{{"type", "human"},
                {"content", Json::array({{{"type", "text"}, {"text", text}}})},
                {"id", id}};
  };
  state = schema.apply(state, Json{{"messages", Json::array({msg("m1", "第一句")})}, {"step", 1}});
  state = schema.apply(state, Json{{"messages", Json::array({msg("m2", "第二句")})}, {"step", 2}});
  dump_messages("  两轮之后", state);
  std::println("  step（未注册 reducer → 覆盖）: {}", state.at("step").get<int>());

  // 同 id 覆盖：模拟"上下文压缩"把历史里的 m1 改写为摘要（这正是压缩中间件要做的事）
  state = schema.apply(state, Json{{"messages", Json::array({msg("m1", "（m1 被压缩成摘要）")})}});
  dump_messages("  压缩改写 m1 后", state);

  // ---- 2. Checkpointer：多轮快照 + 回放 ----
  std::println("\n=== 2. Checkpointer（线程快照与回放）===");
  MemoryCheckpointer cp;
  std::string parent;
  std::vector<std::string> ids;
  for (int turn = 1; turn <= 3; ++turn) {
    state = schema.apply(state,
        Json{{"messages", Json::array({msg("t" + std::to_string(turn), "第 " + std::to_string(turn) + " 轮")})},
              {"step", turn}});
    // next 传 {"checkpoint-node"}：表示"该步跑完后待执行的节点"（这里仅作演示）
    parent = cp.put("thread-A", "", ids.empty() ? std::nullopt : std::optional(ids.back()), state,
                    std::vector<state::Packet>{{"checkpoint-node"}},
                    Json{{"source", "loop"}, {"turn", turn}});
    ids.push_back(parent);
    std::println("  快照 {} ← parent={}", parent, ids.size() > 1 ? ids[ids.size() - 2] : "(根)");
  }

  auto const latest = cp.get_tuple("thread-A", "", std::nullopt);
  std::println("  最新快照 messages 数: {}", latest->values.at("messages").size());

  // 回放到第 1 个快照（时间旅行的机制基础）
  auto const replayed = cp.get_tuple("thread-A", "", ids[0]);
  std::println("  回放到 {} → step={} messages 数={}", ids[0],
               replayed->values.at("step").get<int>(), replayed->values.at("messages").size());

  std::println("  该线程共有 {} 个快照，另一个线程: {}",
               cp.list("thread-A", "", 0).size(),
               cp.get_tuple("thread-B", "", std::nullopt).has_value() ? "存在" : "不存在（隔离）");

  // ---- 3. Store：跨线程长期记忆（按用户命名空间隔离）----
  std::println("\n=== 3. Store（跨线程长期记忆）===");
  MemoryStore store;
  store.put({"user", "u1", "facts"}, "city", Json("住在北京"));
  store.put({"user", "u1", "facts"}, "job", Json("做系统工程"));
  store.put({"user", "u2", "facts"}, "city", Json("住在上海"));

  std::println("  条目总数 {}，命名空间 {} 个", store.size(), store.namespace_count());
  for (auto const& item : store.search({"user", "u1"}, "", 0))
    std::println("  [{}] {} = {}", item.key, item.ns[1], item.value.dump());

  auto const hit = store.search({"user"}, "上海", 0);
  std::println("  搜索\"上海\" → 命中 {} 条（跨用户前缀检索）", hit.size());
  std::println("  namespace 列表: {}", Json(store.list_namespaces({"user"})).dump());

  // ---- 4. 整体可序列化（checkpoint 的地基）----
  std::println("\n=== 4. 状态整体可序列化 ===");
  auto const dumped = latest->values.dump();
  auto const restored = Json::parse(dumped);
  std::println("  快照 dump→parse 相等: {}  ({} 字节)", restored == latest->values, dumped.size());
  return 0;
}