// sa_guard.hpp —— SA 语义层校验纯函数（M0，problems/64 §5.5 S3）
//
// M0 的本质（07 §0/§8）：「给定一个编排，SA 语义层是否安全」—— 必须**不需要 LLM、世界、时钟**，
// 纯函数即可判。本头文件把这一层做成**无 IO 的纯函数模块**（只依赖 std + nlohmann，不链 cppchain），
// 全部可单测：demo 跑两种形态 —— ① 单元自检（oracle 直灌好/坏样本，坏样本必须被拒）；
// ② 集成过检（S2 导演的真实编排轨迹 + samock 里程碑全量过一遍）。
//
// 与 研究 V1–V8 的对应（64 §5.5 S3，agent-group 形态）：
//   · V4 预算 → I05（每维 used ≤ limits，缺键视为违约）
//   · V6 断言 → I07（任务模板必带 goal + goal_assertion：完成必须可证）
//   · V1/V2/V3/V5/V7/V8：依赖 DAG/waits/pivot 等结构，agent-group 形态无这些 → **标注 N/A 不硬凑**，
//     等 Graph Runtime 形态出现再补。
// 组编排层新增不变式（07 §5.2 对抗集的映射）：
//   · I00/I01 未知 agent / 域外或幻觉工具（幻觉工具 → 拒绝）
//   · I02 核心工具全只读（不变式 I1 的静态面）
//   · I03 分诊目标 ∈ 已知域（"spawn 到不存在的域" → 拒绝）
//   · I04 里程碑 ∈ 词汇表（04 §6：词汇表之外一律不进 SA 上下文）
//   · I06 同 agent+工具+参数重复（S4 循环撞墙信号）
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cppchain::sa {

using Json = nlohmann::json;

// 编排轨迹里的一步（可序列化、可回放；验证器的输入）。
struct Step {
  std::string agent;  // "core" | "phone" | "msg" | …
  std::string tool;
  Json args;
};
struct Issue {
  std::string code;     // I00–I07
  std::string message;
};
struct TaskSpec {
  std::string domain;
  std::string goal;
  std::string goal_assertion;  // 完成必须可证
};

// 参数规范化：递归按键排序 → 稳定字符串（同一参数不同 JSON 写法 → 同一指纹）。
inline std::string canonical_json(Json const& v) {
  if (v.is_object()) {
    std::vector<std::string> keys;
    keys.reserve(v.size());
    for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    std::string out = "{";
    bool first = true;
    for (std::string const& k : keys) {
      if (!first) out += ",";
      first = false;
      out += "\"" + k + "\":" + canonical_json(v.at(k));
    }
    out += "}";
    return out;
  }
  if (v.is_array()) {
    std::string out = "[";
    bool first = true;
    for (auto const& e : v) {
      if (!first) out += ",";
      first = false;
      out += canonical_json(e);
    }
    out += "]";
    return out;
  }
  return v.is_string() ? v.get<std::string>() : v.dump();
}

// 域实例清白：agent 身份必须已知，所用工具必须在域白名单内。
inline std::vector<Issue> check_unknown_tools(
    std::vector<Step> const& steps,
    std::map<std::string, std::vector<std::string>> const& whitelist) {
  std::vector<Issue> out;
  for (Step const& s : steps) {
    auto it = whitelist.find(s.agent);
    if (it == whitelist.end()) {
      out.push_back(Issue{"I00", "未知 agent '" + s.agent + "' 调用了 '" + s.tool + "'"});
      continue;
    }
    bool known = false;
    for (std::string const& t : it->second)
      if (t == s.tool) {
        known = true;
        break;
      }
    if (!known)
      out.push_back(Issue{"I01", "agent '" + s.agent + "' 用了域外或幻觉工具 '" + s.tool + "'"});
  }
  return out;
}

// I1 静态面：核心 agent 的工具集必须全是只读（SA 的动作只作用于图）。
inline std::vector<Issue> check_core_tools_read_only(
    std::vector<std::string> const& core_tools,
    std::vector<std::string> const& read_only_tools) {
  std::vector<Issue> out;
  for (std::string const& t : core_tools) {
    bool ro = false;
    for (std::string const& r : read_only_tools)
      if (r == t) {
        ro = true;
        break;
      }
    if (!ro) out.push_back(Issue{"I02", "核心 agent 持有非只读工具 '" + t + "'（违反 I1）"});
  }
  return out;
}

// 分诊目标必须 ∈ 已知域（"spawn 到不存在的域"=客体幻觉）。
inline std::vector<Issue> check_dispatch(std::vector<std::string> const& route_targets,
                                         std::vector<std::string> const& known_domains) {
  std::vector<Issue> out;
  for (std::string const& t : route_targets) {
    bool ok = false;
    for (std::string const& d : known_domains)
      if (d == t) {
        ok = true;
        break;
      }
    if (!ok) out.push_back(Issue{"I03", "分诊路由到不存在的域 '" + t + "'"});
  }
  return out;
}

// 里程碑必须 ∈ 声明的词汇表（词汇表之外的东西一律不进 SA 上下文）。
inline std::vector<Issue> check_vocabulary(std::vector<std::string> const& kinds,
                                            std::vector<std::string> const& vocabulary) {
  std::vector<Issue> out;
  for (std::string const& k : kinds) {
    bool known = false;
    for (std::string const& v : vocabulary)
      if (v == k) {
        known = true;
        break;
      }
    if (!known) out.push_back(Issue{"I04", "词汇表外里程碑 '" + k + "'"});
  }
  return out;
}

// 预算（≈V4）：对 limits 的每个键，used ≤ limit；used 缺键 = 无法证明不超限 → 违约。
inline std::vector<Issue> check_budgets(Json const& used, Json const& limits) {
  std::vector<Issue> out;
  for (auto const& [k, limit_json] : limits.items()) {
    std::int64_t const limit = limit_json.get<std::int64_t>();
    if (!used.contains(k)) {
      out.push_back(Issue{"I05", "缺预算上报 '" + k + "'（无法证明不超限）"});
      continue;
    }
    std::int64_t const u = used[k].get<std::int64_t>();
    if (u > limit)
      out.push_back(Issue{"I05", "预算超限 '" + k + "': 用 " + std::to_string(u) + " > 限 " +
                                    std::to_string(limit)});
  }
  return out;
}

// 循环迹象（07 §5.2「重复调用同一工具同参数」→ S4 撞墙）：
// 同 agent + 工具 + canonical 参数重复超过 max_repeat 次 → 拒。
inline std::vector<Issue> check_dedup(std::vector<Step> const& steps,
                                      std::size_t max_repeat = 2) {
  std::map<std::string, std::size_t> counts;
  for (Step const& s : steps) {
    std::string const key = s.agent + "|" + s.tool + "|" + canonical_json(s.args);
    ++counts[key];
  }
  std::vector<Issue> out;
  for (auto const& [key, c] : counts)
    if (c > max_repeat)
      out.push_back(Issue{"I06", "同 agent+工具+参数重复 " + std::to_string(c) + " 次（S4 循环迹象）: " + key});
  return out;
}

// 任务模板（≈V6 断言）：goal 与 goal_assertion 都必填 —— 完成必须可证。
inline std::vector<Issue> check_task_templates(std::vector<TaskSpec> const& tasks) {
  std::vector<Issue> out;
  for (TaskSpec const& t : tasks) {
    if (t.goal.empty()) out.push_back(Issue{"I07", "任务模板缺少 goal: " + t.domain});
    if (t.goal_assertion.empty())
      out.push_back(Issue{"I07", "任务模板缺少 goal_assertion（完成不可证）: " + t.domain});
  }
  return out;
}

// 轨迹 → 用量（I05 消费方）：域工具轮数 + 拨号最大 attempt（重试上限实证）。
// 纯函数：全部由轨迹自证，宿主不给口报数。
inline Json usage_from_trace(std::vector<Step> const& steps) {
  int core_r = 0, phone_r = 0, msg_r = 0, attempts = 0;
  for (Step const& s : steps) {
    if (s.agent == "core")
      ++core_r;
    else if (s.agent == "phone")
      ++phone_r;
    else if (s.agent == "msg")
      ++msg_r;
    if (s.tool == "samock_make_call") attempts = std::max(attempts, s.args.value("attempt", 0));
  }
  return Json{{"core_tool_rounds", core_r}, {"phone_tool_rounds", phone_r},
             {"msg_tool_rounds", msg_r},    {"call_retry_attempts", attempts}};
}

}  // namespace cppchain::sa