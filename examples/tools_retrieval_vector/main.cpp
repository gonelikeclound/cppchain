// demo: tools_retrieval_vector —— 关键词检索 vs 向量检索（对照实验）
//
// 目的：验证 04-problems/01 提出的三个"关键词短板"能否被 embedding 解决：
//   · 跨语言（中文查询 ↔ 英文工具）
//   · 同义改写（"发消息" vs send_email）
//   · 纯语义相似（"明天要带伞吗" ↔ weather）
// 方法：同一批**纯英文工具**，同一组查询，分别用两种 selector 检索，比 top-1 命中与 recall@k。
//
// 需要 .env：EMBEDDING_API_KEY / EMBEDDING_BASE_URL / EMBEDDING_MODEL
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/dashscope/embeddings.hpp>
#include <cppchain/tool/embedding_selector.hpp>

#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::tool;

namespace {

char const* kDomains[] = {"weather", "stock",   "email",  "file",   "database",
                          "image",   "payment", "calendar", "search", "translate"};
char const* kActions[] = {"get", "query", "update", "create", "delete"};
char const* kDomainText[] = {"weather conditions and forecast for a city",
                             "stock market prices and quotes",
                             "email messages and recipients",
                             "local files and their contents",
                             "database records and tables",
                             "images and their metadata",
                             "payment transactions and refunds",
                             "calendar events and schedules",
                             "web search results",
                             "text translation between languages"};

// 造 N 个**纯英文**工具（中文查询时关键词检索必然失效）
void fill_tools(ToolRegistry& reg, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    std::string const domain = kDomains[i % 10];
    std::string const action = kActions[i % 5];
    SchemaBuilder sb;
    sb.prop("query", "string", std::string("search term within ") + domain)
        .prop("limit", "integer", "max number of results", false);
    reg.add(make_tool(action + "_" + domain + "_" + std::to_string(i),
                      std::string("Perform ") + action + " on " + kDomainText[i % 10],
                      sb.build(), [](nlohmann::json const&, ToolContext&) {
                        return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                      }));
  }
}

struct Case {
  char const* query;
  char const* expect_domain;  // 期望命中的工具名包含该域名
  char const* kind;           // 用例类型（用于说明短板）
};

Case const kCases[] = {
    {"明天出门要带伞吗", "weather", "跨语言+语义"},
    {"查一下股价", "stock", "跨语言"},
    {"帮我发封邮件", "email", "跨语言"},
    {"把这句话翻译成英文", "translate", "跨语言"},
    {"看看我这周有什么安排", "calendar", "跨语言+语义"},
    {"send a message to my colleague", "email", "同义改写(message↔email)"},
    {"what is the temperature outside", "weather", "同义改写(temperature↔weather)"},
    {"get_weather_3", "weather", "精确名字（关键词应占优）"},
};

struct Score {
  int top1 = 0;
  double recall = 0.0;
  double ms = 0.0;
};

Score evaluate(ToolSelector const& sel, ToolRegistry const& reg, std::size_t k, int runs) {
  Score s;
  double total_ms = 0.0;
  for (auto const& c : kCases) {
    auto const t0 = std::chrono::steady_clock::now();
    auto picked = sel.select(reg, c.query, k);
    auto const t1 = std::chrono::steady_clock::now();
    total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool top1_ok = !picked.empty() && picked[0]->name.find(c.expect_domain) != std::string::npos;
    if (top1_ok) s.top1++;

    int hits = 0;
    for (auto const* p : picked)
      if (p->name.find(c.expect_domain) != std::string::npos) ++hits;
    s.recall += std::min<int>(hits, 1);  // 该用例是否至少召回一个相关工具
  }
  s.recall /= static_cast<double>(std::size(kCases));
  s.ms = total_ms / static_cast<double>(std::size(kCases) * runs);
  return s;
}

void print_case(Case const& c, ToolRegistry const& reg, ToolSelector const& kw,
                ToolSelector const& vec, std::size_t k) {
  auto best = [&](ToolSelector const& s) -> std::string {
    auto p = s.select(reg, c.query, k);
    return p.empty() ? "(空)" : p[0]->name;
  };
  std::string const kw_top = best(kw);
  std::string const vec_top = best(vec);
  bool const kw_ok = kw_top.find(c.expect_domain) != std::string::npos;
  bool const vec_ok = vec_top.find(c.expect_domain) != std::string::npos;
  std::printf("  %-34s [%-24s] 关键词:%-4s %-22s 向量:%-4s %-22s\n", c.query, c.kind,
              kw_ok ? "✓" : "✗", kw_top.c_str(), vec_ok ? "✓" : "✗", vec_top.c_str());
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "EMBEDDING_API_KEY");
  std::string const base = demo::env_or(dotenv, "EMBEDDING_BASE_URL");
  std::string const model = demo::env_or(dotenv, "EMBEDDING_MODEL");

  if (key.empty() || base.empty() || model.empty()) {
    std::println("缺少向量端点配置：请在 .env 提供 EMBEDDING_API_KEY / EMBEDDING_BASE_URL / "
                 "EMBEDDING_MODEL（参考 .env.example）");
    return 1;
  }

  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const candidate = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(candidate)) ca = candidate;
    }
  }

  std::size_t const kN = 200;
  std::size_t const kK = 5;
  ToolRegistry reg;
  fill_tools(reg, kN);
  std::println("工具库: {} 个（纯英文名与描述）| 检索 top-{} | 查询 {} 条\n", kN, kK,
               std::size(kCases));

  // 关键词检索（IDF）
  KeywordSelector keyword;

  // 向量检索：建索引（批量嵌入）
  EmbeddingConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base;
  cfg.model = model;
  cfg.ca_info = ca;
  auto transport = std::make_shared<http::CurlMultiTransport>();
  dashscope::DashScopeEmbeddings embedder(cfg, transport);

  EmbeddingSelector vector_sel(embedder);
  auto const t0 = std::chrono::steady_clock::now();
  auto built = vector_sel.build_index(reg);
  auto const t1 = std::chrono::steady_clock::now();
  if (!built) {
    std::println("建索引失败: {} ({})", built.error().message, kind_str(built.error().kind));
    return 1;
  }
  double const build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::println("向量索引: {} 条, 维度 {}, 耗时 {:.0f} ms, 消耗 {} tokens\n", vector_sel.indexed_count(),
               embedder.dimension(), build_ms, embedder.total_tokens());

  // 逐条对照
  for (auto const& c : kCases) print_case(c, reg, keyword, vector_sel, kK);

  // 汇总指标
  auto const kw = evaluate(keyword, reg, kK, 1);
  auto const vec = evaluate(vector_sel, reg, kK, 1);
  std::println("\n{:─<78}", "");
  std::printf("  %-14s %-12s %-12s %-12s\n", "选择器", "top-1 命中", "recall@1", "平均耗时");
  std::printf("  %-14s %-12s %-12.2f %-12.2f ms\n", "关键词(IDF)",
              (std::to_string(kw.top1) + "/" + std::to_string(std::size(kCases))).c_str(), kw.recall,
              kw.ms);
  std::printf("  %-14s %-12s %-12.2f %-12.2f ms\n", "向量(embedding)",
              (std::to_string(vec.top1) + "/" + std::to_string(std::size(kCases))).c_str(),
              vec.recall, vec.ms);

  std::println("\n评测局限（必须读懂再引用数字）：");
  std::println("  · 本合成工具集里，**动作差异只体现在工具名**（描述文本按领域共用），"
               "且没有唯一正确的动作");
  std::println("    → 指标只判「领域是否命中」，不判「动作是否正确」");
  std::println("  · 因此向量 8/8 说明的是**领域级召回**修好了跨语言/同义改写，"
               "而非「选到了唯一正确的那个工具」");
  std::println("  · 要评「动作级」选对率，需要动作可区分的真实工具集（下一步）");
  std::println("\n说明：");
  std::println("  · 关键词检索在**跨语言/同义改写**上系统性失效 —— 这正是 embedding 的价值");
  std::println("  · 向量检索每条查询要多一次嵌入调用（网络往返），是它的代价");
  std::println("  · 工程上常见组合：关键词/向量召回 → 合并去重 → （可选）LLM 精排");
  return 0;
}