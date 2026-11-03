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
if [[ "$html" == *"SA 编排台"* && "$html" == *"任务列表"* && "$html" == *"运行 demo"* && "$html" == *"DAG"* && "$html" == *"源码"* && "$html" == *"生成 DAG 并运行"* && "$html" == *"下载历史"* ]]; then
  ok "页面 /sa/ 渲染骨架齐（SA 编排台 / 任务列表 / 运行 demo / DAG / 源码 / 一句话 / 下载）"
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
assert j["schema"] == "sa-tasklog/v1", j.get("schema")
ids = {t["id"] for t in j["tasks"]}
assert {"t1", "t2"} <= ids, ids
m = json.load(open(d + "/run_meta.json"))
assert m["total"] >= 1
assert os.path.exists(d + "/run.log")
PY
then ok "运行产物落盘：tasklog（含 t1/t2）+ run_meta（total≥1）+ run.log"
else bad "运行产物缺失"; fi

chk "run.log 含「S1 + S2 + S3 + S4 全部通过」（执行真发生）" \
    grep -q "S1 + S2 + S3 + S4 全部通过" "$SA_DIR/run.log"

# S7：一句话 → DAG 的**离线**链路（fixture 跳过真机模型）：校验 → 解释器 → tasklog。
PLANTL="$SA_DIR/.e2e_plan_tasklog.json"
PLANPROG="$SA_DIR/.e2e_plan_progress.jsonl"
if "$SCRIPT_DEMO" --plan-json=tests/sa/fixtures/plan_t1.json --tasklog="$PLANTL" \
      --progress="$PLANPROG" --pace-ms=0 >/dev/null 2>&1 \
   && "$SCRIPT_DEMO" --plan-json=tests/sa/fixtures/plan_t1.json --tasklog="$PLANTL" \
      --pace-ms=0 >/dev/null 2>&1 \
   && python3 - "$PLANTL" <<'PY'
import json, sys
tasks = json.load(open(sys.argv[1]))["tasks"]
assert len(tasks) == 2, len(tasks)                       # 两次运行 → 历史保留两条
assert tasks[0]["id"] != tasks[1]["id"], tasks
for t in tasks:
    assert t["status"] == "Completed", t["status"]
    assert t["facts"]["milestones"] == 7 and t["facts"]["clock_end_sec"] == 27, t["facts"]
    assert len(t["messages"]) == 16, len(t["messages"])
    assert t["dag"]["nodes"][2]["layer"] == 2, t["dag"]["nodes"]
PY
then ok "S7 一句话计划（离线 fixture ×2）：校验 → 解释器 → 16 消息/7 里程碑/钟 27s + 历史保留"
else bad "S7 计划执行失败"; fi

# S7 fail-closed：坏计划（缺 world）必须拒跑，且不产出 tasklog。
BADPLAN="$SA_DIR/.e2e_bad_plan.json"
BADTL="$SA_DIR/.e2e_bad_tasklog.json"
python3 - "$BADPLAN" <<'PY'
import json, sys
p = json.load(open("tests/sa/fixtures/plan_t1.json"))
p["nodes"] = [n for n in p["nodes"] if n["role"] != "world"]
p["edges"] = [e for e in p["edges"] if e["to"] != "n_world"]
json.dump(p, open(sys.argv[1], "w"))
PY
if "$SCRIPT_DEMO" --plan-json="$BADPLAN" --tasklog="$BADTL" --pace-ms=0 >/dev/null 2>&1 \
   || [[ -f "$BADTL" ]]; then bad "S7 fail-closed：坏计划未被拒"; else ok "S7 fail-closed：坏计划（缺 world）拒跑且零产出"; fi
rm -f "$BADPLAN" "$BADTL"

# S8：历史删除必须持久（从 sa_tasklog.json 真删），且再删同一 id 要明确报错。
DELDIR="$(mktemp -d)"
cp "$PLANTL" "$DELDIR/sa_tasklog.json"
DELID="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["tasks"][0]["id"])' "$PLANTL")"
del_resp=$(SA_DATA_DIR="$DELDIR" QUERY_STRING="action=delete&id=$DELID" "$SCRIPT_BIN")
if printf '%s\n' "$del_resp" | tail -n 1 | python3 -c '
import json, sys
j = json.load(sys.stdin)
assert j.get("ok") is True and j.get("deleted") == sys.argv[1] and j.get("remaining") == 1, j
' "$DELID" && python3 - "$DELDIR" "$DELID" <<'PY'
import json, sys
tasks = json.load(open(sys.argv[1] + "/sa_tasklog.json"))["tasks"]
assert len(tasks) == 1 and all(t.get("id") != sys.argv[2] for t in tasks), tasks
PY
then ok "S8 历史删除：持久移除并原子落盘（剩余 1 条）"
else bad "S8 历史删除失败：$del_resp"; fi
del_resp2=$(SA_DATA_DIR="$DELDIR" QUERY_STRING="action=delete&id=$DELID" "$SCRIPT_BIN")
if printf '%s\n' "$del_resp2" | tail -n 1 | python3 -c '
import json, sys
assert json.load(sys.stdin).get("ok") is False
'; then ok "S8 删除不存在任务：明确报错（ok=false）"; else bad "S8 重复删除未报错"; fi
rm -rf "$DELDIR"
rm -f "$PLANTL" "$PLANPROG"

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