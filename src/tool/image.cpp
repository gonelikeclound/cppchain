// cppchain —— tool/image.cpp
// read_image 工具实现：本地文件读字节 / URL 经 http_json 拉原始响应体 → base64 + media_type。
#include "cppchain/tool/image.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppchain/tool/http_json.hpp"

namespace cppchain::tool {

using Json = nlohmann::json;

namespace {

// 图片体上限（防超大 base64 撑爆 prompt；正常截图远小于此）。
constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;

// 标准 base64 编码（无换行）。
std::string base64_encode(std::string_view in) {
  constexpr char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned const v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += k[(v >> 6) & 63];
    out += k[v & 63];
  }
  if (i + 1 == in.size()) {
    unsigned const v = static_cast<unsigned char>(in[i]) << 16;
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    unsigned const v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8);
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += k[(v >> 6) & 63];
    out += "=";
  }
  return out;
}

// 扩展名 → media_type（本地路径用；识别不了给 octet-stream，调用方仍可发）。
std::string media_type_from_ext(std::string_view p) {
  auto const dot = p.find_last_of('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string e;
  for (char c : p.substr(dot + 1))
    e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (e == "png") return "image/png";
  if (e == "jpg" || e == "jpeg") return "image/jpeg";
  if (e == "gif") return "image/gif";
  if (e == "webp") return "image/webp";
  if (e == "bmp") return "image/bmp";
  if (e == "svg") return "image/svg+xml";
  return "application/octet-stream";
}

// 从响应头取 Content-Type（去掉 ;charset 等参数；键大小写敏感，curl 原样透传）。
std::string content_type_from_headers(std::vector<std::string> const& hs) {
  for (std::string const& h : hs) {
    auto const colon = h.find(':');
    if (colon == std::string::npos || h.substr(0, colon) != "Content-Type") continue;
    std::string v = h.substr(colon + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    auto const semi = v.find(';');
    if (semi != std::string::npos) v = v.substr(0, semi);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
  }
  return {};
}

}  // namespace

ToolSpec make_read_image_tool(std::shared_ptr<http::Transport> transport) {
  ToolSpec spec;
  spec.name = "read_image";
  spec.description = "读取一张图片（本地文件 path 或远程 URL），返回 base64 数据 + 媒体类型，"
                     "供图像识别（多模态模型）使用";
  spec.parameters = Json{{"type", "object"},
                         {"properties",
                          Json{{"path", Json{{"type", "string"}, {"description", "本地图片文件路径（与 url 二选一）"}}},
                               {"url", Json{{"type", "string"}, {"description", "远程图片 URL（与 path 二选一）"}}}}},
                         {"oneOf", Json::array({Json{{"required", Json::array({"path"})}},
                                                Json{{"required", Json::array({"url"})}}})}};
  spec.read_only = true;  // 只读，无副作用

  spec.handler = [transport](Json const& args,
                             tool::ToolContext& ctx) -> std::expected<Json, ToolError> {
    std::string const path = args.value("path", "");
    std::string const url = args.value("url", "");
    bool const has_path = !path.empty(), has_url = !url.empty();
    if (has_path == has_url)
      return std::unexpected(ToolError{"read_image 需恰好一个：path（本地文件）或 url（远程图片）"});

    std::string bytes, media_type, source;
    if (has_path) {
      source = path;
      std::ifstream in(path, std::ios::binary);
      if (!in) return std::unexpected(ToolError{"read_image 打不开 '" + path + "'"});
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      media_type = media_type_from_ext(path);
    } else {
      if (!transport)
        return std::unexpected(ToolError{"read_image 的 URL 路径需要注入 http transport"});
      source = url;
      http::HttpRequest req;
      req.url = url;
      req.method = "GET";
      auto resp = http_json(*transport, req, ctx.cancel);
      if (!resp) return std::unexpected(resp.error());
      if (resp->status < 200 || resp->status >= 300)
        return std::unexpected(ToolError{"read_image 拉取 '" + url + "' 返回 HTTP " +
                                         std::to_string(resp->status)});
      bytes = std::move(resp->raw);  // 原始响应体（二进制可含 NUL，std::string 可承载）
      media_type = content_type_from_headers(resp->headers);
      if (media_type.empty()) media_type = media_type_from_ext(url);
    }

    if (bytes.size() > kMaxImageBytes)
      return std::unexpected(ToolError{
          "read_image 图片过大（>" + std::to_string(kMaxImageBytes / 1024 / 1024) + "MB），拒绝注入"});
    return Json{{"media_type", std::move(media_type)},
                {"data", base64_encode(bytes)},
                {"source", std::move(source)},
                {"bytes", bytes.size()}};
  };
  return spec;
}

message::Message make_image_message(std::string text, std::string media_type,
                                    std::string base64_data) {
  message::Message m;
  m.type = message::Role::Human;
  m.content = {message::TextBlock{std::move(text)},
               message::ImageBlock{.media_type = std::move(media_type),
                                   .data = std::move(base64_data),
                                   .url = std::nullopt,
                                   .id = std::nullopt}};
  return m;
}

}  // namespace cppchain::tool