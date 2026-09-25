// demo: tools_frontline —— 第一档补全的 3 个内置工具
//
//   get_datetime  模型不知道"现在几点/星期几" → 联网答时间的基准钟
//   calculator    精确计算兜底（安全表达式求值，非 eval）
//   fetch_url     抓取网页正文 → 补全「web_search 拿列表 → 打开详情页」闭环
//
// 一切离线可跑：前两个纯本地，fetch_url 用 FakeTransport 喂假 HTML；
// 真实场景把 FakeTransport 换成 CurlMultiTransport 即可。
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/fetch_url.hpp>
#include <cppchain/tool/tool.hpp>

#include <expected>
#include <memory>
#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

// 模拟"模型发起一次工具调用"：走统一的 handler 路径，看得见每一步
void run_tool(std::string const& label, ToolSpec const& spec, json args) {
  ToolContext ctx;
  ctx.tool_call_id = "demo";
  auto r = spec.handler(args, ctx);
  std::println("— {} —", label);
  std::println("  参数: {}", args.dump());
  if (r) {
    std::println("  结果: {}", r->dump(2));
  } else {
    std::println("  错误: {}", r.error().message);  // 错误回喂给模型自愈
  }
  std::println("");
}

}  // namespace

int main() {
  std::println("== 第一档工具 demo：get_datetime / calculator / fetch_url ==");
  std::println("");

  // 纯本地工具（零网络零密钥）
  auto const datetime_tool = make_datetime_tool();
  auto const calc_tool = make_calculator_tool();

  // fetch_url：脚本化传输演示抓取一个 HTML 页面
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {R"(<html><head><title>cppchain 项目</title></head><body>
  <h1>cppchain</h1>
  <p>一个 C++26 的 langchain 简化复刻，用于实验 Agent 理论。</p>
  <script>var noise = 1;</script>
  <ul><li>工具层</li><li>状态层</li><li>图引擎</li></ul></body></html>)"};
  tp->push(std::move(s));
  auto const fetch_tool = make_fetch_url_tool(tp);

  run_tool("「现在几点了？」 → get_datetime()", datetime_tool, json::object());
  run_tool("指定时区 → get_datetime(offset=+08:00)", datetime_tool, json{{"offset", "+08:00"}});
  run_tool("口算易错 → calculator((100+50)*0.2)", calc_tool, json{{"expression", "(100+50)*0.2"}});
  run_tool("开方加幂 → calculator(sqrt(144)+2^10)", calc_tool,
           json{{"expression", "sqrt(144)+2^10"}});
  run_tool("除零 → 错误回喂（不是崩溃）→ calculator(1/0)", calc_tool, json{{"expression", "1/0"}});
  run_tool("打开搜索命中详情页 → fetch_url(https://cppchain.example.io)",
           fetch_tool, json{{"url", "https://cppchain.example.io"}});
}