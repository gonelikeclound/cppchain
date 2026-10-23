// examples/vision_basic —— 视觉/图像识别（ImageBlock → image_url）离线 demo。
// 消息层：TextBlock + ImageBlock（base64 内联 / 外链两种）；wire 把 content 从字符串升为
// parts 数组（{"type":"text"} + {"type":"image_url","image_url":{url}}）。
// 本 demo 离线：FakeTransport 脚本化回复，OpenAIChatModel 走完整 invoke 路径；
// 最后从抓到的请求体打印**真实发出的 image_url**（data:image/png;base64,…）。
#include <print>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"
#include "cppchain/model/openai/wire.hpp"  // message_to_openai / build_request 在此

using namespace cppchain;
using namespace cppchain::model;
using Json = nlohmann::json;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }

// 一张 1x1 红色 PNG 的 base64（小到内联无负担）。
constexpr char kTinyPngBase64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg==";

}  // namespace

int main() {
  // —— 视觉消息：系统 + 人类（文字 + 内联图）——
  message::Message sys = message::system_message("你是图像识别助手，告诉我图里有什么。");
  message::Message human;
  human.type = message::Role::Human;
  human.content = {
      message::TextBlock{"这张图里有什么？"},
      message::ImageBlock{.media_type = "image/png",
                          .data = kTinyPngBase64,
                          .url = std::nullopt,
                          .id = std::nullopt},
  };
  std::vector<message::Message> const msgs{sys, human};

  // —— wire 预览：含图消息 → content 数组 + image_url data URL ——
  std::println("── 出站 wire（含图消息）──");
  std::println("{}", openai::message_to_openai(human).dump(2));

  // —— 离线识别：FakeTransport 脚本化回复，真 invoke 路径 ——
  auto transport = std::make_shared<http::FakeTransport>();
  ModelConfig cfg;
  cfg.api_key = "demo-key";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-vision";
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);

  transport->push(http::FakeTransport::Script{
      .status = 200,
      .chunks = {
          sse(R"({"choices":[{"delta":{"content":"图里是"},"finish_reason":null}]})"),
          sse(R"({"choices":[{"delta":{"content":"一个红点。"},"finish_reason":"stop"}]})"),
          sse(R"({"choices":[],"usage":{"prompt_tokens":12,"completion_tokens":7,"total_tokens":19}})"),
          "data: [DONE]\n\n",
      },
      .headers = {},
      .error = {},
      .timeout = false,
      .delay_ms = 0});

  std::println("\n── 离线识别（脚本化回复）──");
  auto r = model->invoke(msgs, CallOptions{});
  if (r) {
    std::println("[识别结果] {}", r->text());
  } else {
    std::println("[识别失败] kind={} {}", static_cast<int>(r.error().kind), r.error().message);
    return 1;
  }

  // —— 抓请求体，证明 image_url 真发出了 ——
  if (!transport->requests().empty()) {
    Json const body = Json::parse(transport->requests()[0].body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_discarded() && body.contains("messages")) {
      std::println("\n── 实际走出的 user.content（从请求体回读）──");
      std::println("{}", body["messages"][1]["content"].dump(2));
    }
  }
  return 0;
}