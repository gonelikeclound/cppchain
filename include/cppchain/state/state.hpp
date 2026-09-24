// cppchain —— state/state.hpp
// 状态 schema 与 reducer（M5）。参照 langchain：每键独立 reducer，默认 overwrite。
// 状态统一用 JSON 承载 —— 直接满足"状态必须整体可序列化"的契约（M1 定的地基）。
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace cppchain::state {

using Json = nlohmann::json;

// 二元归约：new = f(left=当前累计值, right=本次更新)
using Reducer = std::function<Json(Json const& left, Json const& right)>;

// 默认语义：覆盖（丢弃 left）
Json overwrite_reducer(Json const& left, Json const& right);

// 消息归约（对应 langchain add_messages）：
//   · 归一化：接受本项目的规范 JSON、OpenAI 风格 dict({role,content}) 与纯字符串
//   · 按 message id 去重：新 id 追加、**同 id 覆盖**（后者胜）
//   · 无 id 的消息一律追加
Json add_messages(Json const& left, Json const& right);

class StateSchema {
 public:
  void set_reducer(std::string key, Reducer r) { reducers_[std::move(key)] = std::move(r); }

  [[nodiscard]] bool has_reducer(std::string const& key) const {
    return reducers_.find(key) != reducers_.end();
  }

  // 逐键归约：current 为当前状态，update 为本次更新（两者皆为 JSON 对象）
  [[nodiscard]] Json apply(Json const& current, Json const& update) const;

  // 预置：messages 用 add_messages，其余键 overwrite
  static StateSchema messages_state();

 private:
  std::unordered_map<std::string, Reducer> reducers_;
};

}  // namespace cppchain::state