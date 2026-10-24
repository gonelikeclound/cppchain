# 24 · proxyNode：运行期调用子 workflow（隔离执行）设计

> **前文**：arch 21 §四预留原语「proxyNode（隔离委托，先记下不实现）」的**落地档**。
> 参考实现 = neuronic 系的 `workflow.call`（sync/detach + copyState + 审计落盘，见
> miclaw-rs 决策记录），本设计将其语义对齐进 cppchain 的 workflow 层。
> 状态：✅ 设计定稿 → 实现（problems/47 验收）。

---

## 0. 一句话

**新增 `kind:"subworkflow"` 节点：在运行期拉起一个子 workflow（独立 CompiledGraph），
与其隔离执行**。两种模式：

- `sync`：子 workflow 同步跑完，**最终 state 平铺回 `results.<nodeId>`**，主流程继续；
- `detach`：**fire-and-forget**——立即返回 `{detached:true}`，子 workflow 在后台线程执行，
  结果落盘 `.wfr/subflows/<name>.json`（写投影，不回传）。

子 workflow 与主 workflow **共享同一 `config`**（`@config.*` 引用继承），
但**状态完全隔离**（copy_state 显式拷贝 + input 字面量，其余键不进子 state）。

---

## 一、IR 语法（`kind: "subworkflow"`）

```jsonc
{
  "id": "export-report",
  "kind": "subworkflow",
  "workflow": "sub_calc",                  // 子 workflow 名/路径（字面量！见下）
  "mode": "sync",                          // "sync"（默认）| "detach"
  "copy_state": ["config", "cursor"],      // 从主 state 拷贝给子 state 的键（可选）
  "input": { "reason": "after-turn" },     // 字面量入参（可选，右胜：覆盖 copy_state 与 config）
  "next": "out"
}
```

| 字段 | 语义 |
|---|---|
| `workflow` | 子 workflow 名或路径（**字面量，不接受引用**——因为要在**编译期**读 JSON 并校验，
  编译期还没有 config 值）。解析走 compile 期注入的 **subworkflow_loader**（宿主负责按名/路径读
  JSON 并展开其静态 import；约定 = 返回**可直接 parse 的完整 JSON**）。需要按配置复用多个子流程时
  用顶层 `import`（静态展开）或宿主选不同 workflow 编译 |
| `mode` | sync=同步跑完回传（默认）；detach=后台 fire-and-forget，不回传 |
| `copy_state` | 从主 state 按键拷贝进子 state 的初始值（缺省 = 只给 config） |
| `input` | 字面量对象，与子 state 合并（**右胜**：input 覆盖 copy_state 与继承的 config；可含 `$*`/`@config.*` 引用） |
| `next` | 必填。sync/detach 的返回边（e_next） |

**生成顺序**：子 state 初始 = `{"config": <主 config>}` → 合并 `copy_state` 选中键 → 合并 `input`（右胜）。
所以 `@config.*` 在子 workflow 里天然可用（与主同源）；要读主 workflow 的其他键用 `copy_state`。

---

## 二、编译（fail-fast：子 workflow 在主 workflow 编译期即读完、校验完、编译完）

- 主 workflow 编译到 `subworkflow` 节点时（`c_subworkflow`）：
  1. `subworkflow_loader.read(spec)` → 子 workflow JSON；**读不到 → 编译失败**（不是运行期才发现）。
  2. `WorkflowIR::parse` → 子 IR。
  3. `validate`（用主 workflow 的 `config_keys`；子 IR 引用未声明的 `@config.*` → 编译期拒，fail-closed）。
  4. `compile_workflow`（同一 `registry` / `components` / `tool_policy` / `shared_registry`
     / `tool_lib_dirs` / `max_steps` / `max_parallel`）→ 子 `CompiledWorkflow`，捕获在节点闭包。
- **递归防护**（子 workflow 还能再嵌 subworkflow）：
  - 环检测：编译期记录当前 workflow id 链（`active_workflows`），子 workflow id 已在链中 → 报环错误；
  - 深度上限：`max_subflow_depth`（默认 4，`CompileOptions` 可调），超限 → "嵌套过深"编译失败。
- 子 workflow 编译产物 = 独立 `CompiledWorkflow`（自带 sink，但**不注入会话日志**，
  见 §四隔离纪律）。detach 线程用它的**拷贝**（`CompiledGraph` 可拷贝；内部 `emit_mu_` 为
  shared_ptr 共享无妨，本轮日志/状态均为本次 invoke 局部）。

---

## 三、运行（两类模式）

### 3.1 sync

1. 照 §一规则构造子初始 state `sub_state`。
2. `sub_wf.graph.invoke(sub_state)` → 子最终 state `final`。
3. 回传契约（关键）：**把 `final` 的 results 通道键提升为 `results.<nodeId>` 顶层键**（子 workflow
   每个节点的输出 → 帧的顶层字段），外加保留键 `_proxy`（mode/workflow/ok/state=完整 final）：

   ```jsonc
   "results": { "<nodeId>": {
        "<子节点id>": <该子节点的输出>,     // 子 workflow 各节点结果（单段引用可直接命中）
        "_proxy": { "mode": "sync", "workflow": "<名>", "ok": true,
                    "state": <子完整最终 state> }
   } }
   ```

   - 下游即用 `$<nodeId>.result.<子节点id>`（NamedResult 单段字段直接命中子节点产物）；
   - 保留键 `_proxy` 不与子 state 冲突（文档声明：子 workflow 不得产出 `_` 前缀顶层键；
     与 `_last_exec`/`_tool_loop`/`_artifact_ver` 同一条保留命纪律）。
4. `Command{update: results.<nodeId>, goto: next}`。

### 3.2 detach

1. 立即返回 `Command{update: results.<nodeId> = { "detached": true, "_proxy": {mode, workflow} }, goto: next}`
   （**不阻塞主流程**）。
2. 后台 `std::thread` 执行：
   - 拷贝 `sub_wf` → `sub_wf_copy.graph.invoke(sub_state, …)`（带事件收集 sink 以便审计）；
   - 结果落盘 `<config.workspace>/.wfr/subflows/<workflow>-<run>.<seq>.json`：
     `{ok, flow, runId, response, state, events[]}` 或 `{ok:false, error}`；
   - workspace 缺失 → 跳过落盘（fail-open 于可观测性，与参考实现一致）。
3. **线程安全**：每个 detach 用一份 `CompiledGraph` 拷贝 → 并发的多个 detach / sync 互不干扰；
   子 sink 不写会话日志（见 §四），无日志锁竞争。

---

## 四、隔离纪律（与参考实现同款）

1. **子 workflow 不进主会话日志**：子 sink 保持 `log=null`。主会话只记录「proxyNode 节点本身」
   （engine enter/exit，节点 id/`_proxy` 摘要），子 workflow 内部结构事件**不**写主 `.log.jsonl`，
   避免 replay 将子流程的消息/update 混入主历史（replay 铁律 §7.1 只看 engine+exit+ok）。
2. **子 workflow 不写主 state**：除了 `results.<nodeId>`（sync）和审计落盘（detach），
   子 workflow 无法触碰主 state 其他键——这就是「隔离执行」的确切含义。
3. **config 同源**：`@config.*` 与主 workflow 同一份 config 对象，保证子复用主配置
   （workspace/session/模型选择等），且无需在子侧另立声明。

---

## 五、与既有机制的边界（不混淆）

| 已有 | 是什么 | proxyNode 区别 |
|---|---|---|
| `import`（`workflow/import.hpp`） | **编译期静态展开**：子链节点拼进主图同一 IR，同一上下文/状态 | proxyNode 是**运行期调用 + 状态隔离**：主 state 不共享，子有自己的执行路径 |
| `as_agent_node` | 编译期把 `Agent` 包成图节点，**编译期就换来换去** | proxyNode 的子 workflow 是一个完整 workflow，编译期装载、运行期按需调 |
| `plugin_node`（dlopen .so） | 加载动态节点类型 | proxyNode 不涉及 node 类型注册，只涉及 workflow 复用 |

**一句话**：import = 复用**节点/子链**（静态）；proxyNode = 复用**整条 workflow 流程**（运行期、隔离）。
两者互补：子 workflow 内部仍可用 import 复用公共节点。

---

## 六、实现清单

| 层 | 文件 | 内容 |
|---|---|---|
| IR | `include/cppchain/workflow/ir.hpp` | `NodeKind::SubWorkflow`；`NodeIR` 增 `sub_workflow` / `subflow_mode` / `copy_state` / `sub_input` |
| 类型 | `src/workflow/node_types.cpp` | 注册 `struct NodeType`（parse/write/describe/v_/c_/edges/ref_texts）；`c_subworkflow` 编译装载子 workflow |
| 编译 | `src/workflow/compiler.cpp` / `compiler.hpp` | `CompileOptions` 增 `subworkflow_loader` + `max_subflow_depth`；编译期把 loader 注入 `CompileContext`；环/深度防护 |
| 校验 | `src/workflow/node_types.cpp` | `v_subworkflow`（VN24：workflow 非空 / mode ∈ {sync,detach} / copy_state 元素非空 / input 对象 / next 必填） |
| 引用 | `ref_texts` | 覆盖 `workflow` 串 + `input` 全部字符串叶（VR1/VR2/VR4 引用收集共用钩子） |
| 宿主 | `src/wfr/main.cpp` | wfr 宿主提供默认 `subworkflow_loader`（复用 `locate()` 按名解析 + import 展开） |
| 测试 | `tests/wf/proxy_node_test.cpp` | sync 回传 / detach 落盘 / 隔离契约 / 环与深度 / fail-closed |
| demo | `examples/proxy_node/` | 主 workflow 调子 workflow（sync + detach）全离线演示 |

## 七、验收预想（problems/47）

- `examples/proxy_node`：主 workflow `proxy_main`（编排）→ sync 调子 workflow `sub_calc`
  （纯 expr/工具，离线）回传数值 → detach 调子 workflow `sub_audit`（写文件）落盘 `.wfr/subflows/`。
- 全仓全绿（run-tests-report.sh）+ README 测试数更新。
- 单测覆盖：sync 平铺回传可用 `$<id>.result.<键>` 引用；detach 立即返回且后台落盘可断言；
  子 state 隔离（子改不了主 state 其他键）；缺 loader / 子 workflow 不存在 / 环 / 超深 → 编译失败；
  `mode` 非法 / 缺 next → VN24。