// cppchain —— workflow/refs.hpp
// 数据流引用（规范 11-ir-syntax.md §5）与运行时状态布局。
//
// 三种引用（整串或内联在模板字符串里）：
//   $_.result.<f>        最近一步节点的输出的字段 f
//   $<id>.result.<f>     点名帧：节点 <id> 的输出的字段 f
//   @config.<k>          运行前才填的配置占位（fire 时从 state["config"] 取 k）
//
// 解析语义（宽松）：
//   · 目标槽缺失 → json null（不报错；工具按缺失参数走 on_error）
//   · 模板字符串内：每个命中片段替换为解析值的文本呈现（字符串不带引号，其余用 dump()）
//   · 完整匹配一个引用的字符串 = 引用（保留 JSON 类型）；否则视为模板
//   · 语法损坏的 "$/@config" 片段：编译期由 validator 拒绝；运行时原样保留（防御）
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

// ---- 运行时状态布局（CompiledGraph 的输入/输出）----
inline constexpr char kConfigKey[] = "config";
inline constexpr char kMessagesKey[] = "messages";  // M12：对话通道（add_messages reducer）
inline constexpr char kResultsKey[] = "results";
inline constexpr char kLastExecKey[] = "_last_exec";
inline constexpr char kLoopCountsKey[] = "loop_counts";
// Context 节点（⑤）的产物槽：system 消息数组。model 节点装配请求时拼在系统提示之后，
// 不进 messages 通道/会话日志（replay 不回溯 → 上下文只对本 run 生效）。
inline constexpr char kSystemContextKey[] = "system_context";
// wfr-㊾ 批③：交接件 state 键（per-step 子键合并，见 compiler.cpp 的 schema / node_types.cpp c_executor）
inline constexpr char kHandoffStateKey[] = "handoff";
// wfr-㊹ 环检测：工具调用跨轮指纹表（方法 1，c_run_tools 消费点）
// 子键：{ "_prev": [上一轮执行指纹...], "<fp>": 连续次数 } —— merge_slots 右胜，对齐 loop_counts 范式
inline constexpr char kToolLoopKey[] = "_tool_loop";

enum class RefKind { LastResult, NamedResult, Config };

struct Ref {
  RefKind kind = RefKind::LastResult;
  std::string node;   // NamedResult 的节点 id
  std::string field;  // LastResult/NamedResult 的字段名；Config 时为配置键
};

// 扫描结果：一个引用片段及其在源文中的覆盖范围。
// ok=false 表示这是一个"疑似引用但语法损坏"的片段（validator 据此报 invalid_ref）。
struct RefScan {
  std::size_t begin = 0;
  std::size_t end = 0;
  bool ok = true;
  std::string problem;  // ok=false 时的说明
  Ref ref;
};

// 顺序扫描字符串里的全部 $ / @config 引用片段（含损坏的）。
[[nodiscard]] std::vector<RefScan> scan_refs(std::string_view s);

// 整串精确匹配一个引用：整串恰好是一个合法引用 → 该引用；否则 nullopt。
[[nodiscard]] std::optional<Ref> parse_ref(std::string_view s);

// 按引用从状态取 JSON 值；缺失 → null（宽松，见头注释）。
[[nodiscard]] Json resolve_ref(Ref const& r, Json const& state);

// 解析一个参数值：完整引用 → 直接取 JSON 值；含引用的模板串 → 文本替换；
// **数组/对象递归下去**（与校验器的递归 walk 对齐，否则嵌套引用会静默失效）；
// 其余（数字/布尔/无引用文本）原样返回。
[[nodiscard]] Json resolve_parameter(Json const& value, Json const& state);

// 校验用：字符串里是否有语法损坏的 $/@config 片段；有 → 说明此问题。
[[nodiscard]] std::optional<std::string> ref_syntax_problem(std::string_view s);

}  // namespace cppchain::workflow