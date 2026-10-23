# 43 · MCP 二期：三路发现（so:// file:// url://）+ 连接管理器 + provider 节点

> **链路**：MCP P1（problems/42，`MCPClient` + `make_mcp_tool`）→ 本档 = **P2**。
> 42 号文档定的 P2 交付：**provider 节点（tool_registry 加 `type:"mcp"` 源）**。
> 本档是在此之上把"怎么发现 MCP server、怎么连、怎么把工具装进注册表"整条链路落地
> （发现层 + 连接管理层 + provider 装配节点），并保持 P1 的依赖纪律与凭证纪律。

## 一、问题与方案

### 用户诉求（P2 范围，42 号文档拍板）
1. server **发现**：MCP server 从哪来？——三类来源：动态库注册（so://，与 tool_lib_dirs 同族）、
   本地配置文件（file://）、远程列表端点（url://）。要求三路产出**同一**列表格式，坏格式 **fail-closed**。
2. server **连接**：cppchain 内部统一持有/管理连接状态（协议要求客户端/服务端各管各的状态机），
   懒建 + 握手后才可用 + 同名复用。
3. **provider 节点**：把发现到的 server 暴露的工具，装配进 `tool::ToolRegistry`
   （handler 里走 `client->call_tool`），供 workflow/model 消费。
4. **凭证纪律**：meta（token 等）只在注入 MCPClient 时用；发布进 results/state 的面
   **必须剥掉**（token 不外泄）。

### 为什么不是"注册表直接暴露 MCP 方法"
MCP 工具是**动态 schema**（tools/list 才知道），不像 file_write/bash 是编译期 ToolSpec。
所以 provider 必须在**运行时**发现 → 建连 → list → 转 ToolSpec 注册。这与
`load_dynamic_tools`（wfr-㊹）的"dlopen 后按名造 ToolSpec"同族，区别只在**发现源**。

## 二、实现（src/mcp/ 三件套 + 一个库函数）

### 1. `list_schema.{hpp,cpp}` —— 统一列表 schema
- 一个 JSON 结构打通三路：`{"mcpServers":[{"name","type","url","transport","meta","description"}]}`。
- `parse_mcp_server_list(j)`：逐项校验 fail-closed —— 缺 mcpServers / 项非对象 / 缺 name /
  type∉{local,remote} / remote 缺 url / transport∉{http,sse}，任一即整体报错（**三路汇合到同一解析**）。
- `strip_meta(servers)`：发布面（无 `meta` 键）——token 只存活于连接管理器/MCPClient。

### 2. `manager.{hpp,cpp}` —— `MCPConnectionManager`
- `discover(from)`：按前缀分发三路 →
  - `so://<so-name>:<fn>`：找 `lib<so-name>.so`（so_dirs 搜索）→ dlsym `mcp_list_<fn>` → 列表 JSON；
  - `file://<path>`：读本地 JSON；
  - `url://<url>`：GET + `http_json`（复用传输层，同 web_search 纪律）。
  - 未知前缀一句话报错。
- `client_for(info)`：**懒建 + 握手 + 复用池**（name → `unique_ptr<MCPClient>`）。
  - `meta.token` → `Authorization: Bearer`；其余 meta 键 → 自定义头；
  - 握手失败 → 池**不保留**，整体报错，再调会重新握手（不吞坏连接）。

### 3. `provider.{hpp,cpp}` —— `mcp_provider(mgr, from, registry)`
- **两段式**：第一阶段全部 `client_for` + `list_tools`（任一失败 → 整体失败）；成功了才第二阶段
  一次性 `registry.add`——**不留半注册**（同 `load_dynamic_tools` 的"任一失败整体失败"语义）。
- **工具全名 = 清洗后的 `<server>_<tool>`**：跨 server 工具撞名天然隔开；清洗到
  `[A-Za-z0-9_-]`（OpenAI 函数名语料），空格/点等 → `_`（非 ASCII 逐字节换 `_`，确定性）。
- `McpProviderResult`：`servers`（strip_meta 发布面）+ `tools`（实际注册全名）——**凭证零进面**。

### 依赖与链接纪律
- 与 P1 相同：只依赖 `http::Transport` 接口 + `nlohmann`；manager 的 url:// 复用 `tool::http_json`
  （同 `cppchain_tool` 库内）。
- 新增源文件直接进 `cppchain_tool`（`src/CMakeLists.txt`），无新链接依赖（dlopen 在 glibc≥2.34
  已在 libc，无需 `-ldl`）。

## 三、测试（tests/tool/mcp_discover_test.cpp 25 例 + P1 告警清理）

| 组 | 例数 | 覆盖 |
|---|---|---|
| `McpP2ListSchema` | 7 | parse 全字段（meta 在 parse 层保留）；缺 mcpServers / 缺 name / type 越表 / remote 缺 url / transport 越表 全 fail-closed；strip_meta 剥 token（url/description 保留、空 url 不发布） |
| `McpP2Manager` | 15 | file://（happy / 打不开 / 非法 JSON / 合法 JSON 但结构错）；url://（happy 且断言 **GET** / 传输错 / 非 JSON）；未知前缀；so://（**真实 dlopen** 测试插件 / 合法空表 / 库缺失 / 符号缺失 / 格式缺 fn）；`client_for` 懒建+复用（同指针、connected 计数、`server_names`、请求头 **MCP-Protocol-Version + Bearer**）/ 握手失败不落池（重握手、请求数 2） |
| `McpP2Provider` | 3 | 单 server 双工具注册（prefix 全名、strip_meta 无 token、handler 直调 MCP 返回 text）；**两段式**（第二 server 握手失败 → 整体失败 + registry 零改动）；工具名清洗（空格/点 → `_`） |

- **so:// 测试插件**：`tests/plugin_mcp/mcp_list_plugin.cpp`（SHARED，dlsym `mcp_list_weather/empty`），
  仿 `plugin_demo/mock_tools` 纪律；路径经 `MCP_TEST_SO_DIR` 编译期注入。
- **顺带清理**：P1 的 `mcp_client_test.cpp` 有 23 条 `-Wmissing-field-initializers` 隐藏告警
  （整编才暴露），改成显式全字段 `script_raw/script_fail` 构造助手——MCP 测试区回到真零告警。
- 临时文件走 `temp_directory_path()`（file:// 发现源）。

## 四、边界

- **url:// 只发现不建连**：列表用一次 GET 拉取；连 server 以后续 `client_for`（base_url 在其列表项里）。
- **provider 是库函数，尚非 workflow 节点**：本档交付"一个 server → 一串工具进注册表"的装配函数；
  把它包成 workflow 节点 / import 语法让用户在 workflow 里声明 `mcp_` 工具，留给 **P3**（变化监听对齐 QQ）。
- **SSE transport**：列表里 `transport:"sse"` 合法但连接仍是 http 同步（P1 现状），SSE 长连是后续期。
- **local（stdio）**：词表里收着（`type:"local"`），真正拉起子进程是 P5 stdio transport 的事。
- **名称清洗是字节级**：非 ASCII 多字节字符会被逐字节换成多个 `_`——确定性优先，美观其次。

## 五、结果

- 全仓 **818 全绿**（793 P1 基线 + 二期 25 新 + 顺带清理过程零回归）。
- MCP P2 交付闭环：三路发现 → 统一 schema（fail-closed）→ 懒建握手复用 → provider 两段式装配 →
  发布面凭证剥除。离线 demo 见下。

## Demo：examples/mcp_provider（离线）

`ScriptedMcpServer`（内嵌假 MCP server，按 JSON-RPC method 应答）+ file:// 临时列表 + so:// 演示插件
（`mcp_weather_list`，dlsym `mcp_list_weather`）+ url:// 同一列表端点。跑起来输出：

- 三路发现**同一**列表（`weather` remote http）+ meta 在发现层保留；
- provider 注册 `weather_get_weather` / `weather_get_forecast`；
- 发布面 strip_meta（无 `demo-token`）+ 请求头注入 `Authorization: Bearer demo-token`（凭证核对双双确认）；
- 两个 handler 直调 MCP：`{"text":"[上海] 晴，23°C，微风"}` 等。

```bash
source ~/opt/gcc16/env.sh && ./build/dev/examples/mcp_provider
```