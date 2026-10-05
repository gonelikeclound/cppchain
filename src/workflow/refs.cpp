// cppchain —— src/workflow/refs.cpp
#include "cppchain/workflow/refs.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace cppchain::workflow {

namespace {

bool is_ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

[[nodiscard]] std::string_view take_ident(std::string_view s, std::size_t& i) {
  std::size_t const start = i;
  while (i < s.size() && is_ident_char(s[i])) ++i;
  return s.substr(start, i - start);
}

// 解析 "$...result.<f>" 的后半段：要求恰好是 ".result." + 字段。
// i 指向 "$"/"$_" 已被消费后的位置；若匹配则消费完整片段并填 ref。
bool match_result_tail(std::string_view s, std::size_t& i, std::string_view node_part,
                       Ref& out) {
  constexpr std::string_view kResultDot = ".result.";
  if (s.substr(i, kResultDot.size()) != kResultDot) return false;
  i += kResultDot.size();
  std::string_view f = take_ident(s, i);
  if (f.empty()) return false;
  if (node_part == "_") {
    out.kind = RefKind::LastResult;
  } else {
    out.kind = RefKind::NamedResult;
    out.node = std::string(node_part);
  }
  out.field = std::string(f);
  return true;
}

}  // namespace

std::vector<RefScan> scan_refs(std::string_view s) {
  std::vector<RefScan> out;

  auto malformed = [&](std::size_t begin, std::size_t end, std::string problem) {
    RefScan sc;
    sc.begin = begin;
    sc.end = end;
    sc.ok = false;
    sc.problem = std::move(problem);
    out.push_back(std::move(sc));
  };

  std::size_t i = 0;
  while (i < s.size()) {
    char const c = s[i];
    if (c != '$' && c != '@') {
      ++i;
      continue;
    }
    std::size_t const begin = i;

    if (c == '@') {
      constexpr std::string_view kCfg = "config.";
      bool const ok_prefix =
          s.size() - i > kCfg.size() && s.substr(i + 1, kCfg.size()) == kCfg;
      if (!ok_prefix) {
        malformed(begin, i + 1, "'@' 后必须是 config.' (got '" +
                                     std::string(s.substr(i, std::min<std::size_t>(s.size() - i, 8))) +
                                     "...')");
        ++i;
        continue;
      }
      i += 1 + kCfg.size();  // 跳过 '@' 与 "config."
      RefScan sc;
      sc.begin = begin;
      sc.ref.kind = RefKind::Config;
      std::string_view key = take_ident(s, i);
      if (key.empty()) {
        malformed(begin, i, "@config. 后缺少键名");
        continue;
      }
      sc.ref.field = std::string(key);
      sc.end = i;
      out.push_back(std::move(sc));
      continue;
    }

    // '$'
    ++i;
    if (i < s.size() && s[i] == '_') {
      // "$_.result.f"
      ++i;
      Ref r;
      if (!match_result_tail(s, i, "_", r)) {
        malformed(begin, i, "'$_' 后必须是 '.result.<字段>'");
        continue;
      }
      RefScan sc;
      sc.begin = begin;
      sc.end = i;
      sc.ref = std::move(r);
      out.push_back(std::move(sc));
      continue;
    }

    // "$<id>.result.f"
    std::size_t const node_start = i;
    std::string_view id_part = take_ident(s, i);
    if (id_part.empty()) {
      malformed(begin, i, "'$' 后必须是 id 或 '_'");
      continue;
    }
    Ref r;
    if (!match_result_tail(s, i, id_part, r)) {
      malformed(begin, i, "'$" + std::string(id_part) + "' 后必须是 '.result.<字段>'");
      continue;
    }
    RefScan sc;
    sc.begin = begin;
    sc.end = i;
    sc.ref = std::move(r);
    out.push_back(std::move(sc));
  }
  return out;
}

std::optional<Ref> parse_ref(std::string_view s) {
  auto scans = scan_refs(s);
  if (scans.size() != 1 || scans[0].begin != 0 || scans[0].end != s.size() || !scans[0].ok)
    return std::nullopt;
  return scans[0].ref;
}

Json resolve_ref(Ref const& r, Json const& state) {
  switch (r.kind) {
    case RefKind::LastResult: {
      auto last = state.find(kLastExecKey);
      if (last == state.end() || !last->is_string()) return Json(nullptr);
      auto results = state.find(kResultsKey);
      if (results == state.end() || !results->is_object()) return Json(nullptr);
      auto frame = results->find(last->get<std::string>());
      if (frame == results->end() || !frame->is_object()) return Json(nullptr);
      auto it = frame->find(r.field);
      return it == frame->end() ? Json(nullptr) : it.value();
    }
    case RefKind::NamedResult: {
      auto results = state.find(kResultsKey);
      if (results == state.end() || !results->is_object()) return Json(nullptr);
      auto frame = results->find(r.node);
      if (frame == results->end() || !frame->is_object()) return Json(nullptr);
      auto it = frame->find(r.field);
      return it == frame->end() ? Json(nullptr) : it.value();
    }
    case RefKind::Config: {
      auto cfg = state.find(kConfigKey);
      if (cfg == state.end() || !cfg->is_object()) return Json(nullptr);
      auto it = cfg->find(r.field);
      return it == cfg->end() ? Json(nullptr) : it.value();
    }
  }
  return Json(nullptr);
}

namespace {

// 解析值 → 文本呈现：字符串不带引号（模板替换直觉：字段值直接拼进文案），
// 其余用紧凑 JSON 序列化。
[[nodiscard]] std::string text_of(Json const& v) {
  if (v.is_string()) return v.get<std::string>();
  return v.dump();
}

}  // namespace

Json resolve_parameter(Json const& value, Json const& state) {
  // 容器必须**递归**：validator 的 VR1/VR2 是递归走进数组/对象的（它认这些嵌套引用合法），
  // 解析端若只看顶层，嵌套引用就会以字面量 "$src.result.price" 原样丢给工具 ——
  // 校验通过的引用静默失效，传错数据还不报错。
  if (value.is_array()) {
    Json out = Json::array();
    for (Json const& e : value) out.push_back(resolve_parameter(e, state));
    return out;
  }
  if (value.is_object()) {
    Json out = Json::object();
    for (auto it = value.begin(); it != value.end(); ++it)
      out[it.key()] = resolve_parameter(it.value(), state);
    return out;
  }
  if (!value.is_string()) return value;

  std::string const s = value.get<std::string>();
  auto full = parse_ref(s);
  if (full) return resolve_ref(*full, state);  // 整值引用：保留 JSON 类型

  auto scans = scan_refs(s);
  bool has_ref = false;
  for (RefScan const& sc : scans)
    if (sc.ok) {
      has_ref = true;
      break;
    }
  if (!has_ref) return value;  // 无引用模板

  std::string out;
  std::size_t pos = 0;
  for (RefScan const& sc : scans) {
    if (!sc.ok) continue;  // 损坏片段：原样保留（validator 会在编译期兜住）
    if (sc.begin > pos) out.append(s, pos, sc.begin - pos);
    out += text_of(resolve_ref(sc.ref, state));
    pos = sc.end;
  }
  if (pos < s.size()) out.append(s, pos, s.size() - pos);
  return out;
}

std::optional<std::string> ref_syntax_problem(std::string_view s) {
  for (RefScan const& sc : scan_refs(s))
    if (!sc.ok) return sc.problem;
  return std::nullopt;
}

}  // namespace cppchain::workflow