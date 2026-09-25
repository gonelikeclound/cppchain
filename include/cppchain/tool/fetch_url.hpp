// cppchain —— tool/fetch_url.hpp
// 网页正文提取工具（fetch_url）：给定 URL，GET 拉取 HTML，剥离脚本/样式/标签，
// 解码实体 → 纯文本 + 标题。
// 补全检索闭环：web_search 拿文档列表 → fetch_url 打开详情页读正文。
// 依赖注入 http::Transport（测试用 FakeTransport 离线跑）；无需密钥。
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct FetchUrlConfig {
  long default_max_chars = 4000;  // 返回正文上限（UTF-8 安全截断）
  long max_chars_cap = 20000;     // 调用方可请求的字数上限
  std::string ca_info;            // 企业代理/自签 CA
  long timeout_ms = 20000;
  // 页面快照时效性中等：30 分钟后视为可能已过期（由 agent::StaleStateGuard 消费）。
  std::chrono::seconds ttl = std::chrono::minutes(30);
};

// 纯函数：HTML → 纯文本（剥离 script/style/注释/标签，解码常见实体，压平空白）。
// 供离线单测与复用（arxiv/wiki 等返回清洗也会用）。
[[nodiscard]] std::string html_to_text(std::string_view html);

// 提取 <title>…</title> 的内容（解码 + 去空白；无标题 → 空串）。
[[nodiscard]] std::string extract_title(std::string_view html);

// 生成 fetch_url 工具。参数：url（必填）、max_chars（可选 1–cap，默认 default_max_chars）。
// 返回：{url, status, title, content, source}。
// 传输失败 / 非 2xx / 提取不出正文 → unexpected(ToolError)，让模型换源或如实告知。
[[nodiscard]] ToolSpec make_fetch_url_tool(std::shared_ptr<http::Transport> transport,
                                           FetchUrlConfig cfg = {});
}  // namespace cppchain::tool