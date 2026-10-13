//! Office 文档读取：docx / xlsx / pptx 的文本、表格、图片提取
//!
//! - XLSX：calamine（原生电子表格解析）
//! - DOCX：docx-rs 自带的 Reader（纯 Rust）
//! - PPTX：calamine 不解析幻灯片，此处用 zip 直接读取 slide XML 提取文本

use crate::model::{DocType, ExtractedImage, ParsedDocument, Table};
use crate::utils::base64_encode;
use calamine::{open_workbook_auto, Data, Reader};

/// 统一读取入口：根据类型分发
pub fn parse(path: &str, typ: DocType, extract_images: bool) -> Result<ParsedDocument, String> {
    match typ {
        DocType::Docx => parse_docx(path, extract_images),
        DocType::Xlsx => parse_xlsx(path),
        DocType::Pptx => parse_pptx(path),
        _ => Err("office_reader: 不支持的文档类型".to_string()),
    }
}

/// 单元格数据 → 显示文本
fn data_to_str(d: &Data) -> String {
    match d {
        Data::String(s) => s.clone(),
        Data::Float(f) => {
            if f.fract() == 0.0 && f.abs() < 1e15 {
                format!("{}", *f as i64)
            } else {
                format!("{}", f)
            }
        }
        Data::Int(i) => i.to_string(),
        Data::Bool(b) => b.to_string(),
        Data::DateTime(dt) => format!("{dt}"),
        Data::DateTimeIso(s) | Data::DurationIso(s) => s.clone(),
        Data::Error(e) => format!("#{:?}", e),
        Data::Empty => String::new(),
    }
}

/// 解析 XLSX：每个工作表一张表，文本为各单元格文本拼接
fn parse_xlsx(path: &str) -> Result<ParsedDocument, String> {
    let mut wb = open_workbook_auto(path).map_err(|e| format!("打开 XLSX 失败: {e}"))?;

    let mut tables = Vec::new();
    let mut text = String::new();
    for (name, range) in wb.worksheets() {
        let mut rows = Vec::new();
        for row in range.rows() {
            // calamine 的 Rows 迭代器 Item 为整行切片 &[Data]
            let cells: Vec<String> = row.iter().map(data_to_str).collect();
            if cells.iter().all(|c| c.is_empty()) {
                continue; // 跳过全空行
            }
            text.push_str(&cells.join("\t"));
            text.push('\n');
            rows.push(cells);
        }
        tables.push(Table { name, rows });
    }

    let mut out = ParsedDocument::default();
    out.file_type = "xlsx".to_string();
    out.text = text;
    out.tables = tables;
    Ok(out)
}

/// 解析 DOCX：段落文本 + 表格 + 内嵌图片（docx-rs Reader）
fn parse_docx(path: &str, extract_images: bool) -> Result<ParsedDocument, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("读取 DOCX 失败: {e}"))?;
    let doc = docx_rs::read_docx(&bytes).map_err(|e| format!("解析 DOCX 失败: {e}"))?;

    let mut paragraphs = Vec::new();
    let mut tables = Vec::new();
    for child in &doc.document.children {
        match child {
            docx_rs::DocumentChild::Paragraph(p) => {
                let t = paragraph_text(p);
                if !t.trim().is_empty() {
                    paragraphs.push(t);
                }
            }
            docx_rs::DocumentChild::Table(t) => {
                tables.push(Table {
                    name: format!("表{}", tables.len() + 1),
                    rows: docx_table_rows(t),
                });
            }
            _ => {}
        }
    }
    let text = paragraphs.join("\n");

    let mut images = Vec::new();
    if extract_images {
        for (idx, (_rid, path, image, _png)) in doc.images.iter().enumerate() {
            let bytes = &image.0;
            images.push(ExtractedImage {
                index: idx,
                format: detect_by_ext(path).to_string(),
                width: 0,
                height: 0,
                size_bytes: bytes.len(),
                data_base64: base64_encode(bytes),
            });
        }
    }

    let mut out = ParsedDocument::default();
    out.file_type = "docx".to_string();
    out.text = text;
    out.paragraphs = paragraphs;
    out.tables = tables;
    out.images = images;
    Ok(out)
}

/// 提取 Paragraph 内全部文字（含超链接内的文字）
fn paragraph_text(p: &docx_rs::Paragraph) -> String {
    let mut out = String::new();
    for child in &p.children {
        match child {
            docx_rs::ParagraphChild::Run(r) => {
                for rc in &r.children {
                    if let docx_rs::RunChild::Text(t) = rc {
                        out.push_str(&t.text);
                    } else if let docx_rs::RunChild::Break(bk) = rc {
                        let _ = bk;
                        out.push('\n');
                    }
                }
            }
            docx_rs::ParagraphChild::Hyperlink(h) => {
                for sub in &h.children {
                    if let docx_rs::ParagraphChild::Run(r) = sub {
                        for rc in &r.children {
                            if let docx_rs::RunChild::Text(t) = rc {
                                out.push_str(&t.text);
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }
    out
}

/// 从 docx-rs Table 递归提取二维字符串表
fn docx_table_rows(t: &docx_rs::Table) -> Vec<Vec<String>> {
    let mut out = Vec::new();
    for child in &t.rows {
        // TableChild/TableRowChild 均为单变体枚举，可直接解构
        let docx_rs::TableChild::TableRow(row) = child;
        let mut cells = Vec::new();
        for c in &row.cells {
            let docx_rs::TableRowChild::TableCell(cell) = c;
            let mut cell_text = String::new();
            for content in &cell.children {
                if let docx_rs::TableCellContent::Paragraph(p) = content {
                    let t = paragraph_text(p);
                    if !cell_text.is_empty() && !t.is_empty() {
                        cell_text.push(' ');
                    }
                    cell_text.push_str(&t.trim());
                }
            }
            cells.push(cell_text);
        }
        out.push(cells);
    }
    out
}

/// 解析 PPTX：读取 ppt/slides/slide*.xml 提取 <a:t> 文本
fn parse_pptx(path: &str) -> Result<ParsedDocument, String> {
    let file = std::fs::File::open(path).map_err(|e| format!("打开 PPTX 失败: {e}"))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("PPTX 不是有效的 zip: {e}"))?;

    // 收集并排序 ppt/slides/slideN.xml
    let mut slide_entries: Vec<(u32, String, usize)> = Vec::new();
    for i in 0..archive.len() {
        let name = archive
            .by_index(i)
            .map_err(|e| format!("读取 zip 目录失败: {e}"))?
            .name()
            .to_string();
        if let Some(num) = slide_number(&name) {
            slide_entries.push((num, name.clone(), 0));
        }
    }
    slide_entries.sort_by_key(|(n, _, _)| *n);

    let mut paragraphs = Vec::new();
    for (num, name, _) in &slide_entries {
        let mut entry = archive
            .by_name(name)
            .map_err(|e| format!("读取 {name} 失败: {e}"))?;
        let mut xml = String::new();
        std::io::Read::read_to_string(&mut entry, &mut xml)
            .map_err(|e| format!("读取 {name} 内容失败: {e}"))?;
        let text = slide_xml_text(&xml);
        // 幻灯片内保留换行
        paragraphs.push(format!("【幻灯片 {num}】\n{}", text));
    }

    let text = paragraphs.join("\n\n");
    let mut out = ParsedDocument::default();
    out.file_type = "pptx".to_string();
    out.text = text;
    out.paragraphs = paragraphs;
    Ok(out)
}

/// 从文件名提取幻灯片序号：slide12.xml → 12
fn slide_number(name: &str) -> Option<u32> {
    let base = name.strip_prefix("ppt/slides/slide")?;
    let base = base.strip_suffix(".xml")?;
    base.parse().ok()
}

/// 从 PPTX slide XML 提取文本（a:t 文本 + a:br 换行 + a:p 段落）
fn slide_xml_text(xml: &str) -> String {
    let mut out = String::new();
    let b = xml.as_bytes();
    let mut i = 0usize;
    while i < b.len() {
        if b[i..].starts_with(b"</a:p>") {
            out.push('\n');
            i += 6;
            continue;
        }
        if b[i..].starts_with(b"<a:br") {
            // <a:br/> 或 <a:br r:.../>：另起一行，跳到该标签结束处
            if let Some(e) = b[i..].iter().position(|&c| c == b'>') {
                out.push('\n');
                i += e + 1;
                continue;
            }
        }
        if b[i..].starts_with(b"<a:t") {
            // 找到标签结束 '>'
            if let Some(g) = b[i..].iter().position(|&c| c == b'>') {
                let content_start = i + g + 1;
                // 找内容结束 </a:t>
                if let Some(e) = b[content_start..].windows(6).position(|w| w == b"</a:t>") {
                    let end = content_start + e;
                    out.push_str(&String::from_utf8_lossy(&b[content_start..end]));
                    i = end + 6;
                    continue;
                }
            }
        }
        i += 1;
    }
    // 去掉行尾连续空行
    while out.ends_with('\n') {
        out.pop();
    }
    out
}

/// 依据扩展名猜格式
fn detect_by_ext(p: &str) -> &'static str {
    let l = p.to_ascii_lowercase();
    if l.ends_with(".png") {
        "png"
    } else if l.ends_with(".jpg") || l.ends_with(".jpeg") {
        "jpeg"
    } else if l.ends_with(".gif") {
        "gif"
    } else if l.ends_with(".bmp") {
        "bmp"
    } else if l.ends_with(".tiff") || l.ends_with(".tif") {
        "tiff"
    } else {
        "unknown"
    }
}