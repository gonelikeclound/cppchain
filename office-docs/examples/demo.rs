//! 主机 CLI demo：解析任意受支持文档并打印结构化 JSON。
//!
//! 用法：
//! ```bash
//! cargo run --example demo -- <文件路径> [--pretty] [--max-bytes N] [--max-pages N] [--max-text N]
//! ```
//! - `--max-bytes N`：单文件大小上限，0 = 不限制（大文档分段读取）
//! - `--max-pages N`：PDF/PPTX 只解析前 N 页，0 = 不限
//! - `--max-text N`：正文最大字符数，超出截断，0 = 不限
//!
//! PDF 解析需可加载 PDFium 动态库：
//! ```bash
//! RUST_DOC_ENGINE_PDFIUM_LIB=$PWD/tests/e2e/lib/libpdfium.so \
//!   cargo run --example demo -- tests/e2e/sample.pdf --pretty
//! ```
//!
//! 生成篇（把 JSON 写入指令交给 build_* 生成 Office 文件）：见 tests/e2e 的
//! 往返用例，或直接调用 `rust_doc_engine::build_xlsx/build_docx/build_pptx`。

use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("用法: cargo run --example demo -- <文件路径> [--pretty] [--max-bytes N] [--max-pages N]");
        eprintln!("  PDF 需设置 RUST_DOC_ENGINE_PDFIUM_LIB（或已系统安装 libpdfium）");
        return ExitCode::from(2);
    }
    let path = &args[1];
    let pretty = args.iter().any(|a| a == "--pretty");

    // 解析可选项
    let mut max_bytes: Option<i64> = None;
    let mut max_pages: u64 = 0;
    let mut max_text: u64 = 0;
    let mut i = 2usize;
    while i < args.len() {
        match args[i].as_str() {
            "--max-bytes" => {
                max_bytes = args.get(i + 1).and_then(|v| v.parse().ok());
                i += 2;
            }
            "--max-pages" => {
                max_pages = args.get(i + 1).and_then(|v| v.parse().ok()).unwrap_or(0);
                i += 2;
            }
            "--max-text" => {
                max_text = args.get(i + 1).and_then(|v| v.parse().ok()).unwrap_or(0);
                i += 2;
            }
            "--pretty" => i += 1,
            other => {
                eprintln!("未知参数: {other}");
                return ExitCode::from(2);
            }
        }
    }

    // 默认选项：带图片摘要；base64 数据默认不内嵌（体积大）
    let mut options = serde_json::json!({
        "extract_images": false,
        "include_image_data": false,
    });
    if let Some(b) = max_bytes {
        options["max_bytes"] = serde_json::Value::from(b);
    }
    if max_pages > 0 {
        options["max_pages"] = serde_json::Value::from(max_pages);
    }
    if max_text > 0 {
        options["max_text_chars"] = serde_json::Value::from(max_text);
    }
    let options = options.to_string();

    match rust_doc_engine::parse_document(path, &options) {
        Ok(json) => {
            if pretty {
                // 友好输出：把文本字段裁剪展示，避免刷屏
                match serde_json::from_str::<serde_json::Value>(&json) {
                    Ok(mut v) => {
                        if let Some(t) = v["text"].as_str() {
                            let clipped: String = t.chars().take(300).collect();
                            v["text"] = serde_json::Value::String(clipped);
                            v["_text_clipped"] = serde_json::Value::Bool(true);
                        }
                        println!("{}", serde_json::to_string_pretty(&v).unwrap());
                    }
                    Err(_) => println!("{json}"),
                }
            } else {
                println!("{json}");
            }
        }
        Err(e) => {
            eprintln!("解析失败: {e}");
            return ExitCode::FAILURE;
        }
    }
    ExitCode::SUCCESS
}