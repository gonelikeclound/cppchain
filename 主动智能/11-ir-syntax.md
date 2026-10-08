# Workflow 语法格式

Workflow 以一段顶层 JSON 输出。本篇是它的完整语法：结构、字段、文法、约束与示例。
trigger 一节覆盖**全部触发源**及其参数格式。

---

## 1. 顶层结构

```jsonc
{
  "id": "wf_xxx",          // workflow 标识
  "trigger": { ... },      // 触发：只表达"何时醒"
  "nodes": [ ... ]         // 有序节点数组
}
```

三个键齐全即一个合法 Workflow；`nodes` 不得省略任何节点。

## 2. trigger（触发）

### 2.1 通用结构

```jsonc
{
  "trigger": {
    "source": "time",           // 必填：触发源（见 2.3 总览）
    "kind": "schedule",         // 必填：源内形态
    "args": { "schedule": "0 8,20 * * *" },  // 必填：该 source+kind 的参数（结构由二者决定）

    // ── 通用可选字段 ──
    "enabled": true,                  // 默认 true
    "tolerance_sec": 1800,            // 可接受的延迟（秒）
    "provider_preference": [ "geofence_provider", "proximity_alert" ],  // 降级链，按序尝试
    "requires_privilege": "L2a",      // 所需能力档；定义期与设备能力比对

    // ── 过滤层：与 args 同级（匹配/判定进 filter，args 只放通道与配额参数）──
    "filter": {
      "expr": ["contains", {"field": "attrs.title"}, "已到达"],
      "depends_on": ["attrs.title"],
      "evalMode": "STRICT_3V"
    }
  }
}
```

两条铁律：
- **`source` 必须与归一化事件的 `source` 一一对应**（`time` 事件只能配 `time` 源，其余类推）。
- **`args` 只放"通道与配额相关参数"；一切匹配/判定放进独立的 `filter`**，不混进 `args`。

### 2.2 当前产出的形态

目前只产出 `time` 源，用下述简写（`kind` 提到 `timing` 下、`schedule` 直接放 `args` 的位置）：

```jsonc
{ "source": "time", "timing": { "kind": "schedule", "schedule": "0 8,20 * * *" } }
```

与通用结构等价（`timezone` 由宿主/系统时区补充，不写进 workflow）：

```jsonc
{ "source": "time", "kind": "schedule", "args": { "schedule": "0 8,20 * * *" } }
```

- `trigger` **不得装业务条件**（不允许 `condition`）：价格高低、库存有无等一律下沉到 `expr` 节点。
- 能编进调度的约束就编进调度（"避开深夜"用 `0 8,20 * * *` 表达，不产生分支）。

### 2.3 触发源总览

| # | `source` | 语义 | 形态（kind） |
|---|---|---|---|
| 1 | `time` | 时刻 / 周期 / 日历 | `at` `schedule` `interval` `calendar` |
| 2 | `location` | 进入 / 离开 / 停留 | `enter` `exit` `dwell` |
| 3 | `environment` | Wi-Fi / 蓝牙 / NFC / USB / 近场 | `wifi_connected` `wifi_disconnected` `bt_connected` `bt_disconnected` `nfc_transaction` `usb_attached` `usb_detached` |
| 4 | `content` | 通知 / 短信 / 使用 | `notification_posted` `notification_removed` `sms_received` `usage_event` |
| 5 | `system` | 锁屏 / 电源 / 网络 / 开机 / Doze / 前后台 | `keyguard_unlocked` `screen_on` `screen_off` `power_connected` `power_disconnected` `battery_below` `network_available` `network_lost` `boot_completed` `package_replaced` `doze_enter` `doze_exit` `time_changed` `foreground_app_changed` |
| 6 | `push` | 外部推送（FCM / 厂商 / 长连接） | `fcm` `oem_push` `longconn` `unifiedpush` |
| 7 | `user` | 用户显式发起 | `widget_click` `tile_click` `shortcut` `assistant_invoke` `appfunction_invoke` |
| 8 | `dnd` | DND 状态 / 规则状态变化 | `policy_changed` `rule_status_changed` |
| 9 | `inference`（预留） | 模型判定"此刻是否值得介入" | `model_decision`（未实现） |

### 2.4 `time`（时间）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `at` | `at` | string | ✅ | ISO 8601 本地时间（含偏移），如 `2026-09-22T08:00:00+08:00`；一次性 |
| | `timezone` | string | ✅ | IANA 时区，如 `Asia/Shanghai` |
| `schedule` | `schedule` | string | ✅ | 5 字段 cron：`分 时 日 月 周`，如 `0 8,20 * * *` |
| | `timezone` | string | ✅ | 同上 |
| `interval` | `interval_sec` | int | ✅ | 固定间隔，≥ 900 |
| | `start_after_sec` | int | ❌ | 首次延迟，默认 0 |
| `calendar` | `calendar_id` | string | ❌ | 默认全部日历 |
| | `title_contains` | string | ❌ | 事件标题包含（日历侧过滤） |
| | `offset_sec` | int | ✅ | 相对事件时刻偏移，负值 = 提前（如 `-1800` = 提前 30 分钟） |
| | `require_background_flag` | bool | ❌ | 默认 true |

**示例**
```jsonc
{ "source": "time", "kind": "schedule", "args": { "schedule": "0 8,20 * * *", "timezone": "Asia/Shanghai" } }
{ "source": "time", "kind": "calendar", "args": { "title_contains": "评审", "offset_sec": -1800 } }
```

**约束**：`time` 必须带 `timezone`；cron 只接受 5 字段（拒绝秒字段、`@daily` 等宏、`L`/`W`/`#`）；`interval_sec ≥ 900`；声明 `schedule`/`at` 时若 `tolerance_sec < 60` 会被拒（平台给不了秒级精确）。

### 2.5 `location`（地理）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `enter` / `exit` / `dwell` | `lat` | double | ✅ | 纬度 |
| | `lng` | double | ✅ | 经度 |
| | `radius_m` | int | ✅ | 半径，建议 ≥ 100 |
| | `dwell_sec` | int | `dwell` 必填 | 停留多久算命中，≥ 300 |
| | `exit_source` | enum | `exit` 必填 | 只能 `cdm`（`PendingIntent` 拿不到 `onLost`） |
| | `label` | string | ❌ | 用户可读标签（"公司"/"家"） |

**示例**
```jsonc
{ "source": "location", "kind": "enter",
  "args": { "lat": 31.2304, "lng": 121.4737, "radius_m": 200, "label": "公司" },
  "provider_preference": ["geofence_provider", "proximity_alert"] }
```

**约束**：`exit` 必须声明 `exit_source: "cdm"`；`provider_preference` 非空且每条在目标设备可得；禁止"持续轨迹"类声明。

### 2.6 `environment`（环境与设备）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `wifi_connected` / `wifi_disconnected` | `ssid` | string | 🟡 | 二者至少一个 |
| | `bssid_set` | string[] | 🟡 | 更稳（SSID 可重复）；不得含假 BSSID `02:00:00:00:00:00` |
| | `require_location_permission` | bool | ❌ | 默认 true（BSSID 需位置权限） |
| `bt_connected` / `bt_disconnected` | `mac` | string | 🟡 | 二者至少一个 |
| | `cod` | int | ❌ | Class of Device，如 `0x0420`（AUDIO_VIDEO_CAR_AUDIO） |
| | `cod_mask` | int | ❌ | 与 `cod` 配对，推荐 `0x1FFC`（major+minor 精确）/ `0x1F00`（放宽到 AUDIO_VIDEO 大类） |
| | `require_acl` | bool | ❌ | 默认 true：CoD 不可单独判定 |
| `nfc_transaction` | `aid` | string | ❌ | 指定 AID |
| | `require_screen_unlocked` | bool | ❌ | 默认 true |
| `usb_attached` / `usb_detached` | `vid` / `pid` | string | ❌ | USB 设备标识 |

**示例**
```jsonc
{ "source": "environment", "kind": "bt_connected",
  "args": { "mac": "AA:BB:CC:DD:EE:FF", "cod": 1056, "cod_mask": 8188, "require_acl": true } }
```

**约束**：`wifi_*` 必须有 `ssid` 或 `bssid_set`；`bt_*` 必须有 `mac` 或 `cod`；`cod` 与 `cod_mask` 同时出现或同时缺席；`cod` 存在时 `require_acl` 必须为 true。

### 2.7 `content`（内容）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `notification_posted` / `notification_removed` | `pkg_filter` | string[] | ❌ | 索引提示；空 = 全部包 |
| | `channel_filter` | string[] | ❌ | 频道 id 过滤 |
| | `exclude_ongoing` | bool | ❌ | 默认 true（排除常驻通知） |
| `sms_received` | `sender_allowlist` | string[] | ✅ | 发送方白名单，必填 |
| | `require_signature` | bool | ✅ | 默认 true（四道鉴权之一） |
| `usage_event` | `pkg` | string | ✅ | 需 `PACKAGE_USAGE_STATS` 特殊授权 |
| | `event` | enum | ✅ | `foreground` / `background` |
| | `min_duration_sec` | int | ❌ | 最短停留 |

**示例**
```jsonc
{ "source": "content", "kind": "notification_posted",
  "args": { "pkg_filter": ["com.taobao.taobao"], "exclude_ongoing": true },
  "filter": { "expr": ["contains", {"field": "attrs.title"}, "已到达"],
              "depends_on": ["attrs.title"], "evalMode": "STRICT_3V" } }
```

**约束**：`args` 里不得出现 `title_contains`/`text_contains`/正则（必须改到 `filter`）；`sms_received` 必须有非空 `sender_allowlist`。

### 2.8 `system`（系统与应用行为）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `keyguard_unlocked` | — | — | — | 用户解锁 |
| `screen_on` / `screen_off` | — | — | — | 仅系统级/前台可见 |
| `power_connected` / `power_disconnected` | — | — | — | 仅系统级/前台可见 |
| `battery_below` | `threshold_pct` | int | ✅ | 0–100 |
| `network_available` / `network_lost` | `transport` | enum | ❌ | `wifi` / `cellular` / `any` |
| `boot_completed` | `locked_boot` | bool | ❌ | 默认 false |
| `package_replaced` | — | — | — | 应用更新 |
| `doze_enter` / `doze_exit` | — | — | — | 设备空闲进出 |
| `time_changed` | — | — | — | 时间 / 时区变更 |
| `foreground_app_changed` | `pkg_filter` | string[] | ❌ | 系统级独占 |

**示例**
```jsonc
{ "source": "system", "kind": "network_available", "args": { "transport": "wifi" } }
{ "source": "system", "kind": "battery_below", "args": { "threshold_pct": 15 } }
```

**约束**：`battery_below` 的 `threshold_pct ∈ [0,100]`；禁止 `input_activity` 类声明（隐私红线）。

### 2.9 `push`（外部推送）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `fcm` | `topic` | string | ❌ | 订阅主题；国内不可声明（无 GMS） |
| | `priority_hint` | enum | ❌ | `high` / `normal`；`high` 绑"必须产生可见通知" |
| `oem_push` | `channel` | enum | ✅ | `xiaomi` / `huawei` / `oppo` / `vivo` / `honor` |
| `longconn` | — | — | — | 自建长连接 |
| `unifiedpush` | `distributor` | string | ❌ | 分发器标识 |

**示例**
```jsonc
{ "source": "push", "kind": "oem_push", "args": { "channel": "xiaomi" },
  "filter": { "expr": ["eq", {"field": "attrs.biz"}, "restock"],
              "depends_on": ["attrs.biz"], "evalMode": "STRICT_3V" } }
```

**约束**：`push` 不得作为"精确时机"声明——必须同时声明 `tolerance_sec ≥ 60`；`fcm` 在国内为错误声明。

### 2.10 `user`（用户显式）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `widget_click` | `widget_id` | string | ✅ | 桌面组件 |
| `tile_click` | `tile_id` | string | ✅ | 快捷设置磁贴 |
| `shortcut` | `shortcut_id` | string | ✅ | 快捷方式 |
| `assistant_invoke` | — | — | — | 需 `ROLE_ASSISTANT`（设备唯一） |
| `appfunction_invoke` | `function_id` | string | ✅ | 动作面，非触发面 |

**示例**
```jsonc
{ "source": "user", "kind": "shortcut", "args": { "shortcut_id": "check_shipment" } }
```

**约束**：`user` 类不算"主动"触发源（用户显式发起），定义期打 `is_proactive: false`。

### 2.11 `dnd`（DND 状态）

| `kind` | 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `policy_changed` | `interruption_filter` | enum | ❌ | `none` / `priority` / `alarms` / `all`（不能叫 `filter`，会撞顶层字段） |
| | `require_dnd_access` | bool | ✅ | 默认 true：监听变化需 DND 授权 |
| `rule_status_changed` | `rule_id` | string | ❌ | 只看某条规则；空 = 全部 |

**示例**
```jsonc
{ "source": "dnd", "kind": "policy_changed",
  "args": { "interruption_filter": "none", "require_dnd_access": true },
  "requires_privilege": "L1" }
```

**约束**：读取路径必须二选一且显式声明——`poll`（轮询读状态，零权限）或 `broadcast`（需 DND 授权，必须探测到授权否则静默失效）。

### 2.12 `inference`（推断，预留不实现）

| `kind` | 参数 | 必填 | 说明 |
|---|---|---|---|
| `model_decision` | `model_id` / `min_confidence` | ✅ | 系统级独占（第三方无接口）；首版校验一律 ERROR |

### 2.13 通用可选字段速查

| 字段 | 类型 | 说明 |
|---|---|---|
| `enabled` | bool | 默认 true |
| `tolerance_sec` | int | 可接受的延迟秒数（映射投递策略） |
| `provider_preference` | string[] | 降级链，按序尝试，定义期逐个探测 |
| `requires_privilege` | string | 所需能力档（L1/L2a/L2b/L3），定义期与设备能力比对 |
| `budget_hint` | object | 配额提示，非权威，如 `{ "max_wakes_per_day": 24 }` |
| `filter` | object | 匹配/判定层（`expr` + `depends_on` + `evalMode`），与 `args` 同级 |

## 3. 节点类型

`nodes` 数组里每个元素是一个节点对象。

| 类型 | 判定键 | 用途 |
|---|---|---|
| `tool` | `tool` 键（或 `kind:"tool"`） | 调用一个目录内工具，产物进 `results[<id>]` |
| `expr` | `expr` 键（或 `kind:"expr"`） | 条件判定，二选一出口 |
| `loop` | `kind:"loop"` | **循环容器**：自带 `body` 子树 + 有界计数（wfr-㉚ 取代 label/goto） |
| `model` | `kind:"model"` | **一次** LLM 调用（M12） |
| `route` | `kind:"route"` | match 分派：按判别式选下一跳（M12） |
| `run_tools` | `kind:"run_tools"` | 执行最后一条 AI 消息的 tool_calls（M12） |

**判定规则（fail-closed，不是"第一个命中即定"）**：

- `kind` 是**通用**判别键，上表七种都可用它显式声明；`kind` 的值不在表内 → **编译期拒绝**。
- 也支持靠键判别（不带 `kind`）：`expr` / `tool`。**`label`/`goto` 已废弃**——写了会在 parse 期被拒，报错会提示改用 `kind:"loop"` + `body`。
- **歧义即错**：与 `kind` **冲突**的键、或多个判别键并存 → 拒绝。
  （与声明**一致**的冗余键不算冲突，如 `{"kind":"tool","tool":...}` 合法。）

入口是隐式的（无需写任何节点）；终止由两个**保留目标名**表达（**只在 `next`/`then`/`else`/`cases`/`default` 里写，不写出节点**）：

| 保留名 | 语义 |
|---|---|
| `_turn_end` | 正常终止本次触发 |
| `_fail` | **以错误终止**本次触发（fail-closed；错误信息带 IR id + 节点 id，细节见 `state.messages`） |
| `_continue` | **仅 loop 的 body 内**：进入下一轮（回 loop 节点头） |
| `_break` | **仅 loop 的 body 内**：跳出循环，去 loop 的 `next` |

## 4. 各类型节点的字段

**tool 节点（必填 + 可选）**

```jsonc
{
  "id": "fetch_mcp",
  "tool": "fetch_price",                          // 必须与目录内工具同名
  "parameters": { "item_id": "@config.product_id" }, // 只放该工具的 input 字段
  "output_schema": { "price": "number", "in_stock": "boolean" }, // 声明的字段即下游可引用集
  "next": "valid_price",                          // 必填；成功出口
  "on_error": { "retry": 2, "fallback": "fetch_web" }  // 可选；失败重试与降级
}
```

- `on_error`：`retry` 为整数重试次数；`fallback` 为降级目标节点 id。降级路径不在主链上（成功出口走 `next`，失败才走 `fallback`）。

**expr 节点**

```jsonc
{ "id": "valid_price", "expr": "$_.result.price != null && $_.result.price > 0", "then": "check_drop", "else": "_turn_end" }
```

- `expr` 是受限文法（见 §6）；`then`/`else` 必填，各指向一个节点 id（可为 `_turn_end`）。

**loop 节点（wfr-㉚）**

```jsonc
{ "id": "poll", "kind": "loop", "max": 4, "next": "notify", "on_max": "_turn_end",
  "body": [
    { "id": "fetch_st", "tool": "fetch_price", "parameters": { "item_id": "@config.product_id" },
      "output_schema": { "price": "number", "in_stock": "boolean" }, "next": "check_st" },
    { "id": "check_st", "expr": "$fetch_st.result.in_stock == true",
      "then": "_break", "else": "_continue" }
  ] }
```

- `body` 是**子树**（循环体），不得为空、**不得再含 loop**（不支持嵌套）。
- `max` 是 body 的最大执行次数（正整数）。
- `next` 是**正常出口**（`_break` 去这里）；`on_max` 是**跑满 max 仍没 break** 时的落点，
  缺省 `_turn_end`。两者可以不同（"查到了就通知" vs "查满次数还没到货"）。
- body 内的节点指针：同 body 节点 id / `_continue` / `_break` / `_turn_end` / `_fail`；
  **不得指向 body 外的普通节点**（那是漏写 `_break`）。
- `load_history` 不得放进 body（每轮 replay 会让 messages 反复膨胀）。

**model 节点（M12）**

```jsonc
{
  "id": "think",
  "kind": "model",
  "model": "main",              // 必填：**模型组件名**（宿主注册，不是端点模型 id）
  "tools": "research_toolset",  // 可选：**工具集组件名**（宿主注册；空 = 不带工具）
  "system_prompt": "…",         // 可选：本节点追加的 system 提示
  "next": "route"               // 必填
}
```

- **只调用一次模型**。多轮往返由 `route` + `run_tools` + 回边构成，**不在本节点内循环** ——
  这样每一步都在图上可见、都由引擎落快照（可观测、可恢复）。
- 回复以"新增消息"形式追加进 `messages` 通道（`add_messages` reducer）。
- `model` / `tools` 组件名必须在宿主注册表里，否则**校验期**拒绝（VN10，与 §8 对工具名的做法一致）。

**route 节点（M12）**

```jsonc
{
  "id": "route", "kind": "route",
  "on": "last_ai_outcome",         // 判别式：封闭词表，当前只有这一个
  "cases": {
    "tool_calls":         "act",     // 最后一条 AI 消息有**合法** tool_calls
    "invalid_tool_calls": "_fail",   // 有**非法** tool_calls（参数解析失败 / 缺 name）
    "text":               "_turn_end"// 纯文本回复
  },
  "default": "_fail"               // 未命中目标；**必填**（见下）
}
```

- `on` 与 `cases` 的键都是**封闭词表**（outcome 只有 `tool_calls` / `invalid_tool_calls` / `text`），
  未知值编译期拒绝。
- **`cases` 必须覆盖全部 outcome，或显式给出 `default`** —— 不允许"漏一档就静默走某分支"。
- 状态里没有 AI 消息时 outcome 为空串，必然落到 `default`（fail-closed 兜底）。
- **判别式用 cppchain 归一化过的 outcome，不用端点给的 `finish_reason` 字符串** ——
  后者取值随端点而异（实测差异大），拿它做路由会把端点差异漏进工作流。

**run_tools 节点（M12）**

```jsonc
{ "id": "act", "kind": "run_tools", "next": "loop" }
```

- 取**最后一条 AI 消息**的 `tool_calls` 执行，产出的 `ToolMessage` 追加进 `messages`（协议闭环）。
- 若最后一条 AI 消息没有 `tool_calls`，运行期报错（被编排到这条路径本身就是错）。

**M12：agent 循环的规范写法（ReAct）**

```jsonc
{ "nodes": [
  { "id": "loop", "kind": "loop", "max": 8, "next": "_turn_end",
    "body": [
      { "id": "think", "kind": "model", "model": "main", "tools": "ts", "next": "route" },
      { "id": "route", "kind": "route", "on": "last_ai_outcome",
        "cases": { "tool_calls": "act", "invalid_tool_calls": "_fail", "text": "_turn_end" },
        "default": "_fail" },
      { "id": "act",   "kind": "run_tools", "next": "_continue" }
    ] }
]}
```

`think → route → act →（_continue 回节点头）think` 就是**循环本身**；`max` 是循环上界
（等价于 langgraph 的 `recursion_limit`，但写在 IR 里）。`act` 的 `next` 写 `_continue`
而不是直接指回 `think` —— 后者会被判为前向环（见 §7 的链约束）。

## 5. 数据流与参数引用

- `$_.result.<字段>` —— 取 **最近一步**节点的输出。`$_` 是"最近节点"的占位，**不是节点 id**：它以 `_` 开头，不可能与任何合法节点 id 撞名（§7 节点 id 不得以 `_` 开头），因此绝不会被误读成点名帧。用 `$_` 而非 `$result`，是因为 `result` 本身就是保留名（§7），`$result` 的写法会被当成 `$<节点id>.result` 的一路。
- `$<节点id>.result.<字段>` —— 取**点名帧**的输出字段（`llm_extract` 等可被多处引用）。
- 参数（`parameters`）里的引用：最近一步写 `$_.result.<字段>`，其余写 `$<节点id>.result.<字段>`；不写花括号形式（`${{...}}` 不被识别）。
- `@config.<键>` —— 运行前才填的配置占位（如商品 id）。
- 只能引用**上游节点 `output_schema` 里声明过**的字段（下游到上游，单向不可变）。

## 6. expr 文法（只判定，不做计算）

- **值**：字符串、数字、`true`/`false`/`null`、`$` 引用。
- **运算符**：`!` `&&` `||` `>` `>=` `<` `<=` `==` `!=` `in` `contains`，以及括号分组。
- **没有函数，没有数组**：`len(x)`、`[]`、`x.length` 都是语法错误。
- 惯用法：
  - 判"取没取到"：`$节点.result.字段 != null`
  - 判"字符串非空"：`$节点.result.字段 != ""`

## 7. 名称规则与链约束

- id：`[A-Za-z][A-Za-z0-9_]*`；**不以 `_` 开头**；不得叫 `result`；全图唯一（**含 loop body 内的节点**）。
- 保留名：`_turn_start`、`_turn_end`、`_fail`，以及只在 loop body 内可用的 `_continue`、`_break`。
- 每个 tool/model/run_tools 必须有 `next`；每个 expr 必须有 `then` 和 `else`；
  route 必须有 `cases`（覆盖全部 outcome）或 `default`。每个指针指向**紧接着要写的那一个**节点，
  链尾到 `_turn_end`；全部节点要连成单链，无死节点。
- 循环只能用 `loop` 节点表达，禁止用 `expr`/`tool` 自环或绕路。**前向边
  （`next`/`then`/`else`/`cases`/`default`/`on_max`）不得成环** —— 唯一的回边是 body 内
  指针写 `_continue` 的那些边（指向 loop 节点头）。在 ReAct 写法里，`run_tools` 的 `next`
  写 **`_continue`**，而不是直接指回循环体入口。
- 通知文案只描述**实际发生**的事：没有 "下单/购买" 工具时不得写"已自动下单"，这类意图应写"提醒用户确认"。

## 8. 可用工具（tool 节点能引用的名字，必须与目录完全同名）

| 工具 | 必填 input | output |
|---|---|---|
| `fetch_price` | `item_id` | `price:number` `in_stock:boolean` `currency:string` |
| `web_search` | `query` | `html:string` |
| `llm_extract` | `prompt` `text` | `nl:*`（动态，实际由节点 output_schema 定） |
| `send_notification` | `title` | `ok:boolean`（`body` 可选） |
| `http_get` | `url` | `status:number` `body:string`（`timeout_ms` 可选） |
| `llm` | `prompt` | `text:string`（`max_tokens` 可选） |
| `sum` | `a` `b` | `value:number` |
| `format` | `v` | `text:string` |
| `echo` | — | `value:string` |
| `read` | `path` | `summary:string` |
| `query_content` | — | `items:*`（`query`/`category`/`limit` 可选） |
| `search_local` | `query` | `matches:*`（`kinds` 可选） |
| `setting_get` | `key` | `value:*` |
| `setting_set` | `key` | `ok:boolean`（`value` 可选） |

`output_schema` 的类型取值：`number` / `string` / `boolean` / `*`（动态，如 `llm_extract`、`query_content`）。

## 9. 完整示例（一段真实产物）

```jsonc
{
  "id": "wf_demo_s3a_os",
  "trigger": { "source": "time", "timing": { "kind": "schedule", "schedule": "0 20 * * *" } },
  "nodes": [
    { "id": "fetch_hot", "tool": "web_search",
      "parameters": { "query": "内容平台 今日热门视频 榜单 Top3" },
      "output_schema": { "html": "string" },
      "next": "extract_top3",
      "on_error": { "retry": 2, "fallback": "fetch_hot_backup" } },
    { "id": "fetch_hot_backup", "tool": "fetch_price",
      "parameters": { "item_id": "hot_video_ranking" },
      "output_schema": { "price": "number", "in_stock": "boolean" },
      "next": "extract_top3" },
    { "id": "extract_top3", "tool": "llm_extract",
      "parameters": { "prompt": "从榜单数据中提取当天热门视频 Top3，输出标题与链接",
                      "text": "$fetch_hot.result.html" },
      "output_schema": { "nl": "*" },
      "next": "check_result" },
    { "id": "check_result",
      "expr": "$extract_top3.result.nl != null && $extract_top3.result.nl != \"\"",
      "then": "notify", "else": "notify_fail" },
    { "id": "notify", "tool": "send_notification",
      "parameters": { "title": "🔥 今日热门视频 Top3",
                      "body": "今天内容平台热门视频 Top3 已整理好，点击查看：$extract_top3.result.nl" },
      "output_schema": { "ok": "boolean" },
      "next": "_turn_end" },
    { "id": "notify_fail", "tool": "send_notification",
      "parameters": { "title": "⚠️ 今日热门视频获取失败",
                      "body": "今天未能成功获取内容平台热门视频 Top3，请稍后手动查看。" },
      "output_schema": { "ok": "boolean" },
      "next": "_turn_end" }
  ]
}
```