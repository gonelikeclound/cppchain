#!/usr/bin/env python3
# 【4/5 自进化接点·续跑观察】把长程任务档案 + 批记录导出成分析/训练 JSONL。
# 用法: python3 tools/export_task_life.py <workdir> [--out life.jsonl]
# 输出每行 = 一次批的"运行档案"：task/cursor/batch_no/last_summary/stepsN/日志尾部判定。
import json, glob, os, sys
ws = sys.argv[1]
out = 'life.jsonl'
if '--out' in sys.argv: out = sys.argv[sys.argv.index('--out')+1]
rows = []
for arch in glob.glob(os.path.join(ws, '.wfr/tasks/*.json')):
    if arch.endswith('.history/') : continue
    if '/tasks/' in arch and '.history/' in arch: continue
    try: a = json.load(open(arch))
    except Exception: continue
    rows.append({'kind':'task_archive','task':a.get('task'),'cursor':a.get('cursor'),
                 'batch_no':a.get('batch_no'),'last_summary':a.get('last_summary',''),
                 'steps':len(a.get('steps',[])),'created':a.get('created','')})
for sess in glob.glob(os.path.join(ws, '.wfr/sessions/*.log.jsonl')):
    plog=None
    for line in open(sess):
        if not line.strip(): continue
        try: r=json.loads(line)
        except: continue
        u=r.get('update',{})
        if isinstance(u,dict) and 'plan_log' in u: plog=u['plan_log']
    if plog:
        rows.append({'kind':'batch_verdicts','session':os.path.basename(sess),
                     'verdicts':[(e.get('step'),e.get('kind'),e.get('verdict')) for e in plog]})
with open(out,'w') as f:
    for r in rows:
        f.write(json.dumps(r,ensure_ascii=False)+'\n')
print(f"exported {len(rows)} rows -> {out}")
