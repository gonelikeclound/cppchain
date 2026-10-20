# 08 · web_search 工具配置说明（feedcoop 联网搜索）

> 本文档基于源码整理：`include/cppchain/tool/web_search.hpp`、`src/tool/web_search.cpp`、
> `src/wfr/main.cpp`（注册处）。说明 web_search 工具是什么、怎么配置、在哪用、常见坑。

---

## 一、这是什么

**真实联网搜索工具**：通过 feedcoop global search 端点检索最新网页资料。
返回**原始文档列表**（标题/链接/来源/发布时间/摘要片段），**不含成品答案**——
由调用它的 LLM 基于文档组织回答。

- 对应源码：`include/cppchain/tool/web_search.hpp`（声明）、`src/tool/web_search.cpp`（实现）
- 参考项目：simple-agent 的 handle_web_search（思路同源）

**协议**（`web_search.cpp:111-151`）：
```
POST {base_url}
Authorization: Bearer $WEB_SEARCH_API_KEY
{"query": q, "doc_count": N, "max_snippet_length": L, "max_image_count_per_doc": 0}
→ Result.Documents[]（Title/Url/HostInfo.Hostname/DocumentInfo.PublishTime/Snippet[].Text）
```

## 二、配置项（WebSearchConfig，`web_search.hpp:23-33`）

| 字段 | 默认值 | 说明 |
|---|---|---|
| `base_url` | `https://open.feedcoopapi.com/search_api/global_search` | feedcoop 端点 |
| `api_key` | 空 | feedcoop 搜索 API Key（**Bearer 认证**） |
| `doc_count` | 5 | 请求文档数（代码 clamp 1–10） |
| `max_snippet_length` | 200 | 每条摘要片段最大字符数 |
| `ca_info` | 空 | 企业代理/自签 CA（见 §四 坑 3） |
| `timeout_ms` | 30000 | 请求超时 |
| `ttl` | 15 分钟 | 搜索快照时效（热点/价格/新闻 15 分钟后视为过期，由 `StaleStateGuard` 消费） |

## 三、在 wfr 里怎么配置（宿主注入）

注册处：`src/wfr/main.cpp:476-488`。

```cpp
// wfr/main.cpp:485-487
tool::WebSearchConfig ws_cfg;
ws_cfg.api_key = env_or(dotenv, "WEB_SEARCH_API_KEY");   // 关键：从环境变量/.env 读 key
registry.add(make_web_search_tool(transport, std::move(ws_cfg)));
```

**只要一个配置项**：`WEB_SEARCH_API_KEY`。读取顺序（`src/wfr/util.hpp:51-58`）：

1. **真环境变量** `WEB_SEARCH_API_KEY`（`getenv` 优先）
2. **`.env` 文件**里的 `WEB_SEARCH_API_KEY`
3. 都没有 → `api_key` 为空 → 运行时调用搜索会报错（见 §五 坑 1）

其他配置（base_url/doc_count/...）在源码里是硬编码默认值，wfr 注册处**没有覆盖**——
即默认用 feedcoop 官方端点 + doc_count=5 + snippet 200。要改需改源码或扩展注册逻辑。

`.env` 模板（`src/wfr/main.cpp:477` 读 `{".env","../.env","../../.env"}`）：
```bash
# 仓库根 .env
WEB_SEARCH_API_KEY=your_feedcoop_key_here
```

## 四、工具集绑定（哪些 workflow 能用它）

`src/wfr/main.cpp:517-520` 注册工具集：

| toolset 名 | 含 web_search? | 用在哪 |
|---|---|---|
| `chat` | ✅ | 内置 chat / chat2（`get_weather, web_search, file_read, list_dir, file_write`） |
| `chat_bash` | ✅ | mch 等（`get_weather, web_search, file_read, list_dir, file_write, bash`） |

workflow 里 `think`/`model` 节点 `tools: "chat"`（或 `chat_bash`）就能让模型调 web_search。
例：`share/wfr/workflows/chat.json`、`mch.json` 的 Executor 用 `chat_bash`。

## 五、常见坑

1. **没配 key 只报运行时错**：`web_search.cpp:124-127` —— `api_key` 空 → 返回
   `"web_search 未配置 API Key：请在 .env 设置 WEB_SEARCH_API_KEY"`（回喂给模型，不崩）。
   所以**配置错不会在启动时暴露**，只有真机调用搜索才见。验证配置：跑一次带搜索的 workflow
   看回执。

2. **空结果当成功**：`web_search.cpp:143-146` —— 搜索返回 0 文档 → `unexpected`
   "搜索未返回有效内容…请换一个更具体的查询词"。语义：**避免把"空结果"误当"已联网取到资料"**。

3. **企业代理 MITM**：web_search 走**共享 transport**（`CurlMultiTransport`，与 LLM/天气同一个）。
   本项目实测企业代理会拦截 HTTPS（见记忆/README：DeepSeek 被 MITM）。CA 处理：
   `WebSearchConfig.ca_info` 可传 CA bundle（`util.hpp:60-70`：`LLM_CA_BUNDLE` 或自动 `~/.geo/corp-ca.pem`）。
   但 **wfr 注册处当前没给 ws_cfg.ca_info 赋值**（`main.cpp:486` 只设 api_key）——
   若企业代理导致搜索失败，需在注册处补 `ws_cfg.ca_info = ca_bundle_from_env()`（与模型配置一致）。

4. **时效性**：`ttl=15 分钟`——搜索结果会被 `StaleStateGuard` 标过期，模型不会拿 15 分钟前
   的搜索当"最新"。这是特性不是 bug。

## 六、离线单测 / 纯函数

`make_web_search_tool` 依赖真实 transport（联网）。但协议相关纯函数可离线测（FakeTransport 注入形状）：

- `build_web_search_request(cfg, query)`（`web_search.cpp:15-20`）→ 请求体 JSON
- `parse_web_search_response(raw, query)`（`web_search.cpp:63-107`）→ 归一化 `{query,total,documents[]}`
  - 兼容 `Result.Documents`（实际 API）与 `Result.GlobalSearchResp.Documents`（旧 spec）
  - 兼容 key 大小写（`Title/title`、`Snippet/snippet` 等）

---

*整理自源码：web_search.hpp / web_search.cpp / wfr main.cpp:476-488 / util.hpp:51-70。文档不进 git（用户要求不提交），内容可复现。*
