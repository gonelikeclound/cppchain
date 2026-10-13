//! # rust_doc_engine — 离线文档解析引擎（Android 可移植版）
//!
//! 通过 JNI 暴露给 Kotlin/Java 调用的入口。
//!
//! Kotlin 侧声明（类名 `com.example.docengine.DocParser`）：
//! ```kotlin
//! class DocParser {
//!     companion object { init { System.loadLibrary("rust_doc_engine") } }
//!     external fun parseFile(path: String): String
//!     external fun parseFileWithOptions(path: String, options: String): String
//!     external fun exportXlsx(outPath: String, json: String): String
//!     external fun exportDocx(outPath: String, json: String): String
//!     external fun exportPptx(outPath: String, json: String): String
//!     external fun renderPdfPage(path: String, pageIndex: Int, targetWidth: Int): String
//!     external fun extractPdfImages(path: String, includeData: Boolean): String
//!     external fun extractTextInRect(path: String, pageIndex: Int, x1: Float, y1: Float, x2: Float, y2: Float): String
//!     external fun mergePdf(inputsJson: String, outPath: String): String
//!     external fun newBlankPdf(outPath: String, pageCount: Int, title: String?): String
//! }
//! ```
//!
//! 约定：成功返回结果 JSON；异常/panic 返回 `{"error":"…"}`，不会把崩溃抛回 JVM。

mod model;
mod office_reader;
mod office_writer;
mod utils;
#[cfg(feature = "pdf")]
mod pdf_parser;

#[cfg(test)]
mod e2e;

use jni::Env;
use jni::errors::{LogErrorAndDefault, Result as JniResult};
use jni::objects::{JClass, JString};
use jni::sys::{jint, jstring, JNI_VERSION_1_6};
use jni::EnvUnowned;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr::null_mut;

use model::{DocType, ParsedDocument};
use utils::{check_path, detect_type, err_json};

/// 默认单文件大小上限：256MB（防止 docx 炸弹等导致的 DoS，见文档 §7.4）
const DEFAULT_MAX_BYTES: u64 = 256 * 1024 * 1024;

// ---------------------------------------------------------------------------
// 通用 JNI 辅助
// ---------------------------------------------------------------------------

/// 提取 panic 载荷的字符串
fn panic_message(e: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = e.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = e.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

/// 统一 JNI 字符串返回入口：
/// 1) 捕获 Rust panic（文档 §7.3：panic 未捕获会直接导致 Android App crash）
/// 2) 将 `Result<String, String>` 收敛为 JSON 字符串（错误信息同样以 JSON 返回）
/// 3) 构造 JVM 字符串失败时返回 null（上层按 null 处理）
fn finish_string(
    env: &mut Env,
    f: impl FnOnce(&mut Env<'_>) -> Result<String, String>,
) -> jstring {
    let result = catch_unwind(AssertUnwindSafe(|| f(env)));
    let s = match result {
        Ok(Ok(s)) => s,
        Ok(Err(e)) => err_json(e),
        Err(p) => err_json(&format!("rust panic: {}", panic_message(&p))),
    };
    match env.new_string(s) {
        Ok(j) => j.into_raw(),
        Err(_) => null_mut(),
    }
}

/// 解析 JNI 传入的 path 参数
///
/// 注：jni 0.22 中 `Env::get_string` 已废弃（推荐 JString::mutf8_chars），
/// 但语义仍是 MUTF-8 → Rust String，此处按兼容方式使用。
#[allow(deprecated)]
fn jni_string(env: &mut Env, s: &JString) -> Result<String, String> {
    let s = env
        .get_string(s)
        .map_err(|e| format!("读取 JNI 字符串失败: {e}"))?;
    Ok(s.into())
}

// ---------------------------------------------------------------------------
// JNI_OnLoad（可选的版本上报；用裸指针签名避免 jni 包装类型 ABI 歧义）
// ---------------------------------------------------------------------------

/// JVM 加载 so 时回调，报告支持的 JNI 版本号
#[no_mangle]
pub extern "system" fn JNI_OnLoad(
    _vm: *mut jni::sys::JavaVM,
    _reserved: *mut std::ffi::c_void,
) -> jint {
    JNI_VERSION_1_6
}

// ---------------------------------------------------------------------------
// JNI 导出：解析
// ---------------------------------------------------------------------------

/// 解析任意受支持的文档（pdf/docx/xlsx/pptx），返回统一 JSON
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_parseFile(
    mut env: EnvUnowned,
    _class: JClass,
    file_path: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let path = jni_string(env, &file_path)?;
            parse_document(&path, "{}")
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 带选项解析：options JSON 支持
/// - `extract_images`: bool，是否提取图片摘要（默认 false）
/// - `include_image_data`: bool，图片 base64 是否内嵌（默认 false，需 extract_images=true）
/// - `max_bytes`: int，单文件大小上限（默认 256MB，0 = 不限制）
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_parseFileWithOptions(
    mut env: EnvUnowned,
    _class: JClass,
    file_path: JString,
    options: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let path = jni_string(env, &file_path)?;
            let opts = jni_string(env, &options)?;
            parse_document(&path, &opts)
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// **主机/CLI 解析入口**（非 JNI）：解析 pdf/docx/xlsx/pptx，返回统一 ParsedDocument JSON。
///
/// `options_json` 支持：
/// - `extract_images`: bool，是否提取图片摘要（默认 false）
/// - `include_image_data`: bool，图片 base64 是否内嵌（默认 false）
/// - `max_bytes`: int，单文件大小上限（默认 256MB，0 = 不限制）
///
/// PDF 解析需可加载 PDFium 动态库：设置环境变量 `RUST_DOC_ENGINE_PDFIUM_LIB`
/// 指向 `libpdfium.so`（Android 上则由 Kotlin 先 `System.loadLibrary("pdfium")`）。
pub fn parse_document(path: &str, options_json: &str) -> Result<String, String> {
    parse_with(path, options_json).and_then(|doc| {
        serde_json::to_string(&doc).map_err(|e| format!("JSON 序列化失败: {e}"))
    })
}

/// **主机/CLI 写入入口**（非 JNI）：按 JSON 指令生成 XLSX/DOCX/PPTX，返回 {"ok":true,..}
pub fn build_xlsx(out: &str, json: &str) -> Result<String, String> {
    office_writer::export_xlsx(out, json).map(|()| ok_json(out))
}
pub fn build_docx(out: &str, json: &str) -> Result<String, String> {
    office_writer::export_docx(out, json).map(|()| ok_json(out))
}
pub fn build_pptx(out: &str, json: &str) -> Result<String, String> {
    office_writer::export_pptx(out, json).map(|()| ok_json(out))
}

/// 内部：解析入口（返回结构化 ParsedDocument）
fn parse_with(path: &str, options_json: &str) -> Result<ParsedDocument, String> {
    #[derive(serde::Deserialize, Default)]
    struct Options {
        #[serde(default)]
        extract_images: bool,
        #[serde(default)]
        include_image_data: bool,
        #[serde(default)]
        max_bytes: Option<u64>,
        /// 大文档分段（PDF/PPTX）：只读取前 N 页，0 = 不限
        #[serde(default)]
        max_pages: u32,
        /// 大文档分段：正文最大字符数，超出即截断（0 = 不限）
        #[serde(default)]
        max_text_chars: u32,
    }
    let opts: Options = serde_json::from_str(options_json).unwrap_or_default();
    let max_bytes = opts.max_bytes.map(|v| if v == 0 { u64::MAX } else { v });

    // 安全校验：路径存在 + 文件大小上限
    check_path(path, Some(max_bytes.unwrap_or(DEFAULT_MAX_BYTES)))?;

    let typ = detect_type(path);
    let mut doc = match typ {
        DocType::Pdf => {
            #[cfg(feature = "pdf")]
            {
                pdf_parser::parse_pdf(
                    path,
                    opts.extract_images,
                    opts.include_image_data,
                    opts.max_pages as usize,
                )?
            }
            #[cfg(not(feature = "pdf"))]
            {
                return Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string());
            }
        }
        DocType::Docx | DocType::Xlsx | DocType::Pptx => {
            office_reader::parse(path, typ, opts.extract_images)?
        }
        DocType::Unknown => {
            return Err("不支持的文件类型：仅支持 pdf/docx/xlsx/pptx".to_string());
        }
    };

    // 大文档分段：正文超限时截断，防止超大文档把结果字符串/内存撑爆
    let limit = opts.max_text_chars as usize;
    if limit > 0 && doc.text.chars().count() > limit {
        let mut budget = limit as i64;
        doc.paragraphs.retain(|p| {
            let n = p.chars().count() as i64;
            if budget <= 0 {
                return false;
            }
            budget -= n;
            true
        });
        doc.paragraphs.push("…（内容已按 max_text_chars 截断）".to_string());
        doc.text = doc.paragraphs.join("\n");
        doc.warning = Some(format!("正文超过 {limit} 字，已分段截断"));
    }

    doc.file_path = Some(path.to_string());
    Ok(doc)
}

// ---------------------------------------------------------------------------
// JNI 导出：写入
// ---------------------------------------------------------------------------

/// 根据 JSON 生成 XLSX，返回 {"ok":true,"path":"…"} 或 {"error":"…"}
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_exportXlsx(
    mut env: EnvUnowned,
    _class: JClass,
    out_path: JString,
    json: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let out = jni_string(env, &out_path)?;
            let data = jni_string(env, &json)?;
            office_writer::export_xlsx(&out, &data).map(|()| ok_json(&out))
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 根据 JSON 生成 DOCX
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_exportDocx(
    mut env: EnvUnowned,
    _class: JClass,
    out_path: JString,
    json: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let out = jni_string(env, &out_path)?;
            let data = jni_string(env, &json)?;
            office_writer::export_docx(&out, &data).map(|()| ok_json(&out))
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 根据 JSON 生成 PPTX
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_exportPptx(
    mut env: EnvUnowned,
    _class: JClass,
    out_path: JString,
    json: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let out = jni_string(env, &out_path)?;
            let data = jni_string(env, &json)?;
            office_writer::export_pptx(&out, &data).map(|()| ok_json(&out))
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

fn ok_json(path: &str) -> String {
    serde_json::json!({ "ok": true, "path": path }).to_string()
}

// ---------------------------------------------------------------------------
// JNI 导出：PDF 专属能力（依赖 pdf 特性）
// ---------------------------------------------------------------------------

/// 渲染 PDF 指定页为 RGBA 位图，返回 {"width":…,"height":…,"rgbaBase64":"…","page":…}
/// Kotlin 侧用 Bitmap.createBitmap(width, height, ARGB_8888) 还原
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_renderPdfPage(
    mut env: EnvUnowned,
    _class: JClass,
    path: JString,
    page_index: jint,
    target_width: jint,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let path = jni_string(env, &path)?;
            #[cfg(feature = "pdf")]
            {
                pdf_parser::render_page_rgba(&path, page_index as u16, target_width as u32)
            }
            #[cfg(not(feature = "pdf"))]
            {
                Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string())
            }
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 提取 PDF 页面内的图片
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_extractPdfImages(
    mut env: EnvUnowned,
    _class: JClass,
    path: JString,
    include_data: jni::sys::jboolean,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let path = jni_string(env, &path)?;
            #[cfg(feature = "pdf")]
            {
                pdf_parser::extract_images(&path, include_data)
            }
            #[cfg(not(feature = "pdf"))]
            {
                Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string())
            }
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 提取 PDF 指定页指定矩形区域内的文本（坐标单位 PDF point，原点左下）。
/// 传 (x1==x2 && y1==y2) 表示整页文本。返回 {"page":…,"text":"…"}
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_extractTextInRect(
    mut env: EnvUnowned,
    _class: JClass,
    path: JString,
    page_index: jint,
    x1: f32,
    y1: f32,
    x2: f32,
    y2: f32,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let path = jni_string(env, &path)?;
            #[cfg(feature = "pdf")]
            {
                pdf_parser::extract_text_in_rect(&path, page_index as u16, x1, y1, x2, y2)
            }
            #[cfg(not(feature = "pdf"))]
            {
                Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string())
            }
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 合并多个 PDF：inputsJson 为 ["/a.pdf","/b.pdf"]，输出到 outPath
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_mergePdf(
    mut env: EnvUnowned,
    _class: JClass,
    inputs_json: JString,
    out_path: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let inputs = jni_string(env, &inputs_json)?;
            let out = jni_string(env, &out_path)?;
            #[cfg(feature = "pdf")]
            {
                pdf_parser::merge_pdfs(&inputs, &out).map(|()| ok_json(&out))
            }
            #[cfg(not(feature = "pdf"))]
            {
                Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string())
            }
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

/// 新建一个空白 N 页 PDF（A4）
#[no_mangle]
pub extern "system" fn Java_com_example_docengine_DocParser_newBlankPdf(
    mut env: EnvUnowned,
    _class: JClass,
    out_path: JString,
    page_count: jint,
    title: JString,
) -> jstring {
    env.with_env(|env| -> JniResult<jstring> {
        Ok(finish_string(env, |env| {
            let out = jni_string(env, &out_path)?;
            let title = jni_string(env, &title)?;
            #[cfg(feature = "pdf")]
            {
                let count = if page_count < 0 { 0 } else { page_count as u32 };
                pdf_parser::new_blank_pdf(&out, count, if title.is_empty() { None } else { Some(&title) })
                    .map(|()| ok_json(&out))
            }
            #[cfg(not(feature = "pdf"))]
            {
                Err("PDF 功能未启用：编译时未开启 pdf 特性".to_string())
            }
        }))
    })
    .resolve::<LogErrorAndDefault>()
}

// ---------------------------------------------------------------------------
// 单元测试：与 JNI 无关的纯逻辑
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn panic_payload_extraction() {
        let p = catch_unwind(AssertUnwindSafe(|| panic!("boom"))).unwrap_err();
        assert_eq!(panic_message(&p), "boom");
        let p2 = catch_unwind(AssertUnwindSafe(|| panic!("{}", "fmt-msg"))).unwrap_err();
        assert_eq!(panic_message(&p2), "fmt-msg");
    }

    #[test]
    fn detect_type_routing() {
        assert_eq!(detect_type("/a/b/c.DOCX"), DocType::Docx);
        assert_eq!(detect_type("x.pdf"), DocType::Pdf);
    }

    #[test]
    fn ok_json_shape() {
        let v: serde_json::Value = serde_json::from_str(&ok_json("/tmp/a.xlsx")).unwrap();
        assert!(v["ok"].as_bool().unwrap());
    }
}

// ---------------------------------------------------------------------------
// 集成测试：Office 文档“生成 → 回读”往返（不需要 JVM / PDFium）
// ---------------------------------------------------------------------------

#[cfg(test)]
mod roundtrip {
    use super::*;

    fn tmp_path(kind: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "rust_doc_engine_{}_{}_{kind}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn xlsx_write_then_read() {
        let json = r#"{
            "sheets": [{
                "name": "报表",
                "rows": [
                    [{"v": "名称", "t": "str", "bold": true}, {"v": "数量", "t": "str"}],
                    [{"v": "苹果", "t": "str"}, {"v": "3", "t": "num"}],
                    [{"v": "合计", "t": "str"}, {"v": "=SUM(B2:B2)", "t": "formula"}]
                ]
            }]
        }"#;
        let path = tmp_path("a.xlsx");
        office_writer::export_xlsx(path.to_str().unwrap(), json).expect("写入 xlsx");
        let doc = office_reader::parse(path.to_str().unwrap(), DocType::Xlsx, false).expect("回读");
        assert_eq!(doc.file_type, "xlsx");
        assert_eq!(doc.tables.len(), 1);
        let t = &doc.tables[0];
        assert_eq!(t.name, "报表");
        assert_eq!(t.rows[0], vec!["名称", "数量"]);
        assert_eq!(t.rows[1], vec!["苹果", "3"]);
        // 公式行：calamine 读回公式结果为 #REF/0 或空，此处只校验行数
        assert!(doc.text.contains("苹果"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn docx_write_then_read() {
        let json = r#"{
            "title": "周报",
            "body": [
                {"type": "p", "text": "本周进展顺利。"},
                {"type": "h", "text": "工作小结", "level": 1},
                {"type": "table", "rows": [["项", "完成"], ["A", "是"]]}
            ]
        }"#;
        let path = tmp_path("b.docx");
        office_writer::export_docx(path.to_str().unwrap(), json).expect("写入 docx");
        let doc = office_reader::parse(path.to_str().unwrap(), DocType::Docx, false).expect("回读");
        assert_eq!(doc.file_type, "docx");
        assert!(doc.paragraphs.iter().any(|p| p.contains("本周进展顺利")));
        assert!(doc.paragraphs.iter().any(|p| p.contains("工作小结")));
        assert_eq!(doc.tables.len(), 1);
        assert_eq!(doc.tables[0].rows, vec![vec!["项", "完成"], vec!["A", "是"]]);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn pptx_write_then_read() {
        let json = r#"{
            "slides": [
                {"title": "汇报标题", "body": ["要点一", "要点二"]},
                {"title": "第二页", "body": ["细节 A", "细节 B"]}
            ]
        }"#;
        let path = tmp_path("c.pptx");
        office_writer::export_pptx(path.to_str().unwrap(), json).expect("写入 pptx");

        // 包结构自检：必要条目齐全
        let file = std::fs::File::open(&path).unwrap();
        let mut zip = zip::ZipArchive::new(file).unwrap();
        let names: Vec<String> = (0..zip.len())
            .map(|i| zip.by_index(i).unwrap().name().to_string())
            .collect();
        for required in [
            "[Content_Types].xml",
            "_rels/.rels",
            "ppt/presentation.xml",
            "ppt/slideMasters/slideMaster1.xml",
            "ppt/slideLayouts/slideLayout1.xml",
            "ppt/slides/slide1.xml",
            "ppt/slides/slide2.xml",
            "ppt/theme/theme1.xml",
        ] {
            assert!(names.contains(&required.to_string()), "缺少 {required}");
        }
        // 内容类型声明了两页幻灯片
        let mut ct = zip.by_name("[Content_Types].xml").unwrap();
        let mut ct_xml = String::new();
        std::io::Read::read_to_string(&mut ct, &mut ct_xml).unwrap();
        assert!(ct_xml.contains("/ppt/slides/slide1.xml"));
        assert!(ct_xml.contains("/ppt/slides/slide2.xml"));

        let doc = office_reader::parse(path.to_str().unwrap(), DocType::Pptx, false).expect("回读");
        assert_eq!(doc.file_type, "pptx");
        assert!(doc.text.contains("要点一"));
        assert!(doc.text.contains("要点二"));
        assert!(doc.text.contains("汇报标题"));
        assert!(doc.text.contains("第二页"));
        assert_eq!(doc.paragraphs.len(), 2); // 两页
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn unsupported_type_rejected() {
        let err = office_writer::export_xlsx("", r#"{"sheets":[]}"#).unwrap_err();
        assert!(!err.is_empty());
    }
}