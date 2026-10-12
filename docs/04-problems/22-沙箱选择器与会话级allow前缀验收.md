# 22 · wfr-㊸ 批③④ 验收：沙箱选择器 + 会话级 allow 前缀 + git 快照回滚实践

> **这份是什么**：wfr-㊸ 的 ③④ 两批收官验收。③ = **SandboxManager**（bwrap provider /
> 受限子进程兜底 / 无 provider fail-closed）+ L5 timeout/输出截断落到执行器；④ =
> **会话级 allow 前缀持久化**（范围不变量）+ **git 快照回滚实践**。设计稿与逐批落地记录在
> `docs/01-discuss/06`（§5 L4/L5/L6/L7、§11/§12）；框架机制沿 wfr-㊷（problems/21）的
> ExecutionGuard / approve_tools / 会话日志事实源。**全仓 652 全绿**。

## 一、批③ · SandboxManager：沙箱不再"假装有"

### 交付

| 件 | 说明 |
|---|---|
| `tool/sandbox.{hpp,cpp}` | provider 选择器：按配置顺序解析第一个**可用**执行器；全不可用 → `available()==false`（fail-closed） |
| `tool/spawn_runner.{hpp,cpp}` | 从旧 `PlainSpawnExecutor::run` 抽出的**共享内核**（fork/exec 任意 argv + 进程组超时 SIGKILL + 输出封顶截断），PlainSpawn 与 BwrapExecutor 共用 |
| `bwrap_usable()` | **可用性探针**：二进制存在 **且** 能实际建 namespace（跑一次 `true`）——避免「带沙箱标志却因 user ns 被禁而裸跑失败还装作成功」 |
| `subprocess` provider | `make_restricted_spawn_executor`：chdir workspace + setrlimit（FSIZE 256MB / NOFILE 1024）；`describe()` **显式标注「非真沙箱」** |
| wfr 装配 | bash 后端 = SandboxManager；无 provider → 空执行器注册 → handler fail-closed；启动打印后端身份 |

### 本机实测：bwrap 可用性探针自隐退

本机是嵌套受限环境（user namespace 被禁，`bwrap: setting up uid map: Permission denied`）——
探针如实判定 bwrap 不可用，自动落到受限子进程 provider，且**不冒充沙箱**：

```
bash 执行后端: 受限子进程（**非真沙箱**：文件系统/网络无约束，仅 chdir workspace + setrlimit + 超时/输出封顶）
```

真机（approve_bash_demo）：`bash echo hello-sandbox` → 审批 → 执行 `exit_code: 0` + 输出 + 审计 1 行。

### 验收用例（防纪律空洞）

- 只给不可用 provider（`container` 预留）→ `available()==false`；bash 空执行器 → handler **拒绝执行**。
- 强制 bwrap 但建不了 namespace → **拒绝不回退**（fail-closed；bwrap 可用环境该用例 GTEST_SKIP）。
- `bwrap_argv` 纯函数断言隔离声明：`--unshare-all`/`--die-with-parent`/根只读/`--tmpfs /tmp`/`--clearenv`/`--bind <ws>` 可写/`--chdir <ws>`。
- L5 回传：subprocess 下超时强杀（`sleep 5` × 60ms → ToolError「超时」）；输出封顶（`seq 1 1000` × max_output=64 → 截断 note 回喂）。

## 二、批④ · 会话级 allow 前缀：人审"一次性"升级成"会话内复用"

### 机制

批准 bash 命令时，把命令**原文**作为前缀追加到 `<session>.allowlist.jsonl`；下一次触发（跨进程续聊 /
replay）时，wfr 把批准记录灌进 `@config.approved_prefixes`，`approve_tools` 对「前缀命中」的
NeedHuman 命令**自动放行**，不再打断人审。

```
wfr approve_bash_demo --set allow_prefixes=on # 开关 = 显式开启；缺省行为不变（每次仍人审）
```

### 范围不变量（三条，缺一不可）

1. **会话作用域**：allowlist 文件按会话存放（`sessions/<session>.allowlist.jsonl`），不跨会话、不跨
   工作区；与 `--reset` 清会话的纪律一致（清会话应连带删 allowlist）。
2. **前缀匹配语义**：`命令.starts_with(批准前缀)` —— **允许 P 即允许任意以 P 开头的命令**
   （含 `P; …` 串联）。这是 OpenAI exec-policy prefix 同款的明确斜面，批准者须接受；
   模型看到的还是完整命令，只是免了再次打断。
3. **沙箱身份绑定**：批准记录带当时的 `sandbox provider`；只有**当前沙箱身份一致**才自动放行。
   沙箱降级（bwrap → 受限子进程）后旧批准**自动失效**，强制重新人审。

### 真机 A/B/C

| 场景 | 观测 |
|---|---|
| **A 首次批准** | 悬挂 → `approve` → 执行 `echo hi-allow` → 输出 + 落盘 `{"prefix":"echo hi-allow","provider":"subprocess"}`；`已把 1 条批准命令记入本会话 allow 前缀` |
| **B 同会话续跑同前缀** | 第二次启动（stdin 不给裁决）→ `会话级 allow 前缀: 1 条` 加载 → **无审批提示**，模型再发 `echo hi-allow` → 自动放行 + 批② 重放（`执行 0 · 重放 1`）→ 输出一致 |
| **C 拒绝** | 新命令 `echo blocked-cmd-123`（前缀不命中）→ 悬挂 → `reject 不需要这条` → **不执行** + 反馈回喂，模型如实报告被拒 |

B 尤其说明组合价值：② 的重放（幂等）与 ④ 的自动放行同时生效——跨进程、免打扰、还是同一份事实。

### 验收用例

`WfApprovePersist.*`（5 例，MemoryCheckpointer）：同前缀+同沙箱 → 自动放行不悬挂（副作用计数器==1）；
命令不以前缀开头 → 照旧悬挂；**provider 不一致 → 前缀失效**（沙箱降级反例）；无配置 → 与批② 行为
完全一致（全部悬挂）；allowlist 辅助 append→load 往返 + 坏行 fail-open。

## 三、git 快照回滚实践（L7，仓库场景的兜底）

bash 副作用不可追踪（Claude Code checkpointing 明示不追踪 bash 改的文件）→ 不指望"撤销那条命令"，
改在**跑之前做快照、跑完必要时 checkout 还原**。仓库场景的标准动作：

```bash
# 1) 跑危险批准命令之前：确认工作区干净，打一个匿名快照
git status --porcelain | head
SNAP=$(git stash create "[wfr] before risky bash")   # 不动的 stash，仅返回 commit 对象
[ -n "$SNAP" ] || git commit -am "wfr 快照 $(date +%s)"  # 有未提交改动时的替代
echo "快照: $SNAP / $(git rev-parse --short HEAD)"

# 2) 执行带审批的 bash（如 git clean -fdx / 批量改写）
wfr approve_bash_demo --set allow_prefixes=on "git clean -fdx 之类"

# 3) 发现不对 → 回滚 = checkout（边界级丢弃，不做命令反做）
git checkout -- . && git clean -fd
#    或退回快照：git reset --hard "$SNAP"
```

三条纪律（与 06 §7 L7 一致）：① 回滚只发生在**边界**（不尝试逐命令反做）；② 快照在**批准之后、
执行之前**打（先记住"批准了才危险"的时间点）；③ 沙箱/容器场景回滚 = 销毁即还原（物理丢弃），
git 快照是仓库场景的等价物；`ToolSpec::probe`（㊷①）负责崩溃缺环的意向校验。

## 四、验收

- 全仓 **652/652 全绿**、零告警（639 → 647 {@③ +8} → 652 {@④ +5}）。
- 真机 A/B/C 三场景闭环（上述数据均来自会话日志与 allowlist 落盘，非凭印象）。
- 遗留：`container` provider 未实现（预留）；seccomp 未纳入 subprocess provider（setrlimit 子集 +
  诚实标注）；逐调用/编辑参数人审（`update_state`）未做；bwrap 在本机不可用属环境限制，有能力的
  宿主会按探针走通 bwrap 路径。