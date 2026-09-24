# cppchain

> 用 **C++26** 从底向上复刻 langchain / langgraph 的**思路**（不是完整实现），
> 一边学 Agent 开发，一边把每类坑用"问题 + demo"的方式研究透。

- 项目基调与构建原则：[`docs/01-discuss/01-项目基调与构建原则.md`](docs/01-discuss/01-项目基调与构建原则.md)
- 里程碑路线图（M0–M10）：[`docs/01-discuss/03-langchain结构与构建路线图.md`](docs/01-discuss/03-langchain结构与构建路线图.md)
- 问题域现状盘点：[`docs/04-problems/05-五大问题现状盘点.md`](docs/04-problems/05-五大问题现状盘点.md)

## 快速开始

```bash
# 工具链：GCC 16 部署在 ~/opt/gcc16（免 sudo，见 docs/01-discuss/02）
source ~/opt/gcc16/env.sh

cmake --preset dev            # 配置（首次会 FetchContent 拉 nlohmann/gtest）
cmake --build --preset dev    # 编译
ctest --preset dev            # 全部测试（136 项）

./run-demo.sh                 # 编译并运行全部 demo
./run-demo.sh --list          # 列出 demo
./run-demo.sh agent_loop      # 只跑指定的（真机 demo 需 .env）
```

真实端点配置放 `.env`（**已被 git 忽略**，模板见 `.env.example`）：
`LLM_API_KEY` / `LLM_BASE_URL` / `LLM_MODEL`（对话），`EMBEDDING_*`（向量）。
企业代理环境需 CA bundle（本机自动探测 `~/.geo/corp-ca.pem`，也可用 `LLM_CA_BUNDLE`）。

## 分层结构（自底向上）

```
agent/    Agent 循环（M7）：middleware = 压缩 / 截断 / 审批
graph/    Graph 引擎（M6）：StateGraph、条件边、每步快照、恢复续跑
state/    状态层（M5）：reducer / add_messages / Checkpointer / Store
tool/     工具层（M3）：ToolSpec、注册表、**可插拔检索**、ToolNode（并行）
model/    模型层（M2/M4）：ChatModel（同步/协程/流式/取消）、结构化输出、Embeddings
http/     传输层（M2）：Transport 抽象 + FakeTransport（离线）+ curl_multi（真网络）
message/  消息层（M1）：Message/ContentBlock/**JSON 序列化契约**
```

约束：**每层只依赖下层**；`cppchain_model` 不链接 libcurl（传输由外部注入）；
状态全程 JSON（快照可直接落盘/跨进程）。

## 里程碑

| | 内容 | 状态 |
|---|---|---|
| M0 | 技术底座（GCC16/CMake/nlohmann/libcurl） | ✅ |
| M1 | Message + ContentBlock + 序列化契约 | ✅ |
| M2 | ChatModel + OpenAI 兼容 + curl_multi + 协程糖 | ✅ |
| M3 | Tool 层 + 工具检索（关键词 IDF / 向量） | ✅ |
| M4 | 结构化输出（两策略 + 校验 + 回喂重试） | ✅ |
| M5 | State/Reducer + Checkpointer + Store | ✅ |
| M6 | Graph 引擎（顺序 super-step + 快照恢复） | ✅ |
| M7 | Agent 循环 + 三中间件（压缩/截断/审批） | ✅ |
| M8–M10 | Subagent / 问题收官 / 运行时补全（Streaming·Interrupt·Send） | ⬜ |

## 几条实测结论（省得再踩）

- **标准有 ≠ 实现有**：`std::execution`（P2300）在 GCC 16 上不存在；C++26 反射的
  `std::meta` 是空壳（无法推导工具 schema）；本端点的 `response_format=json_schema` 返回 400。
- **端点差异**：本端点只支持 `json_object`；强制工具调用可作结构化输出的替代；
  embedding 端点是 DashScope 原生格式（`input.texts`、1024 维、**批量上限 20**、返回**无序**需按 `text_index` 重排）。
- **评测纪律**：检索类实验必须同时报 precision/recall（压缩比单独看会骗人）；
  中间件类实验必须确认被测路径真的被走到（否则"跑通"是假象）。

## 目录约定

```
include/cppchain/  头文件（按层分目录）      src/       实现（按层分目录）
tests/             gtest（按层分目录）      examples/  每个 demo 验证一件事（见 examples/README.md）
docs/              01-discuss / 02-arch-design / 03-detail-design / 04-problems
run-demo.sh        编译并运行 demo
```
