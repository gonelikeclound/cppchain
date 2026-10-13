# rust_doc_engine

离线解析与生成 **PDF / DOCX / XLSX / PPTX** 的本地文档引擎，纯 Rust 实现，支持交叉编译为 Android `librust_doc_engine.so`，通过 JNI 供 Kotlin/Java 调用。

> 设计说明见 [docs/README.md](docs/README.md)（技术选型、模块接口、Android 移植、性能风险）。

## 目录结构

```
.
├── Cargo.toml
├── docs/README.md          # 原始设计文档
├── src/
│   ├── lib.rs              # JNI 导出入口、panic 防护、统一 JSON 契约
│   ├── model.rs            # 数据结构、JSON 序列化定义
│   ├── utils.rs            # 文件后缀判断、base64、安全检查
│   ├── pdf_parser.rs       # PDF 解析/渲染/合并（pdfium-render，可选特性）
│   ├── office_reader.rs    # docx/xlsx/pptx 文本/表格读取（+docx-rs reader / zip）
│   └── office_writer.rs    # XLSX/DOCX 生成 + 自研最小 PPTX 生成器
│       └── pptx.rs         # PPTX 直写 OOXML（zip + XML）
├── android/DocParserDemo/  # Android 示例 App（SAF 选文件→JNI 解析→展示）
├── tests/e2e/              # 真实样张 + 生成脚本（第三方工具产出）
├── .github/workflows/ci.yml# CI：主机端到端 + Android 交叉编译
└── src/lib.rs 等 — 自带单元/往返/e2e 测试
```

## 快速开始（主机）

```bash
# 编译（默认开启 pdf 特性，会为当前主机下载 PDFium 预编译库）
cargo build

# 运行自测（xlsx/docx/pptx 读→写→读往返）
cargo test

# 关闭 pdf 特性（去掉 PDFium，so 显著变小）
cargo build --no-default-features
```

## 运行 demo（主机 CLI）

`examples/demo.rs` 是主机可直接调用的 CLI：解析任意受支持文档并打印结构化 JSON。

```bash
# 解析 Office 样张
cargo run --example demo -- tests/e2e/hello.docx --pretty
cargo run --example demo -- tests/e2e/报表.xlsx --pretty
cargo run --example demo -- tests/e2e/演示.pptx --pretty

# 解析 PDF（需 PDFium 库）
RUST_DOC_ENGINE_PDFIUM_LIB=$PWD/tests/e2e/lib/libpdfium.so \
  cargo run --example demo -- tests/e2e/sample.pdf --pretty
```

`--pretty` 对超长文本做 300 字截断；不带则输出完整 JSON。
程序化调用请用公开函数 `rust_doc_engine::{parse_document, build_xlsx, build_docx, build_pptx}`。

## 交叉编译 Android so

```bash
cargo install cargo-ndk          # 需要时安装
rustup target add aarch64-linux-android armv7-linux-androideabi x86_64-linux-android

# 全功能（推荐 arm64-v8a，参数见 docs §5）
cargo ndk -t arm64-v8a -o ./target/android build --release --features pdf

# 降级方案：不编译 PDF，避免 PDFium 依赖（无需 libpdfium.so）
cargo ndk -t arm64-v8a -o ./target/android build --release --no-default-features
```

产物：`target/android/arm64-v8a/librust_doc_engine.so`，放入
`app/src/main/jniLibs/arm64-v8a/`。**启用 PDF 时**还需为对应 ABI 提供
`libpdfium.so`（可从 [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
获取；引擎在运行时通过 `dlopen` 绑定，不静态内嵌）。目录下额外产出的
`libpdfium_render-*.so` 是 pdfium-render 声明 cdylib 的伴生产物，**不需要**放入 app。

> 已实测：本机 Rust 1.96 + NDK 29 下，
> `--release` 无 pdf 产 4.5MB so、带 pdf 产 5.8MB so（均 arm64、stripped）。

## Android 运行（示例 App）

`android/DocParserDemo/` 是一个可运行的示例 App（Android Studio 工程）：

- 系统文件选择器选择手机上 **pdf / docx / xlsx / pptx**
- 拷贝到沙盒 `cacheDir` 后由引擎离线解析，展示统一 JSON
- PDF 额外渲染首页为 Bitmap 预览；不依赖 androidx，无第三方 Java 依赖
- `jniLibs/arm64-v8a/` 已内置引擎 so（含 pdf）与 PDFium；用 Android Studio
  Open 后 Run 即可，详见 `android/DocParserDemo/README.md`

## JNI 接口（Kotlin 侧，详见 `android/DocParserDemo/.../DocParser.kt`）

| 方法 | 说明 |
|---|---|
| `parseFile(path)` / `parseFileWithOptions(path, opts)` | 解析文档，返回统一 JSON |
| `exportXlsx(out, json)` / `exportDocx(out, json)` / `exportPptx(out, json)` | 按 JSON 指令生成 Office 文档 |
| `renderPdfPage(path, index, width)` | 渲染 PDF 页为 RGBA 位图（JSON 返回） |
| `extractPdfImages(path, includeData)` | 提取 PDF 内嵌图片 |
| `extractTextInRect(path, index, x1, y1, x2, y2)` | 提取指定矩形区域文本（PDF point） |
| `mergePdf(inputs[], out)` / `newBlankPdf(out, pages, title)` | PDF 合并 / 新建空白 |

统一约定：**成功**返回结果 JSON；**失败/panic**返回 `{"error":"…"}`，不会崩溃 JVM。

## 端到端校验（真实样张）

`src/e2e.rs` 解析**真实办公软件生成的样张**（非引擎自身产物），样张统一放在
`tests/e2e/`，覆盖 4 类文档：

| 样张 | 生成工具 | 校验点 |
|---|---|---|
| `tests/e2e/hello.docx` | python-docx | 标题、段落、加粗、2×3 表格 |
| `tests/e2e/报表.xlsx` | openpyxl | 多工作表、加粗表头、数字、公式列 |
| `tests/e2e/演示.pptx` | python-pptx | 多页标题、正文要点（含中文） |
| `tests/e2e/sample.pdf` | fpdf2 | 页数、文本、元信息标题、渲染 RGBA、区域文本、合并、空白页 |

```bash
# 1. 生成样张（需 python3：pip install python-docx openpyxl python-pptx fpdf2）
python3 tests/e2e/gen_samples.py

# 2. 提供 PDFium 动态库（Linux x64 示例，来源 pdfium-binaries chromium/8076）
curl -sL -o /tmp/pdfium-linux-x64.tgz \
  "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8076/pdfium-linux-x64.tgz"
mkdir -p tests/e2e/lib && tar -xzf /tmp/pdfium-linux-x64.tgz -C tests/e2e/lib

# 3. 运行全部测试（含 e2e）
cargo test
```

PDF 用例已实测通过：sample.pdf 解析 2 页 183 字；第 1 页渲染 595×842 像素
RGBA；区域文本提取正常；两份合并后 4 页；空白 3 页生成并回读。
样张缺失时相关测试会打印提示并跳过，`cargo test` 默认保持绿色。

## 与原始设计文档的差异（务须知悉）

1. **pptx-rs 不可用**：crates.io 上的 `pptx-rs 0.1.0` 是空壳占位包（仅 Hello World，
   无 library target，无法作为依赖）。已改为**自研最小 PPTX 生成器**
   （`zip` + OOXML），能力与文档 §3 描述一致（基础幻灯片，无动画/复杂图表）。
2. **PDF“文字生成”降级**：pdfium-render 不擅长从零生成带文字的 PDF，因此导出侧
   提供 `newBlankPdf`（空白 N 页）与 `mergePdf`（合并）。文档 §8 建议的 lopdf 方案
   可作补充。
3. **PPTX 读取**：calamine 不解析 pptx（只解析电子表格），此处用 `zip` 直读
   slide XML 提取文本；DOCX 读取用 docx-rs 自带的 Reader。
4. **文本坐标**：pdfium-render 0.9 的 `PdfRect` 未公开左下/右上访问器，坐标能力
   以 `extractTextInRect`（区域文本提取）形式提供。

## 已知边界（与 docs 一致）

- 不支持老式二进制 `.doc / .xls / .ppt`；不支持 PDF OCR（图片扫描件）；VBA 宏不解析。
- PPTX 写入能力有限；复杂 PPT 建议走后端服务。
- PDFium 会增大 so 体积（arm64 约 +1~2MB 内嵌绑定，真实的 pdfium 本体需另带
  `libpdfium.so`）。
## 本仓库引入说明（cppchain 内）

- 以**独立 Rust 子工程**方式vendror入仓库（约 17 个源码文件，排除 android/、target/、e2e 样张）。
- **编译**：`cd office-docs && cargo build --example demo --release`（独立于 cppchain，不进主程序）。
- **运行**：`office-docs/bin/office <file.pdf|docx|xlsx|pptx>` → stdout JSON。
- **PDF 依赖**：`tests/e2e/lib/libpdfium.so`（pdfium 预编译动态库，~7.5MB）**遵循原工程 .gitignore 不纳入版本库**；
  本地放一份即可（当前工作副本已就位）。缺它时 PDF 解析会降级/报错，docx/xlsx/pptx 不受影响。
