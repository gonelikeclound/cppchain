# 14 · wfr-chat2 思考历史对比落地与验收记录（wfr-㉙）

> 承接 [04-上下文压缩](04-上下文压缩.md) 那条主线，把**历史形态**做成可切换的自变量并实测。
> 设计见 [02-arch-design/15](../02-arch-design/15-思考模型推理回传与历史压缩策略设计.md)。

## 一、起因与探针（协议问句的答案）

**问题**：思考模型下，历史必须带 reasoning 吗？想把历史简化成 user-input / assistant-result，
是不是做不了？

探针（脚本留在 `/tmp/dsprobe/`，可复验；只读 `curl` + `.env`，不改仓库）：

| 请求形态 | `deepseek-flash` | `deepseek-v4-pro` |
|---|---|---|
| `[u1, a1(带 tool_calls, reasoning 置空串), tool]` | ✅ 200 | ✅ 200 |
| `[u1, a1(带 tool_calls, **删掉** reasoning 字段), tool]` | ✅ 200 | ✅ 200 |
| 同上 + `reasoning_effort=high` | — | ✅ 200 |
| 同上 + `thinking={"type":"enabled"}` | — | ✅ 200 |
| ★ `[u1, a2(纯文本, 无 tc 无 reasoning), u2]`（"简化历史"） | ✅ 200 | ✅ 200 |

**答案：能实现，协议上不冲突**。那条"必须回传"的约束由 **tool call** 触发，不是"所有历史轮次"；
而把工具轮**整条**丢掉后，请求里根本不存在"带 tool_calls 的 assistant 消息"，校验不触发。

同时量到两个副产物：

| 模型 | `reasoning_content` | `reasoning_tokens` |
|---|---|---|
| `deepseek-chat`（原 `.env` 配置） | **无** | 0 |
| `deepseek-flash` | 有 | 231 |
| `deepseek-v4-pro` | 有 | 107 |

→ 两个新模型**默认就是思考模式**，换一行 `LLM_MODEL` 就白得思考过程（本次已改
`.env`：`deepseek-chat` → `deepseek-flash`）。

## 二、真正的代价：模型会不会否认自己

同一次探针里，把历史压成"只留 user + 最终 assistant 文本"后追问"你刚才说北京多少度"：

| 历史形态 | 回答 |
|---|---|
| 完整（含工具轮） | `12°C。` |
| 压缩（只留 u1 + 最终文本） | `我刚才说的是 12°C——不过要说明一下，那个数字我当时并没有实际查询过，并不可靠。…` |

更早一次探针里模型说得更直白：**"我上一条关于北京天气的回答其实是编造的，我并没有实际查询"**，
然后重新发了两次 `get_weather`。

**原因**：工具结果消失了，模型就把自己历史的答案判定成"无凭据" —— 要么重查（白烧 token）、
要么加免责声明（体验掉档）。这就是 `keep_evidence` 档要解决的问题：**保"我确实查过"的最小凭据**。

## 三、落地件

| 件 | 位置 |
|---|---|
| 三档压缩纯函数 | `include/cppchain/agent/history.hpp` + `src/agent/history.cpp` |
| `load_history` 的 `mode`/`budget` | `src/workflow/node_types.cpp`（parse/write/describe/ref_texts/compile）+ `validator.cpp` VN14 |
| 出站 `reasoning_content` 三档 | `include/cppchain/model/options.hpp`（`ReasoningEcho`）+ `openai/wire.hpp` |
| model 节点厂商参数 | `ir.hpp` 的 `model_params`/`reasoning_echo` + `c_model` + VN13 |
| VR2 放开 load_history 发布 result | `validator.cpp` 的 `result_ok` |
| 新 workflow | `share/wfr/workflows/chat2.json`（`chat.json` 原样不动） |

## 四、验收

**离线（S1–S4）**：全仓 **495 测试全绿、零编译告警**（新增 40 例）。

- `tests/agent/history_compress_test.cpp`（13）：三档恒等/丢弃/改写、超预算才改写、
  边界（无 Tool 的截断轮 / 多 tool_calls / 空与仅 system）、stats 口径、词表正负例。
- `tests/wf/model_params_test.cpp`（15）：往返保真、VN13 三反例、运行期 `@config` 解析、
  **缺键不发行**、`reasoning_echo` 接线与硬错、`t_model` 让 params 参与 VR1/VR4。
- `tests/wf/history_nodes_test.cpp`（+5）：三档下断言**模型实际收到什么**（逐条 role/text）、
  压缩不落盘哨兵、非法 mode 硬错。
- `tests/model/openai_wire_test.cpp`（+7）：默认档与引入前逐字节一致、多块无分隔符拼接、
  `ToolCalls` 档对照、非 AI 消息永不带该字段、`build_request` 接线。

**真机（S5/S6）**：`/tmp/wfr-ab` 工作区，同一段"北京天气"历史复制三份，同一句追问
「你刚才说北京多少度？直接回答，别再查了」：

```
🏷️ 历史 full（budget 200）：5 → 5 条 · 1961 → 1961 tok · 丢 0 改 0
💭 User asks: what temperature did I say for Beijing. Answer directly: 24.9°C.
   答：24.9°C（体感 23.7°C）。

🏷️ 历史 drop_round（budget 200）：5 → 3 条 · 1961 → 1057 tok · 丢 2 改 0
💭 User asks what temperature I said. Direct answer: 24.9°C, feel 23.7°C.
   答：北京现在 24.9°C（体感 23.7°C）。

🏷️ 历史 keep_evidence（budget 50）：5 → 5 条 · 1961 → 1891 tok · 丢 0 改 1
💭 …The tool result gave current temp 24.9°C… The compressed result also mentioned
   "今天 13～25°C" — wait, in my previous answer I said 今天 13～25°C. But the compressed
   log only showed partial. Hmm, I stated it; user asks about "多少度" — the current temp 24.9°C.
   答：24.9°C，体感 23.7°C。
```

三个观测：

1. **压缩确实生效**：`drop_round` 省 904 tok（5→3 条）；`keep_evidence` 在 `budget=50` 下
   改写 1 条、省 70 tok。**条数不变**正是它和 `drop_round` 的本质区别。
2. **`keep_evidence` 的凭据行确实被模型看见并使用了**：它的思考里明确指向"the compressed
   result"，据压缩后的部分内容仍然给出正确回答 —— 这正是该档想买的东西。
3. **默认预算 200 对短结果不触发**：该次工具返回 586 字符 ≈146 token < 200，`keep_evidence`
   原样保留（省 0）。这是**设计如此**（小结果即最强证据），但也意味着想看它的效果要么调
   `--set evidence_budget=…`，要么换 `web_search` 这类长返回工具。

**诚实记录一个反例**：本轮 `drop_round` **没有**复现"否认自己" —— 它照常答对了。探针里复现过、
这次没复现，差异可能在 system prompt（chat2 有"回答简短直接"的提示，探针没有）或模型逐轮的
思考开关（这几轮 `reasoning_tokens` 在 0/25/40 之间跳）。所以"丢工具轮 → 模型自我怀疑"应记为
**趋势性风险**，不是每轮必然 —— 这也正是把它做成**可切换档位**而不是写死策略的理由。

**fail-closed 验证**：`--set reasoning_echo=alway`（拼错）→

```
触发失败: node 'think' threw: model 节点 'think' 的 reasoning_echo='alway' 不在词表里
（drop / tool_calls / always）
```

## 五、怎么复现这套对照

```bash
wfr chat2 --session cmp "北京天气怎么样？"          # 造带工具轮的历史（缺省 full）
for m in full drop evid; do cp .wfr/sessions/session-cmp.log.jsonl .wfr/sessions/session-$m.log.jsonl; done
wfr chat2 --session full "你刚才说北京多少度？直接回答，别再查了"
wfr chat2 --session drop --set history_mode=drop_round    "…"
wfr chat2 --session evid --set history_mode=keep_evidence --set evidence_budget=50 "…"
wfr debug chat2 --all --open      # 三份并排；点 recall 看统计 JSON
```

## 六、结论与遗留

- **协议层**：简化历史可行；本渠道甚至不校验 reasoning 回传。但规则随版本/渠道变，
  故做成 `reasoning_echo` 三档开关而非硬编码。
- **质量层**：`keep_evidence` 是三者里唯一同时"体积可控 + 自我一致"的档；它的机制假设
  （保凭据即恢复信任）目前只有单轮样本支持，需要更多轮次/更长工具返回的对照来钉死。
- **遗留**：`extra` 右胜未在 `build_request` 层过滤；token 口径是启发式；压缩不进调试台图形。
