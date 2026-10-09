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
//   · **op（wfr-㊵ 新增）**：right 若形如 `{"op": "<名>", ...}` —— 即"对累计消息施加一次
//     操作"，用于表达追加/覆盖表达不了的**折叠与删改**（历史摘要、精简、长内容裁剪）：
//       {"op":"fold", "keep_last":N, "base_size":M, "prepend":[<消息>…]}
//           剥掉头部旧摘要 → 校验 base_size 与当前条数相符（不符则**忽略本次 op**）→
//           保留尾部 N 条（防孤儿 ToolMessage）→ 前置 prepend。
//       {"op":"transform", "base_size":M, "drop":[下标…], "rewrite":[{"index":i,"message":…}]}
//           按下标删/改。越界下标忽略；base_size 不符则忽略本次 op。
//     语义要点：**op 是 left 的纯函数**（所需数据全部固化在 op 里，reducer 不调模型、不查
//     注册表、不看时钟）→ 会话日志重放同一条 op 必然得到同样结果，运行期与 replay 不会分叉。
//     未知 op 名 / 形状非法 → **原样返回 left（忽略）**，不抛异常（replay 与 wfr 打印会话
//     条数都会走这里，抛异常会把它们带崩）。
//     摘要消息靠 additional_kwargs 里的 `cppchain_summary` 标记被识别与剥离，从而"世代
//     交替"而不是层层堆叠。
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