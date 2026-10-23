# cppchain 默认 MCP

默认清单：[`mcp_servers.default.json`](./mcp_servers.default.json)。

## 为什么只有「everything」一项

坦诚的现状（2026-10 实测排查）：**公网免密钥、可匿名直连 HTTP 的 MCP 端点基本不存在**——绝大多数
远程 MCP 都在 token/key 后面，其余官方参考 server 只跑 **stdio**（我们的 HTTP 客户端要到 P5 才能接）。
**`Everything` 是唯一零凭证、且经 MCP P1~P3 全链路真机验证**（初始化握手 / SSE 单发 / 会话回带 /
长连接收 `list_changed` 推送）的货真价实之选。

## 怎么用

```bash
# 1. 起服务（前台跑；Ctrl+C 停）
./share/mcp/start-default.sh
#   等价于：npx -y @modelcontextprotocol/server-everything streamableHttp --port 3001

# 2. 用 provider 装配进 tool_registry（仓库内任意消费者）
#   mcp_provider(mgr, "file://share/mcp/mcp_servers.default.json", registry,
#                McpProviderOptions{.watch = {"everything"}});
```

Everything 暴露的工具（2.0.0 实测 16 个）：`echo`、`get-sum`、`get-structured-content`、
`gzip-file-as-resource`、`trigger-list-changed`、`trigger-log-messages`、`get-tiny-image`…（能力门控的
`get-roots-list`/`sample-llm` 等需 client 声明 capabilities，属 P3 后续）。

## 要加真实远程 MCP（带 token）怎么改

复制本清单到你自己的文件（如 `tmp/mcp_servers.json`，仓库根 `tmp/` 已 gitignored），补 `meta`：

```json
{
  "mcpServers": [
    { "name": "my-remote", "type": "remote", "url": "https://api.example.com/mcp",
      "transport": "http", "meta": { "token": "sk-..." },
      "description": "你的远程 MCP" }
  ]
}
```

`provider` 会把 `meta.token` 做成 `Authorization: Bearer <token>` 注入连接（凭证只活在连接层，
发布的 `strip_meta` 面绝不含 token）。注：本机经企业代理访问外部 HTTPS 时，可能还需类似
`~/.geo/corp-ca.pem` 的 CA（与 DeepSeek 端点同款问题）。