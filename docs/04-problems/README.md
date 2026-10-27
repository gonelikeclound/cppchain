# 04 · 问题分析（problems）

本项目文档的**核心主线**：记录大模型与 Agent 开发中的常见问题，对每类问题进行分析，并**为各种解法提供可运行的 demo 进行验证**。

每个问题一篇文档，统一采用如下结构：

1. **问题场景与产生原因** —— 什么场景下出现、为什么会这样
2. **常见解决思路（横向对比）** —— 多种解法的原理、优缺点
3. **langchain / langgraph 中的对应做法** —— 作为参考基准
4. **本项目选型与理由** —— 在 cppchain 中选哪种、为什么
5. **Demo 设计与验证结论** —— 可运行 demo、数据、结论

## 已知问题清单（持续增补，编号按最终顺序排列）

| 编号 | 问题 | 状态 |
|------|------|------|
| [01](01-大批量工具查找.md) | 大批量工具查找（Tool Retrieval） | 🟢 解法①②已验证（关键词+IDF / 向量对照 2/8→8/8） |
| [02](02-超长工具返回.md) | 超长工具返回（Long Tool Response） | 🟢 数据+最小实现（M9 artifact 落盘；2026-10-27 追加全景：七法三轴 + longdoc 联动 + 评测/落地计划；**S1 分页 / S2 file_grep / S3 结果句柄化 / S4 结构提取 / S5 采样 / S6 运行时路由 + 统一评测台已落地**） |
| [03](03-工具副作用.md) | 工具副作用（Tool Side Effects） | 🟢 最小实现（M9 补审批记忆化） |
| [04](04-上下文压缩.md) | 上下文压缩 | 🟢 数据+最小实现（M9 补 token 阈值触发） |
| [05](05-五大问题现状盘点.md) | 五大问题现状盘点（M7 收口） | ✅ 盘点完成 |
| [06](06-状态时效性.md) | 状态时效性（必须实时获取，不能凭历史推断） | 🟢 数据+最小实现 |
| [07](07-意图路由与联网取数兜底.md) | 意图路由与联网取数兜底 | 🟢 真机双路径验证 |
| [08](08-命令行Agent四策略对比验证.md) | 流式 usage 缺回传 + 压缩后切片漏计（agent_cli 验收暴露） | 🟢 已修复+复验 |
| [09](09-子代理与副作用隔离.md) | 子代理与副作用隔离（M8） | 🟢 真机三形态验证 + fork 语义对照 |
| [10](10-五大问题收官对照.md) | 五大问题收官对照（M9） | 🟢 收官件全部实现 + 观测数据 |
| [11](11-主动智能Workflow模块落地与验收记录.md) | 主动智能 Workflow 模块落地与验收 | 🟢 真机 12 例矩阵 |
| [12](12-主动智能proactive命令行demo落地与验收记录.md) | 主动智能 proactive 命令行 demo 落地与验收 | 🟢 真机矩阵 |
| [13](13-M10图运行时补全落地与验收记录.md) | M10 图运行时补全落地与验收（Streaming / Interrupt / Send / 时间旅行） | 🟢 全仓 422 全绿 + 4 离线 demo |
| [14](14-wfr-chat2思考历史对比落地与验收记录.md) | wfr-chat2 思考历史对比（历史三档压缩 + reasoning 回传 / 参数透传开关） | 🟢 495 全绿 + 真机三档对照 |
| [15](15-loop节点落地与label-goto迁移记录.md) | loop 节点落地与 label/goto 迁移（IR/validator/compiler/调试台/import/生成器 + 四个 workflow） | 🟢 517 全绿 + 真机 |
| [16](16-历史消息处理专题.md) | **历史消息处理专题**：摘要 / 精简 / 长内容裁剪 —— op 落日志、规则引擎、三个内置 workflow | 🟢 570 全绿 + 真机三方向对照 |
| [17](17-历史处理真机对照数据.md) | **历史处理真机对照数据（附录）**：A/B/C 三组实验的逐臂原始数据（stats / 摘要原文 / 思考 / 回答） | 🟢 脚本从会话日志抽取 |
| [18](18-历史处理组合对照数据.md) | **摘要 × 精简组合对照数据（附录二）**：顺序决定成败（先摘要后精简 vs 先精简后摘要） | 🟢 同历史五臂对照 |
| [19](19-历史处理补测与统计口径修复.md) | **历史处理补测（wfr-㊵ 收尾）**：artifact 落盘 3 离线测试 + 真机 12 条落盘；clip_ai_text 真机首触发；修 after_tokens 统计口径 bug；**wfr-㊶ 把「用文件工具读取」兑现**（只读文件对入 loop + 引用行改根内相对，真机 file_read total=10） | 🟢 585 全绿 + 真机 |
| [20](20-副作用全景分析.md) | **副作用全景分析（wfr 审批专题开篇）**：能力盘点 + 双通道差异（Agent 循环有纵深 / Workflow 图裸奔）+ 遗留开口 + 五专题血缘 | 🟢 全景落盘，待设计 |
| [21](21-副作用防护落地与验收记录.md) | **副作用防护落地与验收（wfr-㊷ 收官）**：ExecutionGuard/probe ②挂执行点③wfr 装配④approve_tools 节点；真机三场景（写/防重放/拒写）；615 全绿 | 🟢 全线收官 |
| [22](22-沙箱选择器与会话级allow前缀验收.md) | **wfr-㊸ ③④ 收官**：SandboxManager（bwrap provider / 受限子进程兜底 / fail-closed）+ 会话级 allow 前缀（范围不变量）+ git 快照回滚实践；真机 A/B/C；652 全绿 | 🟢 全线收官 |
| [28](28-长程任务专题验收.md) | **wfr-㊿ 长程任务汇总验收**：任务档案+批还原（批①）+ revision-fork（历史快照续跑，SKIP 转正）+ 批间 L3 复查（audit_prev）；678/678 | 🟢 批①② 落地 |
| [27](27-长程任务批1验收.md) | **wfr-㊿ 批①（长程）**：任务档案 `task` begin/end + 批还原 + lot 两批真机（cursor 6·batch 2·跨批 week.txt 累积）；676 全绿 | 🟢 批① 落地 |
| [26](26-B2批对照验收.md) | **B2 批对照（办公文件流/干扰注入）**：A2 好案例三级都真过（带重试）+ A1/A3 难案例 L1 盲 done vs L2/L3 重试耗竭 fail；V1 注入裁判抓"未产出"；B2-B 出行 DB 面待建 | 🟢 A/C 已对照 · B 待工具面 |
| [25](25-复杂任务批4中断与回退验收.md) | **wfr-㊹ 批④（收官）**：`plan_review` 步骤边界中断（continue/rollback_to:K/abort）+ 引擎 goto 容器名解析 + aborted 标记与审计；674 全绿 + 真机 | 🟢 收官 |
| [24](24-复杂任务三级验证对照验收.md) | **wfr-㊹ PEV 三级对照验收**：L1 盲跑保证 / L2 确定性产物门 / L3 语义门 —— 7 外部真实 case × 三级 × 2 次的实测差异 + 探针子串能力 | 🟢 已对照 |
| [23](23-提示注入健壮性.md) | **提示注入健壮性（wfr-㊸ ⑤）**：注入文本驱动模型 ≠ 防线被破——修复两个语义洞（人审批准被 guard 二次拦 / allow 前缀被串联升级→类别封顶）+ 对抗语料；658 全绿 | 🟢 已对抗 |

## 文件列表

| 编号 | 文档 | 说明 | 状态 |
|------|------|------|------|
| 01 | [01-大批量工具查找.md](01-大批量工具查找.md) | 工具检索：体积/质量实测、关键词+IDF、向量对照实验、评测局限 | 🟢 解法①②已验证 |
| 02 | [02-超长工具返回.md](02-超长工具返回.md) | 16KB ≈ 3.2k token/轮；截断（UTF-8 安全）+ artifact 落盘（M9）；**追加全景**：七法 × 三轴 + longdoc 两时相 + 评测方案与 S1~S6 落地顺序；**S1** 分页、**S2** `file_grep`、**S3** 结果自动句柄化、**S4** `file_extract`、**S5** `file_sample`、**S6** `file_context` 运行时路由；离线评测 8 任务（grep 6/6·56B、extract 5/5·581B、sample 3/8、paging 8/8·96.9KB、混合 8/8）；**真机**（DeepSeek-flash，7 题三臂，含 1 题路由盲区）：route 6/6·0 调用·2.2k token｜hybrid 7/7·1 调用·14.6k（≤1 轮工具 + 强制收口）｜base 6/7·16 调用·39.3k；两个真机发现（提取结果格式 / 工具诱惑→结构限轮）已修；34 新测试，全仓 1011 绿；设计/评测设计/最终结果汇总见 [arch 29](../02-arch-design/29-超长工具返回混合解法设计.md) | 🟢 六切片落地 + 真机验证 |
| 03 | [03-工具副作用.md](03-工具副作用.md) | 工具副作用：策略拒绝 + 回喂（M7）+ **审批记忆化**（M9）；幂等/审计未做；含"验证陷阱"复盘；现状速览见 §0 | 🟢 最小实现+M9/M10 补强 |
| 04 | [04-上下文压缩.md](04-上下文压缩.md) | 历史线性增长 199→113；TrimHistory + LLM 摘要 | 🟢 数据+最小实现 |
| 05 | [05-五大问题现状盘点.md](05-五大问题现状盘点.md) | **问题域现状总盘**（M7 收口） | ✅ 盘点完成 |
| 06 | [06-状态时效性.md](06-状态时效性.md) | 状态时效性：观测时间戳 + TTL 失效标注（真机 A/B 对照） | 🟢 数据+最小实现 |
| 07 | [07-意图路由与联网取数兜底.md](07-意图路由与联网取数兜底.md) | 意图路由 + 联网取数兜底保证：Router+Skill 单 Agent 连续聊天（真机双路径验证；2026-10-27 复跑修复端点兼容：json_object 小写 json / thinking 模式 tool_choice 降级，并加防空真断言）；**§8 P0 会话单飞锁**：同 session 并发 query 默认拒绝（`--wait` 排队）、死进程锁自动回收 | 🟢 数据+双路径验证 + P0 锁 |
| 08 | [08-命令行Agent四策略对比验证.md](08-命令行Agent四策略对比验证.md) | agent_cli 验收记录：4 模式 × 3 输入源 × --json 对比；修复流式 usage 缺回传（include_usage）与压缩后切片漏计；含验证陷阱复盘 | 🟢 真机四策略验证 |
| 09 | [09-子代理与副作用隔离.md](09-子代理与副作用隔离.md) | 子代理与副作用隔离（M8）：ask_expert / orchestrator-串行 / handoff 三形态真机验证；token 会计证明主上下文干净；fork 语义对照 | 🟢 真机三形态验证 |
| 10 | [10-五大问题收官对照.md](10-五大问题收官对照.md) | **集中收官（M9）**：混合召回 / artifact 落盘 / 审批记忆化 / token 阈值与成本量化；横向对照总表 | 🟢 收官件全部实现 + 观测数据 |
| 11 | [11-主动智能Workflow模块落地与验收记录.md](11-主动智能Workflow模块落地与验收记录.md) | **主动智能（M11）**：IR → 校验 → 编译成图 → 生成端；规范来源见 `主动智能/`；12 例固件矩阵 | 🟢 真机 12 例矩阵 |
| 12 | [12-主动智能proactive命令行demo落地与验收记录.md](12-主动智能proactive命令行demo落地与验收记录.md) | **proactive CLI**：`ask_user` 菜单澄清 → 易读 IR；交互/批量/矩阵三模式 | 🟢 真机矩阵 |
| 13 | [13-M10图运行时补全落地与验收记录.md](13-M10图运行时补全落地与验收记录.md) | **M10 图运行时补全**：Streaming / Interrupt（真 HITL 原语）/ Send 动态 fan-out / 时间旅行；checkpoint 包队列契约 | 🟢 全仓 422 全绿 + 4 离线 demo |
| 14 | [14-wfr-chat2思考历史对比落地与验收记录.md](14-wfr-chat2思考历史对比落地与验收记录.md) | **wfr-㉙**：思考模型的历史形态三档对照（full / drop_round / keep_evidence）+ `reasoning_content` 回传与厂商参数透传开关；含协议问句的探针答案 | 🟢 495 全绿 + 真机三档对照 |
| 15 | [15-loop节点落地与label-goto迁移记录.md](15-loop节点落地与label-goto迁移记录.md) | **wfr-㉚**：循环从 label+goto 换成 loop 容器；`on_max` 分离出口；调试台按迭代分组；含四个被测试逼出来的真 bug 复盘 | 🟢 517 全绿 + 真机 |
| 16 | [16-历史消息处理专题.md](16-历史消息处理专题.md) | **历史消息处理**：可持久化的历史摘要（op 落日志→重跑不重算）/ 规则化精简 / 长内容裁剪；3 个内置 workflow | 🟢 570 全绿 + 真机对照 |
| 17 | [17-历史处理真机对照数据.md](17-历史处理真机对照数据.md) | **真机对照数据附录**（wfr-㊵）：三组实验、13 个臂的完整数据留存，供后续研究 | 🟢 从日志抽取 |
| 18 | [18-历史处理组合对照数据.md](18-历史处理组合对照数据.md) | **组合对照数据附录二**（wfr-㊵）：摘要×精简的顺序之争 —— 先摘要后精简 2028 tok 且最好；先精简后摘要模型开篇免责 | 🟢 同历史五臂 |
| 19 | [19-历史处理补测与统计口径修复.md](19-历史处理补测与统计口径修复.md) | **补测 + 修复**（wfr-㊵ 收尾 + ㊶）：artifact 落盘 3 离线测试与真机 12 条落盘；clip_ai_text 真机首触发（85554→56876）；`after_tokens` 口径 bug 修复；**只读文件工具入 loop + 引用行改根内相对 → 真机 file_read 兑现（total=10）** | 🟢 585 全绿 + 真机 |
| 20 | [20-副作用全景分析.md](20-副作用全景分析.md) | **副作用全景**（wfr 审批专题开篇）：能力盘点 / 双通道差异（Agent 循环纵深 vs Workflow 图裸奔）/ 五处断层 / 与五专题血缘；§七列下一步 | 🟢 全景落盘 |
| 21 | [21-副作用防护落地与验收记录.md](21-副作用防护落地与验收记录.md) | **落地 + 验收**（wfr-㊷ 收官）：四批实现（guard/probe → 挂执行点 → wfr 装配 → approve_tools）+ 615 全绿 + 真机三场景（写 / 防重放 / 拒写）| 🟢 全线收官 |
| 22 | [22-沙箱选择器与会话级allow前缀验收.md](22-沙箱选择器与会话级allow前缀验收.md) | **验收**（wfr-㊸ ③④）：SandboxManager（bwrap/受限子进程/fail-closed）+ 会话级 allow 前缀（范围不变量）+ git 快照回滚实践；652 全绿 + 真机 A/B/C | 🟢 全线收官 |
| 23 | [23-提示注入健壮性.md](23-提示注入健壮性.md) | **验收**（wfr-㊸ ⑤）：验证「模型被骗 ≠ 防线被破」——修两个语义衔接洞（人审批准被 guard 二次拦 → pre_approved 豁免；allow 前缀被串联升级 → 类别封顶）+ 对抗语料；658 全绿 | 🟢 已对抗 |
| 24 | [24-复杂任务三级验证对照验收.md](24-复杂任务三级验证对照验收.md) | **验收**（wfr-㊹）：C1–C7 × {l1,l2,l3} × 2 = 42+ 轮真机对照——L1 盲跑 / L2 产物门 / L3 语义门三板差异规律；probe_file_contains 子串探针 | 🟢 669 全绿 + 真机 42+ 轮 |
| 25 | [25-复杂任务批4中断与回退验收.md](25-复杂任务批4中断与回退验收.md) | **验收**（wfr-㊹ ④）：`plan_review` 节点（interrupt 悬挂→continue/rollback_to K/abort）+ 引擎 goto 容器名解析 + 两级回退 + `pev_review` | 🟢 674 全绿 + 真机 |
| 26 | [26-B2批对照验收.md](26-B2批对照验收.md) | **验收**（B2 批）：办公三案三级对照 + 注入（误导/判据变更）+ 出行 DB 工具面 `pev_trip` + `office-docs` 外部 Rust 引擎 + 裁剪版 A1 三级闭环 | 🟢 674 全绿 + 真机 |
| 27 | [27-长程任务批1验收.md](27-长程任务批1验收.md) | **验收**（wfr-㊿ ①）：`task` begin/end 任务档案 + `lot` 批工作流 + 批还原 cursor/batch_no | 🟢 676 全绿 + 真机 |
| 28 | [28-长程任务专题验收.md](28-长程任务专题验收.md) | **验收**（wfr-㊿ 收官）：批① 档案 + 批② revision-fork 与批间 L3 复查（recheck_prev）+ 批③ 轻量 cron + 自进化导出 + C1 分批真实案例 | 🟢 678 全绿 + 真机 |
| 29 | [29-多Agent角色化mch验收.md](29-多Agent角色化mch验收.md) | **验收**（wfr-㊾ 批①）：`mch` 三角色 workflow（Planner/Executor/独立 Reviewer 批级终审）——verify 加 `review` 相位 + `role` 审计；同任务 pev_l3×2 vs mch×2 对照（产物落盘差异 + 角色身份串落 plan_log） | 🟢 681 全绿 + 真机 4 轮 |
| 30 | [30-Send并行波与冲突护栏验收.md](30-Send并行波与冲突护栏验收.md) | **验收**（wfr-㊾ 批②）：`max_parallel>1` 时同一批 Send 在 worker 线程并发执行（波=屏障，join 后按声明序归约与串行 state 逐键相等）；冲突护栏——同键并发写 overwrite 通道即报 `ConcurrentWriteConflict`（fail-closed）；波成员 interrupt 明确拒绝。12 单测 + demo m10_send_parallel | 🟢 693 全绿（离线） |
| 31 | [31-交接式A→B交接件契约与并行Executor真机对照验收.md](31-交接式A→B交接件契约与并行Executor真机对照验收.md) | **验收**（wfr-㊾ 批③）：交接式 A→B handoff（executor 复合 worker + parallel 扇出/幂等收拢 + `handoff` 键 + `.wfr/handoffs/` 落盘）+ `--max-parallel` + `mch_par` workflow；9 单测 + 真机串行 mch vs 并行 mch_par 对照 | 🟢 702 全绿 + 真机 |
| 32 | [32-副作用治理对照总纲评估与规划.md](32-副作用治理对照总纲评估与规划.md) | **评估与规划**：对照《Agent 执行治理总纲》（discuss 18）重审 wfr-㊸ 副作用治理——8 闸门 × 三层保障 × 12 缺口覆盖矩阵；关键结论（账本三态=ExecutionGuard、task 层已闭合 G1、五个实质缺口）；规划三档（P0 自检/UNVERIFIED/effectClass · P1 终态四档/抽检 · 押后补偿/路由/Signal） | 📋 评估已成立（待实施） |
| 33 | [33-治理全链workflow落地与验收.md](33-治理全链workflow落地与验收.md) | **验收**（wfr-㊹ 治理）：governed 单 workflow 完整实现治理思想——UNVERIFIED 双档（无判据→unverified）/ _timeout（on_max→超时档）/ _cancelled（plan_review abort→取消档）/ effect_class+启动自检（G4）；终态四档穷尽 | 🟢 720 全绿（governance 6 + effect_class 3） |
| 34 | [34-动态工具库与活动工具集落地.md](34-动态工具库与活动工具集落地.md) | **落地**（wfr-㊹ 治理）：tool_registry 节点两源——`.so` 动态库（裸 C 契约 dlopen，mock 顶替内置）+ `select` **内置按名选择**（不拷定义）；活动工具集发布进 results.active_tools，model/run_tools 从 context 取（共享优先、内置兜底）；run 允许名单防注入；12 新测试 | 🟢 732 全绿 |
| 35 | [35-副作用mock场景库与可逆性梯度workflow验收.md](35-副作用mock场景库与可逆性梯度workflow验收.md) | **验收**（wfr-㊹）：sidefx_mock_demo + `libsidefx_notify.so`——可逆性梯度工具（订单幂等/微信窗口可撤/短信不可撤/电话任状态可关）+ 模拟时钟；31 case（30 清单）；**引擎修两处**（approve 共享回退、desc_for 批量 dlsym 稳定指针）；wfr 接 shared_registry/tool_lib_dirs | 🟢 763 全绿 |
| 36 | [36-测试全景报告与方法论.md](36-测试全景报告与方法论.md) | **全景盘点**：763 测试 × 8 层（workflow 307 占 40%）；tool 纯函数+副作用计数 / agent 记忆化 / graph 引擎 / workflow 脚本化模型+动态库+probe 矩阵+模拟钟+串行链 九法；附纪律五条与运行命令 | 🟢 763 全绿 |
| 37 | [37-副作用工具实现方法参考.md](37-副作用工具实现方法参考.md) | **Agent 可复用参考**：副作用工具自包含方法论——声明契约（read_only/effect_class/approval/probe/ttl）· 治理链三挂载（guard/approve/活动集允许名单）· 动态库工程（跨 DSO + 三个真实坑）· 可逆性梯度状态机四模式 · 测试 M1~M8 各配实测与有效性；含落地 checklist | 🟢 763 全绿 |
| 38 | [38-Agent工具环检测落地.md](38-Agent工具环检测落地.md) | **落地**（wfr-㊹）：run_tools 跨轮指纹环检测（方法 1）——state `_tool_loop` 表 + `key_for(NameAndArgs)`；连续 3 次拦截不执行（error 点醒）；只读豁免 / 换参换工具复位 / 拦截计数保留；6 新测试 + 两个实测修正（拦截后计数、canonical_json 紧凑式）| 🟢 769 全绿 |
| 39 | [39-治理手册对照评估与A6验收规划.md](39-治理手册对照评估与A6验收规划.md) | **对照评估 + 规划**：discuss 19 手册（9 系统跨优点）逐条对照 cppchain 现状（14 组 × 判定）；结论——T1/T2·unknown·G8·环检测已落地，缺 4 事（A6 独立验收者 · B2 版本单调 · A7 impossible · D4 逃生阀）；A6/B2 已落地 + 后续顺序 | 🟢 781 全绿（A6/B2 已落地）|
| 40 | [40-A6独立验收subagent落地.md](40-A6独立验收subagent落地.md) | **落地**（A6/治 K1）：verify 加 `kind=agent` 独立验收 subagent——复用 `agent::Agent` + 只读 toolset（verify_ro），三态 done/fail/unverified（判不了推进+诚实标注）；transcript 隔离；修落账 move-null bug；6 新测试 | 🟢 775 全绿 |
| 41 | [41-B2证据版本单调性落地.md](41-B2证据版本单调性落地.md) | **落地**（B2/A9 正解）：verify 判据可声明 `version` 校验"产物新鲜度"——版本号由引擎单调源 `next_artifact_version()` 分配（模型/工具伪造不了），state `_artifact_ver`（merge_slots）+ task 档案快照；节点层按 `call.args.path` 登记（零 tool 层改动）；过时→Partial fail-closed；6 新测试 | 🟢 781 全绿 |
| 42 | [42-MCP首期client库落地.md](42-MCP首期client库落地.md) | **落地**（MCP P1）：`src/mcp/` `MCPClient`——JSON-RPC 2.0 over HTTP（复用 http::Transport + http_json，依赖纪律同 web_search）；initialize / tools/list / tools/call / resources/list + `make_mcp_tool` 工具桥（content→text / structuredContent）；9 离线单测（FakeTransport mock server）| 🟢 793 全绿 |
| 43 | [43-MCP二期三路发现与连接管理器落地.md](43-MCP二期三路发现与连接管理器落地.md) | **落地**（MCP P2）：三路发现（so:// file:// url://，统一 schema fail-closed）+ `MCPConnectionManager`（懒建握手复用池 + 凭证注入）+ provider 节点（两段式装配进 tool_registry，工具名 `<server>_<tool>` 清洗；发布面 strip_meta 无 token）；25 新测试 + P1 隐藏告警清理；离线 demo examples/mcp_provider | 🟢 818 全绿 |
| 44 | [44-副作用·防重放·防环·结果可信·可回溯·可恢复.md](44-副作用·防重放·防环·结果可信·可回溯·可恢复.md) | **全景汇总**：七问题各成一篇（副作用 / 防重放 / 防环调用 / 结果可信 / 可回溯 / 可恢复 / **防幻觉与数字一致性**），每篇按七段式独立说明——什么问题 / 用户会怎么问 / 过去 badcase / 解决思路 / cppchain 实现 / 实测结果 / 效果评价；附回归基线链 161→…→781（+A7；数值与防幻觉主线 783 待建档）| ✅ 汇总落盘 |
| 45 | [45-MCP三期SSE单发与会话回带落地.md](45-MCP三期SSE单发与会话回带落地.md) | **落地**（MCP P3 批①）：响应头上浮（curl header_cb / FakeTransport.Script.headers → `JsonResponse.headers`）+ `MCPClient` SSE 单发解析（`data:` 帧抽取）+ `mcp-session-id` 会话捕获回带；400+RPC error 上浮可读消息；真机（Everyting）发现并锁定 **cursor:null 被拒 → 省略**；6 新测试 + 真机 probe 直连注册 16 工具、echo 调通 | 🟢 824 全绿 |
| 46 | [46-MCP三期批②事件驱动变化监听落地.md](46-MCP三期批②事件驱动变化监听落地.md) | **落地**（MCP P3 批②，P3 全齐）：长连接收 `notifications/tools/list_changed`（**非轮询**）——`http/stream.hpp` 可选 `StreamProvider`（curl 挂进现有 multi，0 线程；FakeTransport 同步可测）+ `MCPClient::listen`（GET+会话头）+ `ToolRegistry::remove` + `McpListener`（SSE 组帧/半帧拼接 + 自有 worker 线程跑 on_change）+ provider `watch` 配置（默认零长连接；能力门缺 listChanged → error 观察位）；**gdb 挖出两雷**——闭包悬垂引用 SEGV（refresh 提为自由函数 + 只捕持久指针）、IO 线程自我死锁（on_change 移 worker）；7 新测试 + 真机 Everything 推流验收（trigger-list-changed → changes=1）| 🟢 831 全绿 |
| 47 | [47-proxyNode子workflow调用验收.md](47-proxyNode子workflow调用验收.md) | **落地**（proxyNode = 运行期调用子 workflow，参考 neuronic miclaw-rs `workflow.call`）：`subworkflow` 节点——编译期经 loader 装载子 IR（fail-fast）+ 环/深度防护；sync 子 results 通道提升为帧顶层键 + `_proxy`；detach fire-and-forget 落盘 `.wfr/subflows/` 审计；config 同源 / 状态隔离；`examples/proxy_call` + 9 新测试 | 🟢 851 全绿 |
| 48 | [48-P1.5声明式配置三形态验收.md](48-P1.5声明式配置三形态验收.md) | **落地**（P1.5 · config 三形态，参考 miclaw-rs `crates/config`）：`declarations.configs` 声明区（`{name,from,target?}`）——命名空间 `@config.<name>.<路径>` 点分引用（**仅声明名导航，存量多级写法零回归**）+ target 顶层键平铺 + `${workdir}/${sessionId}/${runId}` 运行变量 + 编译期启动即炸；`fire` 合并缺省 ∪ 宿主覆盖；proxyNode 子 workflow 自声明（config_dir 同一根）；wfr 宿主开箱即用；`examples/config_decl` + 11 新测试 | 🟢 861 全绿 |
| 49 | [49-project自管理第一刀验收.md](49-project自管理第一刀验收.md) | **落地**（Project 自管理第一刀，arch 26）：`.wfr/projects/<p>/` 目录体系 + `project` 节点（begin/end）——项目级上下文注入 `system_context`（目标/完成标准/纪律/lessons/本任务）+ `@config.task ∈ tasks[]` 校验（fail-closed）+ 经验沉淀 lessons.json 追加 + progress 更新；**任务档案路径双轨**（`@config.project` 非空下沉项目目录，空走旧 `.wfr/tasks` 零回归）；发布模板 `lot_project`；`examples/project_run` + 8 新测试 | 🟢 869 全绿，自管理 loop 押后 |
| 50 | [50-项目级完成标准自动判定验收.md](50-项目级完成标准自动判定验收.md) | **落地**（arch 26 第二刀）：`verify phase=review` 新增 `project` 字段 → **项目级终审**——Reviewer 读项目简报（目标/验收标准/纪律 + 各任务进度/last_summary，复用批级终审输入隔离）；verdict 落 plan_log + `state.project_review` → `project end` 写 `project.json.completion`；判据空/“null”回退 completion_criteria；缺模型 fail-closed；`lot_project` 内置终审行；`project_review_test` 5 例 | 🟢 874 全绿，fail 后自动重跑押后 |
| 51 | [51-自管理loop调度循环验收.md](51-自管理loop调度循环验收.md) | **落地**（arch 27 自管理 loop 第一刀）：新 `project_drive` 节点（meta 决策规则档/模型档）读 project.json → 选下一个未完成任务（或停机）；proxyNode `input.config` **逐键合并**进子 config（运行期动态分派 `@config.task`）；expr 门吃停机判据；发布模板 `project_drive.json`（loop 调度 lot_project）；`project_drive_test` 6 例 + 离线 demo | 🟢 880 全绿，meta 学习/依赖图押后 |
| 52 | [52-meta从lessons学习改进决策验收.md](52-meta从lessons学习改进决策验收.md) | **落地**（arch 27 第二刀· 经验回流）：project_drive 读 lessons → **相关性挑选**（applicable_if 命中任务 id/name）+ 简报【项目经验】段（[lesson N] 编号）+ **规则档加权**（命中未完成排前）+ 模型档提示要求 reason 标注 `[lesson N]`（可审计回流锚点）+ `lessons_used` 计数；【项目经验】段只列相关；无经验时行为零变化；`project_drive_test` +5 | 🟢 885 全绿，真机量化待跑 |
| 53 | [53-任务依赖图调度验收.md](53-任务依赖图调度验收.md) | **落地**（arch 27 第三刀）：`tasks[].deps` 任务级前置依赖——就绪=未完成&&deps 全 done（缺失/未知→不满足）；规则档在**就绪**未完成里做经验加权（依赖门挡下"文件序在前但依赖未满足"）；**阻塞停机新判据**（有未完成但无一就绪→`blocked`、`done=false`，≠完成，不死循环）；模型档只能选就绪任务（选未就绪→拒绝回落）；`done` 语义修正=停机且非阻塞；`project_drive_test` +6 | 🟢 891 全绿 |
| 54 | [54-自适应plansteps验收.md](54-自适应plansteps验收.md) | **落地**（arch 27 第四刀）：任务计划来源三级递退——预置 `tasks[].steps` > 模型档同轮输出 `plan_steps`（每步含 id/goal，非法 fail-closed）> 规则档**单步兜底**（任务目标，免 missing_seed 空转）；预置优先幂等（模型 plan 不覆盖）；结果 `plan_source`（preset/model/fallback）可观测；`project_drive_test` +5 | 🟢 896 全绿 |
| 55 | [55-评审fail专线重跑路由验收.md](55-评审fail专线重跑路由验收.md) | **落地**（arch 27 第五刀，自管理 loop 收官）：项目终审 fail 可点名 `rework:[任务id]` → `completion.rework` 落档 → `project_drive` 返工模式**重跑清单任务（即使 done）**（依赖仍就绪，无一阵就绪→阻塞停机）；子 workflow 再评审闭环（done 退出返工）；模型选非清单→回落；`rework` 计数；`project_drive_test` +5 | 🟢 901 全绿，六刀收官 |
| 56 | [56-wfrcron触发接入验收.md](56-wfrcron触发接入验收.md) | **落地**（使能② / arch 27 §十一）：新 `wfr_cron_batch`——`.wfr/cron.json` job 支持 `project` 字段 → 触发 **lot_project**（project 版单批）+ 两段 session `"<project>.<task>"` + `plan_steps` 注入（读 project.json steps，无→单步兜底）+ goal（job 覆盖任务）；无 project → lot 现状兼容；`wfr cron` 分支改用 helper；`cron_batch_test` 6 例 + E2E 冒烟 | 🟢 907 全绿 |
| 57 | [57-longdoc超长内容通用方法验收.md](57-longdoc超长内容通用方法验收.md) | **落地**（arch 28，方法为目标·蜀山为测试物）：应对超长内容的通用机制 `longdoc` 三件——**ingest**（单遍扫目录每文件=单元 → 骨架 {blurb,keys} 双档 → `longdoc.index.json` units+倒排）/ **retrieve**（query 分词 → keys 倒排 IDF top-k → 读命中原文切片 → system_context 注入【用户问题】+片段）/ **ask 模板**（检索→模型答带 [单元id] 引用）；与语料无关；蜀山 42 块 demo 量化三问共读 ≈ 全量 13.83%；`longdoc_test` 8 例 | 🟢 915 全绿，向量臂/真机质量押后 |
| 58 | [58-longdoc分块策略进阶验收.md](58-longdoc分块策略进阶验收.md) | **落地**（longdoc 下一刀）：分块策略 file/chapter/fixed（`@config.longdoc_unit_of` + chunk_chars/overlap）；章节按「第X回/章」行切块、无标题退化整文件；固定窗 UTF-8 安全边界带 overlap；索引 units 带 offset、retrieve 按字节区间读块（旧索引兼容）；4 新测试 + 修 tokenize 多字节字面量恒 false 死代码；920 全绿 |
| 59 | [59-longdoc向量臂验收.md](59-longdoc向量臂验收.md) | **落地**（longdoc 向量臂）：ComponentRegistry 增 embeddings 槽；ingest 用 `@config.longdoc_embed` 指名组件 → blurb 批量嵌入存 units[].emb；retrieve hybrid 打分（`longdoc_vector_weight` 混合余弦+IDF 归一，无关键词命中→纯向量，无 embedding→纯关键词兼容）；泛问"女儿"无 keys 命中靠向量命中 a.txt；3 新测试；923 全绿 |
| 60 | [60-longdoc骨架模型档真机质量验证.md](60-longdoc骨架模型档真机质量验证.md) | **验证**（longdoc 骨架模型档真机）：DeepSeek-flash 当骨架器，蜀山前 6 块 6/6 出实质 blurb（故事摘要）+ 丰富专名 keys（周淳/李宁/李英琼/毛太/醉道人/白云大师全中）；「妙一」不在前 6 块故缺；证实"向量臂依赖 blurb 质量"前提真机成立；成本=每块一次轻量调用 |
| 61 | [61-longdoc人物关系故事线索落地.md](61-longdoc人物关系故事线索落地.md) | **落地**（longdoc 全局综合）：ingest 模型档提示词一次调用追加 relations/thread → 索引 v4 units 带 relations/thread + 预聚合 rel_net（a-b 排序键去重+引用块，展示保留原序）+ threads；新节点 longdoc_relation（问题+关系网+线索→system_context→model 作答）+ 模板 longdoc_relask；真机实证 39 条关系网、「李宁—周淳→故交」「双线主线汇向赴成都杀毛太」；3 新测试；926 全绿 |
| 62 | [62-事实幻觉的范围化处理与防幻觉信号灯验收.md](62-事实幻觉的范围化处理与防幻觉信号灯验收.md) | **验收记录（2026-10-27）**：防幻觉信号灯五切片落地——message 63 / workflow 421 全绿、默认关零影响；`phantasma_demo` 三场景离线验收（范围内强制查证 / 范围外快答 / 有效信号免查，`run-demo.sh` 一键）；`phantasma_in_scope` 范围域、`phantasma_reason_passes` 防伪表、c_model 合成 web_search 强制取数；联动 [44](44-副作用·防重放·防环·结果可信·可回溯·可恢复.md)·篇七 防幻觉主线；遗留见 §8（plan_log 行 / 范围机械化）| 🟢 已验收（离线 demo） |
