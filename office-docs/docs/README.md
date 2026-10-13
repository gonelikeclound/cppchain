# Rust 本地文档解析引擎（Android 可移植版）说明文档

文件名：rust_doc_engine_docs.md
项目目标：离线解析与生成 PDF、DOCX、XLSX、PPTX；支持交叉编译为 Android aarch64/armv7a so，通过 JNI 供 Kotlin/Java 调用
支持范围：现代 OpenXML 格式（.docx/.xlsx/.pptx）+ PDF；不支持老式二进制 .doc/.xls/.ppt
备注：图片内文字识别需要额外接入 OCR 库（PaddleOCR / Tesseract），本引擎不包含OCR能力

## 1. 整体技术栈选型

| 模块 | Crate | 作用 |
|---|---|---|
| PDF 渲染&解析 | pdfium-render | PDF 文本提取、页面渲染、图片提取、页面修改；底层基于 Google Pdfium，Android 生态成熟 |
| Office 文档读取 | calamine | 读取 docx/xlsx/pptx/ods；提取文本、表格、图片；纯 Rust，无系统依赖 |
| XLSX 写入 | rust_xlsxwriter | 新建/修改 XLSX，支持公式、样式、图表，写入能力强 |
| DOCX 写入 | docx-rs | 生成、修改 DOCX，支持段落、样式、图片；复杂样式还原有限 |
| PPTX 写入 | pptx-rs | 基础 PPTX 生成，能力偏弱，适合简单幻灯片 |
| JNI 绑定 | jni | Rust ↔ Android Java/Kotlin JNI 桥接 |
| 异步IO（可选） | tokio | 异步文件读取；移动端建议关闭多线程，避免 Android 内存抖动 |

推荐组合：pdfium-render + calamine + rust_xlsxwriter + docx-rs + pptx-rs + jni

## 2. 核心能力清单

### ✅ PDF（pdfium-render）
- 读取PDF页面、提取文本、文本坐标
- 提取页面内图片（JPG/PNG）
- 页面渲染输出位图
- 新建空白页、删除页面、合并PDF
- 读取元信息（标题、作者、创建时间）

### ✅ Office OpenXML（calamine）【读取】
- DOCX：段落文本、表格、内嵌图片、文档元数据
- XLSX：工作表遍历，单元格文本/数字/公式，行列范围
- PPTX：幻灯片文本、形状、图片
- 支持加密文档检测（部分加密文档无法解密）

### ✅ Office 写入
1. rust_xlsxwriter（推荐）：XLSX 生成最强，支持单元格样式、合并单元格、图表、公式
2. docx-rs：生成 DOCX，段落、字体、图片、简单表格；复杂页眉页脚、高级样式支持一般
3. pptx-rs：基础幻灯片、文本框、图片；缺少动画、母版、复杂图表支持

### ❌ 不支持
- 旧二进制格式 .doc .xls .ppt
- PDF OCR（图片扫描件无文本无法提取）
- DOCX/PPTX 复杂样式 1:1 还原（例如复杂排版、艺术字、高级控件）
- 宏（VBA）解析/执行

## 3. Android 移植说明

### 3.1 编译目标
交叉编译产出 Android NDK 动态库 librust_doc_engine.so
目标 ABI：arm64-v8a(aarch64)，可选 armeabi-v7a / x86_64（模拟器）

### 3.2 环境依赖
1. Rust toolchain + cargo-ndk（一键交叉编译Android so）
2. Android NDK r25+
3. 目标平台：Android API 24+（推荐）

### 3.3 Cargo.toml 基础配置示例
[package]
name = "rust_doc_engine"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"] # 编译为C动态库，供JNI调用

[dependencies]
jni = "0.21"
pdfium-render = "0.8"
calamine = "0.24"
rust_xlsxwriter = "0.77"
docx-rs = "0.4"
pptx-rs = "0.11"

### 3.4 关键注意事项（Android）
1. 静态链接优先：pdfium-render 自带 Pdfium 预编译静态库；calamine / docx-rs / rust_xlsxwriter / pptx-rs 均为纯 Rust，无系统库依赖，不会出现 libxml2/fontconfig 这类跨平台依赖坑。
2. 内存限制：Android APP 内存有限，大文档建议流式分段读取，一次性加载超大文件容易触发 OOM；JNI 层要严格释放字符串/数组引用。
3. 文件权限：Rust 代码只能读写 Android App 沙盒目录（context.filesDir / cacheDir），不能直接访问外部存储，权限由上层 Kotlin 控制。

## 4. 模块接口设计（JNI 简易接口定义）
上层 Kotlin 通过 JNI 调用 Rust，返回结构化 JSON / 原始二进制数据

use jni::objects::{JClass, JString};
use jni::sys::jstring;
use jni::JNIEnv;
use std::panic::catch_unwind;

#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_parseFile(
    env: JNIEnv,
    _class: JClass,
    file_path: JString,
) -> jstring {
    // 捕获panic，防止APP崩溃
    let result = catch_unwind(|| {
        let path: String = env.get_string(file_path).unwrap().into();
        parse_document(&path)
    }).unwrap_or_else(|e| {
        format!("{{\"error\":\"rust panic: {:?}\"}}", e)
    });

    let output = env.new_string(result).unwrap();
    output.into_raw()
}

/// 文档入口：自动根据后缀选择解析器
fn parse_document(path: &str) -> String {
    // 1. 判断后缀 pdf / docx / xlsx / pptx
    // 2. 分发至 pdfium-render / calamine
    // 3. 提取文本、图片、表格，序列化为JSON返回
    // 返回 {"type":"docx","text":"...","tables":[...]}
    todo!()
}

// 其它对外导出接口
// export_pdf()        // 生成PDF
// export_xlsx()       // 生成XLSX
// export_docx()       // 生成DOCX
// extract_images()    // 提取文档内图片

## 5. 编译命令（cargo-ndk）
# 安装 cargo ndk
cargo install cargo-ndk

# 编译 arm64-v8a release
cargo ndk -t arm64-v8a -o ./target/android build --release

输出：target/android/arm64-v8a/librust_doc_engine.so，直接放入 Android 项目 app/src/main/jniLibs/arm64-v8a/

## 6. Android Kotlin 调用示例
class DocParser {
    companion object {
        init {
            System.loadLibrary("rust_doc_engine")
        }
    }
    external fun parseFile(filePath: String): String
    external fun exportXlsx(outputPath: String, jsonData: String): String
}

// 使用示例
val parser = DocParser()
val resultJson = parser.parseFile("/data/data/com.example/files/test.docx")

## 7. 性能与风险评估

### 性能
纯Rust实现，无GC，内存可控；解析 10MB 以内普通文档速度优于 Chaquopy Python 方案；大文件建议分块处理。Pdfium PDF 渲染性能接近商业PDF阅读器。

### 风险点
1. pptx-rs 写入生态较弱，复杂PPT生成能力不足；如重度PPT生成，建议考虑后端服务。
2. pdfium-render 包体积：Pdfium 静态库会增加 so 体积，release 下 arm64 大概增加 4~8MB。
3. JNI 边界：Rust panic 如果未捕获，会直接导致 Android App crash；所有业务代码必须包裹 catch_unwind。
4. 安全：所有文档解析都存在恶意文档风险（畸形PDF/docx炸弹），建议增加文件大小上限、递归深度限制，防止DoS。

## 8. 备选降级方案
- 如果不需要PDF渲染：移除 pdfium-render，替换为 lopdf，so体积显著减小，但是丢失页面渲染、图片提取能力，仅文本读写。
- 如果只做读取，不做写入：calamine 一个库搞定全部Office读取，依赖最小。

## 9. 部署架构建议
Android App(Kotlin)
        ↓ JNI
Rust librust_doc_engine.so
├─ pdfium-render: PDF处理
├─ calamine: Office读取
├─ rust_xlsxwriter / docx-rs / pptx-rs: Office写入
└─ JNI glue layer: 类型转换、异常捕获

## 10. 工程目录结构参考
rust_doc_engine/
├── Cargo.toml
├── src/
│   ├── lib.rs          # JNI导出入口
│   ├── pdf_parser.rs   # pdfium-render PDF逻辑
│   ├── office_reader.rs# calamine 读取docx/xlsx/pptx
│   ├── office_writer.rs# rust_xlsxwriter/docx-rs/pptx-rs写入
│   ├── model.rs        # 数据结构、JSON序列化定义
│   └── utils.rs        # 文件后缀判断、工具函数
└── target/
    └── android/        # 交叉编译产出so
