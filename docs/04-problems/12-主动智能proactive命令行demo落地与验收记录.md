# 12. proactive 命令行 demo —— ask_user 菜单澄清 + 批量测试

> 落地：2026-09-27 · 前置：`docs/04-problems/11`（主动智能 Workflow 模块）

## 1. 动机

M11 打通生成端后缺一个"主动智能"专用的命令行演示：用户在**生成途中**遇到澄清点，由
demo 以 **ask_user 机制**打编号菜单（`<问题>` + `1. 选项1 / 2. 选项2 …`）等输入，选完继续，
最终输出易读 IR。并提供非交互批量模式把 12 例评测固件并进来自动跑。

## 2. 文件

| 文件 | 职责 |
|---|---|
| `include/cppchain/workflow/generator.hpp` | `ClarifyQuestion` 增加可选 `options`（小改） |
| `src/workflow/generator.cpp` | 澄清 JSON round-trip 支持 options；提示词引导 LLM 给 2–4 个选项 |
| `examples/proactive/ask.hpp` | **ask_user 通用提问器**：交互读 stdin（编号菜单）/ 批量查 `--question-answer` 规则 |
| `examples/proactive/main.cpp` | 交互 REPL / `--intent` 单发 / `--cases` 批量矩阵；易读 IR 打印 |
| `tests/wf/generator_test.cpp` | options round-trip 与新答案续跑用例 |

## 3. 关键决策

- **ask_user = 生成期澄清**（区别于"执行期 ask_user 工具"）：不向工具目录注册新工具，
  澄清问答菜单化；选项是 LLM 在 `questions.options` 里给的，向后兼容（缺 options = 自由文本）。
- **answer 数字化**：交互选数字或直接输文本；批量按 `--question-answer <问题正则> <序号|文本>`
  匹配问题文本，数字 → `options[n-1]`（夹 1..n），非数字当文本答案；**未命中 → 默认选项 1**，
  流程不中断（无人值守友好）。
- **`--cases` 语义**：expect=Clarify 的首轮澄清按 `must_ask` 关键词即判（不再追问）；
  golden/boundary 的 Clarify 走规则自动续跑，Generated 判特征 + 编译（不 fire，保持生成端聚焦）。
- **易读 IR**：库内无现成打印器（NodeIR 字段全 public），demo 自写逐节点链式输出 +
  `to_json().dump(2)` 规范形兜底。

## 4. 验收

**离线**：`generator_test` 新增 `clarify_options_roundtrip`（带 options 读回、按选项文本作答续跑、
缺 options 兼容空向量），全套 `workflow_tests` 通过（41 例），40 例无回归。

**真机（`./run-demo.sh -n proactive`）**：
- 交互 REPL / `--intent`：澄清以 ask_user 编号菜单等待输入（实测"每天提醒我喝水"类意图
  触发双问菜单：`每天几点提醒你喝水？1. 早8/午12/晚6 …`，选数字或直接输文本均可，
  Ctrl-D 中断返回"已取消"），选完输出易读 IR（trigger schedule + 逐节点链 + JSON 规范形）；
- 批量 `--intent ... --question-answer "几点" 1`：澄清按规则自动作答、stdin 不阻塞；
- `--cases`（每例独立 gen、独立调用预算，usage 跨例累计）：12 例矩阵实时可见
  `[规则命中 → …]`/`[未命中规则 → 默认 …]`，Generated 判特征 + 编译，期望一致时退出码 0；
- `run-all` 冒烟分支用单条稳定意图（`--intent "每天早上 8 点提醒我喝水"`）不占终端。

**快捷脚本 `./run-proactive.sh`**（对齐 run-demo.sh 的 env/构建约定）：无参自动
`build + --cases`（内置默认澄清规则）；`--interactive` 进 REPL；其余参数原样透传给
二进制（`--intent / --question-answer / --help`）；退出码与二进制一致（--cases 有过 → 1）。

## 5. 边界与后续留意

- 菜单选项质量取决于 LLM 是否给 `options`：不给则退化为自由文本输入（batch 用规则文本作答）。
- 批量未命中规则的默认值（"默认"/选项 1）是尽力而为：对依赖具体时间的意图，
  建议显式给 `--question-answer` 规则。
- `--cases` 只做生成 + 编译校验，不做执行期触发（执行端验收仍在 `wf_eval` 的调度模拟段）。