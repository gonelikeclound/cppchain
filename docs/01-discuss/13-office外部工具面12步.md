# 13 · office 外部工具面 12 步（不编译进主程序的姿势 · 培训用）

> **适用场景**：Agent 需要"解析/处理 PDF/DOCX/XLSX…"，但不想把重型解析库编进主程序。
> 公元 2026-10-13 的做法（rust_doc_engine → `office` 外部命令）原样拆成 12 步，可直接照做。
> 项目内实例：`office-docs/` + `pev_py` 提示词 + problems/26。

## 12 步

1. **找现成引擎/CLI**：优先可独立编译的有 `CLI` 的库（如 Rust `rust_doc_engine`：PDF/DOCX/XLSX/PPTX 离线解析+生成）。
2. **vendror 进仓库为独立子工程**：`rsync --exclude target --exclude .git --exclude android`，保留源码 + Cargo/CLI + 必需依赖库；
   重型样张/构建产物不入库（原工程 .gitignore 常已排除）。
3. **独立编译**：`cd office-docs && cargo build --example demo --release`（不进 cppchain 构建/链接）。
4. **本地冒烟**：直接在**真实目标文件**上跑通（`demo 采购合同/contract_038.pdf` → 结构化 JSON 含编号/文本）。
5. **包外部命令**：写 `bin/office` 壳（`exec demo "$@"` + 设引擎依赖 env，如 RUST_DOC_ENGINE_PDFIUM_LIB）。
6. **提示词明说**：在 workflow 的 `system_prompt` 里写出 **绝对路径 + 用法**（模型不会主动发现系统工具；不给提示它多半掉进 pip 库坑）。
7. **教批量**：提示"优先写 python 脚本用 subprocess 批量调 office 一次处理全部文件"（避免逐文件来回工具调用、烧预算）。
8. **给足 loop max + 审批**：难任务 `max 6→24`；bash 类需 approve → `approve_policy=allow` 真机免交互。
9. **先裁小输入跑通**：全量 28 份先减到 3 份（txt+pdf+docx）验证链路（端点超时是现实：批/裁减是解法）。
10. **断言产物 + L2 判据**：检查脚本（check_export.py）写 `.out/verdict.txt` → L2 判据 probe(PASS)；产物磁盘断言 + plan_log。
11. **记录坑**：pdfium 动态库不入库（README 注明需本地放置）；模型不会主动找系统工具/误试 pip 装库；DeepSeek-flash 偶发把工具 DSL 当正文（真机特性，留档）。
12. **沉淀**：把结论写进 problems 验收 + 培训底稿（01-discuss/11）。

## 一句话
**外部工具面 = 独立子工程 + 一处 `bin` 壳 + 一句提示词点破**；验证靠产物判据，稳定靠批量化与提示。