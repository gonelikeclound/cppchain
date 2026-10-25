// cppchain —— workflow/ir.hpp
// 主动 workflow 的 IR 模型与解析（规范：主动智能/11-ir-syntax.md §1–§7）。
// IR = 一段顶层 JSON {id, trigger, nodes}；本模块只负责"看懂它"，不负责执行。
//
// 两条纪律：
//   · trigger 只表达"何时醒"（D1：元数据 + 校验，不做 OS 调度，编译期不使用）。
//   · 节点类型判定按 §3 优先级：显式 kind:label → goto → expr → tool；
//     同时出现多个决定性键 → 判为 ambiguous（比 §3 的"第一个命中即定"更严，fail-closed）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

// ---- M12：保留目标名（编译期映射，不是节点）----
inline constexpr char kTurnEndName[] = "_turn_end";  // 正常终止
inline constexpr char kFailName[] = "_fail";         // 以错误终止（route 的 fail-closed 档）
// wfr-㊹ 治理（problems/33）：终态四档补齐 —— loop max 触达（超时）与用户取消（计划级 abort）
inline constexpr char kTimeoutName[] = "_timeout";    // 超时终止（loop on_max 触达未收口）
inline constexpr char kCancelledName[] = "_cancelled";  // 取消终止（plan_review abort / 用户中止）
// A7（problems/39）：做不到终止（verify judge 判 impossible —— 独立于"失败"，正常收尾不抛异常）
inline constexpr char kImpossibleName[] = "_impossible";
// ---- wfr-㉚：loop 体内的两个保留目标 ----
inline constexpr char kContinueName[] = "_continue";  // 下一轮（回 loop 节点头；算回边）
inline constexpr char kBreakName[] = "_break";        // 跳出循环（去 loop 的 next）

// ---- M12：route 的判别式与 outcome（**封闭词表**）----
// 判别式用 cppchain 自己归一化过的 outcome，不用端点给的自由字符串（如 finish_reason）——
// 分层上"端点差异由适配层吃住"，且端点字符串有实测差异（见 02-arch-design/11 §5.2.3）。
inline constexpr char kOnLastAiOutcome[] = "last_ai_outcome";
inline constexpr char kOutcomeToolCalls[] = "tool_calls";
inline constexpr char kOutcomeInvalidToolCalls[] = "invalid_tool_calls";
inline constexpr char kOutcomeText[] = "text";

// 校验与编译共用同一份词表，避免两处漂移
[[nodiscard]] inline bool is_known_discriminant(std::string_view s) {
  return s == kOnLastAiOutcome;
}
[[nodiscard]] inline bool is_known_outcome(std::string_view s) {
  return s == kOutcomeToolCalls || s == kOutcomeInvalidToolCalls || s == kOutcomeText;
}

// ---- trigger：v1 只放行 time/schedule（宿主导入的简写形）----
struct TriggerIR {
  std::string source = "time";               // 触发源：v1 只放行 "time"
  std::string kind = "schedule";             // timing.kind（简写形把 kind 提到 timing 下）
  std::string schedule;                      // 5 字段 cron，如 "0 8,20 * * *"
  std::optional<std::string> timezone;       // 简写形：由宿主时区补充，可缺省
  bool enabled = true;
  Json raw = Json::object();                 // 原样保留（元数据；校验只查 raw 的禁项）
};

enum class NodeKind {
  Tool,
  Expr,
  // ---- wfr-㉚：循环容器（已替代 label + goto，见 02-arch-design/16）----
  // loop 是**真实图节点**：节点头自己做「计数 + 分支」（Command.goto_nodes），
  // body 是它的子树；body 里的 `_continue` 回节点头（回边）、`_break` 跳到 loop 的 next。
  Loop,
  // ---- M12（组件化与命令行 agent，见 02-arch-design/11）----
  Model,     // 一次 LLM 调用：读 state.messages → 追加回复（不循环）
  Route,     // match 分派：按 last_ai_outcome 选下一跳
  RunTools,  // 执行最后一条 AI 消息的 tool_calls → ToolMessage 追加进 messages
  LoadHistory,   // 工作路径的历史消息 → 追加进 state.messages（④：日志的 replay 投影）
  // （save_history 已随 ④ 删除：history = 日志的投影，不再单独落盘）
  // ---- Context（⑤：上下文注入）----
  // 环境（env 键值对）+ 文本/文件（system 条目）→ 组装 system 消息写入 state[system_context]，
  // 由 model 节点装配请求时统一拼在系统提示之后；不进 messages 通道/会话日志（避免 replay 污染）。
  Context,
  // ---- wfr-㉗：回显节点（屏之道回显 thinking/工具摘要，纯副作用不改 state）----
  Echo,
  // ---- wfr-㊵：历史消息处理（见 02-arch-design/17）----
  Summarize,      // 把较早的一段历史折叠成一条 LLM 摘要（产 fold op 落日志）
  RefineHistory,  // 按规则精简/裁剪历史（产 transform op 落日志）
  // ---- wfr-㊷：副作用审批（见 02-arch-design/18）----
  ApproveTools,  // 审批决策：deny 名单 + read_only 免审；denied → error ToolMessage 回喂
  // ---- wfr-㊹：任务级 P-E-V 环（见 02-arch-design/19）----
  Plan,   // 产出/校验 `state.plan`（default_plan 预置 或 模型生成）
  Verify, // 每步门：phase=announce（把当前步骤目标喂给模型 + in_progress）/
          //         phase=judge（规则/schema 确定性判据或模型裁判 → done/fail + 推进 cursor）
  PlanReview, //（wfr-㊹ 批④）步骤边界任务级 HITL：interrupt 悬挂 → resume continue/rollback_to:K/abort
  Task, //（wfr-㊿ 长程）任务档案 begin/end：跨批还原（读 .wfr/tasks/<task>.json）与写回 + 批号/日期注入
  // ---- wfr-㊾ 批③：并行 Executor 与接收器（Send 波的 workflow 表达，见 02-arch-design/21 §四批③）----
  Executor, // 复合 worker：一次节点调用完成「模型 → 工具 → 自查」，是 `parallel` 扇出的 Send 目标
  Parallel, // 扇出+收拢编排：按 plan 步骤的 deps 把互不依赖的步以 Send 派给 Executor；join 后
            // 幂等收拢（唯一写 plan/plan_log 者），全 done → _break。宿主决定 max_parallel（IR 不写）
  // ---- wfr-㊹ 治理：动态工具源（见 02-arch-design/24，problems/34）----
  ToolRegistry, // 声明一组工具源（{from,type,load}），运行期加载进共享 ToolRegistry →
                // NodeContext.shared_tools；think/run_tools 从 context 取（共享优先、内置兜底）
  // ---- wfr-㊿：proxyNode —— 运行期调用子 workflow（见 02-arch-design/24）----
  SubWorkflow, // 编译期装载子 workflow（隔离 CompiledGraph），运行期按 mode 执行：
               //   sync  = 同步跑完，子最终 state 平铺回 results.<id>；
               //   detach = fire-and-forget（后台线程），子结果落盘 .wfr/subflows/ 审计。
  // ---- arch 26：project —— 项目级自管理的容器节点（begin/end 双相位）----
  Project,   // begin：读 .wfr/projects/<p>/project.json（fail-closed 缺档）→ 校验 @config.task
             //    ∈ tasks[] → 读 lessons.json → 拼项目级上下文写 state[system_context]
             //    （目标/完成标准/纪律/经验/本任务，与 Context 节点同形状，model 自动带上）；
             // end：state.lesson → lessons.json 追加沉淀；project.json.progress[task] 更新。
             //    任务档案仍归 task 节点（@config.project 非空时路径下沉到项目目录，见 arch 26）。
  // ---- ⑤ 动态库导入 ----
  Dynamic,   // 内含 kind 名匹配`导入的动态节点`（见 cppchain/workflow/plugin_node.hpp）；
             // 注意：**框架分发一律按名字走**（node_type_of_name），Dynamic 仅供
             // label/goto/on_error 等结构性判定时区分"不是内置节点"。
};

// ---- ⑤ 动态库导入声明（docs/02-arch-design/12 §5.3）----
struct ImportIR {
  std::string lib;                 // 库路径（**信任单元**，永远显式列出）
  std::vector<std::string> nodes;  // 显式节点名（A1 枚举落地前省略即报错）；取不到 → 校验期拒
};

struct OnErrorIR {
  int retry = 0;                             // 重试次数（总尝试 = retry + 1）
  std::optional<std::string> fallback;       // 失败降级目标节点 id（可为 _turn_end）
};

struct NodeIR {
  std::string id;
  NodeKind kind = NodeKind::Tool;

  // Tool 节点
  std::string tool;                          // 必须与目录内工具完全同名（§8）
  Json parameters = Json::object();          // 只放该工具的 input 字段（可含 $ / @config 引用）
  Json output_schema = Json::object();       // 声明的字段即下游可引用集（§5）
  std::string next;                          // 成功出口（必填）
  std::optional<OnErrorIR> on_error;

  // Expr 节点（else 是 C++ 关键字 → 用 else_）
  std::string expr;
  std::string then;
  std::string else_;

  int max = 0;  // Loop：body 的最大执行次数（正整数）

  // Loop 节点（wfr-㉚）：循环容器。id 全局唯一、**不 mangle**（图/日志/results 键按 id 对齐）。
  //   next   = 循环的**正常出口**（`_break` 去这里，也是"循环成功结束"的落点）；
  //   on_max = **跑满 max 仍没 break** 时的落点，缺省 `_turn_end`（= 旧 goto 超限跳 kEnd 的行为）。
  // 两者必须分开：轮询"查到了就通知"与"查满次数还没到货"是不同结局（loop_wf 正是如此）。
  // body 非空、max>0、next 必填；body 是子树，不得再含 loop。
  std::string on_max;                        // 可选（缺省 = _turn_end）
  std::vector<NodeIR> body;                  // 循环体子树（不得再含 loop）

  // Model 节点（M12）：一次模型调用。model/toolset 都是**组件名**（宿主注册，见 components.hpp）
  std::string model;                         // 必填：模型组件名
  std::string toolset;                       // 可选：工具集组件名（空 = 不带工具）
  std::string system_prompt;                 // 可选：本节点追加的 system 提示
  // wfr-㉙：厂商私有参数，原样并入请求体（→ CallOptions::extra）。值是运行期参数，
  // 可含 `@config.*`；解析为 null 的**顶层键会被丢弃**（不把 "k":null 发给端点）。
  Json model_params = Json::object();
  // wfr-㉙：出站是否回传 reasoning_content（drop / tool_calls / always，可写 `@config.*`）。
  std::string reasoning_echo;
  // 逐 token 流式：模型节点用 stream() 走真实请求流（请求体 stream:true），
  // 每个 delta 经 NodeContext::emit("token") 发布（--stream 时透出）。缺省 false = 非流式 invoke。
  bool stream = false;

  // Route 节点（M12）：match 分派。判别式与 outcome 都是**封闭词表**
  std::string on;                            // 判别式名，初版只有 "last_ai_outcome"
  std::map<std::string, std::string> cases;  // outcome → 目标节点 id（或保留名）
  std::string default_;                      // 未命中目标；**必须显式给出**（禁止漏档静默）

  // LoadHistory 节点（M12 / ④）：workspace/session 是运行期取值的**参数**
  // （可写 `@config.workspace` 这类引用，与该层其它参数同一套解析；历史即日志的投影）
  std::string path;
  std::string session;
  // wfr-㉙：读出来的历史按策略渲染（full / drop_round / keep_evidence，可写 `@config.*`）。
  // 压缩是**投影的渲染期变换**：结果绝不落盘（回声抑制见 compiler.cpp），故不影响 replay。
  std::string history_mode;
  std::string history_budget;                // keep_evidence 的单条工具结果内联 token 预算（可选）

  // Context 节点（wfr-context，⑤：上下文注入）。
  //   env    : {<键>: 字符串引用} —— 各值运行期解析（可为 @config.* 整串引用），
  //            聚成一条「【当前环境】key: value」system 消息；
  //   system : [{from:<字符串引用>} | {file:<字符串引用>}] —— 前者文本注入，
  //            后者读文件内容注入（宿主侧可信读；file 的值可为 @config.* 组合路径）。
  //   产物是系统消息 JSON 数组，写入 state[system_context]（见 refs.hpp kSystemContextKey），
  //   由 model 节点装配请求时统一拼在其系统提示之后 —— 不进 messages 通道与会话日志。
  Json context_env = Json::object();
  Json context_system = Json::array();

  // Summarize 节点（wfr-㊵）：模型调一次，把较早的一段历史折成一条摘要。
  // 下面这些都可以写成 `@config.*`（运行期解析：缺省回落、拼错硬错，同 wfr-㉙ 的手法）。
  std::string when;          // off | messages | tokens | always（缺省 messages）
  std::string max_msgs;      // when=messages 的阈值（条）
  std::string max_tokens;    // when=tokens 的阈值
  std::string keep_last;     // 折叠后保最近 N 条（缺省 6）
  std::string style;         // concise | detailed | evidence（提示词风格，缺省 concise）
  std::string summary_role;  // system | human | ai（摘要消息的角色，缺省 system）
  std::string on_fail;       // skip | placeholder（LLM 失败怎么办，缺省 skip）

  // RefineHistory 节点（wfr-㊵）：按规则精简/裁剪（规则见 agent/history.hpp 的规则表）
  std::string preset;        // minimal | lean | aggressive | clip（缺省空 = 只用 rules）
  std::string clip_scope;    // tool_results | ai_text | both（缺省 both）
  std::string clip_mode;     // truncate | artifact（缺省 truncate）
  std::string tool_budget;   // 单条工具结果的内联预算（缺省 200）
  std::string ai_budget;     // 单条终态 AI 回答的预算（缺省 400）
  std::string artifact_dir;  // clip_mode=artifact 时的落盘目录
  std::vector<std::string> rules;       // 静态规则清单（与 preset 取并集）
  std::vector<std::string> categories;  // drop_category 的类别（元素可为 @config.*，逗号分隔）

  // ApproveTools 节点（wfr-㊷）：副作用审批。deny = 要拒绝的工具名单
  // （元素可为 @config.resources 这类整串引用，运行期解析成数组）。
  // read_only 免审由 registry 推断（deny 一个只读工具会在校验期被 VN17 拒绝）。
  std::vector<std::string> deny;

  // Plan / Verify 节点（wfr-㊹，见 02-arch-design/19）
  //   plan  ：default_plan 非 null → 预置固定步骤 {steps:[…]}（三个测试 workflow 用它，verify 档写死）；
  //          否则走 model（组件名）让模型生成步骤（开放目标档）。goal 可写 @config.*（缺省 ""）。
  //   verify：phase = announce | judge | review（同一节点多态复用，见 c_verify）。
  //           judge  = 步内判据（done/fail + 推进 cursor）；review = 批级独立终审
  //           （wfr-㊾）：只喂计划 + 各步判定 + 判据，不喂执行思考（输入隔离的 Reviewer）。
  nlohmann::json default_plan = Json(nullptr);
  std::string goal;      // plan 节点的目标（可 @config.*）
  std::string phase;     // verify: announce / judge / review；task：begin/end；project：begin/end
  std::string task;       // task（㊿）：任务名（可 @config.*）
  std::string project;    // 项目绑定（arch 26）：project 节点 = 项目名（可 @config.*；空 = 走 @config.project）；
                          // verify phase=review 复用 = 启动"项目级终审"（判 project 整体是否达标，见 arch 26 第二刀）
  std::string confirm;   // plan_review（批④）："true/false"（假 = 关确认门直接过）；可 @config.*
  std::string role;      // verify（wfr-㊾）：审计角色名（如 "executor"/"reviewer"），plan_log 落行时带
  std::string criteria;  // verify phase=review（wfr-㊾）：批级判据（可 @config.*，缺省空 = 用 plan.goal）
  // wfr-㊾ 批③：parallel 节点扇出 Send 的目标节点 id（= Executor 节点），见 c_parallel
  std::string target;
  // wfr-㊹ 治理（problems/34）：tool_registry 节点的工具源声明
  // [{from:"mock_tools", type:"so",   load:["mock1","mock2"]}]（加载 .so 动态工具）
  // [{from:"builtin",   type:"select",load:["web_search","bash"]}]（仅激活内置工具，
  //  定义留在宿主内置注册表，不拷贝；load 名编译/校验期必须能于内置表解析）
  // 两类源的 load 并集 = **活动工具集**，由节点发布进 results.<id>.active_tools，
  // model/run_tools 从 context 取（共享优先、内置兜底）。from/type/load 结构二者共用。
  struct ToolSource { std::string from; std::string type; std::vector<std::string> load; };
  std::vector<ToolSource> tool_sources;

  // ---- wfr-㊿：proxyNode（见 02-arch-design/24）----
  // 编译期装载子 workflow（loader 由 CompileOptions 注入）；运行期隔离执行。
  std::string sub_workflow;              // 子 workflow 名/路径（字面量，编译期 loader 解析）
  std::string subflow_mode;              // "sync"（缺省）| "detach"（fire-and-forget）
  std::vector<std::string> copy_state;   // 从主 state 拷贝给子 state 的键（缺省只给 config）
  Json sub_input = Json::object();       // 字面量入参（可含 $ / @config 引用；右胜覆盖）

  Json raw = Json::object();                 // 原始节点对象（校验回读用）
};

// ---- P1.5（arch 25）：声明式配置 —— declarations.configs 声明区 ----
// 每个声明把一份配置文件（from，相对 CompileOptions.config_dir）注册进全局 config 表：
//   · 有 target  → 文件**顶层键平铺**为 config 默认值（"该类型节点的默认参数包"，全局可见）；
//   · 有 name    → 另注册命名空间 `config.<name>`（整个文件对象），节点用 `@config.<name>.<子路径>` 引用。
// 宿主 fire 时给的 config 顶层键覆盖声明缺省（命名空间整块覆盖）。
struct ConfigDecl {
  std::string name;              // 命名空间名（标识符，无 target 时也可用）
  std::string from;              // 配置文件相对路径（基准 = CompileOptions.config_dir）
  std::optional<std::string> target;  // 命中该节点类型的节点则声明视为"默认参数包"；可选
};

struct WorkflowIR {
  std::string id;
  // 可选：无 trigger 即「宿主手工触发」（wfr 直接拉起），调度语义由宿主决定（D1 无 OS 调度）
  std::optional<TriggerIR> trigger;
  std::vector<NodeIR> nodes;                 // nodes[0] = 入口（隐式，§3）
  std::vector<ImportIR> imports;             // ⑤：动态库导入声明（顶层"imports"键）
  // P1.5（arch 25）：声明式配置来源（顶层"declarations.configs"键）
  std::vector<ConfigDecl> declarations;

  // 从顶层 JSON 解析；失败返回人类可读错误（可直接回喂给生成器）。
  // 只做"看懂结构"，不做规则校验（规则在 validator.hpp，是另一道关）。
  static std::expected<WorkflowIR, std::string> parse(Json const& j);

  // 规范序列化（触发用简写形 {source, timing:{kind, schedule}}），用于 demo 落盘/展示。
  [[nodiscard]] Json to_json() const;

  // 按数组下标查节点；不存在 → nullopt。
  [[nodiscard]] std::optional<std::size_t> index_of(std::string const& id) const;

  // 节点类型判定（§3 优先级 + 歧义即错）。
  static std::expected<NodeKind, std::string> classify(Json const& node);
};

}  // namespace cppchain::workflow