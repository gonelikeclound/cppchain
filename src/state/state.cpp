// cppchain —— src/state/state.cpp
#include "cppchain/state/state.hpp"

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

}  // namespace

Json overwrite_reducer(Json const& /*left*/, Json const& right) { return right; }

Json add_messages(Json const& left, Json const& right) {
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