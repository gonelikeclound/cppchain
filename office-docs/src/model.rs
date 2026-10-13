//! 数据结构与 JSON 序列化定义
//!
//! 上层 Kotlin/Java 通过 JNI 与本引擎交换的数据均为 JSON 字符串：
//! - 解析结果使用 `ParsedDocument` / `DocType` 序列化
//! - 写入指令使用 `XlsxBook` / `DocxDocument` / `PptxDeck` 反序列化

use serde::{Deserialize, Serialize};

/// 支持的文档类型（仅现代 OpenXML + PDF，不支持老式 .doc/.xls/.ppt）
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DocType {
    Pdf,
    Docx,
    Xlsx,
    Pptx,
    Unknown,
}

impl DocType {
    /// 统一的小写扩展名字符串，供 JSON 返回
    #[allow(dead_code)] // 对外模型 API，供 Kotlin/测试使用
    pub fn as_str(&self) -> &'static str {
        match self {
            DocType::Pdf => "pdf",
            DocType::Docx => "docx",
            DocType::Xlsx => "xlsx",
            DocType::Pptx => "pptx",
            DocType::Unknown => "unknown",
        }
    }
}

/// 解析一个文档后的统一结果结构
#[derive(Debug, Default, Serialize)]
pub struct ParsedDocument {
    /// 文档类型：pdf/docx/xlsx/pptx
    pub file_type: String,
    /// 输入文件路径（冗余字段，便于上层排查）
    #[serde(skip_serializing_if = "Option::is_none")]
    pub file_path: Option<String>,
    /// 文档标题（PDF 元信息 / 文档内部标题，缺失为 None）
    #[serde(skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    /// PDF 页数
    #[serde(skip_serializing_if = "Option::is_none")]
    pub page_count: Option<usize>,
    /// 全文拼接文本
    pub text: String,
    /// 分段文本（PDF 按页、DOCX 按段落、PPTX 按幻灯片）
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub paragraphs: Vec<String>,
    /// 表格（XLSX 工作表 / DOCX 表）
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub tables: Vec<Table>,
    /// 文档内图片摘要（数据是否带 base64 由调用参数控制）
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub images: Vec<ExtractedImage>,
    /// 元信息键值对（标题、作者、创建时间等）
    #[serde(default)]
    pub metadata: serde_json::Map<String, serde_json::Value>,
    /// 局部解析失败提示（例如某页提取失败），非致命错误
    #[serde(skip_serializing_if = "Option::is_none")]
    pub warning: Option<String>,
}

/// 一张表格
#[derive(Debug, Serialize)]
pub struct Table {
    /// 表名（XLSX 为工作表名，DOCX 为序号表）
    pub name: String,
    /// 行集合，每行为单元格文本数组
    pub rows: Vec<Vec<String>>,
}

/// 文档内的一张图片
#[derive(Debug, Serialize)]
pub struct ExtractedImage {
    /// 出现顺序索引
    pub index: usize,
    /// 原始格式：jpeg / png / bmp 等
    pub format: String,
    pub width: u32,
    pub height: u32,
    pub size_bytes: usize,
    /// base64 编码的图片数据；未请求时为空字符串
    pub data_base64: String,
}

// ---------------------------------------------------------------------------
// 写入指令（Kotlin 侧序列化后经 JNI 传入）
// ---------------------------------------------------------------------------

/// XLSX 单元格
#[derive(Debug, Clone, Deserialize)]
pub struct XlsxCell {
    /// 单元格值 / 公式字符串
    #[serde(default)]
    pub v: String,
    /// 单元格类型：auto | num | str | formula
    #[serde(default = "default_cell_type")]
    pub t: String,
    #[serde(default)]
    pub bold: bool,
    /// 字体大小；None 使用默认
    #[serde(default)]
    pub font_size: Option<f64>,
}

fn default_cell_type() -> String {
    "auto".to_string()
}

/// 一个工作表
#[derive(Debug, Deserialize)]
pub struct XlsxSheet {
    #[serde(default)]
    pub name: Option<String>,
    pub rows: Vec<Vec<XlsxCell>>,
}

/// 整个工作簿
#[derive(Debug, Deserialize)]
pub struct XlsxBook {
    #[serde(default)]
    pub sheets: Vec<XlsxSheet>,
}

/// DOCX 内容块：段落 / 标题 / 表格 / 空行
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum DocxBlock {
    /// 普通段落
    P { text: String },
    /// 标题（level 1-6）
    H { text: String, level: u8 },
    /// 简单表格，rows 为每行单元格文本
    Table { rows: Vec<Vec<String>> },
    /// 空行
    Br,
}

/// 一个 DOCX 文档
#[derive(Debug, Deserialize)]
pub struct DocxDocument {
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub body: Vec<DocxBlock>,
}

/// 一张 PPTX 幻灯片
#[derive(Debug, Deserialize)]
pub struct PptxSlide {
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub body: Vec<String>,
}

/// 一个 PPTX 演示文稿
#[derive(Debug, Deserialize)]
pub struct PptxDeck {
    pub slides: Vec<PptxSlide>,
}