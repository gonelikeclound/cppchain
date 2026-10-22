# 38 · Agent 工具环检测（方法 1 · 跨轮指纹）落地

> **链路**：problems/37 的「防重复调用/成环」盘点发现——重复执行已被 guard 防住、但**成环打转
> 只有次数上界** → 用户选定方法 1（workflow 侧 `c_run_tools` 消费点）+ 拍板语义
> （连续 3 次才提示、拦截不执行、只读豁免）→ 本落地。

## 一、问题与决策

### 现状盘点（problems/37 §六）
- **重复执行同一调用**：已被 ExecutionGuard（replay/probe）与 agent SideEffectGuard 双重防住。
- **agent 反复调工具成环**：此前只有次数上界兜底（workflow `max_steps=100` / loop `on_max→_timeout`
  / agent `max_iterations=10`），**无环语义检测**——换参数/换工具绕圈时仅"跑满轮次强停"。

### 用户拍板语义
1. **连续 3 次才提示**：第 1、2 次正常执行；**第 3 次起拦截**（error 回喂「已连续执行过多次，
   请换策略」）。容忍一次试探性重试，第三次真正掐。
2. **拦截不执行**：达阈值调用不进 to_exec、不进 guard（对齐 allowlist/deny 纪律）。
3. **只读工具完全豁免**（重复只读无害，同 guard 的 read_only 早退纪律）。

## 二、实现

### 1. 新 state 键 `kToolLoopKey = "_tool_loop"`（跨轮指纹表）
- `refs.hpp`：加常量；`compiler.cpp` schema `set_reducer(kToolLoopKey, merge_slots)`（对齐 loop_counts 范式）。
- 表结构（子键右胜合并）：
  ```json
  "_tool_loop": { "_prev": ["<上一轮执行指纹>..."], "<fp>": "<连续次数>", ... }
  ```

### 2. `c_run_tools` 判定块（node_types.cpp）
插在 allowlist 收尾与 exec 之间（**guard.prepare 之前**，与 done_ids/allowlist 同纪律）：

```
读 state[kToolLoopKey] → prev_exec 集 + 上一轮各指纹计数
for each all_calls[i]（非 denied）:
    spec = tn_eff.find(name)
    spec->read_only → 豁免（next_exec + continue，不进 prev/计数）
    fp = ExecutionGuard::key_for(call, NameAndArgs)      // 复用公开指纹（canonical_json 紧凑式）
    cur = fp ∈ prev_exec ? 旧计数+1 : 1                  // 连续；换工具/换参则复位
    cur >= 3 → repeat[i]=1（拦，计数保留、fp 仍进 this_prev 供下轮继续累进）
    否则 → next_exec + fp_map[fp]=cur + this_prev.push_back(fp)
写回 update[kToolLoopKey] = {_prev: this_prev, fp: cur, ...}
```

回喂归并：`repeat[i]` → `message::tool_message("工具 '<name>' 已连续执行过多次（疑似在兜圈子），
请换策略或换个参数，避免空转", id, "error")`。仅当本轮有非只读调用参与才写 state 键（`fp_map.size()>1`），
无环/纯只读 workflow 不产该键 = 零回归。

### 3. 实现中的两个修正（被测试逼出）
- **拦截后计数丢失**：最初拦截调用没进 `this_prev` → 下一轮 `prev_exec` 空 → 计数复位（第 4 次放行）。
  修复：**拦截也把指纹计入"上轮来过"**，下轮继续累进（第 3、4 次都拦）。
- **指纹键名猜测误差**：以为 canonical_json 带引号 `{"order_id":"S1"}`，实测是**紧凑式** `{order_id:S1}`
  （探针验证）→ 测试断言按实测字符串写。

## 三、测试（`tests/wf/tool_loop_test.cpp` 6 例）

| 测试 | 场景 | 断言 |
|---|---|---|
| `three_consecutive_same_call_blocks_on_third` | order{O1}×3 | 前 2 success、第 3 error「已连续执行过多次」；guard executions=2 |
| `different_args_reset_chain` | O1,O1,O2,O2 | 全 success（换参复位）+ executions=4 |
| `switching_tool_breaks_chain` | order,wechat,order | 全 success（换工具打断环）+ executions=3 |
| `read_only_tool_exempt_from_loop_detect` | fetch_price{P}×3（只读）| 全 success + executions=3（不拦）|
| `blocked_call_count_persists` | order{P}×4 | 第 3、4 次都 error + executions=2（计数保留）|
| `state_key_visible_after_execution` | 一次 order | `_tool_loop` 存在，含 `_prev` 与 `mock_order_create|{order_id:S1}` |

## 四、边界 / 已知限制

- **只识别"连续重复"**（同一指纹紧挨着），不识别"A→B→A→B"跨工具环——那需要周期检测，不在本次范围。
- **指纹 = NameAndArgs**：换参数即视为"新调用"，不会误伤"参数递进的正当循环"（如遍历分页）。
- **并行 run_tools 多写者**：若存在并行分支双写指纹表会丢更新（同 handoff 的 step 前缀缺陷，
  记录边界；loop 内单写者无碍）。
- **resume 一致性**：指纹表完全 state 派生（读 → 算 → 写），快照恢复重算一致（同 approve 纯态纪律）。
- 阈值 3 次是常量（node_types.cpp `kToolLoopThreshold`），未来可透出 `@config` 做成可配。

## 五、结果

- 全仓 **769 全绿**（763 基线 + 6 新）。
- 行为：模型对着同一工具同参打转，第 3 次起被 error 点醒（可换策略/换参继续，非死路）。