# 06 · bash 工具副作用治理（wfr-㊸ 设计稿）

> **状态**：🟢 进行中（批①② 已落地，批③④ 待启动；落地点位见 §9/§10）
> **一句话**：bash 内容不确定 → 不预测语义，改管三件事——**声明**（把任意效果标成不可信输入）、
> **环境**（可丢弃的沙箱边界）、**决策点**（词面保守分类 + interrupt 人审），审计兜底、回滚=丢弃边界。
> **研究来源**：六路并行深度研究（Claude Code / OpenAI Codex+Agents SDK+Code Interpreter /
> Gemini CLI / OpenHands / LangGraph / cppchain 代码库测绘），机制细节均有官方文档/源码出处，
> 术语均经逐版本/逐 tag 核实（含三处对我方假设的纠正，见 §8）。

---

## 1. 动机与问题

新增 bash 工具：它调用任意命令行工具，执行内容事先不确定 → **框架无法在编写期枚举其副作用**，
因此"管理工具副作用"这条路对 bash 是断的。问题不是"去掉这个工具"，而是设计一个
**把不确定性显式建模**的治理体系——这正是所有顶级 agent 共同面对、且已给出成熟答案的题。

直接回答：**不管理副作用本身，管理产生副作用的三要素**。
1. **声明**：bash 工具契约里显式标注"任意效果/不可信输入"——这是后续所有门禁的前提；
2. **环境**：把 bash 关进可销毁的边界（沙箱/容器/只读挂载），内容再刁钻也出不来；
3. **决策点**：在命令**词面上**做保守裁决（能证纯只读才放行，其余/未知 fail-closed 升级给人），
   审批结果回流给模型使其自纠；
再叠加 **限额**（timeout/输出截断）与 **审计**（先记后执，回滚=丢弃边界而非命令反做）。

## 2. 六家治理体系对照（7 层 × 6 系统）

| 层 | Claude Code | OpenAI Codex/Agents SDK | Gemini CLI | OpenHands | LangGraph | cppchain 现状 |
|---|---|---|---|---|---|---|
| 1 声明层 | permission rules + modes | `SandboxPolicy` 四档 / `needs_approval` | 工具按 Kind 分类，bash=任意效果执行工具 | Action 类型化（副作用绝不发生在黑盒里） | 无内建"副作用等级"字段，由编排层声明 | ✅ ToolSpec `tool.hpp:34-68`（read_only / probe:`53`） |
| 2 静态门禁 | **AST 解析**：读不了→强审；只读命令表+危险签名+critical-path 熔断 | 前缀 token 三态 Allow/Prompt/Forbidden，不做语义解析 | 危险签名+只读白名单+逐参数路径校验；headless 默认 deny | deterministic analyzer（两套语料） | 编译期 `interrupt_before` 名单（=白名单） | ◐ ExecutionGuard `exec_guard.cpp:86-170`（只读早退 `:89`）可演化为该层 |
| 3 审批/HITL | 弹窗 allow once/会话/永久 + PreToolUse hook + 理由回流 | `@tool(needs_approval)` → interruption → approve/reject → resume | plan/default/auto_edit/yolo 四档，`ask_user` 非交互=deny | `ConfirmRisky`：**UNKNOWN 也确认**；拒绝理由成 UserRejectObservation | `interrupt_before=["tools"]` + `Command(resume=…)` | ◐ approve_tools `node_types.cpp:1006-1051` 是静态策略，接 interrupt 是 doc 18 明示的后续迭代 |
| 4 沙箱 | bubblewrap+netns / Seatbelt | bwrap+seccomp / Seatbelt / Docker | bwrap 全套 / Seatbelt / Windows native | **每会话一个容器，`docker rm` 即回滚** | ❌ 框架本体没有 | ❌ 仅 `resolve_path_in_root` 路径沙箱 |
| 5 限额 | 120s 默认超时 / 输出 30k 内联 / 5GB 击杀 / cgroup 内存 | 输出分片 + `truncated_after_lines` 截断标记 | /tmp tmpfs、后台 `/shells` 看板可 kill | timeout=300s、输出按 order 分片、PIDs 512 | ❌ 官方 bash 无 timeout（`subprocess.run` 可无限挂） | ❌ ExecutionGuard 无 timeout |
| 6 审计 | JSONL transcript 全量 + bashEditDiff | trace/span | 会话上下文全记录 | **append-only 事件流 = 事实源**，重放恢复状态 | checkpoint 只回放状态/消息，OS 副作用不进 | ✅ 会话日志 `session_log.hpp:52-104` + audit 落盘 `main.cpp:450-471`，已超 LangGraph |
| 7 回滚 | `/rewind` **明确不追踪 bash 改动**，回滚靠 git | 默认 read-only，写域显式 | git worktree + 沙箱只读 | 容器销毁=回滚，无命令级 undo | 时间旅行只回退状态 | ◐ 时间旅行 fork 已备（M10），bash 回滚是留白 |

**每家的招牌结论（原文）**：
- **Claude Code**："Checkpointing does not track files modified by Bash commands"——bash 副作用天然不可追踪，是搬回来时最重要的已知边界。
- **OpenAI**："宁可沙箱里放跑 + 边缘审批 + 全量审计，也绝不解析命令词面来预测会发生什么。"
- **Gemini**："不试图知道命令会做什么，默认只读+禁网+沙箱跑、默认问人、白名单里的纯只读才静默放行；被弹回就带更多权限重跑同一命令。"
- **OpenHands**："UNKNOWN means 'I don't know', not 'safe'."——静态分析与模型自评都不可靠，兜底是容器可销毁 + 最终人审。
- **LangGraph**：bash 工具本身裸奔（源码唯一警告 "The shell tool has no safeguards by default"），治理全在编排层。
- **Anthropic 官方（on-call 指引原文）**："commands are untrusted model output — 放进隔离环境；用可执行文件白名单；拒绝 shell 元字符；设 timeout 与资源限额；记录每条命令；黑名单不够。"

## 3. 收敛出的四条公理（六家一致）

1. **不预测语义，只做词面保守分类**：能证"纯只读"才免审；解析不了/未知/含危险签名一律升级——没有"看起来能跑就放行"。
2. **环境隔离是唯一真正兜底**：C++ 库内无法拦截任意命令，exec 一律经外部执行器（见 §5-L4）。
3. **审批是"类别"级而非"枚举内容"级**：无法预枚举的副作用交给"需人审"这个类别；拒绝理由必须回流给模型（可自纠闭环）。
4. **审计先记后执，回滚=丢弃边界**：不做命令级 undo；仓库场景 git 快照，容器/沙箱场景直接销毁。

## 4. cppchain 现状盘点（已具备的机械）

- 执行点两处：`c_tool`（`node_types.cpp:666-736`，guard 决策门 `:694-708`）、`c_run_tools`（`:894-997`，`prepare` 全过→并行 `run` `:932-961`）；注入链 `CompileOptions::tool_policy`（`compiler.hpp:61`）→ `guard`（`node_type.hpp:76`）→ 宿主保活（`main.cpp:472-476`）。
- ExecutionGuard：两步 `prepare/run` + 一步 `execute`；决策树（read_only 早退 `:89`、completed 重放 `:101-124`、Intent 残留→probe 兜底 `:126-169`）；审计 `AuditRecord` + decision 词表。
- approve_tools 节点：决策层纯 state 函数；deny 名单 + read_only 免审 + 未知工具 fail-closed deny；防 deny 被静默反转（`c_run_tools` 前置过滤 `:913-929`）。
- M10 全原语：`interrupt(payload, ctx)`（`graph.hpp:133-136`）、resume（`graph.hpp:65-70`）、时间旅行 fork（`ForkSpec{source_thread,checkpoint_id}` `graph.hpp:59-63`，实现 `graph.cpp:133-146`）、每节点快照（checkpointer）。
- 事实源：会话日志 append-only JSONL + guard 重放阅读器 `replay_exec_log_reader`（`session_log.cpp:277-341`）+ 审计落盘。
- 独有优势：`ToolSpec::probe`（判据闭合四格，`tool.hpp:53`；file_write 已示范 `file_tools.cpp:200-220`）——LangGraph 没有的崩溃兜底。

## 5. cppchain 落地设计（L1–L7）

> 铁律：**库内永远不直接 `system()` 跑 bash**。执行一律走 L4 的 executor 抽象。

### L1 声明层 —— ToolSpec 补"副作用契约"
新增字段（对标 Gemini per-Kind + OpenAI SandboxPolicy + LangGraph `interrupt_on`）：
```
ToolSpec::approval      : never | policy | need_human     // bash 出厂 = need_human
ToolSpec::timeout_ms    : uint64                          // 缺省 120_000
ToolSpec::executor      : SandboxManager*                 // 见 L4，缺省进程内 handler
```
bash 注册为 `read_only=false, category="action", approval=need_human`——把"任意效果"显式声明成不可信输入。

### L2 静态门禁 —— 壳感知分类器（纯函数）
轻量 POSIX shell lexer（按 `&& || ; | &` 与换行拆子命令、剥 `timeout/nice/nohup/stdbuf/command/builtin` 类 wrapper），
对每个子命令 tokenize 后二分：
- **只读白名单**（`ls cat echo pwd head tail grep find wc which diff stat du cd` + git 只读形式）→ 放行；
- **危险签名**（`rm -rf`、`sudo`、`>` 重定向、`curl/wget`、`base64 -d`、shell 变量套娃、`eval`、整体无法解析）→ 强制升级；
- 挂进 `ExecutionGuard::prepare` 只读早退之后（`exec_guard.cpp:89` 旁），**解析失败一律走升级而非放行**。
作用不是预测命令干什么，而是只放行"词面可证只读"的一档；**UNKNOWN 与"看起来能跑"一律过审**。

### L3 审批/HITL —— approve_tools 接 M10 interrupt（doc 18 明示的后续迭代）
M10 的 `interrupt`/resume/逐节点快照/`Packet.is_restart` 已齐备，缺的是装配：
```
c_approve_tools (node_types.cpp:1006)：
  policy 判 need_human → interrupt({tool:"bash", command, options:[approve,reject,edit]})
  悬挂 → 快照落盘 → 外部从 state 的 tool_call 读命令
  恢复 Command(resume={action, feedback, edit}) → 条件边：
      approve → run_tools（edit 时改写参数）
      reject  → 反馈注入回 agent（对标 UserRejectObservation，模型可见可自纠）
```
会话级持久审批：只存指涉确定的 pattern key（命令前缀），复合命令逐子命令存、上限 5 条
（Claude Code 的"范围不变量"，防批准 `git *` 被 `-c` 逃逸）。

### L4 沙箱 —— 外部 executor 抽象（真正兜底）
借鉴 Gemini `SandboxManager`：接口 `prepareCommand(req)→{argv,env,cwd,cleanup}`，provider 可插拔：
- **bwrap provider**（Linux）：`--unshare-all --ro-bind / / --ro-bind-try <workspace> --tmpfs /tmp --die-with-parent` + seccomp；
- **容器 provider**（docker/podman，workspace 同路径挂载）；
- **受限子进程 provider**（fork + `setrlimit` + seccomp，**必须显式标注"非真沙箱"**，仅兜底）；
- **无 provider 时 fail-closed 报错**，绝不静默无沙箱执行（Codex 同纪律）。
默认禁网、workspace 可写、其余只读、/tmp 独立。

### L5 限额 —— 补齐 ExecutionGuard 唯一缺口
`timeout_ms` 在执行点落实：`waitpid` + 进程组 `SIGKILL`；stdout 流式分片回传并带截断标记
（对标 OpenAI `truncated_after_lines`，防上下文爆掉）。LangGraph 恰好漏了这层，是反超点。

### L6 审计 —— 已有事实源上加"命令现场"
会话日志已是 append-only 事实源（`session_log.hpp:52`）。纪律照 OpenHands：**先落 Action 事件、再执行、再落 Observation**；
bash 记录打进 `{command, cwd, exit_code, stdout(截断), decision, reviewer}`；审计 `.audit.jsonl` 记 decision。
模型只看到 ToolMessage 结果，transcript 保留原文——replay/时间旅行（M10 fork）免费获得。

### L7 回滚 —— 边界级丢弃，不做命令反做
坦率接受 Claude Code 的边界：bash 改动不可追踪、rewind 式撤销不适用。三条兜底：
1. **语义挤压**：仓库场景先 git 快照/worktree，回滚=checkout；
2. **物理丢弃**：L4 沙箱/容器销毁即回滚；
3. **probe 意向校验（cppchain 独有）**：bash 可挂 probe 检查落盘后置条件（如 file_write 的 `probe_file`），
   配合 ExecutionGuard 的 completed 重放做意向校验。

## 6. 落地分期（wfr-㊸，延续 problems/NN 验收格式）

| 批 | 内容 | 交付 | 验收 |
|---|---|---|---|
| ① ✅ | ToolSpec 契约扩展（approval/timeout_ms/executor）+ 壳感知分类器（lexer+白/黑名单，纯函数） | `tool/` + `tests/tool/*` | 首条 bash 命令过 guard；全绿数字 |
| ② ✅ | approve_tools 接 interrupt：need_human 悬挂 + resume 放行/拒绝，真机人工审批闭环 | `node_types.cpp` + `examples/` | 「真机批/拒」闭环，审计断言 |
| ③ ✅ | SandboxManager 抽象（bwrap provider + fail-closed 无 provider 拒绝）+ timeout/输出截断 | `src/tool/` + `tests/` | 无 provider 时拒绝执行用例 |
| ④ ✅ | 会话级 allow 前缀持久化（范围不变量）+ git 快照回滚实践 + problems/NN 验收记录定稿 | `share/wfr/workflows/*.json` + `docs/04-problems/22` | 652→全绿，真机 A/B/C 场景 |

## 7. 衔接

- 讨论见结论后按规则分流：本稿落 `01-discuss/` 建档；开工时 → `02-arch-design/19` 定架构、
  `03-detail-design/08` 定细节 → `examples/` 出 demo → `docs/04-problems/22` 验收。
- 与既有线的接口：wfr-㊷（problems/21）的 approve_tools 是**静态策略**，本设计把它升级为
  "静态策略 + interrupt 人审"两态；M10 时间旅行 fork 提供回滚/复盘的底座。
- 与（已知边界）：bash 副作用不可追踪（Claude Code checkpointing 明示）；静态分析不保证覆盖
  novel threats（OpenHands defense-in-depth 自述）；授权持久化必须遵守"范围不变量"。

## 8. 研究修正（我方 prompt 中三处假设与官方不符，均已核实纠正）

- `timeout/return_stderr/args_only` **不是** LangGraph 官方 bash 工具构造参数（逐版本核对 wheel 不存在；
  `ask_human_input` 是进程内阻塞 `input()`，不落 checkpoint）；官方唯一警告是 "The shell tool has no safeguards by default"。
- Gemini 的 `--dangerously-bypass-approvals-and-sandbox` / “proper sandboxes” 在公开仓库各 tag 中**不存在**，
  等效是 `--yolo` / `--approval-mode` 四档（plan/default/auto_edit/yolo）。
- OpenHands 三档实际是 `NeverConfirm / ConfirmRisky / AlwaysConfirm`（非 unconfined/runner/controlled）；
  `goal_exec_box` 是 SWE-bench 评测术语，非 OpenHands 结构。

## 9. 批① 落地记录（2026-10-11）

批次 ① 已实现并全绿（615→**633**，全量构建零告警），落下三件新组件 + 两处接入：

- **新增头/实现**：`tool/command_classify.{hpp,cpp}`（词面分类器，闭合四格 ReadOnly/Write/Risky/Unknown；
  壳感知 lexer：引号/转义/注释、`&& || ; | &` 拆分、wrapper 剥离、`sh -c` 脚本级递归、重定向/`$()`探测）、
  `tool/executor.{hpp}` + `src/tool/spawn_executor.cpp`（`ToolExecutor` 接口 + `PlainSpawnExecutor`：
  fork/exec `/bin/sh -c` + 进程组超时强杀 + 输出封顶，**定位为受限子进程 provider，非沙箱**）、
  `tool/bash_tool.{hpp,cpp}`（`make_bash_tool`：`read_only=false / category=action /
  `ToolApprovalLevel::NeedHuman / timeout_ms=120s / command_classify 绑定`，无执行器时 handler fail-closed 拒跑）。
- **Field 命名与 §5 略有出入（已定夺）**：审批级别枚举命名为 `ToolApprovalLevel`，
  **避开与 `agent::ToolApproval`（审批中间件类）撞名**；L2 注入点为 `ToolSpec::command_classify`（参数级回调）；
  静态门禁落在 `ExecutionGuard::prepare` 的**"未开始"分支** —— `ReadOnly → command_readonly 放行`、
  其余（Write/Risky/Unknown）`command_write/risky/unknown` fail-closed 阻断（批② 起对该类 interrupt 悬挂）；
  已完成（日志重放）与 Intent 残留（probe）分支**不**经过词面门禁（重放安全、崩溃兜底不变）。
- 测试 +18：`command_classify_test.cpp`（只读/写/危险/未知/复合/嵌套/wrapper/why 全文矩阵）、
  `bash_tool_test.cpp`（契约字段、验收"首条 bash 命令过 guard"、写/危险/未知/缺参阻断、
  无执行器拒跑、已完成重放不经过门禁、PlainSpawn 输出/退出码/超时强杀）。
- 已知边界（留给后续批）：`exec env VAR=1 cmd` 判 Write 属保守过拦；glob 敏感命令集为固定表；
  超时/输出限额的对模型回传（L5）随批③ SandboxManager 一起落实。

## 10. 批② 落地记录（2026-10-11）

批次 ② 已实现并全绿（633→**639**，全量构建零告警），核心 = **把批① 的静态"阻断"升级为
interrupt "悬挂"**，NeedHuman 工具（bash）的副作用进入「悬挂 → 人审 → 放行/拒绝」闭环：

- **`c_approve_tools` 接 M10 interrupt**（`node_types.cpp`）：逐 call 判定 —— 未知工具/deny 名单
  静态拒绝（先于人审，硬名单 > 人审）、read_only 免审（`approve_tool_call` 内）、
  `ToolApprovalLevel::NeedHuman` → 攒入 pending → 整节点 `interrupt()`（payload 形如
  `{"kind":"approve_tools_request","node","calls":[{id,tool,args}]}` 供宿主渲染）；
  恢复时节点从头重跑（**纯 state 函数，重跑一致**）、`interrupt()` 返回裁决 ——
  `action=="approve"` → 放行进 `approved_ids`；其余（缺失/坏值）→ 拒绝 + feedback 进
  error ToolMessage 回喂（fail-closed）。**图拓扑不变**：拒绝复用 wfr-㊷ 的
  `c_run_tools` 过滤（error ToolMessage → 跳过不执行 → 模型下一轮可见）。
- **`FireOptions.resume` 通道**（`compiler.hpp/cpp`）：`fire_once` 透传 `InvokeOptions.resume`；
  配合 `thread_id` + 编译期 checkpointer，第二次 fire 从悬挂快照恢复续跑。
- **wfr 真机闭环**（`src/wfr/main.cpp`）：注册 bash（`PlainSpawnExecutor` 执行后端，见 §9）
  + 新工具集 `chat_bash`；编译注入进程内 `MemoryCheckpointer`；触发改"fire 循环"——
  悬挂时打印待批请求、读一行标准输入（`approve | reject <理由> | abort`），再 resume 续跑。
  不悬挂的 workflow 只走一次 fire（既有行为不变）；stdin EOF / abort → 止于审批点。
  新增真机演示 workflow：`share/wfr/workflows/approve_bash_demo.json`（`chat_bash`）。
- **ToolNode 注入执行器修正**（`src/tool/tool_node.cpp`）：执行器从 `ToolSpec::executor` 注入
  `ctx.executor`（此前只在 `ExecutionGuard::run` 注入）—— 命令工具在无 guard 的直跑路径
  也能执行（demo 不加 guard 即验证）。
- **测试 +6**（`wf/approve_tools_interrupt_test.cpp`，全带 MemoryCheckpointer）：
  悬挂→approve 真执行（含 guard 审计"allow"）、拒绝+feedback 回喂不执行（审计为空）、
  坏裁决 fail-closed 拒绝、只读不悬挂、deny 名单硬拦不悬挂、混合（静态放行+悬挂同批，
  恢复后都放行）。既有 approve_tools 8 例回归不变。
- **示例**：`examples/approve_interrupt/main.cpp`（workflow 级 HITL 闭环，真实 bash 子进程：
  批准 → `exit_code: 0` + 输出；拒绝 → 不执行 + denied 记录）。
- 已知边界：人为裁决目前是**批决策**（action 作用于全部 pending 调用），逐调用/编辑参数
  （`update_state` 改写）是后续项；悬挂态在 wfr 里是进程内的（跨进程续聊仍走会话日志
  replay，批④ 会话级 allow 前缀持久化时一并定夺）。

## 11. 批③ 落地记录（2026-10-12）

批次 ③ 已实现并全绿（639→**647**，全量构建零告警），核心 = **L4 SandboxManager + L5 限额落到执行器**：

- **`tool/sandbox.{hpp,cpp}`**：`SandboxManager`（provider 选择器）—— 按配置顺序解析第一个**可用**
  执行器，全不可用 → `available()==false`（fail-closed，调用方必须拒绝，绝不静默裸奔）。
  - `bwrap` provider：`--unshare-all`（禁网 + 新 pid/ipc/uts）+ `--die-with-parent` + 根只读 +
    workspace `--bind` 可写 + `/tmp` tmpfs + `--clearenv` + `--chdir workspace`。**可用性探针**：
    二进制存在 **且能实际建 namespace**（`bwrap_usable()` 跑一次真命令）——本机是嵌套受限环境、
    user namespace 被禁（`setting up uid map: Permission denied`）→ 探针判不可用 → 自动隐退，
    **绝不让带沙箱标志的命令裸跑失败还装作成功**。
  - `container` provider：预留（需 container_image + docker/podman，未实现前跳过）。
  - `subprocess` provider：新工厂 `make_restricted_spawn_executor(cwd, max_output)` —— chdir 工作区 +
    setrlimit（FSIZE 256MB / NOFILE 1024）+ 超时强杀 + 输出封顶；`describe()` **显式标注
    「非真沙箱」**（不冒充），是任何宿主机都有的兜底。
- **spawn 内核收敛**：抽出 `tool/spawn_runner.{hpp,cpp}`（fork/exec 任意 argv + 进程组超时 SIGKILL +
  输出封顶截断），`PlainSpawnExecutor` 与 `BwrapExecutor` 共用 —— L5 的 timeout/截断对模型回传
  在两条 provider 上同构（bash handler 把 `timed_out`/截断 note 原样回报）。
- **wfr 装配**：bash 执行后端 = `SandboxManager`（本地无可用沙箱 → 显式打印「受限子进程（非真沙箱）」）；
  解析不到任何 provider 时 bash 以**空执行器**注册，handler 侧 fail-closed 拒跑。
  本机判定为「受限子进程（非真沙箱）」 —— 这正是该 provider 的预期携带场景。
- **测试 +8**（`tool/sandbox_test.cpp`）：只给不可用 provider → fail-closed；未知名跳过；
  auto 任何宿主机都非空；**强制 bwrap 但建不了 namespace → 拒绝不回退**（主机相关路径 GTEST_SKIP）；
  `bwrap_argv` 纯函数断言隔离声明（unshare/ro-bind/tmpfs/clearenv/bind ws/chdir ws）；subprocess
  运行于工作区 cwd 且诚实标注非沙箱（写入落宿主可证）；超时强杀 + 输出截断回传（L5 验收）。
- 真机（`approve_bash_demo`）：`bash 执行后端: 受限子进程（**非真沙箱**…）` → 模型调
  `bash echo hello-sandbox` → 审批悬挂 → 人工 approve → 执行 `exit_code: 0` + 输出 + 审计 1 行。
- 已知边界：bwrap 在本机不可用，属环境限制（bwrap 有能力的状态机已按探针走通）；
  container provider 未实现（预留）；seccomp 未纳入 subprocess provider（setrlimit 子集 + 诚实标注）。

## 12. 批④ 落地记录（2026-10-12）

批次 ④ 已实现并全绿（647→**652**，全量构建零告警），核心 = **会话级 allow 前缀持久化 + 回滚实践**：

- **allowlist 落盘辅助**（`workflow/wfr.hpp`）：`.wfr/sessions/<session>.allowlist.jsonl`，每行
  `{"prefix","provider"}`；`wfr_append_allow_prefix` 追加、`wfr_load_allow_prefixes` 读回（坏行
  fail-open 跳过）。**范围不变量三约束**：① 会话作用域（不跨会话/工作区）；② 前缀匹配
  （`starts_with`，允许 P 即允许 `P; …`，OpenAI exec-policy prefix 同款斜面）；③ 沙箱身份绑定
  （provider 不一致 → 前缀失效，沙箱降级后强制重新人审）。
- **approve_tools 自动放行分支**：`@config.approved_prefixes`（数组）+ `@config.sandbox_provider`
  （当前沙箱身份）从 state 读；NeedHuman 命令「前缀命中且沙箱身份一致」→ 直接进 approved_ids
  不悬挂；缺配置 → 行为与批② 完全一致（全部人审）。节点依旧 state 纯函数（config 从 state 来）。
- **wfr 装配**：开关 = `--set allow_prefixes=on`（缺省关 = 既有行为不变）；开 → 启动加载 allowlist
  进 config、批准时把命令原文追加进 allowlist（只记 `tool=="bash"` 且带 command 的真实执行的命令）；
  沙箱身份无论开关都注入 config。
- **真机 A/B/C**（problems/22 §二）：A 首次批准 → 执行 + 落盘记录；B 同会话续跑同前缀 → **无审批
  提示**直接完成（自动放行 × 批② 重放，`执行 0 · 重放 1`）；C 前缀不命中的新命令 → 悬挂 → reject
  → 不执行 + 反馈回喂。
- **git 快照回滚实践**（L7 仓库场景）：批准后执行前打快照（`git stash create`），跑完不对 → checkout
  还原——边界级丢弃、不做命令反做；沙箱/容器场景 = 销毁即还原。
- 测试 +5（`wf/approve_tools_persist_test.cpp`，全带 MemoryCheckpointer）：同前缀+同沙箱自动放行
  （副作用计数器==1）、不以前缀开头仍悬挂、provider 不一致前缀失效、无配置等同批②、allowlist
  往返+坏行 fail-open。验收 docs/04-problems/22。