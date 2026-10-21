# 35 · 副作用 mock 场景库与可逆性梯度工作流（sidefx_mock_demo）验收

> **链路**：tool_registry 节点（problems/34，732 全绿）→ 本验收：用 mock 动态库把**可逆性梯度**
> 副作用工具（订单幂等 / 微信窗口可撤 / 短信不可撤 / 电话任状态可关）组织成一个可测工作流，
> 覆盖 30 个治理场景 case。

## 一、落地（改了哪里）

### 1. mock 场景库 `tests/plugin_demo/sidefx_notify.cpp`（新，`libsidefx_notify.so`）
纯 C descriptor + 裸函数指针（跨 DSO 安全，不链 cppchain）。每工具自带
`read_only=false + 显式 effect_class + approval + probe`：

| 工具 | effect_class | approval | 语义 |
|---|---|---|---|
| mock_order_create | Idempotent | Policy | 同 order_id 二次 → existing（幂等防重发）|
| mock_wechat_send / revoke | Idempotent | Policy | send 记 sent_at(模拟钟)；revoke 判 age < 120s 窗口 |
| mock_sms_send | **Irreversible** | Policy | **无反向工具**（不可撤 = 注册表层面没有撤销）|
| mock_call_dial / connect / hangup | Idempotent | NeedHuman(Policy) | 任何状态可挂：ring=接通前取消、answered=接通后关闭 |
| mock_clock_forward | Idempotent | Never | 拨快模拟钟（distinct N 幂等台账）→ 离线复现窗口过期 |
| mock_state_reset | Idempotent | Never | test-only：清库状态，隔离跨用例 static |

- **模拟钟**：`now() = system_clock + offset`，全库共用 → "相对 sent_at 的 age"可离线断言。
- **probe 故障注入档**：id 前缀 `?`→Unknown、`#`→Partial（F4/B5 测 probe 四格）。
- **状态表** `std::mutex` 守卫（handler 可并行、probe 可并发读）。

### 2. 引擎修复（两处，problems/34 边界收口）
- **`c_approve_tools` 共享回退**（node_types.cpp）：spec 解析先用内置表，`nc.shared_tools`
  兜底 —— 动态 mock 工具走 approve 不再被当"未知工具"静态拒（此前 approve 是全链唯一的断点）。
- **`desc_for` 稳定 descriptor**（mock_tools.cpp + sidefx_notify.cpp）：原 `static thread_local`
  在**批量 dlsym** 时所有 creator 复用同一个 d（全串成最后一把工具）→ 改按 name 的进程期
  `static map`。mock_tools 此前只单名加载未暴露，本次修复消除潜伏 bug。

### 3. wfr 宿主接线（src/wfr/main.cpp）
- `publish_tool_lib_dirs()`：仿 publish_workflow_dirs 三档 —— `WFR_TOOL_LIB_DIRS` env（: 分隔）
  → `<bin>/../lib`（安装布局）→ 编译宏 `WFR_TOOL_LIB_DIRS` 兜底（dev 自动命中 tests 目录）。
- `CompileOptions.shared_registry`（`tool::ToolRegistry shared_registry`）+ `tool_lib_dirs` 接线。
- usage 补 `sidefx_mock_demo` 说明。
- 边界记录：`effect_class_violations` 复检留在 fire 后（动态工具加载才发生）；release 无 .so
  需 `WFR_TOOL_LIB_DIRS` env 或装 `<prefix>/lib`。

### 4. workflow `share/wfr/workflows/sidefx_mock_demo.json`
入口 `tool_registry`（so 源 8 工具）→ `load_history` → `loop{ think(无 tools,活动集优先)
→ route(tool_calls→approve) → approve_tools(deny:[]) → run_tools → echo(_continue) }`。

### 5. 测试 `tests/wf/sidefx_notify_test.cpp`（31 case，映射 30 case 清单）
- **串行链 `chain_loop_ir`**：依赖型调用（send→revoke、dial→connect→hangup）拆成
  loop 每轮一个 tool_call —— 避开 run_tools **同批并行**导致的顺序竞态。
- A 装配 5 / B 订单幂等 5 / C 微信可撤 5 / D 短信不可撤 4 / E 电话可关 5 / F 治理审批 4 / G 时序 2。

## 二、验收判据

1. **可逆性梯度表达完整**：幂等（订单）/ 可撤（微信窗口）/ 不可撤（短信无反向工具）/
   可关（电话任状态）—— 全部走 mock 状态机 + probe，guard 只兜幂等防重放那半边。
2. **approve 全链对动态工具可用**：共享回退后 Policy 自动放行、NeedHuman 悬挂→resume→执行
   （F2 实测）。
3. **治理契约**：每工具 effect_class 显式、`mock_sms_send=Irreversible`；probe 四格闭环
   （Complete→replay / Partial→blocked / Absent→安全执行 / Unknown→blocked）。
4. **零回归**：全仓 763 全绿（732 基线 + 31 新）；现有 workflow（chat/governed/sidefx 等）不变。

## 三、边界（记录在案）

- **run_tools 同批并行**：一条 AI 消息多个 tool_call 并行执行 → mock 库状态依赖的调用
  必须走 loop 拆分（测试层选择；真机由模型自然分步）。库内 mutex 保线程安全但保不了语义顺序。
- **approve deny 只查内置表（VN17 限界）**：deny mock 名会被 VN17 拒（F1b 实测），
  deny 演示沿用内置名（F1a：send_notification）。
- **全程需真 LLM**：has_model_node 判定（model 节点存在即要 key）；单测用 RecordingModel。
- release 构建不出 .so：真机跑需 `WFR_TOOL_LIB_DIRS` env 或安装到 `<prefix>/lib`。

## 四、结果

- 全仓 **763 全绿**（732 基线 + 31 新）。
- `wfr debug sidefx_mock_demo --graph-only` 生成调试页（tool_registry 编译装配在 wfr 层通过）。
- `wfr list` 显示 `sidefx_mock_demo`。

## 附录 · 30 case 编排表（case → 测试 → 断言 → 工具）

> 断言统一走**库内状态 + guard 计数（replays/blocked/executions）+ ToolMessage status**，
> 不看模型嘴上（problems/03 §5 纪律）。`串行` = 用 `chain_loop_ir`（loop 每轮一个调用）驱动，
> 避开 run_tools 同批并行竞态；其余用并行 4 节点链。

### A 组 · tool_registry 装配与活动集（5）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| A1 | `A1_active_set_published_in_order` | 入口 tool_registry(so, 8 工具) 一次 fire | `results.treg.loaded` 与 `.active_tools` 均 == 声明序 8 名单 | tool_registry 节点 |
| A2 | `A2_model_schema_equals_active_set` | 同左；model 无 toolset 组件 | `last_opts.tools` function.name == 活动集（8 把） | model 消费活动集 |
| A3 | `A3_allowlist_rejects_non_active_tool` | 模型调用 `mock_ghost_tool`（非活动名） | allowlist 拒绝：error + 「不在本 workflow 的活动工具名单」，guard 不执行 | run 允许名单 |
| A4 | `A4_so_source_runtime_load_shares_active` | `mock_order_create{O-A4}` | success exec + created（so 工具已加载进 shared 并被 run 消费） | mock_order_create |
| A5 | `A5_no_tool_registry_zero_regression` | 无 tool_registry 的老 3 节点链 | model 空 schema；results 无 active_tools 帧 | — |

### B 组 · 订单 · 幂等（5）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| B1 | `B1_order_create_succeeds_and_persists` | `mock_order_create{O1}` | success created；guard executions=1 | mock_order_create |
| B2 | `B2_intent_residue_probe_complete_replays` | 先真建 O2 → 换 intent 残留 guard 同参 | probe Complete → **replay**（executions=0、replays≥1、文案含「已执行」） | guard+probe |
| B3 | `B3_same_args_same_order_replays` | 库有 O3 + FakeLog completed | NameAndArgs 命中 → replay（executions=0、replays=1） | guard 重放 |
| B4 | `B4_different_order_creates_new` | `mock_order_create{O4}` | executions=1 新订单 | mock_order_create |
| B5 | `B5_probe_unknown_fail_closed_blocked` | `mock_order_create{?O5}`（`?`=Unknown 注入） | probe Unknown → **blocked**（blocked=1、executions=0） | probe 故障注入 |

### C 组 · 微信 · 可撤回窗口（5）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| C1 | `C1_send_records_and_returns_msg` | `mock_wechat_send{M1}` | success sent；返回含 revoke_window_sec | mock_wechat_send |
| C2 | `C2_revoke_within_window_succeeds` | 串行 send M2 → revoke M2 | send success；revoke 窗口内 success revoked | send + revoke |
| C3 | `C3_revoke_out_of_window_errors` | 串行 send M3 → forward(500s) → revoke M3 | revoke error + 「已过撤回窗口」 | clock_forward 拨窗口 |
| C4 | `C4_double_revoke_errors` | 串行 send → revoke → revoke 同 M4 | 首次 revoked success；二次 error + 「已撤回」 | revoke 幂等 |
| C5 | `C5_revoke_result_referenceable` | 串行 send M5 → revoke M5 → text | act 帧有 summary（revoke 结果对下游 echo 可引用） | loop 链 echo |

### D 组 · 短信 · 不可撤回（4）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| D1 | `D1_sms_send_irreversible` | `mock_sms_send{S1}` → 查 shared spec | success；`effect_class==Irreversible`、read_only=false | mock_sms_send |
| D2 | `D2_no_reverse_tool_for_sms` | 一次 fire 看 schema | 含 "sms" 工具**只有** `mock_sms_send` 一把（无任何 revoke/undo） | 工具集无反向 |
| D3 | `D3_sms_same_args_replays` | 真发 S3 + FakeLog completed 同参 | guard replay（executions=0、replays=1）——不可撤 = 至少不重发 | guard 重放 |
| D4 | `D4_sms_intent_probe_complete_replays` | 真发 S4 + intent 残留 | probe Complete → replay（replays=1、executions=0） | guard+probe |

### E 组 · 电话 · 任何状态可关闭（5）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| E1 | `E1_dial_to_ring` | `mock_call_dial{C1}` | success + ring 状态 | mock_call_dial |
| E2 | `E2_hangup_before_connect_cancels` | 串行 dial → hangup C2 | hangup success + 「接通前取消」（未发生副作用） | dial + hangup |
| E3 | `E3_hangup_after_connect_closes` | 串行 dial → connect → hangup C3 | hangup success + 「接通后关闭」（已发生通话） | dial+connect+hangup |
| E4 | `E4_dial_intent_probe_complete_does_not_redial` | 真拨 C4 + intent 残留同参 | probe Complete → replay（replays=1、executions=0）不重呼 | guard+probe |
| E5 | `E5_ended_call_ops_error` | 串行 dial → hangup → connect C5 | ended 后 connect → error + 「通话已结束」 | 状态机完整性 |

### F 组 · 治理 · 审批 · guard 矩阵（4，5 节点 approve 链）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| F1a | `F1a_deny_builtin_tool_static_rejects` | approve deny `send_notification`；模型调它 | 审批静态拒绝 error；收件箱空（副作用被拦 in 审批链）；guard 未执行 | approve_tools deny |
| F1b | `F1b_deny_mock_name_vn17_rejected` | approve deny `mock_sms_send` | VN17 校验期拒（deny 存在性只查内置表——限界） | VN17 限界 |
| F2 | `F2_needhuman_dial_suspend_then_approve` | `mock_call_dial`（approval=NeedHuman）→ approve | interrupt 悬挂（payload calls 含 dial）→ resume approve → 执行 ring（guard executions=1） | approve 悬挂 |
| F3 | `F3_read_only_tool_not_in_active_by_default` | 一次 fire 扫活动集 | 全部场景工具 read_only=false（无只读可测免审——预期如此） | 场景工具契约 |
| F4 | `F4_probe_matrix_partial_blocked` | `mock_order_create{#O-F4}`（`#`=Partial 注入） | probe Partial → blocked（blocked=1、executions=0、error 回喂） | probe 故障注入 |

### G 组 · 时序与状态隔离（2）

| # | 测试名 | 输入场景 | 断言要点 | 工具 |
|---|---|---|---|---|
| G1 | `G1_clock_global_and_forward_idempotent` | 串行 forward(60)×2 | 首个 forwarded、第二个 already_applied（distinct N 幂等台账） | mock_clock_forward |
| G2 | `G2_state_accumulates_independent_across_rounds` | 串行 order O-G2 / wechat M-G2 / dial C-G2 | 各自 success 且结果含各自对象名（互不串扰） | 全场景工具累积 |