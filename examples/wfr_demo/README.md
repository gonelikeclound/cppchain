# wfr_demo —— 演示 `wfr` 的离线工作目录

这个目录就是 **wfr 的演示工作目录**：`.wfr/` 托管 workflow 的全部数据
（正式 CLI 在 `src/wfr`，构建产物 `build/dev/bin/wfr`；本目录只放它的离线样例数据）。

```
.wfr/workflows/notify_wf.json   # B：单工具节点（send_notification）
.wfr/workflows/loop_wf.json     # A：import B（用其节点 id "n" 引用）+ label→goto 循环，无 trigger
.wfr/sessions/                  # 运行后的会话日志（唯一事实来源）
.wfr/debug/                     # wfr debug 生成物
.wfr/.gitignore                 # 忽略运行态、保留 workflows 源（demo 数据入库、运行态不入库）
```

在 `examples/wfr_demo` 下（用正式二进制；或仓库根 `./wfr.sh` 便捷封装）：

```bash
../../build/dev/bin/wfr list                    # 内置(chat) + 本目录 workflows
../../build/dev/bin/wfr debug loop_wf --all --open   # 单 HTML 调试台
./wfr.sh debug loop_wf --workdir examples/wfr_demo   # 同上（wfr.sh 便捷封装）
```

> ⚠️ **本目录的 tool 未注册进 wfr CLI —— 只能 `debug` 出图，不能真机跑**：
> `fetch_price` / `send_notification` 是 `examples/support/fakes.hpp` 的演示假工具
> （`fetch_price` 默认价 799 即假动作），正式 CLI（`src/wfr/main.cpp`）运行时注册表
> 只内置 `datetime` / `calculator`。所以直接跑 `loop_wf` 会被 IR 校验 VN6 拒绝
> （工具不在目录注册表）；`notify_wf` 更是纯 import 子链（引用父链 `poll` 节点），
> 不可单独运行。真机可跑请用内置 `chat`，或用 `datetime`/`calculator` 等已注册工具
> 写自己的 workflow。

`loop_wf`：`fetch_price` 默认价 799（不 < 500）→ else → `gp` goto 回 `l0` 循环 `max:3` 次终止；
日志可见 `gp` 的 `loop_counts` 1..3 —— 调试台「循环 1,2,3.. 分组」的演示素材。
改价走 then → 命中被 import 的 `notify_wf__n`，验证 import 边连通。
（`item_id` 已内置 P1，debug 开箱可出图；`--set product_id=X` 可演示 @config 注入。）
