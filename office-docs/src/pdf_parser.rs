//! PDF 解析与处理（基于 pdfium-render 0.9）。
//!
//! 只在本 crate 开启 `pdf` 特性时编译（默认开启）。Android 交叉编译若没有
//! 对应 ABI 的 `libpdfium.so` 预编译库，可用 `--no-default-features` 关闭。

use crate::model::{ExtractedImage, ParsedDocument};
use crate::utils::base64_encode;
use pdfium_render::prelude::*;
use std::sync::{Mutex, OnceLock};

/// PDFium 是进程内单例绑定（pdfium-render 内部通过全局 OnceCell 保存绑定表，
/// 第二次 bind 会返回 AlreadyInitialized），因此这里也将其缓存为全局单例。
///
/// `Pdfium` 的字段包含裸指针，默认不是 `Send`；但我们通过全局唯一的 `Mutex`
/// 把**所有** PDFium 调用串行化（pdfium-render 非 thread_safe 特性下本身要求
/// 单线程使用），跨线程移动该值在互斥下是安全的。
///
/// # Safety
/// 仅在 `PDFIUM` 全局互斥锁保护下访问内部 `Pdfium`，同一时刻至多一个线程触及。
struct SharedPdfium(Pdfium);
unsafe impl Send for SharedPdfium {}

static PDFIUM: OnceLock<Mutex<Option<SharedPdfium>>> = OnceLock::new();

/// 获取已绑定好的 PDFium 全局互斥锁（首次调用时绑定动态库）。
/// 通过返回 `MutexGuard` 把整个 PDFium 访问区间串行化（pdfium-render
/// 非 thread_safe 特性下要求单线程使用；全局只绑定一次，第二次 bind 会报错）。
fn acquire_pdfium() -> Result<std::sync::MutexGuard<'static, Option<SharedPdfium>>, String> {
    let lock = PDFIUM.get_or_init(|| Mutex::new(None));
    let mut guard = lock.lock().map_err(|_| "PDFium 锁被污染".to_string())?;
    if guard.is_none() {
        // 绑定优先级：
        // 1) 环境变量 RUST_DOC_ENGINE_PDFIUM_LIB 指定的路径（主机测试/自定义安装）
        // 2) 系统库（Android 上由 Kotlin 先 System.loadLibrary("pdfium") 注入）
        // 3) 平台惯例库名
        let bindings = match std::env::var("RUST_DOC_ENGINE_PDFIUM_LIB") {
            Ok(p) if !p.is_empty() => Pdfium::bind_to_library(&p),
            _ => Pdfium::bind_to_system_library().or_else(|_| {
                Pdfium::bind_to_library(Pdfium::pdfium_platform_library_name())
            }),
        };
        let bindings = bindings.map_err(|e| format!("绑定 PDFium 动态库失败: {e}"))?;
        *guard = Some(SharedPdfium(Pdfium::new(bindings)));
    }
    Ok(guard)
}

/// 解析 PDF，输出统一 ParsedDocument JSON
///
/// 大文档分段：`max_pages` 设为 >0 时只解析前 N 页（0=不限），
/// 配合 parse 选项里的 `max_text_chars` 可把超大 PDF 的内存控制在可接受范围。
pub fn parse_pdf(
    path: &str,
    extract_images: bool,
    include_image_data: bool,
    max_pages: usize,
) -> Result<ParsedDocument, String> {
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let doc = pdfium
        .load_pdf_from_file(path, None)
        .map_err(|e| format!("打开 PDF 失败: {e}"))?;

    let mut paragraphs = Vec::new();
    let mut text = String::new();
    let total = doc.pages().len() as usize;
    let page_count = if max_pages == 0 { total } else { total.min(max_pages) };
    for i in 0..page_count {
        let page = doc
            .pages()
            .get(i as PdfPageIndex)
            .map_err(|e| format!("读取第 {i} 页失败: {e}"))?;
        let page_text = page
            .text()
            .map_err(|e| format!("提取第 {i} 页文本失败: {e}"))?;
        let s = page_text.all();
        text.push_str(&s);
        text.push('\n');
        if !s.trim().is_empty() {
            paragraphs.push(s.trim_end().to_string());
        }
    }

    // 文档元信息
    let meta = doc.metadata();
    let mut metadata = serde_json::Map::new();
    for (tag, key) in [
        (PdfDocumentMetadataTagType::Title, "title"),
        (PdfDocumentMetadataTagType::Author, "author"),
        (PdfDocumentMetadataTagType::Subject, "subject"),
        (PdfDocumentMetadataTagType::Keywords, "keywords"),
        (PdfDocumentMetadataTagType::Creator, "creator"),
        (PdfDocumentMetadataTagType::Producer, "producer"),
        (PdfDocumentMetadataTagType::CreationDate, "creation_date"),
        (PdfDocumentMetadataTagType::ModificationDate, "modification_date"),
    ] {
        if let Some(t) = meta.get(tag) {
            metadata.insert(key.to_string(), serde_json::Value::String(t.value().to_string()));
        }
    }
    let title = metadata
        .get("title")
        .and_then(|v| v.as_str())
        .map(|s| s.to_string());

    // 图片提取
    let mut images = Vec::new();
    if extract_images {
        images = collect_images(path, include_image_data)?;
    }

    let mut doc_out = ParsedDocument::default();
    doc_out.file_type = "pdf".to_string();
    doc_out.title = title;
    doc_out.page_count = Some(page_count);
    doc_out.text = text;
    doc_out.paragraphs = paragraphs;
    doc_out.images = images;
    doc_out.metadata = metadata;
    Ok(doc_out)
}

/// 收集 PDF 全部页面中的图片，返回完整 JSON 列表
pub fn extract_images(path: &str, include_data: bool) -> Result<String, String> {
    let images = collect_images(path, include_data)?;
    serde_json::json!({ "images": images }).to_string().pipe_ok()
}

fn collect_images(path: &str, include_data: bool) -> Result<Vec<ExtractedImage>, String> {
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let doc = pdfium
        .load_pdf_from_file(path, None)
        .map_err(|e| format!("打开 PDF 失败: {e}"))?;

    let mut images = Vec::new();
    for page_idx in 0..doc.pages().len() {
        let page = doc
            .pages()
            .get(page_idx)
            .map_err(|e| format!("读取第 {page_idx} 页失败: {e}"))?;
        for object in page.objects().iter() {
            if object.object_type() != PdfPageObjectType::Image {
                continue;
            }
            let Some(img) = object.as_image_object() else {
                continue;
            };
            let bytes = img
                .get_raw_image_data()
                .map_err(|e| format!("读取第 {page_idx} 页图片数据失败: {e}"))?;
            let w = img.width().unwrap_or(0).max(0) as u32;
            let h = img.height().unwrap_or(0).max(0) as u32;
            images.push(ExtractedImage {
                index: images.len(),
                format: detect_image_format(&bytes),
                width: w,
                height: h,
                size_bytes: bytes.len(),
                data_base64: if include_data { base64_encode(&bytes) } else { String::new() },
            });
        }
    }
    Ok(images)
}

/// 依据文件头魔数判断图片格式
fn detect_image_format(bytes: &[u8]) -> String {
    if bytes.starts_with(b"\xFF\xD8\xFF") {
        "jpeg".to_string()
    } else if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        "png".to_string()
    } else if bytes.starts_with(b"BM") {
        "bmp".to_string()
    } else if bytes.starts_with(b"%PDF") {
        "pdf".to_string()
    } else {
        "unknown".to_string()
    }
}

/// 渲染 PDF 指定页为 RGBA 位图，返回
/// {"width":…,"height":…,"rgbaBase64":"…","page":…}
///
/// target_width<=0 时按原尺寸（72dpi）渲染。
pub fn render_page_rgba(path: &str, page_index: u16, target_width: u32) -> Result<String, String> {
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let doc = pdfium
        .load_pdf_from_file(path, None)
        .map_err(|e| format!("打开 PDF 失败: {e}"))?;
    let idx = page_index as PdfPageIndex;
    let page = doc
        .pages()
        .get(idx)
        .map_err(|e| format!("读取第 {idx} 页失败: {e}"))?;

    let config = if target_width > 0 {
        PdfRenderConfig::new().set_target_width(target_width as Pixels)
    } else {
        PdfRenderConfig::new()
    };
    let bitmap = page
        .render_with_config(&config)
        .map_err(|e| format!("渲染页面失败: {e}"))?;
    let w = bitmap.width().max(0) as u32;
    let h = bitmap.height().max(0) as u32;
    let rgba = bitmap.as_rgba_bytes();
    if rgba.len() < (w as usize) * (h as usize) * 4 {
        return Err(format!("渲染结果缓冲区异常({}字节)", rgba.len()));
    }
    Ok(serde_json::json!({
        "width": w,
        "height": h,
        "page": idx,
        "rgbaBase64": base64_encode(&rgba),
    })
    .to_string())
}

/// 合并多个 PDF：inputs 为路径 JSON 数组，输出到 out
pub fn merge_pdfs(inputs_json: &str, out: &str) -> Result<(), String> {
    let paths: Vec<String> = serde_json::from_str(inputs_json)
        .map_err(|e| format!("PDF 列表 JSON 解析失败: {e}"))?;
    if paths.is_empty() {
        return Err("合并列表为空".to_string());
    }
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let mut target = pdfium
        .create_new_pdf()
        .map_err(|e| format!("创建新 PDF 失败: {e}"))?;
    for p in &paths {
        let src = pdfium
            .load_pdf_from_file(p, None)
            .map_err(|e| format!("打开 {p} 失败: {e}"))?;
        target
            .pages_mut()
            .append(&src)
            .map_err(|e| format!("导入 {p} 页面失败: {e}"))?;
    }
    target
        .save_to_file(out)
        .map_err(|e| format!("保存合并 PDF 失败: {e}"))
}

/// 新建空白 PDF（A4, 纵向），title 参数暂未写入元信息（本版 API 受限）
pub fn new_blank_pdf(out: &str, page_count: u32, _title: Option<&str>) -> Result<(), String> {
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let mut doc = pdfium
        .create_new_pdf()
        .map_err(|e| format!("创建新 PDF 失败: {e}"))?;
    let pages = doc.pages_mut();
    for _ in 0..page_count {
        pages
            .create_page_at_end(PdfPagePaperSize::a4())
            .map_err(|e| format!("创建页面失败: {e}"))?;
    }
    doc.save_to_file(out)
        .map_err(|e| format!("保存 PDF 失败: {e}"))
}

/// 提取指定页文本（全页或指定矩形区域 [x1,y1,x2,y2]，单位 PDF point，原点左下）
/// 用于“文本坐标”类能力：先由上层定位矩形，再取该区域文本。
pub fn extract_text_in_rect(
    path: &str,
    page_index: u16,
    x1: f32,
    y1: f32,
    x2: f32,
    y2: f32,
) -> Result<String, String> {
    let guard = acquire_pdfium()?;
    let pdfium = &guard.as_ref().unwrap().0;
    let doc = pdfium
        .load_pdf_from_file(path, None)
        .map_err(|e| format!("打开 PDF 失败: {e}"))?;
    let page = doc
        .pages()
        .get(page_index as PdfPageIndex)
        .map_err(|e| format!("读取第 {page_index} 页失败: {e}"))?;
    let page_text = page
        .text()
        .map_err(|e| format!("提取文本失败: {e}"))?;
    let text = if x1 <= x2 && y1 <= y2 {
        page_text.inside_rect(PdfRect::new_from_values(y1, x1, y2, x2))
    } else {
        page_text.all()
    };
    Ok(serde_json::json!({ "page": page_index, "text": text }).to_string())
}

/// 小工具：Ok(String) 便捷转换
trait PipeOk {
    fn pipe_ok(self) -> Result<String, String>;
}
impl PipeOk for String {
    fn pipe_ok(self) -> Result<String, String> {
        Ok(self)
    }
}