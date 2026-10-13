//! 端到端校验：解析「真实」样张文件（第三方库生成，见 tests/e2e/gen_samples.py）。
//!
//! 运行前先生成样张：
//! ```bash
//! python3 tests/e2e/gen_samples.py
//! ```
//!
//! PDF 相关校验还需要 pdfium 动态库（默认取 `tests/e2e/lib/libpdfium.so`，
//! 也可用环境变量 `RUST_DOC_ENGINE_PDFIUM_LIB` 指定）：
//! ```bash
//! RUST_DOC_ENGINE_PDFIUM_LIB=$PWD/tests/e2e/lib/libpdfium.so cargo test --features pdf
//! ```
//!
//! 若样张目录缺失，测试会打印提示并跳过（保证默认 `cargo test` 仍为绿色）。

use crate::model::DocType;
use crate::office_reader;
#[cfg(feature = "pdf")]
use crate::pdf_parser;

/// 样张目录：环境变量优先，默认 ./tests/e2e
fn samples_dir() -> std::path::PathBuf {
    std::env::var("RUST_DOC_ENGINE_SAMPLES")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from("tests/e2e"))
}

fn is_ready() -> bool {
    samples_dir().join("hello.docx").exists()
}

// ---------------------------------------------------------------------------
// DOCX（python-docx 生成）
// ---------------------------------------------------------------------------

#[test]
fn e2e_docx_real() {
    if !is_ready() {
        eprintln!("跳过 e2e_docx：请先执行 `python3 samples/gen_samples.py`");
        return;
    }
    let path = samples_dir().join("hello.docx");
    let doc = office_reader::parse(path.to_str().unwrap(), DocType::Docx, false)
        .expect("解析真实 docx 失败");
    assert_eq!(doc.file_type, "docx");
    // 标题 + 段落 + 加粗句
    let text = doc.text.clone();
    assert!(text.contains("周报"), "标题缺失: {text}");
    assert!(text.contains("python-docx 生成的示例文档"));
    assert!(text.contains("这一句是加粗的"));
    // 表格 R{r}C{c}
    assert_eq!(doc.tables.len(), 1);
    assert_eq!(doc.tables[0].rows.len(), 2);
    assert_eq!(doc.tables[0].rows[1], vec!["R1C0", "R1C1", "R1C2"]);
    eprintln!("[e2e docx] OK 段落{} 表格{}x{}", doc.paragraphs.len(), doc.tables[0].rows.len(), doc.tables[0].rows[0].len());
}

// ---------------------------------------------------------------------------
// XLSX（openpyxl 生成：多工作表、加粗表头、数字、公式）
// ---------------------------------------------------------------------------

#[test]
fn e2e_xlsx_real() {
    if !is_ready() {
        eprintln!("跳过 e2e_xlsx：请先执行 `python3 samples/gen_samples.py`");
        return;
    }
    let path = samples_dir().join("报表.xlsx");
    let doc = office_reader::parse(path.to_str().unwrap(), DocType::Xlsx, false)
        .expect("解析真实 xlsx 失败");
    assert_eq!(doc.file_type, "xlsx");
    // 两个工作表
    assert_eq!(doc.tables.len(), 2);
    assert_eq!(doc.tables[0].name, "销售");
    // 表头：商品/数量/单价（D2 是公式单元格，列数由数据区决定，不对整行做全等断言）
    assert_eq!(doc.tables[0].rows[0][0], "商品");
    assert!(doc.tables[0].rows[1].contains(&"30".to_string()), "数字应转字符串: {:?}", doc.tables[0].rows[1]);
    assert!(doc.tables[0].rows[1].contains(&"3.5".to_string()));
    assert_eq!(doc.tables[1].name, "备注");
    assert!(doc.text.contains("纯文本说明"));
    eprintln!("[e2e xlsx] OK sheet{} 首表{}行", doc.tables.len(), doc.tables[0].rows.len());
}

// ---------------------------------------------------------------------------
// PPTX（python-pptx 生成：标题占位 + 正文占位）
// ---------------------------------------------------------------------------

#[test]
fn e2e_pptx_real() {
    if !is_ready() {
        eprintln!("跳过 e2e_pptx：请先执行 `python3 samples/gen_samples.py`");
        return;
    }
    let path = samples_dir().join("演示.pptx");
    let doc = office_reader::parse(path.to_str().unwrap(), DocType::Pptx, false)
        .expect("解析真实 pptx 失败");
    assert_eq!(doc.file_type, "pptx");
    assert!(doc.text.contains("端到端校验·第一页"), "标题缺失: {}", doc.text);
    assert!(doc.text.contains("要点一"));
    assert!(doc.text.contains("要点三"));
    assert!(doc.text.contains("第二页标题"));
    assert!(doc.text.contains("详情 乙 含中文"));
    assert_eq!(doc.paragraphs.len(), 2, "应有 2 页");
    eprintln!("[e2e pptx] OK 页数{} 文本{}字", doc.paragraphs.len(), doc.text.chars().count());
}

// ---------------------------------------------------------------------------
// PDF（fpdf2 生成；需 pdfium 动态库）
// ---------------------------------------------------------------------------

#[cfg(feature = "pdf")]
#[test]
fn e2e_pdf_real() {
    if !is_ready() {
        eprintln!("跳过 e2e_pdf：请先执行 `python3 samples/gen_samples.py`");
        return;
    }
    // 若未指定 pdfium 库路径且默认位置存在，则设置环境变量（供 acquire_pdfium 使用）
    if std::env::var("RUST_DOC_ENGINE_PDFIUM_LIB").is_err() {
        let candidate = samples_dir().join("lib/libpdfium.so");
        if candidate.exists() {
            std::env::set_var("RUST_DOC_ENGINE_PDFIUM_LIB", &candidate);
        }
    }
    let pdf = samples_dir().join("sample.pdf");

    // 1) 解析：页数、文本、元信息
    let doc = pdf_parser::parse_pdf(pdf.to_str().unwrap(), false, false, 0)
        .expect("解析真实 pdf 失败");
    assert_eq!(doc.page_count, Some(2));
    assert!(doc.text.contains("End-to-End Sample"), "第1页文本: {}", doc.text);
    assert!(doc.text.contains("Page Two"), "第2页文本: {}", doc.text);
    assert!(doc.text.contains("12345"));
    assert_eq!(
        doc.metadata.get("title").and_then(|v| v.as_str()).map(|s| s.as_bytes()),
        Some("Sample PDF title".as_bytes()),
        "PDF 元信息标题应被读出，实际: {:?}",
        doc.metadata
    );
    eprintln!(
        "[e2e pdf] OK 页数{} 正文{}字 页码提示",
        doc.page_count.unwrap(),
        doc.text.chars().count(),
    );

    // 2) 渲染第 1 页为 RGBA（原尺寸）
    let r = pdf_parser::render_page_rgba(pdf.to_str().unwrap(), 0, 0).expect("渲染失败");
    let v: serde_json::Value = serde_json::from_str(&r).unwrap();
    let w = v["width"].as_u64().unwrap();
    let h = v["height"].as_u64().unwrap();
    let rgba = v["rgbaBase64"].as_str().unwrap();
    assert!(w > 100 && h > 100, "渲染尺寸异常 {w}x{h}");
    // 验证 base64 长度 ≈ w*h*4
    let decoded_len = crate::utils::base64_decode_len(rgba).expect("base64 长度计算");
    assert_eq!(decoded_len, (w * h * 4) as usize);
    eprintln!("[e2e pdf 渲染] OK {w}x{h} rgba{q} 字节", q = decoded_len);

    // 3) 区域文本（整页矩形）
    let t = pdf_parser::extract_text_in_rect(pdf.to_str().unwrap(), 0, 0.0, 0.0, 2000.0, 2000.0)
        .expect("区域文本失败");
    assert!(t.contains("End-to-End"));
    eprintln!("[e2e pdf 区域文本] OK");

    // 4) 合并两份 sample.pdf → 4 页
    let merged = samples_dir().join("_merged_e2e.pdf");
    let inputs = serde_json::json!([pdf.to_str().unwrap(), pdf.to_str().unwrap()]).to_string();
    pdf_parser::merge_pdfs(&inputs, merged.to_str().unwrap()).expect("合并 pdf 失败");
    let m = pdf_parser::parse_pdf(merged.to_str().unwrap(), false, false, 0).expect("读取合并结果");
    assert_eq!(m.page_count, Some(4), "两份 2 页应合为 4 页");
    let _ = std::fs::remove_file(&merged);
    eprintln!("[e2e pdf 合并] OK 4 页");

    // 5) 空白 PDF + 回读
    let blank = samples_dir().join("_blank_e2e.pdf");
    pdf_parser::new_blank_pdf(blank.to_str().unwrap(), 3, None).expect("空白 pdf 生成失败");
    let b = pdf_parser::parse_pdf(blank.to_str().unwrap(), false, false, 0).unwrap();
    assert_eq!(b.page_count, Some(3));
    let _ = std::fs::remove_file(&blank);
    eprintln!("[e2e pdf 空白] OK 3 页");
}