#!/usr/bin/env bash
# wfr.sh —— wfr 便捷封装：默认在仓库 tmp/ 的临时工作区里跑 workflow
#
# 用法（透传 wfr 全部子命令与参数，首参必须是子命令或 workflow 名）：
#   ./wfr.sh list                     # 列出各来源目录的 workflow
#   ./wfr.sh debug loop_wf --open     # 生成可视化调试台（默认落 tmp 工作区）
#   ./wfr.sh chat 你好                # 跑 workflow（多轮续聊；需 .env 里 LLM key）
#   ./wfr.sh chat --workdir my-ws 你好   # 显式指定工作区（此时不注入默认 tmp）
#
# 约定：
#   · 默认工作区 = <仓库>/tmp/wfr —— 会话日志 / checkpoint / debug 产物都落在
#     tmp/（已被 .gitignore 排除，不上 git）。可用 WFR_WS 环境变量改默认工作区；
#     命令行里自带 --workdir 时原样透传，不注入默认值。
#   · 每次运行自动（重）生成 <ws>/.wfr/settings.json，workflow_dirs 指向仓库内
#     各 workflow 目录（绝对路径）：.wfr/workflows、share/wfr/workflows、
#     examples/wfr_demo/.wfr/workflows。
#   · 二进制优先 build/release/bin/wfr（静态运行时，免本机 GCC 环境）——
#     **改过 IR / 内置 workflow 后记得 `cmake --build --preset release`**，
#     否则这里跑的还是上一次的产物（dev 构建不会自动同步过去）；
#     否则 dev 版（需 source ~/opt/gcc16/env.sh，未构建时自动 cmake 配置+编译）。
#   · import 按工作区的 .wfr/workflows 解析：demo workflow（loop_wf→notify_wf）带
#     跨目录 sibling import，请在 examples/wfr_demo 下跑或 --workdir 指定其根目录。
#   · wfr_demo 的 tool（fetch_price / send_notification）是 examples 演示假工具，未注册
#     进 CLI（仅 datetime / calculator）——loop_wf / notify_wf 只能 debug 出图，不能真机跑
#     （IR 校验 VN6 拒绝）；notify_wf 更是 loop_wf 的 import 子链，不可单独运行。真机用内置 chat。
#   · cd 到仓库根运行：wfr 内置 dotenv 按 .env 相对路径加载，LLM 配置才找得到。
#
# 环境变量：WFR_BIN（指定二进制）、WFR_WS（默认工作区）、GCC16_ENV（工具链 env）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GCC_ENV="${GCC16_ENV:-$HOME/opt/gcc16/env.sh}"

# ---- 选取二进制：发布版（静态链接，免环境）→ dev 版（需 GCC16，缺失则自动构建）----
WFR_BIN="${WFR_BIN:-}"
if [[ -z "$WFR_BIN" ]]; then
  if [[ -x "build/release/bin/wfr" ]]; then
    WFR_BIN="build/release/bin/wfr"
  else
    if [[ -f "$GCC_ENV" ]]; then
      set +u
      # shellcheck disable=SC1090
      source "$GCC_ENV"
      set -u
    else
      printf '提示：未找到 %s —— 将尝试直接用系统 g++/libstdc++，可能失败\n' "$GCC_ENV" >&2
    fi
    if [[ ! -f "build/dev/CMakeCache.txt" ]]; then
      printf '首次配置（cmake --preset dev）…\n'
      cmake --preset dev >/dev/null || { printf 'cmake 配置失败\n' >&2; exit 1; }
    fi
    cmake --build --preset dev --target wfr >/dev/null || { printf '编译 wfr 失败\n' >&2; exit 1; }
    WFR_BIN="build/dev/bin/wfr"
  fi
fi
if [[ "$WFR_BIN" != /* ]]; then WFR_BIN="$SCRIPT_DIR/$WFR_BIN"; fi
if [[ ! -x "$WFR_BIN" ]]; then
  printf '找不到 wfr 二进制：%s\n（可先构建，或设 WFR_BIN 指定路径）\n' "$WFR_BIN" >&2
  exit 1
fi

# ---- 默认工作区 = 仓库 tmp/wfr（临时，不上 git）；命令行自带 --workdir 则跳过注入 ----
WS="${WFR_WS:-$SCRIPT_DIR/tmp/wfr}"

has_own_workdir=0
for a in "$@"; do
  if [[ "$a" == "--workdir" ]]; then has_own_workdir=1; break; fi
done
if [[ $has_own_workdir -eq 1 ]]; then
  exec "$WFR_BIN" "$@"
fi

mkdir -p "$WS/.wfr"

# ---- 配好 settings：workflow_dirs = 仓库内各 workflow 目录（绝对路径，顺序即优先级）----
SETTINGS_FILE="$WS/.wfr/settings.json"
read -r -d '' SETTINGS_JSON <<EOF || true
{
  "workflow_dirs": [
    "$SCRIPT_DIR/.wfr/workflows",
    "$SCRIPT_DIR/share/wfr/workflows",
    "$SCRIPT_DIR/examples/wfr_demo/.wfr/workflows"
  ]
}
EOF
if [[ ! -f "$SETTINGS_FILE" ]] || ! cmp -s - "$SETTINGS_FILE" <<<"$SETTINGS_JSON"; then
  printf '%s\n' "$SETTINGS_JSON" > "$SETTINGS_FILE"
  printf '已生成 settings: %s\n' "$SETTINGS_FILE"
fi

# ---- 组装命令行：把 --workdir <WS> 插在首参（子命令/工作流名）之后 ----
first="${1:-}"
case "$first" in
  ""|-h|--help)
    exec "$WFR_BIN" "$@"
    ;;
  debug)
    if [[ $# -lt 2 ]]; then exec "$WFR_BIN" "$@"; fi  # 缺 workflow 名 → 交给 wfr 报用法
    set -- "${@:2}"
    exec "$WFR_BIN" "$first" "$1" --workdir "$WS" "${@:2}"
    ;;
  list|*)
    set -- "${@:2}"
    exec "$WFR_BIN" "$first" --workdir "$WS" "$@"
    ;;
esac