// demo: tools_arxiv —— arXiv 论文检索工具（官方 Atom API，免费无需密钥）
//
// langchain 参照：ArxivQueryRun。
// 看点：①查询 URL 构造"看得见每一步"；②内置轻量 Atom XML 解析（无第三方 XML 库）；
//      ③空结果/网络错误都错误回喂，模型换词再试。
// 一切离线可跑：FakeTransport 喂样例 Atom feed；真实场景换 CurlMultiTransport 即可。
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/tool/arxiv.hpp>

#include <expected>
#include <memory>
#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

void run_tool(std::string const& label, ToolSpec const& spec, json args) {
  ToolContext ctx;
  ctx.tool_call_id = "demo";
  auto r = spec.handler(args, ctx);
  std::println("— {} —", label);
  std::println("  参数: {}", args.dump());
  if (r) {
    std::println("  结果: {}", r->dump(2));
  } else {
    std::println("  错误: {}", r.error().message);
  }
  std::println("");
}

constexpr std::string_view kFeed = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>ArXiv Query</title>
  <opensearch:totalResults>2</opensearch:totalResults>
  <entry>
    <id>http://arxiv.org/abs/2308.08155v2</id>
    <published>2023-08-16T00:00:00Z</published>
    <title>The Era of 1-bit LLMs: All Large Language Models are in 1.58 Bits</title>
    <summary>We introduce BitNet b1.58, a 1-bit LLM where every parameter is ternary.</summary>
    <author><name>Shuming Ma</name></author>
    <author><name>Hongyu Wang</name></author>
    <arxiv:primary_category term="cs.CL" scheme="http://arxiv.org/schemas/atom"/>
  </entry>
  <entry>
    <id>http://arxiv.org/abs/2404.14219v2</id>
    <published>2024-04-22T00:00:00Z</published>
    <title>OpenDevin: An Open Platform for AI Software Developers as Generalist Agents</title>
    <summary>We introduce OpenDevin, a platform for developing powerful AI agents.</summary>
    <author><name>Xingyao Wang</name></author>
    <arxiv:primary_category term="cs.SE" scheme="http://arxiv.org/schemas/atom"/>
  </entry>
</feed>)";

constexpr std::string_view kEmpty = R"(<feed xmlns="http://www.w3.org/2005/Atom"><opensearch:totalResults>0</opensearch:totalResults></feed>)";

}  // namespace

int main() {
  std::println("== arxiv_search：论文检索（arXiv Atom API）==");
  std::println("");

  ArxivConfig cfg;
  std::println("① 查询 URL 构造（空格→+，语法字符保留，其余百分号编码）：");
  std::println("   build_arxiv_query(\"ti:\\\"agents\\\" AND cat:cs.AI\", 3)");
  std::println("     = {}\n", build_arxiv_query(cfg, "ti:\"agents\" AND cat:cs.AI", 3));

  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script ok;
  ok.status = 200;
  ok.chunks = {std::string(kFeed)};
  http::FakeTransport::Script none;
  none.status = 200;
  none.chunks = {std::string(kEmpty)};
  tp->push(std::move(ok));
  tp->push(std::move(none));

  auto const tool = make_arxiv_search_tool(tp, cfg);
  run_tool("② 找相关工作 → arxiv_search(1-bit 模型, max_results=2)", tool,
           json{{"search_query", "all:1-bit LLM"}, {"max_results", 2}});
  run_tool("③ 无匹配 → 错误回喂（total=0，建议换词）", tool,
           json{{"search_query", "zzz_不存在的关键词"}});
}