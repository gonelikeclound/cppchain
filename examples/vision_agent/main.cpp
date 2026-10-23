// examples/vision_agent —— ③ 看图→调工具→回答 组合 demo（全离线）。
// 闭环：模型决定调 read_image → 工具读图得 base64 → make_image_message 注入 ImageBlock
// → 视觉模型"看图"回答。FakeTransport 脚本化两轮回复（tool_call / 识别答案），零网络。
#include <print>
#include <string>
#include <vector>

#include <fstream>
#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"
#include "cppchain/tool/image.hpp"
#include "cppchain/tool/registry.hpp"

using namespace cppchain;
using namespace cppchain::model;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolSpec;
using Json = nlohmann::json;

namespace {

// 1x1 红色 PNG 的原始字节（70 字节）。
unsigned char const kTinyPng[70] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc,
    0xcf, 0xc0, 0x50, 0x0f, 0x00, 0x04, 0x85, 0x01, 0x80, 0x84, 0xa9, 0x8c, 0x21, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

// invoke 底层是 stream_async → 脚本要走 SSE 帧（与 openai_model_test 同形）。
std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
char const* kDone = "data: [DONE]\n\n";

}  // namespace

int main() {
  // 1) 给 read_image 一张真图：写 tmp/vision_sample.png（写完必须 flush/close，否则 ifstream 读空）
  std::ofstream out("tmp/vision_sample.png", std::ios::binary);
  out.write(reinterpret_cast<char const*>(kTinyPng), sizeof kTinyPng);
  out.close();
  std::string const img_path = "tmp/vision_sample.png";

  auto transport = std::make_shared<http::FakeTransport>();
  ModelConfig cfg;
  cfg.api_key = "demo-key";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-vision";
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);

  tool::ToolRegistry reg;
  reg.add(tool::make_read_image_tool(nullptr));  // 本地路径，不需注入 transport

  // 2) 第 1 轮：系统 + 用户（要图）→ 脚本化"模型决定调 read_image"
  std::string const tool_args = Json{{"path", img_path}}.dump();
  // delta 里的 arguments 是"JSON 字符串"，需再经 json().dump() 转义（内层引号才能合法）。
  std::string const tool_delta =
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1",)"
      R"("function":{"name":"read_image","arguments":)" +
      Json(tool_args).dump() + R"(}}]}}]})";
  transport->push(http::FakeTransport::Script{
      .status = 200,
      .chunks = {sse(tool_delta),
                 sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
                 kDone},
      .headers = {}, .error = {}, .timeout = false, .delay_ms = 0});

  std::vector<message::Message> msgs{
      message::system_message("你是图像识别助手：先调 read_image 看图，再回答。"),
      message::human_message("这张图里有什么？（图片文件：" + img_path + "）")};
  CallOptions opts1;
  opts1.tools = reg.to_openai_tools();
  auto round1 = model->invoke(msgs, opts1);
  if (!round1 || round1->tool_calls.empty()) { std::println("[失败] 第一轮没出工具调用"); return 1; }
  std::println("── 第 1 轮：模型决定调用 {} → {}", round1->tool_calls[0].name,
               round1->tool_calls[0].args.dump());

  // 3) 执行 read_image（如同 agent 的 ToolNode 干的事）
  ToolSpec const* ri = reg.find("read_image");
  ToolContext ctx;
  auto pic = ri->handler(round1->tool_calls[0].args, ctx);
  if (!pic) { std::println("[失败] read_image：{}", pic.error().message); return 1; }
  std::string const media_type = (*pic)["media_type"];
  std::string const b64 = (*pic)["data"];
  std::println("── 工具执行：media={} bytes={} base64 前 20={}…", media_type,
               (*pic)["bytes"].get<std::size_t>(), b64.substr(0, 20));

  // 4) 桥接：工具结果 → ImageBlock 用户消息（make_image_message）
  message::Message vision = tool::make_image_message("这张图里有什么？", media_type, b64);
  std::println("── 注入视觉消息：has_images={} 块数={}", vision.has_images(), vision.content.size());

  // 5) 第 2 轮：带图调用 → 脚本化识别答案
  transport->push(http::FakeTransport::Script{
      .status = 200,
      .chunks = {sse(R"({"choices":[{"delta":{"content":"图里是"},"finish_reason":null}]})"),
                 sse(R"({"choices":[{"delta":{"content":"一个红点。"},"finish_reason":"stop"}]})"),
                 kDone},
      .headers = {}, .error = {}, .timeout = false, .delay_ms = 0});
  std::vector<message::Message> msgs2{msgs[0], msgs[1], std::move(vision)};
  auto round2 = model->invoke(msgs2, CallOptions{});
  if (!round2) { std::println("[失败] 第二轮识别：{}", round2.error().message); return 1; }
  std::println("── 第 2 轮（看图）：{}", round2->text());

  std::println("\n✅ 看图 → 调工具 → 回答 闭环跑通（离线）。真机把 demo-vision 换成 qwen-vl 即识别真图。");
  return 0;
}