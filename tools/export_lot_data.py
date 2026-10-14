#!/usr/bin/env python3
# 长程测试数据一键留档：把 tmp/pevcmp_b/ 各 ws 的任务档案 + plan_log 判定导出成
# docs/04-problems/data/lot/metrics.tsv（可提交的"读数留档"；原始会话仍在 tmp/）。
import glob, json, os, sys
OUT = os.path.join('docs', '04-problems', 'data', 'lot')
os.makedirs(OUT, exist_ok=True)
rows = []
wss = sorted(glob.glob('tmp/pevcmp_b/ws_*'))
for ws in wss:
    name = os.path.basename(ws)
    for arch in glob.glob(os.path.join(ws, '.wfr', 'tasks', '*.json')):
        if '.history' in arch or not arch.endswith('.json'):
            continue
        try: a = json.load(open(arch))
        except Exception: continue
        outf = os.path.join(ws, 'output', 'result.xlsx')
        product = os.path.exists(outf)
        nout = len(glob.glob(os.path.join(ws, '.out', '*.*')))
        # plan_log 尾部
        verdicts = []
        for sess in glob.glob(os.path.join(ws, '.wfr', 'sessions', '*.log.jsonl')):
            pl = None
            for line in open(sess):
                if not line.strip(): continue
                try: r = json.loads(line)
                except: continue
                u = r.get('update', {})
                if isinstance(u, dict) and 'plan_log' in u: pl = u['plan_log']
            if pl: verdicts = [(e.get('step'), e.get('kind'), e.get('verdict')) for e in pl]
        audit = any(v[2] == 'model' and v[0] == 'audit_prev' for v in verdicts) or any(v[0]=='audit_prev' for v in verdicts)
        rows.append({'ws': name, 'task': a.get('task',''), 'cursor': a.get('cursor',-1),
                     'batch': a.get('batch_no', -1), 'steps': len(a.get('steps',[])),
                     'product_xlsx': product, 'out_files': nout, 'audit': audit,
                     'last_summary': (a.get('last_summary') or '')[:40],
                     'verdicts_last': str(verdicts[-3:])[:120]})
header = list(rows[0].keys()) if rows else []
with open(os.path.join(OUT, 'metrics.tsv'), 'w') as f:
    if header: f.write('\t'.join(header) + '\n')
    for r in rows:
        f.write('\t'.join(str(r.get(k, '')) for k in header) + '\n')
print(f"wrote {len(rows)} rows -> {OUT}/metrics.tsv")
