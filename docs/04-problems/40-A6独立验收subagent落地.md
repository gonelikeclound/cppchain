# 40 · A6 独立验收 subagent（verify kind=agent）落地

> **链路**：[discuss 19](../01-discuss/19-Agent执行治理-跨系统优点提取.md) A6/A4（agent 型钩子：
> "verification requires inspecting actual files or test output"）→ [problems/39](39-治理手册对照评估与A6验收规划.md)
> 优先级 1（治 K1：判官只读 transcript 的洞）→ 本落地。

## 一、问题与决策

### 现状
verify 节点有三类判据：probe（接地，读文件真判）/ rule（确定性）/ model（裁判）——**model 裁判就是
K1 反模式的"只读 transcript"弱形态**：它只把 `step.goal + criteria + 最近消息文本` 喂给模型，
模型顺着执行记录自说自评，不真读产物文件（`judge_model_review`，node_types.cpp:1802-1836）。

### 用户拍板（AskUserQuestion）
1. **只读工具集 = 显式 toolset**：verify 节点加 `toolset` 字段，宿主预注册纯只读 toolset
   （`verify_ro = file_read/list_dir`）。写工具不在 registry 里 → 验收者模型发不出写调用
   （"不存在而非被拒"，agent.cpp:327 只发 registry 里的工具）。
2. **判不了 → 推进 + unverified**：对齐 problems/33 无判据档 + 总纲 G5（诚实标未验证），
   cursor 照推；靠 Reviewer 批级终审兑责。

## 二、实现

### 1. verify 节点 toolset 字段
`node_types.cpp` verify parse/write 块补 `toolset`（NodeIR 复用 model 节点已有字段，ir.hpp）。
c_verify 编译期解析：`ctx.components->toolset(n.toolset)` → 名字列表 → 从 `ctx.registry` 逐个
`find` 并**只拷贝 `spec->read_only==true`** 成只读 `tool::ToolRegistry` 快照（shared_ptr 捕获进 lambda）。
toolset 空 → 空工具集（验收者只能看 workspace 字符串，判不了 → unverified 推进）。

### 2. 判据函数 `judge_agent_review`（node_types.cpp，judge_step_self 后）
- 输入：`step`（goal）+ `v`（criteria/paths）+ `state` + `judge`（verify 节点 model）+ `ro_registry` + `judge_sys`。
- 构造验收 agent（复用 `agent::Agent`，同 subagent.cpp:53 形态）：
  ```cpp
  std::lock_guard lock(auditor_serial_for(m));   // 独立串行锁（subagent 的 serial_for 未公开，语义同款）
  agent::Agent auditor(*m, *ro, agent::AgentOptions{
      .system_prompt = judge_sys.empty() ? kAuditorPrompt : judge_sys,
      .max_iterations = 8});                    // 对齐 A6 "最多 50 轮"精神，落地缩 8
  auto ret = auditor.invoke({message::human_message(request)});
  ```
- `kAuditorPrompt`（固定，无注入面）："你是独立验收员。用给定只读工具检查产物是否满足【验收条目】。
  只输出一行 JSON `{"verdict":"done|fail|unverified","evidence":"<证据>"}`；证据不足判不了 → unverified；
  不得声称你无法验证的内容；不要修改任何文件。"
- request = goal + criteria + workspace + paths + "只读检查"。
- 三态解析（json_block_of 剥 JSON，同 judge_model_review）：
  - `done` → `{advance:true, status done}`；`fail` → `{advance:false, status failed}`；
  - 其余/解析失败 → `unverified` + **推进**（K2 fail-open 免疫）；模型错 → fail-closed。
- **transcript 隔离**：auditor 的局部 msgs 调 invoke，**不进 state.messages**（同 judge_model_review）；
  只回 verdict + evidence。

### 3. c_verify judge 分支（2317/2332 之间插 agent）
```cpp
} else if (kind == "agent") {
  auto const r = judge_agent_review(step, v, state, judge, ro_registry.get(), judge_sys);
  verdict_obj = Json{{"step", id}, {"kind", "agent"}, {"verdict", r.verdict},
                     {"why", r.evidence}, {"role", judge_role}};
  advance = r.advance;
}
```

### 4. 落账三态（原 has_verify 双档 → 按 verdict 三档）
- 步状态：`advance` 时 `verdict=="unverified"` → `status="unverified"`，否则 `"done"`；`!advance` → `"failed"`。
- results `outcome`：`unverified`（agent 判不了）/ `done` / `fail` / `auto_retry`（对齐 problems/33 + expr 门）。
- **修的一个运行时 bug**：`plan_log.push_back(std::move(verdict_obj))` 后再读 `verdict_obj` 是 null
  （value() with null 崩溃）——先取 `final_verdict` 再 move。

### 5. 宿主注册只读 toolset
`src/wfr/main.cpp` toolsets 区加 `comps.toolsets["verify_ro"] = {"file_read", "list_dir"};`。

## 三、测试（tests/wf/verify_agent_test.cpp 6 例）

| 测试 | 场景 | 断言 |
|---|---|---|
| `auditor_done_advances` | auditor 回 `{"verdict":"done","evidence":...}` | cursor=1、status=done、plan_log kind=agent verdict=done、evidence 进 why |
| `auditor_fail_blocks` | 回 `{"verdict":"fail"}` | cursor=0、status=failed、verdict=fail |
| `auditor_unverifiable_advances_as_unverified` | 回非 JSON | cursor=1、status=unverified、verdict=unverified（推进不卡死）|
| `auditor_tools_are_read_only` | toolset=verify_ro | model 收到的 tools 只有 read（无 file_write）|
| `auditor_without_toolset_gets_no_tools` | 无 toolset | 空工具集 → unverified + 推进 |
| `auditor_model_error_fail_closed` | 模型耗尽返回错误 | verdict=fail、不推进 |

## 四、边界 / 已知限制

- **验收者并发**：auditor_serial_for(judge) 独立锁（subagent.cpp:33 的 serial_for 未公开，语义同款）。
- **toolset 缺省降级**：verify 不写 toolset → 空工具集 → 验收者只能凭 workspace 判不了 → unverified 推进。
  合理但要用例 5 锁死（真机建议显式配 verify_ro）。
- **路径注入面**：paths 来自 `steps[].verify`（计划数据），提示文本拼接；judge_sys 若由宿主注入需自限。
- **验收者能力面**：只读工具真检查已落地；"跑测试命令"（受限 bash）未纳入（bash 是全副作用），
  后续若需"真跑检查脚本"再设计只读 bash 语义（problems/39 后续）。

## 五、结果

- 全仓 **775 全绿**（769 基线 + 6 新）。
- K1 洞补上：verify 现在有第 4 类判据 `kind=agent`——判官不再只读 transcript，而是带只读工具
  真读产物后给三态裁决 + 具体证据。
- 语义与 problems/33 一致：判不了 = 诚实标 unverified 且推进，Reviewer 批级终审兜底。