#!/usr/bin/env bash
# cppchain 默认 MCP 的启动脚本：起官方 Everything 参考 server（streamableHttp，零凭证）。
# 用法：./share/mcp/start-default.sh            # 默认 127.0.0.1:3001
#       PORT=8888 ./share/mcp/start-default.sh  # 自定义端口（改后把 mcp_servers.default.json 的 url 同步改）
set -e
PORT="${PORT:-3001}"
exec npx -y "@modelcontextprotocol/server-everything" streamableHttp --port "$PORT"