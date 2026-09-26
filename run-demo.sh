#!/usr/bin/env bash
# run-demo.sh —— 一条命令编译并运行 examples/ 下的 demo
#
# 用法：
#   ./run-demo.sh                  # 跑全部 demo
#   ./run-demo.sh tools_retrieval  # 只跑指定的（可多个）
#   ./run-demo.sh --list           # 列出可用 demo
#   ./run-demo.sh -n chat_invoke   # -n/--no-build：跳过编译直接跑
#   ./run-demo.sh -h               # 帮助
#
# 说明：
#   · 编译与运行**都**需要 ~/opt/gcc16/env.sh：部分 demo 用到 GCC 16 libstdc++ 的
#     新符号（如 GLIBCXX_3.4.35），系统 libstdc++ 14 不够用。脚本自动 source。
#   · 所有 demo 都在**仓库根目录**下运行：chat_live 靠这里找 .env，
#     messages_serialize 的产物落在这里的 demo-out/。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build/dev"
DEMO_DIR="$BUILD_DIR/examples"
GCC_ENV="${GCC16_ENV:-$HOME/opt/gcc16/env.sh}"

# 已知 demo（顺序 = 展示顺序；描述仅用于 --list 与运行横幅）
KNOWN_DEMOS=(
  "messages_basic:M1 消息类型族与内容块"
  "messages_serialize:M1 序列化 round-trip 与落盘"
  "messages_tool_flow:M1 工具调用协议闭环"
  "messages_stream_chunk:M1 流式帧合并"
  "chat_invoke:M2 同步/协程调用（离线仿真）"
  "chat_stream:M2 流式与取消（离线仿真）"
  "chat_live:M2 真机联调（需 .env）"
  "tools_basic:M3 工具调用闭环与错误回喂"
  "tools_retrieval:M3 大批量工具检索量化"
  "tools_retrieval_vector:关键词 vs 向量检索对照（需向量端点）"
  "tools_frontline:M3 第一档内置工具 datetime/calculator/fetch_url"
  "tools_files:M3 文件工具三件套（沙箱边界·写=副作用）"
  "tools_arxiv:M3 arXiv 论文检索（Atom XML 解析）"
  "tools_approval:M7 file_write 审批端到端（名单/回调拒绝 + 防重放，真实磁盘断言）"
  "structured_output:M4 结构化抽取/分类/两策略对照（需 LLM 端点）"
  "state_basics:M5 状态归约/快照回放/跨线程 Store"
  "graph_basics:M6 线性 workflow/条件路由/中途恢复"
  "agent_loop:M7 Agent 循环与三中间件（需 LLM 端点）"
  "live_tools:M3 真网络工具 get_weather/web_search（需 .env：LLM + 搜索端点）"
  "history_agent:M7 会话历史——同一 thread 多轮记忆/无状态对照/thread 隔离（需 LLM 端点）"
  "research_chat:M7 意图二分连续聊天——casual(不带工具)/research(联网取数)+ 零外源兜底保证（需 .env：LLM + 搜索端点）"
  "agent_cli:M7 命令行 single/react agent——--mode 四策略对比(direct/react/react-trim/react-approve)+ REPL/单条/文件批量（需 .env）"
  "context_budget:M7 token 成本量化（需 LLM 端点）"
  "side_effect_guard:M7 副作用防重放（离线）"
  "stale_state:状态时效性 A/B 对照（需 LLM 端点）"
  "subagent_ask_expert:M8 ask_expert——子代理整体委派 + 主上下文干净 + 副作用透传（需 LLM 端点）"
  "subagent_workers:M8 orchestrator-worker 串行版——supervisor 拆任务 + 条件路由 + finalize 汇总（需 LLM 端点）"
  "subagent_handoff:M8 handoff 简化——worker 上交回主管 + redispatch 上限兜底（需 LLM 端点）"
)

describe() {
  local name="$1"
  for entry in "${KNOWN_DEMOS[@]}"; do
    if [[ "${entry%%:*}" == "$name" ]]; then
      printf '%s' "${entry#*:}"
      return
    fi
  done
  printf '%s' "(未登记的 demo)"
}

usage() {
  sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# ---- 参数解析 ----
NO_BUILD=0
LIST_ONLY=0
TARGETS=()

for arg in "$@"; do
  case "$arg" in
    -h | --help)
      usage
      exit 0
      ;;
    -l | --list)
      LIST_ONLY=1
      ;;
    -n | --no-build)
      NO_BUILD=1
      ;;
    -*)
      printf '未知参数: %s\n\n' "$arg" >&2
      usage >&2
      exit 2
      ;;
    *)
      TARGETS+=("$arg")
      ;;
  esac
done

# ---- 列出已构建的 demo 二进制 ----
discover_built() {
  [[ -d "$DEMO_DIR" ]] || return 0
  find "$DEMO_DIR" -maxdepth 1 -type f -executable -printf '%f\n' 2>/dev/null | sort
}

if [[ "$LIST_ONLY" == 1 ]]; then
  printf '可用 demo（仓库根目录下 ./run-demo.sh <名称>）：\n\n'
  for entry in "${KNOWN_DEMOS[@]}"; do
    name="${entry%%:*}"
    if [[ -x "$DEMO_DIR/$name" ]]; then mark="已构建"; else mark="未构建"; fi
    printf '  %-22s %-34s [%s]\n' "$name" "${entry#*:}" "$mark"
  done
  printf '\n'
  exit 0
fi

# ---- 工具链环境（编译与运行都需要）----
# 外部 env.sh 可能引用未定义变量，source 期间临时关掉 nounset
if [[ -f "$GCC_ENV" ]]; then
  set +u
  # shellcheck disable=SC1090
  source "$GCC_ENV"
  set -u
else
  printf '提示：未找到 %s —— 编译会找不到 g++-16，运行也可能缺 libstdc++ 新符号\n' "$GCC_ENV" >&2
fi

# ---- 编译 ----
build_targets() {
  if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    printf '首次配置（cmake --preset dev）…\n'
    cmake --preset dev >/dev/null || {
      printf '配置失败\n' >&2
      return 1
    }
  fi

  local args=(--build --preset dev)
  if [[ $# -gt 0 ]]; then args+=(--target "$@"); fi
  cmake "${args[@]}"
}

if [[ "$NO_BUILD" != 1 ]]; then
  if ! build_targets "${TARGETS[@]}"; then
    printf '编译失败，已中止\n' >&2
    exit 1
  fi
fi

# ---- 决定要跑哪些 ----
to_run=()
if [[ ${#TARGETS[@]} -gt 0 ]]; then
  for t in "${TARGETS[@]}"; do
    if [[ ! -x "$DEMO_DIR/$t" ]]; then
      printf '找不到 demo: %s\n\n可用：\n' "$t" >&2
      discover_built | sed 's/^/  /' >&2
      exit 2
    fi
    to_run+=("$t")
  done
else
  # 全部：按已知顺序，其后追加未登记的
  for entry in "${KNOWN_DEMOS[@]}"; do
    name="${entry%%:*}"
    [[ -x "$DEMO_DIR/$name" ]] && to_run+=("$name")
  done
  while IFS= read -r extra; do
    [[ -z "$extra" ]] && continue
    found=0
    for n in "${to_run[@]}"; do [[ "$n" == "$extra" ]] && found=1; done
    [[ "$found" == 0 ]] && to_run+=("$extra")
  done < <(discover_built)
fi

if [[ ${#to_run[@]} -eq 0 ]]; then
  printf '没有可运行的 demo（是否未构建？先不带 -n 跑一次）\n' >&2
  exit 2
fi

# ---- 运行 ----
passed=0
failed=0
declare -a failed_names=()

if [[ -f .env ]]; then
  has_env="有"
else
  has_env="无"
fi
printf '运行 %d 个 demo（工作目录=%s，.env=%s）\n' "${#to_run[@]}" "$SCRIPT_DIR" "$has_env"

for name in "${to_run[@]}"; do
  printf '\n──────── %s —— %s ────────\n' "$name" "$(describe "$name")"
  start_ms=$(date +%s%3N)
  # agent_cli 缺省输入源是交互 REPL（会占住终端）——run-all 时给非交互参数，
  # 用样例文件批量跑 react 模式做冒烟。
  if [[ "$name" == "agent_cli" ]]; then
    "./$DEMO_DIR/$name" -m react -f examples/agent_cli/inputs/sample.txt --json >/dev/null
  else
    "./$DEMO_DIR/$name"
  fi
  code=$?
  end_ms=$(date +%s%3N)
  elapsed=$((end_ms - start_ms))
  if [[ $code -eq 0 ]]; then
    printf '✓ %s (%d ms)\n' "$name" "$elapsed"
    passed=$((passed + 1))
  else
    printf '✗ %s (exit=%d, %d ms)\n' "$name" "$code" "$elapsed"
    failed=$((failed + 1))
    failed_names+=("$name")
  fi
done

printf '\n──────────────── 汇总 ────────────────\n'
printf '通过 %d / 失败 %d（共 %d）\n' "$passed" "$failed" "${#to_run[@]}"
if [[ $failed -gt 0 ]]; then
  printf '失败项：%s\n' "${failed_names[*]}"
  exit 1
fi