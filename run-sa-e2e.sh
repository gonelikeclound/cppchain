#!/usr/bin/env bash
# run-sa-e2e.sh —— SA 编排台 WebUI 端到端测试（problems/64 · S6）
#
# 驱动**真实服务**（同 run-webui.sh 的 python http.server --cgi），覆盖三阶段：
#   A 静态 HTTP   ：/sa/ 页面渲染骨架 + sample-tasklog.json 契约
#   B CGI 运行整路：/cgi-bin/sa_cgi?scenario=all 真实触发 sa_cgi→sa_demo，
#                   断言 JSON 回执、tasklog/run_meta/run.log 磁盘效果、确定性数值、
#                   第二次递增（append 语义）、scenario=t1 的 v1 拒答
#   C 浏览器 DOM  ：headless Chrome（--dump-dom）× 页面内建 ?e2e=show/run 钩子：
#                   真实渲染左任务列表 + DAG；真实点击「运行 demo」→ CGI → 「运行完成」
#
# 隔离：跑真实 web/sa/（保证页面↔CGI 数据同源），测试前后的三个运行态文件快照还原。
# 用法：./run-sa-e2e.sh [--no-build] [--no-chrome]
set -uo pipefail

cd "$(dirname "$0")"

PASS=0; FAIL=0
ok(){ echo "  ✓ $1"; PASS=$((PASS+1)); }
bad(){ echo "  ✗ $1"; FAIL=$((FAIL+1)); }
chk(){  # chk <desc> <cmd...>
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}

SCRIPT_BIN="build/dev/examples/sa_cgi"
SCRIPT_DEMO="build/dev/examples/sa_demo"
SA_DIR="web/sa"
NO_BUILD=0; NO_CHROME=0
for a in "$@"; do
  case "$a" in
    --no-build)  NO_BUILD=1 ;;
    --no-chrome) NO_CHROME=1 ;;
    *) echo "未知参数: $a" >&2; exit 2 ;;
  esac
done

# ---- 工具链 + 构建 ----
if [[ -f "$HOME/opt/gcc16/env.sh" ]]; then
  set +u; source "$HOME/opt/gcc16/env.sh"; set -u
fi
if [[ "$NO_BUILD" == 0 || ! -x "$SCRIPT_BIN" || ! -x "$SCRIPT_DEMO" ]]; then
  echo "== 构建 sa_cgi / sa_demo / samock =="
  cmake --build --preset dev --target sa_cgi sa_demo samock || { echo "构建失败" >&2; exit 1; }
fi

# ---- 快照（测试期间改动，测试后还原）----
SNAP="$SA_DIR/.e2e_snapshot"
SRV_PID=""; CHROME_TMP=""
cleanup(){
  for f in sa_tasklog.json run_meta.json run.log sa_progress.jsonl; do
    rm -f "$SA_DIR/$f"
    if [[ -f "$SNAP/$f" ]]; then mv "$SNAP/$f" "$SA_DIR/$f"; fi
  done
  rmdir "$SNAP" 2>/dev/null || true
  [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null
  if [[ -n "$CHROME_TMP" ]]; then
    pkill -f "user-data-dir=$CHROME_TMP" 2>/dev/null
    rm -rf "$CHROME_TMP"
  fi
}
trap cleanup EXIT
mkdir -p "$SNAP"
for f in sa_tasklog.json run_meta.json run.log sa_progress.jsonl; do
  [[ -f "$SA_DIR/$f" ]] && cp -p "$SA_DIR/$f" "$SNAP/$f"
done

# ---- 起服（继承当前 env：gcc16 LD_LIBRARY_PATH → CGI 子进程可用）----
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("",0));print(s.getsockname()[1]);s.close()')"
python3 -m http.server --cgi "$PORT" --directory web >/dev/null 2>&1 &
SRV_PID=$!
BASE="http://127.0.0.1:$PORT"
started=0
for _ in $(seq 1 60); do
  if curl -s -o /dev/null "$BASE/sa/"; then started=1; break; fi
  sleep 0.25
done
if [[ "$started" == 0 ]]; then echo "服务器未就绪" >&2; exit 1; fi

echo ""
echo "==== A · 静态 HTTP ===="
html=$(curl -s "$BASE/sa/")
if [[ "$html" == *"SA 编排台"* && "$html" == *"任务列表"* && "$html" == *"运行 demo"* && "$html" == *"DAG"* && "$html" == *"源码"* ]]; then
  ok "页面 /sa/ 渲染骨架齐（SA 编排台 / 任务列表 / 运行 demo / DAG / 源码标签页）"
else
  bad "页面 /sa/ 骨架缺失"
fi
chk "/sa/sample-tasklog.json HTTP 200" curl -fs -o /dev/null "$BASE/sa/sample-tasklog.json"
if python3 - "$BASE" <<'PY'
import json, sys, urllib.request
j = json.load(urllib.request.urlopen(sys.argv[1] + "/sa/sample-tasklog.json", timeout=10))
assert j.get("schema") == "sa-tasklog/v1", j.get("schema")
ids = [t["id"] for t in j["tasks"]]
assert ids == ["t1", "t2"], ids
assert any(t["facts"].get("milestones", 0) > 0 for t in j["tasks"])
PY
then ok "sample-tasklog.json 契约（schema=sa-tasklog/v1，tasks=[t1,t2]）"
else bad "sample-tasklog.json 契约异常"; fi

echo ""
echo "==== B · CGI 运行整路 ===="
resp=$(curl -s "$BASE/cgi-bin/sa_cgi?scenario=all&pace=80")
if python3 - "$resp" <<'PY'
import json, sys
j = json.loads(sys.argv[1])
assert j.get("ok") is True, j
assert isinstance(j.get("elapsed_ms"), int) and j["elapsed_ms"] >= 0
assert j.get("pace_ms") == 80, j.get("pace_ms")
ids = j.get("task_ids", [])
assert "t1" in ids and "t2" in ids, ids
PY
then ok "scenario=all → ok=true，elapsed_ms≥0，task_ids 含 t1/t2"
else bad "scenario=all 回执异常：$resp"; fi

if python3 - "$SA_DIR" <<'PY'
import json, os, sys
d = sys.argv[1]
assert os.path.exists(d + "/sa_tasklog.json")
j = json.load(open(d + "/sa_tasklog.json"))
assert j["schema"] == "sa-tasklog/v1" and len(j["tasks"]) == 2, j.get("schema")
m = json.load(open(d + "/run_meta.json"))
assert m["total"] >= 1
assert os.path.exists(d + "/run.log")
PY
then ok "运行产物落盘：tasklog（2 任务）+ run_meta（total≥1）+ run.log"
else bad "运行产物缺失"; fi

chk "run.log 含「S1 + S2 + S3 + S4 全部通过」（执行真发生）" \
    grep -q "S1 + S2 + S3 + S4 全部通过" "$SA_DIR/run.log"

if python3 - "$SA_DIR" <<'PY'
import json, sys
evs = [json.loads(l) for l in open(sys.argv[1] + "/sa_progress.jsonl") if l.strip()]
assert evs[0]["t"] == "run" and evs[0]["state"] == "start", evs[0]
assert evs[-1]["t"] == "run" and evs[-1]["state"] == "done", evs[-1]
assert {"task", "dag", "node", "msg"} <= {e["t"] for e in evs}, {e["t"] for e in evs}
msgs = [e["msg"] for e in evs if e["t"] == "msg" and e.get("task") == "t1"]
assert any(m.get("node") == "n_call" for m in msgs), msgs[:2]
assert any(m.get("tool") == "samock_make_call" for m in msgs), msgs[:2]
assert any(m.get("kind") == "assert" for m in msgs), msgs[-2:]
nodes = [e for e in evs if e["t"] == "node" and e.get("task") == "t1"]
assert any(e["id"] == "n_call" and e["state"] == "active" for e in nodes), nodes[:2]
PY
then ok "S6c 实时进度：run/task/dag/node/msg 事件齐全（t1 电话例逐节点）"
else bad "S6c 进度文件缺失或事件不完整"; fi

if python3 - "$SA_DIR" <<'PY'
import json, sys
j = json.load(open(sys.argv[1] + "/sa_tasklog.json"))
t1 = next(t for t in j["tasks"] if t["id"] == "t1")
assert t1["facts"]["milestones"] == 7, t1["facts"]
assert t1["facts"]["clock_end_sec"] == 27, t1["facts"]
assert t1["facts"]["model_calls_core"] == 2, t1["facts"]
PY
then ok "确定性数值贯通：t1 facts.milestones==7 · clock_end_sec==27 · core==2"
else bad "确定性数值不符"; fi

resp2=$(curl -s "$BASE/cgi-bin/sa_cgi?scenario=all")
if python3 - "$resp" "$resp2" <<'PY'
import json, sys
a = json.loads(sys.argv[1]); b = json.loads(sys.argv[2])
assert b["ok"] and b["run_total"] == a["run_total"] + 1, (a["run_total"], b["run_total"])
PY
then ok "第二次运行 run_total 递增（run_meta append 语义）"
else bad "total 未递增"; fi

resp3=$(curl -s "$BASE/cgi-bin/sa_cgi?scenario=t1")
if python3 - "$resp3" <<'PY'
import json, sys
j = json.loads(sys.argv[1])
assert j.get("ok") is False and "v2" in j.get("error", ""), j
PY
then ok "scenario=t1 → v1 契约明确拒答（含 v2 提示）"
else bad "t1 拒答异常：$resp3"; fi

echo ""
echo "==== C · 浏览器 DOM（headless Chrome）===="
if [[ "$NO_CHROME" == 1 ]]; then
  echo "  -（--no-chrome 跳过）"
else
  CHROME_TMP="$(mktemp -d)"
  dump_dom(){
    timeout 60 google-chrome --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
      --user-data-dir="$CHROME_TMP" --virtual-time-budget=60000 --dump-dom "$1" 2>/dev/null
  }
  show_dom=$(dump_dom "$BASE/sa/?e2e=show")
  if [[ "$show_dom" == *'data-e2e-render="ok"'* ]]; then
    ok "headless Chrome · e2e=show：任务列表 + DAG 真实渲染（钩子 ok）"
  else
    bad "e2e=show 渲染钩子失败（data-e2e-render 非 ok）"
  fi
  run_dom=$(dump_dom "$BASE/sa/?e2e=run&pace=80")
  if [[ "$run_dom" == *'data-e2e-run="pass"'* ]]; then
    ok "headless Chrome · e2e=run：真实点击「运行 demo」→ CGI →「运行完成」全通（钩子 pass）"
  else
    m=$(printf '%s' "$run_dom" | grep -o 'data-e2e-run="[^"]*"' | head -1)
    bad "e2e=run 失败：${m:-未见钩子}"
  fi
fi

echo ""
echo "==== 汇总 ===="
if [[ -f "$SA_DIR/run.log" ]]; then
  echo "末次运行 verdict：$(grep -o 'S1 + S2 + S3 + S4 全部通过.*' "$SA_DIR/run.log" | tail -1)"
fi
echo "通过 $PASS / 失败 $FAIL"
if [[ "$FAIL" -eq 0 ]]; then echo "E2E 结论：SA 编排台 WebUI 端到端测试全部通过"; else echo "E2E 结论：存在失败项"; fi
exit "$FAIL"