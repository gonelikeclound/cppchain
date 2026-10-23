// read_image 工具测试：本地文件 / URL → base64 + media_type；make_image_message 组装带图 Human。
// 离线：本地路径直接读文件；URL 走 FakeTransport（脚本回二进制 + Content-Type 头）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/tool/image.hpp"

using namespace cppchain;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolError;
using cppchain::tool::ToolSpec;
using Json = nlohmann::json;

namespace {

// 1x1 红色 PNG 的原始字节（68→70 字节）与其 base64。
unsigned char const kTinyPng[70] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc,
    0xcf, 0xc0, 0x50, 0x0f, 0x00, 0x04, 0x85, 0x01, 0x80, 0x84, 0xa9, 0x8c, 0x21, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
char const* kTinyPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg==";

std::filesystem::path write_png_file() {
  auto const p = std::filesystem::temp_directory_path() / "cppchain_read_image_test.png";
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<char const*>(kTinyPng), sizeof kTinyPng);
  return p;
}

}  // namespace

TEST(ReadImage, local_png_returns_base64_and_media_type) {
  auto const p = write_png_file();
  ToolSpec const spec = cppchain::tool::make_read_image_tool(nullptr);  // 本地路径不需 transport
  ToolContext ctx;
  auto r = spec.handler(Json{{"path", p.string()}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ((*r)["media_type"], "image/png");
  EXPECT_EQ((*r)["data"], kTinyPngBase64);   // 逐字节 base64 相等
  EXPECT_EQ((*r)["bytes"], 70u);
  EXPECT_EQ((*r)["source"], p.string());
}

TEST(ReadImage, local_missing_file_fails_closed) {
  ToolSpec const spec = cppchain::tool::make_read_image_tool(nullptr);
  ToolContext ctx;
  auto r = spec.handler(Json{{"path", "/nonexistent/no_such.png"}}, ctx);
  EXPECT_FALSE(r.has_value());
}

TEST(ReadImage, requires_exactly_one_of_path_or_url) {
  ToolSpec const spec = cppchain::tool::make_read_image_tool(nullptr);
  ToolContext ctx;
  EXPECT_FALSE(spec.handler(Json::object(), ctx).has_value());            // 都没给
  EXPECT_FALSE(spec.handler(Json{{"path", "a.png"}, {"url", "http://x"}}, ctx).has_value());  // 都给了
}

TEST(ReadImage, url_needs_transport_fails_closed) {
  ToolSpec const spec = cppchain::tool::make_read_image_tool(nullptr);
  ToolContext ctx;
  auto r = spec.handler(Json{{"url", "https://example.com/x.png"}}, ctx);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("transport"), std::string::npos);
}

TEST(ReadImage, url_fetches_bytes_and_content_type_header) {
  http::FakeTransport tx;
  tx.push(http::FakeTransport::Script{
      .status = 200,
      .chunks = {std::string(reinterpret_cast<char const*>(kTinyPng), sizeof kTinyPng)},
      .headers = {"Content-Type: image/png"},
      .error = {}, .timeout = false, .delay_ms = 0});
  auto const spec = cppchain::tool::make_read_image_tool(std::shared_ptr<http::Transport>(&tx, [](auto){}));
  ToolContext ctx;
  auto r = spec.handler(Json{{"url", "https://example.com/x.png"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ((*r)["media_type"], "image/png");   // 头里的 Content-Type 优先
  EXPECT_EQ((*r)["data"], kTinyPngBase64);      // 二进制体 → base64
  EXPECT_EQ((*r)["bytes"], 70u);
}

// make_image_message：把 base64 组装成带图 Human 消息（TextBlock + ImageBlock）。
TEST(ReadImage, make_image_message_builds_human_with_image) {
  auto m = cppchain::tool::make_image_message("这是啥？", "image/png", kTinyPngBase64);
  ASSERT_EQ(m.type, cppchain::message::Role::Human);
  ASSERT_EQ(m.content.size(), 2u);
  EXPECT_TRUE(m.has_images());
  auto const* img = std::get_if<cppchain::message::ImageBlock>(&m.content[1]);
  ASSERT_NE(img, nullptr);
  EXPECT_EQ(img->media_type.value_or(""), "image/png");
  EXPECT_EQ(img->data.value_or(""), kTinyPngBase64);
}