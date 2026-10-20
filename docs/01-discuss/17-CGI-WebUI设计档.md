# 17 · CGI WebUI 简易 Agent 对话页设计档（🟡 已录制待启动）

> **定位**：在 wfr（workflow 运行时）之上加一层 **CGI WebUI**：用户在浏览器里像 ChatGPT 那样对话，
> 每次 query 由 CGI 调 **wfr 命令** 跑一条 **CGI 专属 workflow**，回答以结构化 JSON 回前端渲染。
> 参照 dsh 的 web UI（三栏/工作区/会话/消息流/composer）做**简易版**——独做对话流、工作区-会话管理、Think/注入/压缩行。
> 状态：🟡 已讨论收敛、待启动（先立设计，不实现）。

## 一、动机与四条硬性要求

把 wfr 的"命令行 workflow 运行器"挂到浏览器对话页上：会话多轮、工作区可开多个、压缩自动。
用户原话要求：
1. **每次用户 query → CGI 调用 wfr 命令**（不是内嵌库，是跑命令）；
2. **CGI 定义自己的 workflow**（主=日常对话 + 辅=上下文压缩 + 按需加），**不混 share/**；
3. **node 类型必须共享**（只在现有节点里组合，不允许新 node 类型）；
4. **参照 dsh webui**：用户可建**工作区**（= 一个目录），每个工作区可有**多个对话**（一个对话 = 一个 session）。

## 二、技术判定（已确认）

| 项 | 定论 | 理由 |
|---|---|---|
| CGI 形态 | **C++ CGI 可执行**（cgi-bin/wfr_cgi，CGI 协议：环境变量/输入→调 wfr→输出 JSON） | 用户要求，性能与 wfr 同语言 |
| 前端 | **原生单页 HTML+JS**（fetch 调 CGI action，零构建零框架） | 简易对话页够用；dsh 全框架不搬 |
| 对话响应 | **同步阻塞**（前端等 CGI→wfr 跑完再渲染） | 首次最简再谈流式（二期） |
| workflow 隔离 | **cgi-workflows/ 专属目录**，经工作区自己的 `settings.workflow_dirs` 指向（wfr 查找机制：settings→ws→全局→share） | 不碰 share/，要求② |
| 主/辅 workflow | **主内含自动压缩 + 辅独立并存** | 要求②"两条 workflow" + 已选"主内自触发"调和 |

## 三、目录骨架（拟）

```
cgi-workflows/            # CGi 专属 workflow（相对 share/ 隔离）
├── chat_cgi.json         # 主：日常对话 + 自动压缩（一个 IR 完成）
└── compress_cgi.json     # 辅：独立压缩链（按需/import 复用）
web/
├── index.html            # 原生单页
├── app.js                # fetch → CGI action
└── app.css
cgi-bin/wfr_cgi           # C++ 可执行：QUERY_STRING 分发 action → subprocess wfr → JSON
dsh.workspaces.json       # 工作区注册表：{名称 → 目录}（前端侧栏的持久真相）
```

## 四、CREDO / 现有垫底（直接可接）

- **wfr 命令运行器**（`src/wfr/main.cpp`）：`wfr <workflow> --workdir <ws> --session <s> --set k=v <query>`；
  返回前 `last_ai_text(result_state)`（main.cpp:74-88）取最终 AI 文本 → stdout 打印。CGI 子进程拿到即最终回答。
- **子进程执行**：`tool::detail::run_argv`（main.cpp:406 已见于 cron 的 re-entry；argv + timeout + 输出上限）。
- **会话读历史**：`replay_session_messages(path)`（`session_log.cpp:245`）→ `message::Message` 数组；
  message JSON round-trip（`message/serializer.hpp`）可直接序列化给前端渲染历史。
- **workflow 查找/隔离**：`wfr_lookup`（settings.workflow_dirs → ws → 全局 → share），
  `wfr.sh` 已在默认 tmp 工作区生成 settings 指向多目录——CGI 工作区自建 settings 指向 cgi-workflows/ 即可，share/ 一字不动。
- **主 workflow 拼装（全现成节点）**：
  `recall(load_history) → [expr/route 判历史长度: 超 → summarize(折叠) → 再进] → loop{think(model,chat tools) → echo_think → route → act(run_tools) → echo_tool} → _turn_end`。
  - `summarize`（when=messages/max_msgs，产 fold op 落日志）= 自动压缩的持久折叠（区别于 chat2 的渲染期 history_mode 压缩）。
  - `model`/`route`/`run_tools`/`echo` 全内置；**零新 node 类型**（要求③）。
- **辅助 workflow `compress_cgi`**：`load_history → summarize(detailed) → refine_history(preset) → _turn_end`，
  供按需/主 workflow import 复用（独立文件存在即要求②兑现；主内自触发仍满足）。

## 五、CGI action 面（拟，同步阻塞）

| action | 入参 | 出参（JSON） |
|---|---|---|
| `workspaces` | — | 工作区列表（读 dsh.workspaces.json） |
| `sessions` | ws | 某工作区会话列表（扫 `.wfr/sessions/session-*.log.jsonl`，含最近/条数） |
| `chat` | ws, s, q | subprocess `wfr chat_cgi --workdir <ws> --session <s> --set query=<q>` → `{answer, stats}` |
| `history` | ws, s | `replay_session_messages` → 消息 JSON 数组（前端渲染既有对话） |
| `compress` | ws, s | 按需调 `compress_cgi`（历史折叠落日志） |

## 六、dsh 参照的取舍（简易版）

- **做**：三栏骨架（工作区/会话侧栏 + 主会话消息流 + 可后续加详情列）；会话 CRUD（新建/切换/归档）；消息流渲染（Think 行折叠、上下文注入行、压缩标记行）；历史可滚动展开。对应 dsh UI-04/05/07 的核心件。
- **不做**（简易边界）：拖拽面板（UI-03）、流式增量（UI-05-01）、分叉/排队/插话（UI-07）、活动可视化树/后台任务（UI-08）、会话全文搜索（UI-04-08）、工作区分组/排序偏好。

## 七、第一问（启动切入点）

1. **CGI 协议细节**：stdin 帖子化 vs QUERY_STRING；子进程 wfr 的超时/输出上限/wfr 错误如何回前端（fail-closed 展示而非空白）；并发（多次 chat 同时来，子进程各自独立，wfr 本身无共享态？）。
2. **会话的 wfr 语义**：`chat` 对同一 `--session` 多轮 → 会话日志累积，`history` 读回天然连续。新建会话 = 新 session 名；归档 = 前端隐藏（日志不动）；名称显示取首条 user 消息切片？
3. **历史读取与渲染**：`replay_session_messages` 是按 user/ai/tool/reasoning 分块渲染；tool 消息折叠为行（dsh UI-08-01 简化版）；注入行（load_history 的 __input__）标"上下文"。
4. **压缩的实际触发**：主 workflow 内 summarize 的 `when=messages` 阈值定多少；压缩后前端如何标记"上下文已压缩"行（读日志的 fold op？`export_task_life` 同款判断）。
5. **C++ CGI 工程接入**：wfr_cgi 链接哪些库（复用 cppchain_workflow + run_argv？还是纯 external spawn）；CGI 环境变量读 URL（QUERY_STRING/REQUEST_METHOD/CONTENT_TYPE）与 JSON 输出。

## 八、验收预想

- `web/` 单页 + `cgi-bin/wfr_cgi`：浏览器建工作区 → 建会话 → 多轮对话（历史连续、Think 行折叠、压缩自动标记）；切工作区/会话互不串扰；workflow 全来自 cgi-workflows/（验证 share/ 没被动）；全仓零新 node 类型（编译即证）。
- 真机一轮（需 .env）；离线用 ScriptedModel 起一条主 workflow 断言"对话+压缩一体"跑通。

## 待办锚点
- 关联：discuss 16（project 化，工作区=目录的思路同源）；arch 14（wfr CLI + 调试台，本专题的运行时底座）；arch 20/22（长程会话机制）；arch 17（历史处理，压缩节点来源）。
- 启动第一步（可商）：**CGI 壳（wfr_cgi 的 action 分发 + subprocess wfr + JSON）** + **chat_cgi 主 workflow** 先落，前端单页搭三栏最小可用，再补辅助压缩 workflow 与归档。
- 备注：dsh 参照 = 用户提供的一份 web UI 需求规格（UI-03~08，三栏/工作区/会话/消息流/composer/交互/活动可视化），本专题只取其核心件做简易版。