// cppchain —— src/state/state.cpp
#include "cppchain/state/state.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"

namespace cppchain::state {

using cppchain::message::Message;
using cppchain::message::Role;

namespace {

// 把各种形态的消息表示归一化为本项目的规范 JSON（{"type":…}）
Json normalize_message(Json const& in) {
  if (in.is_string()) return Json(message::human_message(in.get<std::string>()));

  if (!in.is_object()) return in;  // 不认识的原样保留

  // 已是规范形态
  if (in.contains("type")) {
    try {
      Message const m = in.get<Message>();
      return Json(m);  // 反序列化再序列化 → 规范化
    } catch (std::exception const&) {
      return in;  // 解析失败则原样保留（不静默丢弃数据）
    }
  }

  // OpenAI 风格 dict：{role, content, tool_call_id?}
  if (in.contains("role")) {
    Message m;
    std::string const role = in.value("role", "user");
    if (role == "system") m.type = Role::System;
    else if (role == "assistant") m.type = Role::Ai;
    else if (role == "tool") m.type = Role::Tool;
    else m.type = Role::Human;

    if (in.contains("content") && in["content"].is_string() && !in["content"].get<std::string>().empty())
      m.content.push_back(message::TextBlock{in["content"].get<std::string>(), std::nullopt});
    if (in.contains("tool_call_id") && in["tool_call_id"].is_string())
      m.tool_call_id = in["tool_call_id"].get<std::string>();
    if (in.contains("name") && in["name"].is_string()) m.name = in["name"].get<std::string>();
    return Json(m);
  }
  return in;
}

std::string id_of(Json const& msg) {
  if (!msg.is_object()) return {};
  auto it = msg.find("id");
  if (it == msg.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

Json as_array(Json const& v) {
  if (v.is_null()) return Json::array();
  if (v.is_array()) return v;
  return Json::array({v});
}

// ---- wfr-㊵：messages 的 op 语义（折叠 / 删改）----
//
// 为什么需要它：add_messages 只能"追加 / 同 id 覆盖"，而 workflow 产出的消息都不带 id，
// 于是"折叠一段""删一批"在数组形态下**表达不出来**。op 把这类操作表达为"对累计消息施加
// 一次操作"：op 里固化了一切所需数据（摘要文本、下标、改后的内容），reducer 只做机械应用
// —— 因此它是 left 的**纯函数**，会话日志重放同一条 op 必然得到同样结果（运行期与 replay
// 不分叉）。规则怎么算、边界怎么选，全在**发出 op 的节点**里（那层才有 token 估算、工具
// 注册表与 LLM）；本层刻意不依赖它们（cppchain_state 只链 cppchain_message）。

// 摘要消息：靠 additional_kwargs 的标记识别（标记常量与 summarize 节点共用，见
// message.hpp 的 kSummaryMarker / is_summary_message）。
// fold 会剥掉头部旧摘要，于是摘要"世代交替"而非层层堆叠。
bool is_summary_message(Json const& msg) {
  if (!msg.is_object()) return false;
  auto const it = msg.find("additional_kwargs");
  if (it == msg.end() || !it->is_object()) return false;
  auto const flag = it->find(cppchain::message::kSummaryMarker);
  return flag != it->end() && flag->is_boolean() && flag->get<bool>();
}

bool is_tool_message(Json const& msg) {
  return msg.is_object() && msg.value("type", "") == "tool";
}

std::size_t head_summary_count(Json const& arr) {
  std::size_t n = 0;
  for (auto const& m : arr) {
    if (!is_summary_message(m)) break;
    ++n;
  }
  return n;
}

// base_size 守卫：op 与"发出它时的那个消息列表"绑定。条数不符（重复 apply / 被人手改过）
// 就忽略本次 op —— 宁可什么都不做，也不要折叠/删改到错误的位置。
bool size_mismatch(Json const& op, std::size_t actual) {
  if (!op.contains("base_size") || !op["base_size"].is_number_integer()) return false;
  return op["base_size"].get<long>() != static_cast<long>(actual);
}

// 应用一条 op。**全函数、不抛异常**：未知 op 名或形状非法 → 原样返回 left。
// （add_messages 有三个读者：运行期 StateSchema::apply、会话日志 replay、wfr 打印会话条数；
//  抛异常会把第三个带崩。非法 op 该由发出端的 validator 在编译期挡住。）
Json apply_message_op(Json const& left, Json const& op) {
  Json const src = as_array(left);
  std::string const name = op.value("op", "");

  if (name == "fold") {
    std::size_t const start = head_summary_count(src);
    Json base = Json::array();
    for (std::size_t i = start; i < src.size(); ++i) base.push_back(src[i]);
    if (size_mismatch(op, base.size())) return src;  // 在"剥掉旧摘要后的 base"上比对

    std::size_t keep = base.size();
    if (op.contains("keep_last") && op["keep_last"].is_number_integer()) {
      long const k = op["keep_last"].get<long>();
      if (k >= 0 && static_cast<std::size_t>(k) < base.size()) keep = static_cast<std::size_t>(k);
    }
    Json tail = Json::array();
    for (std::size_t i = base.size() - keep; i < base.size(); ++i) tail.push_back(base[i]);
    // 防孤儿：保留区不得以 Tool 消息开头（节点侧另有 safe_boundary，这里是兜底）
    while (!tail.empty() && is_tool_message(tail.front())) tail.erase(tail.begin());

    Json out = Json::array();
    if (op.contains("prepend"))
      for (auto const& m : as_array(op["prepend"])) out.push_back(normalize_message(m));
    for (auto const& m : tail) out.push_back(m);
    return out;
  }

  if (name == "transform") {
    if (size_mismatch(op, src.size())) return src;
    std::vector<bool> drop(src.size(), false);
    if (op.contains("drop") && op["drop"].is_array())
      for (auto const& d : op["drop"]) {
        if (!d.is_number_integer()) continue;
        long const i = d.get<long>();
        if (i >= 0 && static_cast<std::size_t>(i) < src.size()) drop[static_cast<std::size_t>(i)] = true;
      }
    std::map<std::size_t, Json> rewrite;
    if (op.contains("rewrite") && op["rewrite"].is_array())
      for (auto const& r : op["rewrite"]) {
        if (!r.is_object() || !r.contains("message")) continue;
        auto const it = r.find("index");
        if (it == r.end() || !it->is_number_integer()) continue;
        long const i = it->get<long>();
        if (i < 0 || static_cast<std::size_t>(i) >= src.size()) continue;
        rewrite[static_cast<std::size_t>(i)] = normalize_message(r["message"]);
      }
    Json out = Json::array();
    for (std::size_t i = 0; i < src.size(); ++i) {
      if (drop[i]) continue;
      auto const it = rewrite.find(i);
      out.push_back(it != rewrite.end() ? it->second : src[i]);
    }
    return out;
  }

  return src;  // 未知 op → 忽略（不抛）
}

}  // namespace

Json overwrite_reducer(Json const& /*left*/, Json const& right) { return right; }

Json add_messages(Json const& left, Json const& right) {
  // op 分支（wfr-㊵）：对累计消息施加一次"操作"——追加/覆盖表达不了的折叠与删改。
  // 判定从严：对象 + op 是字符串。**规范消息 JSON 里永远没有 "op" 键**，所以下面的数组
  // 分支逐字节保持原行为，向后兼容不受影响。
  if (right.is_object() && right.contains("op") && right["op"].is_string())
    return apply_message_op(left, right);

  Json out = as_array(left);
  for (auto const& raw : as_array(right)) {
    Json msg = normalize_message(raw);
    std::string const id = id_of(msg);
    if (!id.empty()) {
      bool replaced = false;
      for (auto& existing : out) {
        if (id_of(existing) == id) {
          existing = msg;  // 同 id 覆盖
          replaced = true;
          break;
        }
      }
      if (replaced) continue;
    }
    out.push_back(std::move(msg));
  }
  return out;
}

Json StateSchema::apply(Json const& current, Json const& update) const {
  Json out = current.is_object() ? current : Json::object();
  if (!update.is_object()) return out;

  for (auto it = update.begin(); it != update.end(); ++it) {
    auto const reducer = reducers_.find(it.key());
    if (reducer == reducers_.end()) {
      out[it.key()] = it.value();  // 未注册 → overwrite
    } else {
      Json const left = out.contains(it.key()) ? out[it.key()] : Json();
      out[it.key()] = reducer->second(left, it.value());
    }
  }
  return out;
}

StateSchema StateSchema::messages_state() {
  StateSchema s;
  s.set_reducer("messages", add_messages);
  return s;
}

}  // namespace cppchain::state