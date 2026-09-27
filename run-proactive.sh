#!/usr/bin/env bash
# run-proactive.sh —— 单独跑 proactive case 的脚本
# 默认：build 后批量跑 12 例评测固件矩阵（--cases + 内置默认澄清规则）。
# 传参：原样透传给 examples/proactive 二进制，可自定义意图/规则/模式。
#
# 用法：
#   ./run-proactive.sh                     # 12 例矩阵（默认规则，跑全部）
#   ./run-proactive.sh --cases --question-answer "几点" 1 ...
#                                          # 自定义澄清规则再跑矩阵
#   ./run-proactive.sh --intent "早上提醒我看新闻" --question-answer "几点|时间" 1
#                                          # 单条批量
#   ./run-proactive.sh --interactive       # 交互 REPL（ask_user 菜单澄清）
#   ./run-proactive.sh --help              # 显示二进制帮助
#   GCC16_ENV=/path/env.sh ./run-proactive.sh   # 指定工具链环境
#
# 退出码与二进制一致：--cases 有未过用例 → 1，全过 → 0。
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"   # .env 相对路径按仓库根解析

DEMO="build/dev/examples/proactive"
GCC_ENV="${GCC16_ENV:-$HOME/opt/gcc16/env.sh}"

# ---- 工具链环境（编译与运行都需要；同 run-demo.sh）----
if [[ -f "$GCC_ENV" ]]; then
  set +u
  # shellcheck disable=SC1090
  source "$GCC_ENV"
  set -u
else
  printf '提示：未找到 %s —— 编译会找不到 g++-16，运行也可能缺 libstdc++ 新符号\n' "$GCC_ENV" >&2
fi

# ---- 编译（增量，快）----
if [[ ! -f "build/dev/CMakeCache.txt" ]]; then
  printf '首次配置（cmake --preset dev）…\n'
  cmake --preset dev >/dev/null || { printf '配置失败\n' >&2; exit 1; }
fi
cmake --build --preset dev --target proactive || { printf '编译失败，已中止\n' >&2; exit 1; }

if [[ ! -x "$DEMO" ]]; then
  printf '构建后仍找不到 %s\n' "$DEMO" >&2
  exit 1
fi

# ---- 无参 → 默认 12 例矩阵；--interactive → REPL；否则原样透传 ----
if [[ $# -eq 0 ]]; then
  exec ./build/dev/examples/proactive --cases \
    --question-answer "几点|时间|频率|时候|星期|周几|哪一天" 1 \
    --question-answer "内容|提醒|推荐|关键词" 1
fi
if [[ "$1" == "--interactive" ]]; then
  exec ./build/dev/examples/proactive
fi
exec ./build/dev/examples/proactive "$@"