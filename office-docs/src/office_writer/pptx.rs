//! 自研最小可用 PPTX 生成器（OOPP 直写 OOXML）。
//!
//! 背景：文档推荐的 `pptx-rs` 在 crates.io 上是空壳占位 crate（仅有 Hello World，
//! 无 library target），无法作为依赖。此模块基于 `zip` 直接拼装一个最小但合法的
//! PPTX 包：一张空白母版 + 一张空白版式 + N 页幻灯片（标题 + 正文）。
//!
//! 能力边界（与 docs/README.md §3 一致）：无动画、母版继承、复杂图表；仅适合
//! “基础幻灯片”。复杂 PPT 建议走后端服务。

use crate::model::PptxDeck;
use std::io::Write;

/// 构造 PPTX 的字节流
pub fn build_pptx(deck: &PptxDeck) -> Result<Vec<u8>, String> {
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut zip = zip::ZipWriter::new(&mut buf);

        let add = |zip: &mut zip::ZipWriter<&mut std::io::Cursor<Vec<u8>>>,
                       name: &str,
                       data: &[u8]| {
            let opts: zip::write::SimpleFileOptions = zip::write::FileOptions::default();
            zip.start_file(name, opts)
                .map_err(|e| format!("zip 写入 {name} 失败: {e}"))?;
            zip.write_all(data)
                .map_err(|e| format!("zip 写入 {name} 失败: {e}"))
        };

        // 根据幻灯片数量生成内容类型
        let content_types = build_content_types(deck.slides.len());
        add(&mut zip, "[Content_Types].xml", content_types.as_bytes())?;
        add(&mut zip, "_rels/.rels", RELS_ROOT.as_bytes())?;
        add(&mut zip, "docProps/app.xml", build_app_props(deck.slides.len()).as_bytes())?;
        add(&mut zip, "docProps/core.xml", build_core_props(deck).as_bytes())?;

        add(&mut zip, "ppt/presentation.xml", build_presentation(deck.slides.len()).as_bytes())?;
        add(&mut zip, "ppt/_rels/presentation.xml.rels", build_presentation_rels(deck.slides.len()).as_bytes())?;

        add(&mut zip, "ppt/theme/theme1.xml", THEME.as_bytes())?;
        add(&mut zip, "ppt/slideMasters/slideMaster1.xml", MASTER.as_bytes())?;
        add(&mut zip, "ppt/slideMasters/_rels/slideMaster1.xml.rels", MASTER_RELS.as_bytes())?;
        add(&mut zip, "ppt/slideLayouts/slideLayout1.xml", LAYOUT.as_bytes())?;
        add(&mut zip, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", LAYOUT_RELS.as_bytes())?;

        for (i, slide) in deck.slides.iter().enumerate() {
            let n = i + 1;
            let xml = build_slide(slide.title.as_deref().unwrap_or(""), &slide.body);
            add(&mut zip, &format!("ppt/slides/slide{n}.xml"), xml.as_bytes())?;
            let rels = RELS_SLIDE.to_string();
            add(&mut zip, &format!("ppt/slides/_rels/slide{n}.xml.rels"), rels.as_bytes())?;
        }

        zip.finish()
            .map_err(|e| format!("zip 收尾失败: {e}"))?;
    }
    Ok(buf.into_inner())
}

// 文件写入统一走 build_pptx 内的闭包（start_file + write_all），无需全局类型别名
const XML_DECL: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>"#;

// ---------------------------------------------------------------------------
// 静态或半静态 XML
// ---------------------------------------------------------------------------

const NS_A: &str = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"";
const NS_R: &str = "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
const NS_P: &str = "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

fn build_content_types(slide_count: usize) -> String {
    let mut s = String::from(XML_DECL);
    s.push_str(r#"<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">"#);
    s.push_str(r#"<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>"#);
    s.push_str(r#"<Default Extension="xml" ContentType="application/xml"/>"#);
    s.push_str(r#"<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>"#);
    s.push_str(r#"<Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>"#);
    s.push_str(r#"<Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>"#);
    s.push_str(r#"<Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>"#);
    for i in 1..=slide_count {
        s.push_str(&format!(
            r#"<Override PartName="/ppt/slides/slide{i}.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>"#
        ));
    }
    s.push_str(r#"<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>"#);
    s.push_str(r#"<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>"#);
    s.push_str("</Types>");
    s
}

const RELS_ROOT: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>"#;

fn build_presentation(slide_count: usize) -> String {
    let mut s = String::from(XML_DECL);
    s.push_str(&format!("<p:presentation {NS_A} {NS_R} {NS_P}>"));
    s.push_str(r#"<p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>"#);
    s.push_str("<p:sldIdLst>");
    for i in 1..=slide_count {
        s.push_str(&format!(r#"<p:sldId id="{i}" r:id="rId{}"/>"#, rid(i)));
    }
    s.push_str("</p:sldIdLst>");
    s.push_str(r#"<p:sldSz cx="12192000" cy="6858000"/>"#);
    s.push_str(r#"<p:notesSz cx="6858000" cy="9144000"/>"#);
    s.push_str("</p:presentation>");
    s
}

fn rid(i: usize) -> usize {
    // 幻灯片 rel id 从 rId2 开始（rId1 留给母版）
    i + 1
}

fn build_presentation_rels(slide_count: usize) -> String {
    let mut s = String::from(XML_DECL);
    s.push_str(r#"<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">"#);
    s.push_str(r#"<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>"#);
    for i in 1..=slide_count {
        s.push_str(&format!(
            r#"<Relationship Id="rId{rid}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide{i}.xml"/>"#,
            rid = i + 1
        ));
    }
    // 主题关系
    s.push_str(&format!(
        r#"<Relationship Id="rId{rid}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="theme/theme1.xml"/>"#,
        rid = slide_count + 2
    ));
    s.push_str("</Relationships>");
    s
}

const MASTER: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldMaster xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
             xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
             xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>
    </p:spTree>
  </p:cSld>
  <p:clrMap accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" bg1="lt1" bg2="lt2" folHlink="folHlink" hlink="hlink" tx1="dk1" tx2="dk2"/>
  <p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rId1"/></p:sldLayoutIdLst>
  <p:txStyles>
    <p:titleStyle><a:lvl1pPr><a:defRPr sz="4000"><a:solidFill><a:srgbClr val="262626"/></a:solidFill></a:defRPr></a:lvl1pPr></p:titleStyle>
    <p:bodyStyle><a:lvl1pPr><a:defRPr sz="1800"><a:solidFill><a:srgbClr val="404040"/></a:solidFill></a:defRPr></a:lvl1pPr></p:bodyStyle>
    <p:otherStyle/>
  </p:txStyles>
</p:sldMaster>"#;

const MASTER_RELS: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="../theme/theme1.xml"/>
</Relationships>"#;

const LAYOUT: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldLayout xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
             xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
             xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" type="blank">
  <p:cSld name="Blank">
    <p:spTree>
      <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>
    </p:spTree>
  </p:cSld>
  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sldLayout>"#;

const LAYOUT_RELS: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="../slideMasters/slideMaster1.xml"/>
</Relationships>"#;

const RELS_SLIDE: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
</Relationships>"#;

/// 最小主题（PowerPoint 打开的必要组成之一）
const THEME: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="rust_doc_engine">
  <a:themeElements>
    <a:clrScheme name="Office">
      <a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1>
      <a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1>
      <a:dk2><a:srgbClr val="44546A"/></a:dk2>
      <a:lt2><a:srgbClr val="E7E6E6"/></a:lt2>
      <a:accent1><a:srgbClr val="4472C4"/></a:accent1>
      <a:accent2><a:srgbClr val="ED7D31"/></a:accent2>
      <a:accent3><a:srgbClr val="A5A5A5"/></a:accent3>
      <a:accent4><a:srgbClr val="FFC000"/></a:accent4>
      <a:accent5><a:srgbClr val="5B9BD5"/></a:accent5>
      <a:accent6><a:srgbClr val="70AD47"/></a:accent6>
      <a:hlink><a:srgbClr val="0563C1"/></a:hlink>
      <a:folHlink><a:srgbClr val="954F72"/></a:folHlink>
    </a:clrScheme>
    <a:fontScheme name="Office">
      <a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/></a:majorFont>
      <a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/></a:minorFont>
    </a:fontScheme>
    <a:fmtScheme name="Office">
      <a:fillStyleLst>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="50000"/><a:satMod val="300000"/></a:schemeClr></a:gs><a:gs pos="35000"><a:schemeClr val="phClr"><a:tint val="37000"/><a:satMod val="300000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:tint val="15000"/><a:satMod val="350000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="16200000" scaled="1"/></a:gradFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:shade val="51000"/><a:satMod val="130000"/></a:schemeClr></a:gs><a:gs pos="80000"><a:schemeClr val="phClr"><a:shade val="93000"/><a:satMod val="130000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="94000"/><a:satMod val="135000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="16200000" scaled="0"/></a:gradFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="80000"/><a:satMod val="300000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="30000"/><a:satMod val="200000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="16200000" scaled="0"/></a:gradFill>
      </a:fillStyleLst>
      <a:lnStyleLst>
        <a:ln w="9525" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"><a:shade val="95000"/><a:satMod val="105000"/></a:schemeClr></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="25400" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="38100" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
      </a:lnStyleLst>
      <a:effectStyleLst>
        <a:effectStyle><a:effectLst/></a:effectStyle>
        <a:effectStyle><a:effectLst/></a:effectStyle>
        <a:effectStyle><a:effectLst><a:outerShdw blurRad="40000" dist="20000" dir="5400000" rotWithShape="0"><a:srgbClr val="000000"><a:alpha val="40000"/></a:srgbClr></a:outerShdw></a:effectLst></a:effectStyle>
      </a:effectStyleLst>
      <a:bgFillStyleLst>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="40000"/><a:satMod val="350000"/></a:schemeClr></a:gs><a:gs pos="40000"><a:schemeClr val="phClr"><a:tint val="45000"/><a:shade val="99000"/><a:satMod val="350000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="20000"/><a:satMod val="255000"/></a:schemeClr></a:gs></a:gsLst><a:path path="circle"><a:fillToRect l="50000" t="-80000" r="50000" b="180000"/></a:path></a:gradFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="80000"/><a:satMod val="300000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="30000"/><a:satMod val="200000"/></a:schemeClr></a:gs></a:gsLst><a:path path="circle"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path></a:gradFill>
      </a:bgFillStyleLst>
    </a:fmtScheme>
  </a:themeElements>
</a:theme>"#;

fn build_core_props(deck: &PptxDeck) -> String {
    let title = deck
        .slides
        .first()
        .and_then(|s| s.title.clone())
        .unwrap_or_else(|| "Presentation".to_string());
    format!(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
 xmlns:dc="http://purl.org/dc/elements/1.1/"
 xmlns:dcterms="http://purl.org/dc/terms/"
 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>{title}</dc:title>
  <dc:creator>rust_doc_engine</dc:creator>
  <cp:lastModifiedBy>rust_doc_engine</cp:lastModifiedBy>
  <dcterms:created xsi:type="dcterms:W3CDTF">2026-09-30T00:00:00Z</dcterms:created>
  <dcterms:modified xsi:type="dcterms:W3CDTF">2026-09-30T00:00:00Z</dcterms:modified>
</cp:coreProperties>"#
    )
}

fn build_app_props(slide_count: usize) -> String {
    format!(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties">
  <Application>rust_doc_engine</Application>
  <PresentationFormat>宽屏幕</PresentationFormat>
  <SlideSize>12192000x6858000</SlideSize>
  <Slides>{slide_count}</Slides>
  <Notes>0</Notes>
</Properties>"#
    )
}

/// 生成一页幻灯片：标题占位 + 正文占位
fn build_slide(title: &str, body: &[String]) -> String {
    let mut s = String::from(XML_DECL);
    s.push_str(&format!("<p:sld {NS_A} {NS_R} {NS_P}>"));
    s.push_str("<p:cSld><p:spTree>");
    s.push_str(
        r#"<p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
<p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>"#,
    );

    // 标题形状
    if !title.is_empty() {
        s.push_str(
            r#"<p:sp><p:nvSpPr><p:cNvPr id="2" name="标题"/><p:cNvSpPr txBox="1"/><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr>
<p:spPr><a:xfrm><a:off x="457200" y="285750"/><a:ext cx="11277600" cy="1328906"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr>
<p:txBody><a:bodyPr vertOverflow="clip"/><a:lstStyle/>"#,
        );
        s.push_str(&format!(
            r#"<a:p><a:pPr algn="l"/><a:r><a:rPr lang="zh-CN" sz="4000" dirty="0"/><a:t>{}</a:t></a:r></a:p>"#,
            xml_escape(title)
        ));
        s.push_str("</p:txBody></p:sp>");
    }

    // 正文本体
    if !body.is_empty() {
        let id = if title.is_empty() { 2 } else { 3 };
        s.push_str(&format!(
            r#"<p:sp><p:nvSpPr><p:cNvPr id="{id}" name="正文"/><p:cNvSpPr txBox="1"/><p:nvPr><p:ph type="body"/></p:nvPr></p:nvSpPr>
<p:spPr><a:xfrm><a:off x="457200" y="1825625"/><a:ext cx="11277600" cy="4498960"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr>
<p:txBody><a:bodyPr vertOverflow="clip"/><a:lstStyle/>"#
        ));
        for line in body {
            s.push_str(&format!(
                r#"<a:p><a:pPr algn="l" marL="0" indent="0"/><a:r><a:rPr lang="zh-CN" sz="1800" dirty="0"/><a:t>{}</a:t></a:r></a:p>"#,
                xml_escape(line)
            ));
        }
        s.push_str("</p:txBody></p:sp>");
    }

    s.push_str("</p:spTree></p:cSld>");
    s.push_str("<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>");
    s.push_str("</p:sld>");
    s
}

/// XML 文本转义（a:t 内容）
pub(crate) fn xml_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            c => out.push(c),
        }
    }
    out
}