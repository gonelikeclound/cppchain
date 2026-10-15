# 30 · Send 并行波 + 冲突护栏 验收（wfr-㊾ 批②）

> **链路**：设计档 [15](../01-discuss/15-多Agent协调设计档.md)（A 方向）→ [arch 21 §四批②](../02-arch-design/21-多Agent角色化PEV设计.md) →
> 本验收。批①（`mch` 三角色、串行 Executor + 独立 Reviewer）见 [29](29-多Agent角色化mch验收.md)。
> **验收标准（arch 21）**：examples + 单测。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `graph.hpp` | `CompileOptions.max_parallel`（默认 1 = 串行不变）；`GraphError::Kind::ConcurrentWriteConflict`；`NodeContext.emit_mu`（事件发射互斥，并行 worker delta 安全）；`CompiledGraph.emit_mu_`（shared_ptr 保持可拷贝可移动） |
| `graph.cpp` | 主循环顶部**波检测**：连续一段 Send 包（size>1）在 `max_parallel>1` 时 worker 线程并发执行（std::thread + join，屏障）；join 后按声明序逐键归约 + 冲突护栏 + 一个 wave 快照；成员路由产物按成员序入队 |
| `compiler.hpp/cpp` | `workflow::CompileOptions.max_parallel`（默认 1）透传给 graph `CompileOptions` |
| 测试 | `tests/graph/send_parallel_test.cpp`：**12 例**（见 §三） |
| demo | `examples/m10_send_parallel`：并行 map-reduce + 冲突护栏 fail-closed 演示（离线纯演示） |

**波语义**：生产者节点先走完整原串行收尾（apply/事件/快照），波再并行执行 → 快照/恢复/步数天然对齐，
串行路径（`max_parallel=1`）一字不改，既有测试零回归。
**冲突护栏**：同键并发写**默认 overwrite** 通道 → `ConcurrentWriteConflict`（fail-closed，点名键+节点）；
累积类 reducer（messages/results）同键合法按声明序折叠。
**明确拒绝**：波成员 `interrupt` → `InterruptUnavailable`（不默默串行化）；`is_restart` 重跑包不参与成波。

## 二、验收判据命中（评审者视角）

1. **确定性契约**：同图同输入，`max_parallel=1` vs `4` → 最终 state **逐键相等**（结果还同序）。并行
   只是「执行方式」，不是「另一条时间线」——会话日志重放与运行期不分叉的根基保住。
2. **真并发有据可查**：`actually_runs_in_parallel` 用「同时活跃峰值 ≥2」断言并发真的发生（不用计时，
   防 CI 抖动）；`width_one_is_serial_fifo` 反向断言串行时峰值恒为 1。
3. **冲突护栏诚实性**：两个成员写同一 overwrite 键 → 即时报错且点名 `'shared' written by nodes 'w' & 'w'`，
   不猜测哪个写者该赢 —— 逼出正确用法「并行步骤各写各的键/文件」（即 mch 并行 Executor 的形态）。
4. **事件确定性**：`event_order_follows_send_order` 断言 node_start/node_end 节点名序列 =
   orchestrator + 声明序，node_end step = {0,1,2,3}，seq 严格单调 —— 结构事件主线程按序发射。
5. **编织边界自洽**：波成员异常按声明序报首个（NodeError 带节点名）；波超 max_steps 提前 RecursionLimit；
   波后单快照可恢复续跑剩余包（`checkpoint_after_wave_and_resume`）。

## 三、测试清单（12 例，tests/graph/send_parallel_test.cpp）

| 例 | 断言 |
|---|---|
| wave_result_equals_serial | 并行 vs 串行最终 state 逐键相等 + results 声明序 |
| actually_runs_in_parallel | 两 worker 同时活跃峰值 ≥2（真并发，非计时） |
| width_one_is_serial_fifo | max_parallel=1 峰值恒 1（默认路径一字不改） |
| same_overwrite_key_conflicts | 同键 overwrite 并发写 → ConcurrentWriteConflict（点名键+节点） |
| accumulate_reducer_shared_key_is_folded | 累积 reducer 同键合法、按声明序 |
| distinct_keys_all_land | 各成员独立 overwrite 键全部落 |
| event_order_follows_send_order | 结构事件按声明序、step={0,1,2,3}、seq 单调 |
| member_interrupt_rejected | 波成员 interrupt → InterruptUnavailable |
| member_exception_reports_first_in_order | 首错按序、消息带节点名 |
| nested_fanout_deterministic | 二级扇出（mgr 并行各自再 sends worker）结果确定 |
| wave_respects_max_steps | 波超界提前 RecursionLimit |
| checkpoint_after_wave_and_resume | 波后快照承载 Send 包、放宽后并行恢复续跑 |

## 四、demo 输出（examples/m10_send_parallel）

```
=== m10_send_parallel: 并行波（4 个 worker 并发 / 5 步）===
  results 按声明序: abcd   max_parallel=4
  outa..outd: a,b,c,d（独立键全落）

=== 冲突护栏：两个成员写同一默认键 'shared' ===
  fail-closed → concurrent_write_conflict: 并行波写冲突（默认 overwrite 键被多个成员并发写）:
  'shared' written by nodes 'w' & 'w' —— 请给并行步骤分配独立输出键或用累积类 reducer
```

## 五、验收

- 全仓 **693/693** 全绿（681 基线 + SendParallel 12 例）；演示编译运行通过。
- 零回归：`max_parallel` 默认 1，串行路径未动一行；`GraphError::Kind` 扩展兼容既有 switch（新增枚举加断言的
  两处已补 —— `graph_error_kind` 与错误消息）。
- 真机并行 Executor（mch workflow 并行化 + 同任务并行对照）**押后批③**（交接式合并收口）。
- **遗留**：proxyNode（隔离委托）仍记下不实现（批②③ 后评估）；并行波内 interrupt 是明确拒绝项（文档化约束）。

## 备注
- 修正回溯一期：初版把 wave 放在「节点 Command 之后、归约之前」——步数/快照语义与串行不对齐
  （超界提前报导致 checkpoint 丢失）。重构为**主循环顶部波检测**（生产者先完整收尾，波再并行），
  语义与串行完全对齐，12 例一次通过。
- README 顶部测试数 681 → **693**（2026-10-15 更新）。