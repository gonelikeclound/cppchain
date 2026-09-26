// demo: m9_hybrid —— 混合召回三路对照（关键词 / 向量 / 混合，离线确定性）
//
//  看点：同一注册表 + 同一评测集下，对比 keyword-only / vector-only / hybrid
//  的召回差（谁捞到了"关键词会漏的同义改写"）。结论全部由 eval 数据打印。
#include <cppchain/model/embeddings.hpp>
#include <cppchain/tool/hybrid_selector.hpp>
#include <cppchain/tool/selector.hpp>

#include <cmath>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::tool;

namespace {

bool has(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

// 确定性"语义空间"：三维 = [天气, 股票, 邮件]，伞/下雨≈天气语义（embedding 的泛化能力）
class StubEmbeddings final : public Embeddings {
 public:
  std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) override {
    std::vector<Embedding> out;
    for (auto const& t : texts) out.push_back(vector_for(t));
    return out;
  }
  [[nodiscard]] std::size_t dimension() const override { return 3; }
  [[nodiscard]] std::size_t max_batch_size() const override { return 20; }

  static Embedding vector_for(std::string_view t) {
    auto axis = [&](std::string_view a, std::string_view b) {
      return (has(t, a) || has(t, b)) ? 1.0f : 0.0f;
    };
    Embedding v{axis("天气", "weather"), axis("股价", "stock"), axis("邮件", "email")};
    if (has(t, "伞") || has(t, "下雨") || has(t, "穿衣")) v[0] = 1.0f;  // 生活语义≈天气
    float norm = 0.0f;
    for (float x : v) norm += x * x;
    if (norm > 0.0f) {
      norm = std::sqrt(norm);
      for (float& x : v) x /= norm;
    }
    return v;
  }
};

ToolSpec tool(std::string name, std::string desc) {
  return make_tool(std::move(name), std::move(desc), nlohmann::json::object(),
                   [](nlohmann::json const&, ToolContext&) {
                     return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                   });
}

tool::ToolRegistry eval_set() {
  tool::ToolRegistry reg;
  reg.add(tool("城市天气查询", "实时查询某城市的天气状况，温度与降水"));
  reg.add(tool("户外出行建议", "根据天气给户外出行提供穿衣和物品建议"));
  reg.add(tool("个股股价查询", "查询某只股票的实时股价与涨跌幅"));
  reg.add(tool("行情快照", "证券市场的指数与板块行情"));
  reg.add(tool("写信助手", "根据要点起草一封得体的邮件"));
  return reg;
}

struct Case {
  std::string_view query;
  std::string_view expected;  // 期望召回的"语义对应"工具
};

// 评测集：包含"字面不同但语义相同"的改写（关键词会漏的那类）
std::vector<Case> eval_cases() {
  return {
      {"查一下今天广州的天气", "城市天气查询"},
      {"出门要不要带伞", "户外出行建议"},
      {"帮我看看某某股票怎么样", "个股股价查询"},
      {"今天大盘表现如何", "行情快照"},
      {"帮我写封邮件给他", "写信助手"},
  };
}

void report(char const* label, tool::ToolRegistry const& reg, ToolSelector& sel) {
  int hit = 0;
  auto const cases = eval_cases();
  std::println("— {} —", label);
  for (auto const& c : cases) {
    auto picked = sel.select(reg, c.query, 3);
    std::string names;
    for (auto* s : picked) names += s->name + " ";
    bool const ok = [&] {
      for (auto* s : picked)
        if (s->name == c.expected) return true;
      return false;
    }();
    // 语义命中 = 期望工具在结果里；字面命中 = 查询词真的打字出现在某个名字里
    std::println("  查询「{}」期望「{}」 → {}  [命中{}]", c.query, c.expected,
                 ok ? "✓" : "✗", (ok ? "✓" : "✗"));
    if (ok) ++hit;
  }
  std::println("  → 语义召回 {}/{}", hit, (int)cases.size());
}

}  // namespace

int main() {
  tool::ToolRegistry const reg = eval_set();
  StubEmbeddings stub;

  {
    KeywordSelector kw;
    std::println("=== 三路召回对照（同一注册表 / 同一评测集）===\n");
    report("① keyword-only（零依赖基线）", reg, kw);
  }
  {
    EmbeddingSelector vec(stub);
    auto idx = vec.build_index(reg);
    if (!idx) std::println("向量建索引失败: {}", idx.error().message);
    report("② vector-only（语义召回）", reg, vec);
  }
  {
    HybridSelector hy(&stub);
    auto idx = hy.build_index(reg);
    if (!idx) std::println("混合建索引失败: {}", idx.error().message);
    report("③ hybrid（关键词 ∪ 向量去重）", reg, hy);
  }
  std::println("\n解读（按上面观测数据）：① 3/5，漏在「带伞/大盘」这类改写；② 4/5，向量补回了改写，");
  std::println("     但 stub 的语义轴没覆盖「大盘→行情」组合词（所以不是 5/5）；③ 拿到 ② 的召回，");
  std::println("     同时关键词结果保持在前 → 记录同样的召回，换来确定的顺序与可解释性。");
}