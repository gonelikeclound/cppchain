#!/usr/bin/env bash
# run-tests-report.sh —— 生成 webui「测试」tab 的静态测试报告快照（方案 A）。
#
# 说明：
#   · 跑 8 个 gtest 测试二进制（--gtest_output=json）→ tools/make_tests_report.py 合并成
#     web/tests_report.json（前端直接 fetch，不碰 CGI）。
#   · 报告是快照：改了代码/跑了新测试要重跑本脚本才更新。
#   · 只需 dev 构建（tests ON）；GLIBCXX_3.4.35 需 GCC16 工具链（同 run-webui.sh）。
#
# 用法：
#   ./run-tests-report.sh        # 产 web/tests_report.json（测试无需各二进制全过才生成，失败点名也会进报告）
set -euo pipefail
cd "$(dirname "$0")"

BINS=(agent graph message model state tool webui workflow)
SRC=tmp/gtest_json

if [[ ! -x build/dev/tests/workflow_tests ]]; then
  echo "缺少 build/dev/tests/*_tests（先 cmake --preset dev && cmake --build build/dev -j）" >&2
  exit 2
fi
source ~/opt/gcc16/env.sh   # GLIBCXX_3.4.35（与 run-webui.sh 一致）

mkdir -p "$SRC"
rm -f "$SRC"/*.json   # 清掉旧产物：避免 gtest 对同名文件加后缀（_1.json）导致重复/错乱
for t in "${BINS[@]}"; do
  bin="build/dev/tests/${t}_tests"
  if [[ -x "$bin" ]]; then
    # || true：单个 binary 失败（如偶发环境问题）不中断整体；失败用例会带进报告
    "$bin" --gtest_output="json:$PWD/$SRC/" >/dev/null 2>&1 || true
  else
    echo "跳过缺失二进制: $bin" >&2
  fi
done

python3 tools/make_tests_report.py "$SRC" web/tests_report.json
echo "测试报告已生成: web/tests_report.json（可 ./run-webui.sh 起服后「测试」tab 查看）"