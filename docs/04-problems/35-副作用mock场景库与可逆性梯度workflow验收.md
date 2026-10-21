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