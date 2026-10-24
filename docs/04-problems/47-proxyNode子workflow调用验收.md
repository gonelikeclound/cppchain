# 47 · proxyNode（subworkflow 运行期调用子 workflow）验收（wfr-㊿）

> **链路**：arch 21 §四预留原语「proxyNode（隔离委托，先记下不实现）」→ 参考实现 = neuronic 系
> miclaw-rs 的 `workflow.call`（sync/detach + copyState + 审计落盘，`crates/core/src/engine.rs`）→
> 本设计 [arch 24](../02-arch-design/24-proxyNode子workflow调用设计.md) → 本验收。
> **验收标准（arch 24 §七）**：examples + 单测，9 例全绿；851 全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `ir.hpp` | `NodeKind::SubWorkflow`（新节点类型）；`NodeIR.sub_workflow` / `subflow_mode` / `copy_state` / `sub_input` |
| `compiler.hpp` / `compiler.cpp` | `CompileOptions.subworkflow_loader`（spec→JSON）+ `max_subflow_depth`；`CompileContext` 注入 loader/config_keys/max_steps/max_parallel；**编译期共用 `subflow_active` 编译链**（`SubflowChainGuard` RAII：顶层+嵌套编译共享一份，环/深度在编译期 fail-closed） |
| `node_type.hpp` | `CompileContext` 增加 `subworkflow_loader` / `max_subflow_depth` / `subflow_active` / `config_keys` / `max_steps` / `max_parallel` |
| `node_types.cpp` | `subworkflow` 节点注册：`v_subworkflow`（VN24）/ `c_subworkflow` / `e_next` / `t_subworkflow`；**c_subworkflow** 编译期经 loader 读子 IR → parse → validate（复用宿主 registry/components/config_keys）→ `compile_workflow` 递归编译（loader 与编译链继续下发，子内可再嵌子）；运行期 entry：构造子 state（config 继承 + copy_state 拷贝 + input 引用解析右胜）→ sync 同步 `graph.invoke` 后**子 results 通道键提升为帧顶层键** + `_proxy`；detach 立即返回 `detached:true` 并 `std::thread` 后台跑子图、落盘审计 |
| `validator.cpp` | VR2：`SubWorkflow` 加入"发布 result 的节点"白名单（`$<id>.result.<子节点id>` 可引用）；VR2 错误文案同步 |
| `wfr/main.cpp` | wfr 宿主提供默认 `subworkflow_loader`：`locate()` 按名/路径定位 + `expand_workflow_imports` 静态展开（子文件同目录基线） |
| 测试 | `tests/wf/proxy_node_test.cpp`：**9 例**（见 §三） |
| demo | `examples/proxy_call/`：主 workflow 编排 sync（sub_calc 求和）+ detach（sub_audit 落盘审计）双双验证，纯离线 |

### 关键设计定局

1. **编译期装载（fail-fast）**：子 workflow 不在运行期才找——主编译到 subworkflow 节点时：
   loader 读 JSON → `WorkflowIR::parse` → `validate`（同一 registry/components/config_keys，
   子引用未声明 @config → 编译期拒）→ `compile_workflow` 递归编译。找不到/校验不过/编译失败
   全部在**主编译期**报错（对齐 import 的"编译即炸"，比 miclaw 运行期 flows 表更早暴露）。
2. **环与深度**：`SubflowChainGuard` 把**当前编译的 workflow id** 放进程内共享集合，子编译
   递归时同一集合继续下发——`A→A` / `A→B→A` / `A→B→C→A` 都是环（编译期报"子 workflow 环引用"）；
   `max_subflow_depth`（默认 4）防线性深链。**注意**：guard 必须带 ctor（`emplace` 对聚合不触发
   成员副作用 → 改用带构造函数的类型，实测踩坑点，见 §三 5a 前身）。
3. **sync 回传 = 子 results 通道提升**：子 workflow 最终 state 的 `results` 对象（`子节点id → 输出`）
   原样提升为 `results.<proxyId>` 顶层键 + 保留键 `_proxy{mode,workflow,ok,state=完整子终态}`。
   于是 `$<id>.result.<子节点id>` 单段引用直接命中子节点产物（NamedResult 单段语法约束下最自然形态）。
4. **detach = fire-and-forget + 唯一写投影**：立即返回，后台线程跑子图（**每次拷贝** CompiledGraph，
   并发互不干扰），结果落盘 `<workspace>/.wfr/subflows/<spec>.json`（ok/flow/response/state/events[]）。
   workspace 缺失 → 跳过落盘（fail-open 于可观测性，与参考实现一致）。子 workflow 自属 sink 无日志
   ——**子不改主会话日志**，主日志只记 proxyNode 节点本身（隔离纪律 §四）。
5. **隔离**：config 同源（子 `@config.*` 用主同一个 config）、copy_state 显式拷贝、其余键不进子 state；
   子结果不回传主 state 其它键（除 results.<id>）；与 `import`（静态展开）互不替代。

## 二、demo 验收（examples/proxy_call，离线）

| 断言 | 结果 |
|---|---|
| sync：主 workflow 调 `sub_calc`（sum 工具，@config.a/@config.b），结果提升为 `results.sub.calc`，主流程 `$sub.result.calc != null` 门通过 | ✅ `results.sub.calc.value = 7`（3+4），`_proxy.ok=true, mode=sync, workflow=sub_calc` |
| detach：主 workflow 调 `sub_audit`（echo 备注），立即返回 `results.detach.detached=true`，`$detach.result.detached==true` 门通过 | ✅ 主 state 见 `detached:true`（不阻塞） |
| detach 审计落盘：`.wfr/subflows/sub_audit.json` 等文件出现 | ✅ `ok=true flow=sub_audit` |
| 编译期装载：loader 未提供 → 编译失败（`missing_loader_fails_compile`） | ✅ fail-closed |

## 三、单测（tests/wf/proxy_node_test.cpp，9 例）

| 编号 | 用例 | 断言 |
|---|---|---|
| 1 | `sync_returns_child_node_results_promoted` | 子 results 通道提升为帧顶层键：`results.sub.calc.value==5`；`_proxy` 携带 mode/workflow/ok/state；主 expr 门 `$sub.result.calc != null` 通过（走 _turn_end 而非 _fail） |
| 2 | `detach_returns_immediately_and_audits_to_disk` | 主 state 立即 `detached:true`（不等待子）；`wait_for_file` 等到 `.wfr/subflows/sub_sum.json`，内容 `ok=true flow=sub_sum` 且含 `state` |
| 3 | `copy_state_and_input_construct_child_state` | copy_state 从主 state 拷贝 `session`；input `@config.seed` 引用解析进子；子读 `@config.name`（同源 config）正常 |
| 4a | `missing_loader_fails_compile` | 未提供 loader → 主编译报 `subworkflow_loader`（编译期 fail-closed） |
| 4b | `missing_sub_workflow_fails_compile` | loader 找不到 spec → 主编译报 `找不到子 workflow` |
| 4c | `invalid_child_ir_fails_compile` | 子 IR 引用未注册工具 → 主编译报 `校验未过` |
| 5a | `self_cycle_fails_compile` | `sub_cyc` 引用自己 → 主编译报 `环`（编译链检测） |
| 5b | `nesting_depth_capped` | 三层链 + `max_subflow_depth=2` → 主编译报 `嵌套过深` |
| 6 | `vn24_invalid_modes_and_fields` | 缺 workflow / mode 非法（async）/ copy_state 空键 / input 非对象 / 缺 next → 均 VN24 |

## 四、回归与边界

- **851 全仓全绿**（842 + 9 新增）；`workflow_tests` 338 例含新测试。
- 已知边界（arch 24 §七 备注沿袭）：
  - 子 workflow 内仍可嵌 subworkflow（loader 递归下发、编译链共享）——递归上限 = `max_subflow_depth`；
  - detach 审计为 best-effort：`config.workspace` 缺失则只跑不落盘；后台线程写文件不 latch 主流程
    （等待靠文件出现，demo/测试用轮询）；
  - 子 workflow 编译沿用宿主组件/工具注册表（无独立注册表）；动态库节点（`imports`）在子内仍由
    宿主进程级加载约定，与主 workflow 同规则。

## 五、与后续（可选方向）

- detach 可设 `copyState` 选择性更强（如只拷某几键、用 `input` 字面量覆盖 config 子集）；
- sync 返回支持"只拷 `returns` 选键"而非全 results 提升（当前已含完整子 state 兜底）；
- 子 workflow 审计可扩展为离线 `fetch`/真机对照（现为纯内联 loader）；`max_subflow_depth` 可接
  宿主配置（`--set subflow_depth=N`），当前固定编译期参数。