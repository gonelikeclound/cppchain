//! 工具函数：文件后缀判断、JSON 错误构造、base64 编码、安全检查

use crate::model::DocType;
use std::path::Path;

/// 根据文件后缀判断文档类型（大小写不敏感）
pub fn detect_type(path: &str) -> DocType {
    let ext = Path::new(path)
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    match ext.as_str() {
        "pdf" => DocType::Pdf,
        "docx" => DocType::Docx,
        "xlsx" => DocType::Xlsx,
        "pptx" => DocType::Pptx,
        _ => DocType::Unknown,
    }
}

/// 构造统一格式的错误 JSON：{"error": "..."}
pub fn err_json(msg: impl AsRef<str>) -> String {
    // 手动转义，避免引入额外依赖
    format!("{{\"error\":\"{}\"}}", escape_json(msg.as_ref()))
}

/// 将任意字符串转义为 JSON 字符串字面量（不含外层引号）
pub fn escape_json(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// 检查文件存在性与大小
/// - `max_bytes`：允许的最大字节数（DoS 防护），None 表示不限制
pub fn check_path(path: &str, max_bytes: Option<u64>) -> Result<u64, String> {
    let meta = std::fs::metadata(path).map_err(|e| format!("无法访问文件: {e}"))?;
    if !meta.is_file() {
        return Err("路径不是文件".to_string());
    }
    if let Some(max) = max_bytes {
        if meta.len() > max {
            return Err(format!(
                "文件过大({}字节>{}字节上限)，请加大文档处理上限或分段处理",
                meta.len(),
                max
            ));
        }
    }
    Ok(meta.len())
}

/// 简易 base64 编码（无第三方依赖）
pub fn base64_encode(data: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0];
        let b1 = *chunk.get(1).unwrap_or(&0);
        let b2 = *chunk.get(2).unwrap_or(&0);
        let n = (u32::from(b0) << 16) | (u32::from(b1) << 8) | u32::from(b2);
        out.push(TABLE[(n >> 18) as usize & 63] as char);
        out.push(TABLE[(n >> 12) as usize & 63] as char);
        if chunk.len() > 1 {
            out.push(TABLE[(n >> 6) as usize & 63] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(TABLE[n as usize & 63] as char);
        } else {
            out.push('=');
        }
    }
    out
}

/// 计算 base64 字符串解码后的字节数（仅计数，不做完整解码）
#[allow(dead_code)] // 供测试校验
pub fn base64_decode_len(s: &str) -> Option<usize> {
    let mut valid = 0usize;
    let mut padding = 0usize;
    for c in s.bytes() {
        match c {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'+' | b'/' => {
                if padding > 0 {
                    return None; // 填充后不允许再有内容
                }
                valid += 1;
            }
            b'=' => padding += 1,
            _ => return None,
        }
    }
    let residue = valid % 4;
    // 期望的填充数：1 字节 → 2 个 '='；2 字节 → 1 个 '='；整块 → 0
    let expected_padding = if residue == 0 { 0 } else { 4 - residue };
    if residue == 1 || padding != expected_padding {
        return None;
    }
    let tail = match residue {
        2 => 1, // 剩下 2 个内容字符 → 1 字节
        3 => 2, // 剩下 3 个内容字符 → 2 字节
        _ => 0,
    };
    Some(valid / 4 * 3 + tail)
}

/// 判断文件后缀是否为支持的 OpenXML 类型
#[allow(dead_code)] // 供上层调用方使用
pub fn is_supported(path: &str) -> bool {
    matches!(
        detect_type(path),
        DocType::Pdf | DocType::Docx | DocType::Xlsx | DocType::Pptx
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detect_type_by_extension() {
        assert_eq!(detect_type("a.PDF"), DocType::Pdf);
        assert_eq!(detect_type("b.docx"), DocType::Docx);
        assert_eq!(detect_type("c.XLSX"), DocType::Xlsx);
        assert_eq!(detect_type("d.pptx"), DocType::Pptx);
        assert_eq!(detect_type("e.doc"), DocType::Unknown);
        assert_eq!(detect_type("ノート.pdf"), DocType::Pdf);
    }

    #[test]
    fn base64_roundtrip() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
    }

    #[test]
    fn err_json_is_valid() {
        let e = err_json("含\"引号\"和\n换行");
        assert!(e.contains("\\\""));
        let v: serde_json::Value = serde_json::from_str(&e).unwrap();
        assert!(v["error"].is_string());
    }
}