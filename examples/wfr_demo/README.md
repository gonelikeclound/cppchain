# wfr_demo —— 演示 `wfr` 命令行工具（离线）

目录就是 `wfr` 的工作目录：`.wfr/` 托管 workflow 的全部数据。

```
.wfr/workflows/notify_wf.json   # B：单工具节点（send_notification）
.wfr/workflows/loop_wf.json     # A：import B（用其节点 id "n" 引用）+ label→goto 循环，无 trigger
.wfr/sessions/                  # wfr 运行后的会话日志（唯一事实来源）
.wfr/.gitignore                 # 忽略运行态、保留 workflows 源
```

跑法（在 `examples/wfr_demo` 下）：

```bash
wfr list                        # 列出 workflow + trigger 形态（manual）
wfr loop_wf --session s1 --reset "检查价格"
wfr debug loop_wf --all --open  # 可视化调试台（wfr-⑤）
```

`loop_wf`：`fetch_price` 默认价 799（不 < 500）→ `ck` 走 else → `gp` goto 回 `l0`，
`max:3` → 循环 3 次后 `_turn_end`。日志里能看到 `gp` 的 `loop_counts` 1..3 ——
正是调试台「循环按 1,2,3.. 分组」的演示素材。改价走 then → 命中被 import 的
`notify_wf__n`（send_notification 收到提醒），验证 import 边连通。

（`item_id` 已内置为 `P1`，离线开箱可跑；`--set product_id=X` 可演示 @config 注入——
想展示的话把 `poll.parameters` 改回 `"item_id": "@config.product_id"` 即可。）
