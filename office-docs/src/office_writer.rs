//! Office 文档写入：XLSX（rust_xlsxwriter）、DOCX（docx-rs）、PPTX（自研最小生成器）
//!
//! 三个导出函数都把 Kotlin 传入的 JSON 指令反序列化为 `model` 中的结构，
//! 然后写入指定路径，返回 `Ok(())` 或 `Err(描述)`。

mod pptx;

use crate::model::{DocxBlock, DocxDocument, XlsxBook, XlsxCell};

/// 根据 JSON 指令生成 XLSX
pub fn export_xlsx(out: &str, json: &str) -> Result<(), String> {
    let book: XlsxBook =
        serde_json::from_str(json).map_err(|e| format!("XLSX 指令 JSON 解析失败: {e}"))?;
    if book.sheets.is_empty() {
        return Err("XLSX 指令缺少 sheets".to_string());
    }

    let mut wb = rust_xlsxwriter::Workbook::new();
    for sheet in book.sheets {
        let ws = wb.add_worksheet();
        if let Some(name) = sheet.name.clone().filter(|n| !n.is_empty()) {
            ws.set_name(name)
                .map_err(|e| format!("设置工作表名失败: {e}"))?;
        }
        for (r, row) in sheet.rows.iter().enumerate() {
            for (c, cell) in row.iter().enumerate() {
                write_xlsx_cell(ws, r as u32, c as u16, cell)
                    .map_err(|e| format!("写入单元格 r{r}/c{c} 失败: {e}"))?;
            }
        }
    }
    wb.save(out).map_err(|e| format!("保存 XLSX 失败: {e}"))
}

/// 写单个单元格，按 t 字段区分：
/// - `num`：数值；`formula`：公式；`str`：强制文本；`auto`（默认）：数值优先，否则文本
fn write_xlsx_cell(
    ws: &mut rust_xlsxwriter::Worksheet,
    r: u32,
    c: u16,
    cell: &XlsxCell,
) -> Result<(), rust_xlsxwriter::XlsxError> {
    match cell.t.as_str() {
        // num：解析为数值，解析失败则按文本写入
        "num" => match cell.v.trim().parse::<f64>() {
            Ok(n) => write_xlsx_with_fmt(ws, cell, |ws, f| {
                let r = match f {
                    Some(f) => ws.write_number_with_format(r, c, n, f),
                    None => ws.write_number(r, c, n),
                };
                r.map(|_| ())
            }),
            Err(_) => write_xlsx_string(ws, r, c, cell),
        },
        "formula" => {
            let f = build_format(cell);
            match &f {
                Some(f) => ws.write_formula_with_format(
                    r,
                    c,
                    rust_xlsxwriter::Formula::new(&cell.v),
                    f,
                ),
                None => ws.write_formula(r, c, rust_xlsxwriter::Formula::new(&cell.v)),
            }
            .map(|_| ())
        }
        // str / auto
        _ => {
            if cell.t == "auto" && cell.v.trim().parse::<f64>().is_ok() {
                let n = cell.v.trim().parse::<f64>().unwrap();
                write_xlsx_with_fmt(ws, cell, |ws, f| {
                    let r = match f {
                        Some(f) => ws.write_number_with_format(r, c, n, f),
                        None => ws.write_number(r, c, n),
                    };
                    r.map(|_| ())
                })
            } else {
                write_xlsx_string(ws, r, c, cell)
            }
        }
    }
}

/// 以指定回调写单元格（r、c 供 callback 使用）
fn write_xlsx_with_fmt<F>(
    ws: &mut rust_xlsxwriter::Worksheet,
    cell: &XlsxCell,
    f: F,
) -> Result<(), rust_xlsxwriter::XlsxError>
where
    F: FnOnce(&mut rust_xlsxwriter::Worksheet, Option<&rust_xlsxwriter::Format>)
        -> Result<(), rust_xlsxwriter::XlsxError>,
{
    f(ws, build_format(cell).as_ref())
}

/// 以字符串类型写入单元格
fn write_xlsx_string(
    ws: &mut rust_xlsxwriter::Worksheet,
    r: u32,
    c: u16,
    cell: &XlsxCell,
) -> Result<(), rust_xlsxwriter::XlsxError> {
    match build_format(cell) {
        Some(f) => ws.write_string_with_format(r, c, &cell.v, &f).map(|_| ()),
        None => ws.write_string(r, c, &cell.v).map(|_| ()),
    }
}

fn build_format(cell: &XlsxCell) -> Option<rust_xlsxwriter::Format> {
    if !cell.bold && cell.font_size.is_none() {
        return None;
    }
    let mut f = rust_xlsxwriter::Format::new();
    if cell.bold {
        f = f.set_bold();
    }
    if let Some(sz) = cell.font_size {
        f = f.set_font_size(sz);
    }
    Some(f)
}

/// 根据 JSON 指令生成 DOCX
pub fn export_docx(out: &str, json: &str) -> Result<(), String> {
    let doc: DocxDocument =
        serde_json::from_str(json).map_err(|e| format!("DOCX 指令 JSON 解析失败: {e}"))?;

    let mut d = docx_rs::Docx::new();

    // 标题作为文档第一段（加粗大字号）
    if let Some(title) = doc.title.clone().filter(|t| !t.is_empty()) {
        d = d.add_paragraph(
            docx_rs::Paragraph::new()
                .add_run(docx_rs::Run::new().add_text(title).bold().size(36)),
        );
    }

    for block in &doc.body {
        d = match block {
            DocxBlock::P { text } => d.add_paragraph(
                docx_rs::Paragraph::new().add_run(docx_rs::Run::new().add_text(text)),
            ),
            DocxBlock::H { text, level } => {
                // 标题字号从 30 阶梯递减，加粗
                let sz = 30usize.saturating_sub(((*level).clamp(1, 6) as usize - 1) * 3);
                d.add_paragraph(
                    docx_rs::Paragraph::new()
                        .add_run(docx_rs::Run::new().add_text(text).bold().size(sz)),
                )
            }
            DocxBlock::Table { rows } => {
                let table = build_docx_table(rows);
                d.add_table(table)
            }
            DocxBlock::Br => d.add_paragraph(docx_rs::Paragraph::new()),
        };
    }

    let xml_docx = d.build();
    let mut f = std::fs::File::create(out).map_err(|e| format!("创建 DOCX 文件失败: {e}"))?;
    xml_docx
        .pack(&mut f)
        .map_err(|e| format!("写入 DOCX 失败: {e}"))
}

/// 把字符串二维数组转成 docx-rs Table
fn build_docx_table(rows: &[Vec<String>]) -> docx_rs::Table {
    let table_rows: Vec<docx_rs::TableRow> = rows
        .iter()
        .map(|row| {
            let cells = row
                .iter()
                .map(|cell_text| {
                    docx_rs::TableCell::new()
                        .add_paragraph(docx_rs::Paragraph::new().add_run(
                            docx_rs::Run::new().add_text(cell_text),
                        ))
                })
                .collect();
            docx_rs::TableRow::new(cells)
        })
        .collect();
    docx_rs::Table::new(table_rows)
}

/// 根据 JSON 指令生成 PPTX（最小可用生成器，见 pptx 子模块）
pub fn export_pptx(out: &str, json: &str) -> Result<(), String> {
    let deck: crate::model::PptxDeck =
        serde_json::from_str(json).map_err(|e| format!("PPTX 指令 JSON 解析失败: {e}"))?;
    if deck.slides.is_empty() {
        return Err("PPTX 指令缺少 slides".to_string());
    }
    let bytes = pptx::build_pptx(&deck)?;
    std::fs::write(out, &bytes).map_err(|e| format!("写入 PPTX 文件失败: {e}"))
}