// demo: tools_retrieval —— 「大批量工具查找」量化实验
//
// 问题：工具一多，把**全部** schema 塞进 prompt 会导致：
//   ① 请求体（与费用）线性膨胀；② 选项过多 → 模型选错/幻觉工具名。
// 本 demo 量化两件事：
//   A. 体积：N 个工具时"全量 vs 检索 top-k" 的 schema 字节数/token 估算
//   B. 质量：关键词检索的 precision@k / recall@k（不能只看压缩比 —— 检索错了，
//      压缩比再高也是把正确答案排除了）
#include <cppchain/tool/selector.hpp>

#include <algorithm>
#include <chrono>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::tool;

namespace {

char const* kDomains[] = {"weather", "stock",   "email",  "file",   "database",
                          "image",   "payment", "calendar", "search", "translate"};
char const* kActions[] = {"get", "query", "update", "create", "delete", "list"};

std::vector<ToolSpec> make_tools(std::size_t n) {
  std::vector<ToolSpec> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string const domain = kDomains[i % 10];
    std::string const action = kActions[i % 6];
    std::string const name = action + "_" + domain + "_" + std::to_string(i);
    SchemaBuilder sb;
    sb.prop("query", "string", std::string("用于 ") + domain + " 的查询参数")
        .prop("limit", "integer", "返回条数", false);
    out.push_back(make_tool(name, std::string("在 ") + domain + " 领域执行 " + action + " 操作",
                            sb.build(), [](nlohmann::json const&, ToolContext&) {
                              return std::expected<nlohmann::json, ToolError>{
                                  nlohmann::json("ok")};
                            }));
  }
  return out;
}

void fill(ToolRegistry& reg, std::size_t n) {
  for (auto& t : make_tools(n)) reg.add(std::move(t));
}

// 粗略 token 估算：混合中英文，取 3.5 字节/token（仅用于量级比较）
double tokens(std::size_t bytes) { return static_cast<double>(bytes) / 3.5; }

void size_report(std::size_t n, std::size_t k) {
  ToolRegistry reg;
  fill(reg, n);

  std::string const query = "weather 天气";
  AllToolsSelector all;
  KeywordSelector keyword;

  auto t0 = std::chrono::steady_clock::now();
  auto all_picked = all.select(reg, query, 0);
  auto t1 = std::chrono::steady_clock::now();
  auto picked = keyword.select(reg, query, k);
  auto t2 = std::chrono::steady_clock::now();

  std::size_t const all_bytes = reg.to_openai_tools(all_picked).dump().size();
  std::size_t const sel_bytes = reg.to_openai_tools(picked).dump().size();
  auto us = [](auto a, auto b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
  };

  std::println("N={:<5} 全量 {:>8} B (~{:>7.0f} tok, {:>5} µs) | top-{} 检索 {:>5} B (~{:>5.0f} tok, "
               "{:>6} µs) | 省 {:.1f}x",
               n, all_bytes, tokens(all_bytes), us(t0, t1), k, sel_bytes, tokens(sel_bytes),
               us(t1, t2),
               static_cast<double>(all_bytes) / static_cast<double>(std::max<std::size_t>(1, sel_bytes)));
}

// 检索质量：以"名字里含 domain"作为相关性标注（合成数据，标注是确定的）
struct Labeled {
  char const* query;
  char const* domain;
};

void quality_report(std::size_t n, std::size_t k) {
  ToolRegistry reg;
  fill(reg, n);
  KeywordSelector keyword;

  static Labeled const cases[] = {{"查询天气 weather", "weather"},
                                  {"发送邮件 email", "email"},
                                  {"get stock price", "stock"},
                                  {"删除文件 file", "file"},
                                  {"translate text", "translate"}};

  std::println("N={} top-{} 的检索质量（precision / recall）:", n, k);
  for (auto const& c : cases) {
    auto picked = keyword.select(reg, c.query, k);

    std::size_t relevant_total = 0;
    for (auto const& t : reg.all())
      if (t.name.find(c.domain) != std::string::npos) ++relevant_total;

    std::size_t hit = 0;
    for (auto const* p : picked)
      if (p->name.find(c.domain) != std::string::npos) ++hit;

    double const precision = picked.empty() ? 0.0
                                            : static_cast<double>(hit) / static_cast<double>(picked.size());
    double const recall = relevant_total == 0
                              ? 0.0
                              : static_cast<double>(hit) /
                                    static_cast<double>(std::min(relevant_total, k));
    std::println("  \"{}\" → 选中 {:>2} 个, 相关 {:>2} 个 | precision {:.2f} recall {:.2f}",
                 c.query, picked.size(), hit, precision, recall);
  }
}

}  // namespace

int main() {
  std::println("=== A. 体积：全量 schema vs 关键词检索 ===");
  std::println("查询: \"weather 天气\"\n");
  for (std::size_t n : {10u, 100u, 1000u, 5000u}) size_report(n, 8);

  std::println("\n=== B. 质量：只看压缩比会骗人 ===");
  quality_report(1000, 8);

  std::println("\n结论：");
  std::println("  · 工具上千时全量 schema 吃掉数万 token；检索把它压回几百 token");
  std::println("  · 但压缩比不是目标——precision/recall 才是。关键词检索的已知短板：");
  std::println("    同义改写（\"查天气\" vs \"weather\"）、纯语义相似、跨语言 —— 需要 embedding 兜底");
  std::println("  · 详见 04-problems/01-大批量工具查找.md");
  return 0;
}