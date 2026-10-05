# 主动智能 · Workflow IR 规范

本目录是**主动智能 Workflow 模块的规范来源**（不是实现文档）。`docs/04-problems/11`、
`12` 是 cppchain 侧的 C++ 落地与验收记录，实现本体在 `include/cppchain/workflow/` 与
`src/workflow/`。

| 文档 | 内容 |
|---|---|
| [10-eval-cases.md](10-eval-cases.md) | 评测用例集（golden / boundary），固件落在 `examples/support/features.hpp` |
| [11-ir-syntax.md](11-ir-syntax.md) | **IR 语法规范**：节点类型、引用（`$_.result.<f>` / `$<id>.result.<f>` / `@config.<k>`）、表达式文法、触发 |
| [12-generation-process.md](12-generation-process.md) | 生成流程：PlanStage（三态）→ IRStage（出 IR + 校验修复） |

> 改名/改语法时注意：`11-ir-syntax.md` 里的引用写法必须与 `src/workflow/refs.cpp` 的
> 实际解析保持一致 —— 例如"最近一步"是 `$_.result.<f>`（`$_` 是占位符，不是节点 id），
> 写 `$result.<f>` 会被解析成"点名节点 result"。
