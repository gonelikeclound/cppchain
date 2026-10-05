# 08 · 命令行 Agent 四策略对比验证（`examples/agent_cli` 验收记录）

> 本文是 M7 的**验收记录**：为 `01-discuss/04-架构模式清单.md` 的架构模式实验做一个「同一可执行体、参数切策略」的
> CLI 对比载体，并在验收过程中暴露并修复了两个 usage 会计层的真问题（流式缺回传、压缩后切片漏计）。
> 是对 [[04-上下文压缩]]、[[03-工具副作用]]、[[02-超长工具返回]] 各自 demo 的**横向综合验证场所**。

## 1. 问题场景与产生原因

已有的 24 个 `examples/*` demo 都是**场景写死的 `int main()`**：

- 每个 demo 只说一件事（压缩 / 审批 / 截断……），想对比「同一输入在不同 Agent 策略下的效果」得改代码或手动拼不同的 demo；
- 全项目**没有 argv 解析、没有 stdin/REPL 先例**（grep 无 `argc/argv`、唯一 `getline` 在读 `.env`）；
- 交互式连续对话（多轮记忆）与脚本化批量测试（文件驱动）都缺一个统一入口。

**需求**：一个命令行的 single-agent/react-agent —— 一个二进制，`--mode` 切换策略对比效果；
除交互外还能从文件读输入做批量验证。

## 2. 常见解决思路

| 方案 | 原理 | 优点 | 缺点 |
|------|------|------|------|
| 多二进制各管一景 | 每个策略一个可执行 | 简单、无参数解析 | 无法同输入对比；散落难维护 |
| 单二进制 + 手工参数 | `--mode` / 子命令 | 一份代码多策略，天然对比 | 需要自研解析（项目零依赖） |
| REPL / 文件驱动 | stdin 交互 / 文件批量 | 交互可多轮记忆、文件可脚本对比 | 需处理 tty 回显、输出纯净性 |

**横向对比的维度**：策略开关（工具 on/off / 中间件 on/off）、输入源（交互 / 单条 / 批量）、
输出（人类可读轨迹 vs 结构化 JSON）。

## 3. langchain / langgraph 的对应做法

- 策略本身 = `create_agent(model, tools, middleware=[...])` —— 中间件组合出不同行为；
- langchain 生态**没有官方 CLI 对比工具**，通常靠 notebook / 脚本轮替参数完成；本项目要的是
  「一份代码、参数驱动、同一输入、结构化可 diff」的载体，因此借鉴 `argparse` 思路但零依赖手写。

## 4. 本项目选型与理由

`examples/agent_cli`（单文件，`main.cpp`）：

- **4 档模式**（全复用现有 `Agent` + 中间件）：
  - `direct` —— 空工具注册表，单轮直答（对照基线）；
  - `react` —— `get_datetime` + `calculator`（离线），.env 有 key 时加 `web_search`；
  - `react-trim` —— react + `TrimHistory(6)`；`--summarize` 换 LLM 摘要器；
  - `react-approve` —— react + 假副作用工具 `send_email`/`delete_file` + 审批；
    `--allow-prefix <前缀>` 从「名单拒绝」切到「按参数放行」。
- **3 输入源**（互斥）：`-i/--interactive` REPL（同 thread + `MemoryCheckpointer` 多轮记忆）、
  `-p/--prompt` 单条、`-f/--file` 文件批量（整行 `---` 分块、`#` 注释、块内多行）。
- **输出**：可读轨迹 + stats 行（iterations / tool_turns / errors / denials / compressions /
  usage tokens / ms）；`--json` 时 stdout **只**输出一个 JSON 记录数组，供 `jq`/`diff` 脚本对比。
- **两个纪律**：
  - *防悬垂*：`Agent`/`ToolNode` 只存非拥有指针 → 空注册表必须具名（`Setup::registry`），
    先 Setup → 再 Agent → 再跑；
  - *输入源*：REPL 用持久 Agent（计数器跨轮累计），`-p/-f` 每块新建 Agent（计数隔离）。

## 5. Demo 与验证结论（真机，DeepSeek，2026-09-25）

### 5.1 同文件三模式对比（`inputs/sample.txt`，3 块：计算 / 时间 / 新闻）

同一输入、换 `--mode`，`--json` 落盘后 `jq` 抽 stats：

| 块 | 指标 | direct | react | react-trim |
|----|------|--------|-------|------------|
| #1 计算 | iters / tool_turns | 1 / 0 | 2 / 1 | 2 / 1 |
| | usage total | 117 | 1608 | 1637 |
| #2 时间 | iters / tool_turns | 1 / 0 | 2 / 1 | 2 / 1 |
| | usage total | 80 | 1666 | 1706 |
| #3 检索 | iters / tool_turns | 1 / 0 | 2 / 2 | 2 / 2 |
| | usage total | 165 | 3678 | 3720 |

- **direct 的效果最典型**：模型「诚实拒答」而非硬答（"没有可调用的 calculator 工具，不能实际
  执行计算"）—— 无工具基线拿不到外部信息的机制差异暴露得干净（`tool_turns=0`）。
- **工具成本可见**：react 的 usage 是 direct 的 10~26×（工具 schema + 结果全文进 prompt）；
  输入 token 跨运行**逐位可复现**（确定性部分），输出 token 随答案长度浮动（模型方差）。
- **react-trim ≈ react（文件模式）**：每块独立 Agent 无共享历史，压缩本就不该触发
  → 恰好验证了「压缩只在长会话有意义」。`comp=0` 是预期而非失效。

### 5.2 审批两档：同一工具、纯参数驱动两种结局

名单拒绝档（`-m react-approve`）：

```
[ai] 调用工具 send_email({"body":"我辞职了","to":"boss@example.com"})
[tool:error] rejected: 工具 'send_email' 被策略禁止（副作用审批）
→ stats: tool_turns=1 tool_errors=1 denials=1      ← denials 跨轮累计 0→1
```

按参数放行档（`-m react-approve --allow-prefix notes/`，`ToolApproval(Decider)` 回调）：

```
删 notes/plan.txt → 调用 delete_file({"path":"notes/plan.txt"})
  → [tool:success] （假）已删除 notes/plan.txt          ← 前缀内 → 放行执行
删 /etc/passwd    → 调用 delete_file({"path":"/etc/passwd"})
  → [tool:error] rejected: path '/etc/passwd' 不在允许前缀 'notes/' 内      ← 前缀外 → 拒绝回喂
```

- 参照 [[03-工具副作用]] 与 `tools_approval` 的"只许 notes/"回调白名单，在 CLI 落地为参数档；
- system prompt 在放行档向模型声明规则（模型才知道合法路径），拒绝后模型收敛不重试。

### 5.3 压缩两档：朴素占位 vs `--summarize` LLM 摘要器（REPL 6 轮）

| | 朴素占位 `TrimHistory(6)` | `--summarize`（LLM 摘要器） |
|---|---|---|
| 名字「小明」（两次折叠后） | ❌ 丢失 | ✅ 保留 |
| 颜色「蓝色」 | ❌ 丢失 | ⚠️ 第一轮保住、第二轮**摘要套摘要漂移**掉 |
| token 封顶 | ✅ ~750–800 | ✅ ~753–815 |
| 每次压缩额外成本 | 0 | +1 次模型调用（不在 Message.usage 内） |

- 朴素版把被折叠旧史替换成占位说明，事实直接蒸发；
- LLM 摘要器保住了多数记忆，但**摘要嵌套会信息漂移**（旧摘要再压缩时 60 字预算筛掉次要事实）——
  这是摘要压缩的固有取舍，区别于 [[04-上下文压缩]] 的最小实现，如实保留为演示点。

### 5.4 过程中冒出的两个真问题（本记录的**看点**）

#### A. 流式请求缺 usage —— `stream_options.include_usage`

- **症状**：压缩后的 REPL 轮次 stats `usage=0/0/0`。
- **原因**：OpenAI 规范中**流式默认不回传 usage**，须显式 `stream_options: {include_usage: true}`；
  原 adapter 未请求，端点是否回 usage 帧全凭缘分。
- **修复**：`include/cppchain/model/openai/wire.hpp:77` 流式分支统一补
  `body["stream_options"] = {{"include_usage", true}}`（DeepSeek/vLLM/Ollama 兼容）；
  单测扩展断言流式带、非流式不带。全量 252 测试过。
- **结论**：usage 会计的前提是"端点真的回 usage"——上游一次性保证，下游才谈得上可靠统计。

#### B. 压缩后轮次 stats 漏计 —— 切片锚点 bug（工具侧）

- **症状（A 修复后仍在）**：`react-trim` REPL 自压缩发生那轮**之后**，usage/工具计数恒 0。
- **诊断**：A/B 对照 —— 纯 `react` 同会话 6 轮**全有** usage；加压缩则第 5+ 轮全 0。
  逐帧推演：`TrimHistory` 折叠会**重建消息列表**（旧史换摘要，净长度不变甚至变短），而
  agent_cli 的「本轮切片」基准是 *invoke 前后 transcript 长度差* → 差为 0 → **切成空片**。
  答案来自 invoke 返回值（不依赖切片）所以此前未暴露。
- **修复**：切片起点改为锚定 invoke 后转录里**最后一条 Human**（本轮输入；模型与工具都不产生
  Human），中间件怎么折叠都影响不了定位。
- **复验**：压缩后轮次 usage `0/0/0` → `784/4/788`，token 封顶曲线保持。

> **验证陷阱复盘**（呼应 [[03-工具副作用]] 的"验证陷阱"传统）：`--json`+REPL 首次测试"失败"，
> 污染源是 `script` 转发 pty 时**带上内核回显**（用 `cat` 对照证实连 `stty -echo` 都压不住）。
> 换成「pty 交互 + stdout 重定向到文件」的真实场景等价法后，文件纯 JSON、jq 可解析（记录数与
> 答案全对上）—— **程序无此问题，是测试工具特性**。

## 6. 运行注意 / 未解与下一步

- `run-demo.sh` run-all 会对 `agent_cli` 传 `-m react -f inputs/sample.txt --json` 冒烟
  （缺省输入源是 REPL，会占终端；另有 `isatty(0)` 双保险）。
- `--json` 是给脚本对比用的（`-p/-f` 命中其价值）；交互 REPL 默认给人看，两者别混。
- **未解 / 可选深化**：
  - LLM 摘要器的级联漂移 → 可给摘要加"事实清单"约束或按重要性分层保留；
  - `--allow-prefix` 目前只覆盖 `delete_file` 路径规则，未做 send_email 收件人域白名单；
  - 若未来某自建端点不认 `stream_options`，需在上游加"空则不发"的开关；
  - `docs/04-problems/05` 的 SubAgent / ForkAgent（M8）仍是 🔴。

## 复现命令

```bash
./run-demo.sh agent_cli -m direct  -f examples/agent_cli/inputs/sample.txt --json > /tmp/direct.json
./run-demo.sh agent_cli -m react   -f examples/agent_cli/inputs/sample.txt --json > /tmp/react.json
./run-demo.sh agent_cli -m react-trim --summarize -i            # 压缩两档之一
./run-demo.sh agent_cli -m react-approve --allow-prefix notes/ -i
./run-demo.sh agent_cli --list-modes                            # 全部 6 档说明
```