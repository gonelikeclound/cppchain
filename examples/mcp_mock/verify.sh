#!/usr/bin/env bash
# 起 mock MCP server，跑 driver 验证整条 MCP 链路，完事停 mock。退出码 = driver 判定。
# 用法：./examples/mcp_mock/verify.sh [MOCK_BIN_DIR]   默认 build/dev/examples
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN_DIR="${MOCK_BIN_DIR:-$ROOT/build/dev/examples}"
PORT="${MOCK_PORT:-3100}"
LOG="$(mktemp -t mcp_mock.XXXXXX.log)"

"$BIN_DIR/mcp_mock" --port "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT
sleep 1
if ! kill -0 "$PID" 2>/dev/null; then
  echo "mock MCP 启动失败，日志："; cat "$LOG"; exit 3
fi
MOCK_PORT="$PORT" "$BIN_DIR/mcp_mock_driver" "$PORT"