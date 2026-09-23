// cppchain —— tool/selector.hpp
// 工具检索：工具数量大时不能把全部 schema 塞进 prompt —— 按 query 挑子集。
// 「大批量工具查找」卡位问题在此落地（04-problems/01）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cppchain/tool/registry.hpp"

namespace cppchain::tool {

class ToolSelector {
 public:
  virtual ~ToolSelector() = default;

  // 从注册表挑出与 query 相关的工具（返回指针，生命周期同注册表）
  [[nodiscard]] virtual std::vector<ToolSpec const*> select(ToolRegistry const& reg,
                                                           std::string_view query,
                                                           std::size_t k) const = 0;
};

// 全量：小集合基线（也是"不检索"的对照）
class AllToolsSelector final : public ToolSelector {
 public:
  [[nodiscard]] std::vector<ToolSpec const*> select(ToolRegistry const& reg, std::string_view,
                                                    std::size_t) const override {
    std::vector<ToolSpec const*> out;
    out.reserve(reg.size());
    for (auto const& t : reg.all()) out.push_back(&t);
    return out;
  }
};

// 关键词评分：可解释、零依赖。规则：
//   - 查询切成 token：ASCII 词（长度≥2）+ CJK 2-gram（去掉少量停用字）
//   - token 命中工具名 → name_weight；命中可检索描述（描述+参数名+参数描述）→ description_weight
//   - **IDF 加权**：高频词（如 "get"）权重自动降低，稀有词（如 "stock"）权重提高
//     —— 实测发现无权重的关键词检索会被 "get" 这类词主导（见 04-problems/01）
//   - 整串等于工具名 → exact_name_bonus
//   - require_match 时丢弃零分项（避免塞无关工具）
class KeywordSelector final : public ToolSelector {
 public:
  struct Options {
    double name_weight = 3.0;
    double description_weight = 1.0;
    double exact_name_bonus = 5.0;
    bool require_match = true;
    bool use_idf = true;  // 关掉即退化为朴素词频加权（便于对比实验）
  };

  KeywordSelector() = default;
  explicit KeywordSelector(Options opts) : opts_(opts) {}

  [[nodiscard]] std::vector<ToolSpec const*> select(ToolRegistry const& reg, std::string_view query,
                                                    std::size_t k) const override {
    std::vector<std::string> tokens;
    {
      std::unordered_set<std::string> seen;
      for (auto const& tk : tokenize(query))
        if (seen.insert(tk).second) tokens.push_back(tk);
    }

    // 文档频率：多少工具含该 token（用于 IDF 降权高频词）
    std::unordered_map<std::string, std::size_t> df;
    if (opts_.use_idf) {
      for (auto const& t : reg.all()) {
        std::string const name_l = to_lower(t.name);
        std::string const desc_l = searchable_description(t);
        for (auto const& tk : tokens)
          if (name_l.find(tk) != std::string::npos || desc_l.find(tk) != std::string::npos) ++df[tk];
      }
    }
    double const n_docs = static_cast<double>(std::max<std::size_t>(1, reg.size()));

    std::vector<std::pair<double, ToolSpec const*>> scored;
    scored.reserve(reg.size());
    for (auto const& t : reg.all()) {
      double const s =
          opts_.use_idf ? score_idf(t, tokens, df, n_docs) : score(t, query);
      if (opts_.require_match && s <= 0.0) continue;
      scored.emplace_back(s, &t);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](auto const& a, auto const& b) { return a.first > b.first; });
    if (k > 0 && scored.size() > k) scored.resize(k);

    std::vector<ToolSpec const*> out;
    out.reserve(scored.size());
    for (auto const& [s, spec] : scored) {
      (void)s;
      out.push_back(spec);
    }
    return out;
  }

  // 评分单列出来，便于单测与解释"为什么选中它"
  [[nodiscard]] double score(ToolSpec const& spec, std::string_view query) const {
    auto const tokens = tokenize(query);
    std::string const name_l = to_lower(spec.name);
    std::string const desc_l = searchable_description(spec);

    double total = 0.0;
    std::unordered_set<std::string> seen;
    for (auto const& tk : tokens) {
      if (!seen.insert(tk).second) continue;  // 同一 token 只计一次
      if (name_l.find(tk) != std::string::npos) total += opts_.name_weight;
      if (desc_l.find(tk) != std::string::npos) total += opts_.description_weight;
    }
    std::string const q_l = to_lower(query);
    // 精确命中工具名（规范化去掉分隔符后完全相等，如查询 "get_weather" ↔ 工具 get_weather）
    if (!q_l.empty() && strip_separators(name_l) == strip_separators(q_l))
      total += opts_.exact_name_bonus;
    return total;
  }

  // 可检索的描述文本 = 描述 + 参数名 + 参数描述（工具的真实语义大量藏在参数里）
  [[nodiscard]] static std::string searchable_description(ToolSpec const& spec) {
    std::string text = spec.description;
    if (spec.parameters.is_object() && spec.parameters.contains("properties") &&
        spec.parameters["properties"].is_object()) {
      for (auto it = spec.parameters["properties"].begin();
           it != spec.parameters["properties"].end(); ++it) {
        text += " ";
        text += it.key();
        if (it.value().is_object() && it.value().contains("description") &&
            it.value()["description"].is_string()) {
          text += " ";
          text += it.value()["description"].get<std::string>();
        }
      }
    }
    return to_lower(text);
  }

  [[nodiscard]] Options const& options() const { return opts_; }

  // ---- 文本处理（public static，便于单测）----

  [[nodiscard]] static std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
  }

  // 去掉空格/下划线/连字符，用于"名字精确相等"判定
  [[nodiscard]] static std::string strip_separators(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
      if (c != ' ' && c != '_' && c != '-') out.push_back(c);
    return out;
  }

  // 切 token：
  //   - ASCII 连续字母数字（长度≥2）→ 一个 token
  //   - CJK 连续段 → **2-gram**（单字切分噪声太大，"天气"应成为一个 token 而不是"天"+"气"）
  //     段长仅 1 时退化为单字；停用字（的/了…）先剔除
  [[nodiscard]] static std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> out;
    std::string word;                   // ASCII 词
    std::vector<std::string> cjk_run;   // CJK 字符序列

    auto flush_ascii = [&out, &word] {
      if (word.size() >= 2) out.push_back(word);
      word.clear();
    };
    auto flush_cjk = [&out, &cjk_run] {
      if (cjk_run.size() == 1) {
        out.push_back(cjk_run.front());
      } else {
        for (std::size_t k = 0; k + 1 < cjk_run.size(); ++k)
          out.push_back(cjk_run[k] + cjk_run[k + 1]);
      }
      cjk_run.clear();
    };

    std::size_t i = 0;
    while (i < text.size()) {
      unsigned char const c = static_cast<unsigned char>(text[i]);
      if (c < 0x80) {
        if (std::isalnum(c)) {
          flush_cjk();
          word.push_back(static_cast<char>(std::tolower(c)));
        } else {
          flush_ascii();
          flush_cjk();
        }
        ++i;
        continue;
      }
      flush_ascii();
      std::size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
      len = std::min(len, text.size() - i);
      std::string const ch(text.substr(i, len));
      if (!is_cjk_stop(ch)) cjk_run.push_back(ch);
      i += len;
    }
    flush_ascii();
    flush_cjk();
    return out;
  }

 private:
  // IDF 加权评分：token 权重 = log(1 + N/df)，压制高频词、抬升稀有词
  [[nodiscard]] double score_idf(ToolSpec const& spec, std::vector<std::string> const& tokens,
                                 std::unordered_map<std::string, std::size_t> const& df,
                                 double n_docs) const {
    std::string const name_l = to_lower(spec.name);
    std::string const desc_l = searchable_description(spec);
    double total = 0.0;
    for (auto const& tk : tokens) {
      auto const it = df.find(tk);
      if (it == df.end() || it->second == 0) continue;  // 无工具含此 token
      double const idf = std::log(1.0 + n_docs / static_cast<double>(it->second));
      if (name_l.find(tk) != std::string::npos) total += idf * opts_.name_weight;
      if (desc_l.find(tk) != std::string::npos) total += idf * opts_.description_weight;
    }
    return total;
  }

  // 少量高频虚词，避免"的/了/是"这类噪声把无关工具拉进来
  [[nodiscard]] static bool is_cjk_stop(std::string const& ch) {
    static const std::vector<std::string> stops = {"的", "了", "是", "和", "与", "在", "我", "你",
                                                   "他", "它", "们", "吗", "呢", "吧", "，", "。",
                                                   "？", "！", "、", "：", "；", "（", "）"};
    return std::find(stops.begin(), stops.end(), ch) != stops.end();
  }

  Options opts_;
};

}  // namespace cppchain::tool