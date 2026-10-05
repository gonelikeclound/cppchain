// cppchain —— src/workflow/expr.cpp
// 受限表达式解析 + 宽松求值（D3）。
//
// 宽松语义（对 null 安全，任何组合都不抛）：
//   · numberish(v)：数字 → 其值；字符串且整体可解析为 double → 其值；其余 nullopt
//   · eq(a,b)：都 numberish → 数值相等；都 null → true；都 string / 都 bool → 相等；
//       其余 → JSON 严格相等（数字 vs 字符串 = false）
//   · < <= > >=：都 numberish → 数值比较；都 string → 字典序；其余 → false
//   · contains(x,y)：x 为数组 → 存在成员 eq(m,y)；x,y 都 string → x.find(y)!=npos；其余 false
//   · in(x,y)：y 为数组 → 存在成员 eq(x,m)；x,y 都 string → y.find(x)!=npos；其余 false
//   · truthy：bool→自身；number→!=0；string→非空；null→false；object/array→true
#include "cppchain/workflow/expr.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::workflow {

namespace {

// ================= token 化 =================

struct Tok {
  enum class K { Or, And, Not, LParen, RParen, Cmp, In, Contains, Str, Num, True, False, Null, Ref, End };
  K k = K::End;
  std::string op;    // Cmp 的运算符文本
  Json lit;          // 字面值
  Ref ref;           // Ref
  std::size_t pos = 0;
};

std::expected<std::vector<Tok>, std::string> tokenize(std::string const& src) {
  std::vector<Tok> out;
  std::size_t i = 0;

  auto fail = [&](std::size_t pos, std::string msg) {
    return std::unexpected("位置 " + std::to_string(pos) + "：" + std::move(msg));
  };

  while (i < src.size()) {
    char const c = src[i];
    Tok t;
    t.pos = i;

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '!') {
      if (i + 1 < src.size() && src[i + 1] == '=') {
        t.k = Tok::K::Cmp;
        t.op = "!=";
        i += 2;
      } else {
        t.k = Tok::K::Not;
        ++i;
      }
      out.push_back(std::move(t));
      continue;
    }
    if (c == '&') {
      if (i + 1 >= src.size() || src[i + 1] != '&') return fail(i, "单 '&' 不是合法运算符（应为 &&）");
      t.k = Tok::K::And;
      i += 2;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '|') {
      if (i + 1 >= src.size() || src[i + 1] != '|') return fail(i, "单 '|' 不是合法运算符（应为 ||）");
      t.k = Tok::K::Or;
      i += 2;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '(') {
      t.k = Tok::K::LParen;
      ++i;
      out.push_back(std::move(t));
      continue;
    }
    if (c == ')') {
      t.k = Tok::K::RParen;
      ++i;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '>' || c == '<' || c == '=') {
      if (i + 1 < src.size() && src[i + 1] == '=') {
        t.op = std::string(1, c) + '=';
        i += 2;
      } else if (c == '=') {
        return fail(i, "单 '=' 不是合法运算符（应为 ==）");
      } else {
        t.op = std::string(1, c);
        ++i;
      }
      if (t.op != "==" && t.op != ">=" && t.op != "<=" && t.op != ">" && t.op != "<")
        return fail(i, "不认识的比较运算符 '" + t.op + "'");
      t.k = Tok::K::Cmp;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '[' || c == ']') return fail(i, "数组语法不支持（§6：没有数组）");
    if (c == '"' || c == '\'') {
      // 用 JSON 解析处理引号与转义；单引号先转成双引号的 JSON 字符串
      char const q = c;
      std::size_t end = i + 1;
      while (end < src.size()) {
        if (src[end] == '\\') { end += 2; continue; }
        if (src[end] == q) break;
        ++end;
      }
      if (end >= src.size()) return fail(i, "字符串未闭合");
      std::string piece = src.substr(i, end + 1 - i);
      if (q == '\'') {
        // 单引号字符串：逃逸单引号 → 转 JSON
        std::string alt;
        alt.reserve(piece.size());
        for (std::size_t k = 1; k + 1 < piece.size(); ++k) {
          char const pc = piece[k];
          if (pc == '\\' && k + 2 < piece.size() && piece[k + 1] == '\'') { alt += '\''; ++k; }
          else if (pc == '"') { alt += "\\\""; }
          else if (pc == '\\') { alt += "\\\\"; }
          else alt += pc;
        }
        try {
          t.lit = Json("\"" + alt + "\"");
        } catch (nlohmann::json::exception const&) {
          return fail(i, "字符串字面量非法");
        }
      } else {
        try {
          t.lit = Json::parse(piece);
        } catch (nlohmann::json::exception const&) {
          return fail(i, "字符串字面量非法");
        }
      }
      t.k = Tok::K::Str;
      i = end + 1;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      std::size_t start = i;
      ++i;
      while (i < src.size() &&
             (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.'))
        ++i;
      if (i == start) return fail(i, "非法数字");
      std::string piece = src.substr(start, i - start);
      try {
        t.lit = Json::parse(piece);
      } catch (nlohmann::json::exception const&) {
        return fail(start, "数字字面量非法：'" + piece + "'");
      }
      if (!t.lit.is_number()) return fail(start, "数字字面量非法：'" + piece + "'");
      t.k = Tok::K::Num;
      out.push_back(std::move(t));
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::size_t start = i;
      while (i < src.size() &&
             (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_'))
        ++i;
      std::string word = src.substr(start, i - start);
      if (word == "and") { t.k = Tok::K::And; }
      else if (word == "or") { t.k = Tok::K::Or; }
      else if (word == "not") { t.k = Tok::K::Not; }
      else if (word == "in") { t.k = Tok::K::In; }
      else if (word == "contains") { t.k = Tok::K::Contains; }
      else if (word == "true") { t.k = Tok::K::True; t.lit = true; }
      else if (word == "false") { t.k = Tok::K::False; t.lit = false; }
      else if (word == "null") { t.k = Tok::K::Null; t.lit = Json(nullptr); }
      else {
        if (i < src.size() && src[i] == '(')
          return fail(start, "函数调用不支持（§6：没有函数）");
        return fail(start, "未知标识符 '" + word + "'（没有函数/数组，也没有 x.length）");
      }
      out.push_back(std::move(t));
      continue;
    }
    if (c == '$' || c == '@') {
      auto scans = scan_refs(std::string_view(src).substr(i));
      if (scans.empty() || scans[0].begin != 0)
        return fail(i, "引用语法损坏（$ 或 @config 后不是合法引用）");
      if (!scans[0].ok) return fail(i, scans[0].problem);
      t.k = Tok::K::Ref;
      t.ref = scans[0].ref;
      i += scans[0].end;
      out.push_back(std::move(t));
      continue;
    }
    return fail(i, "不认识的字符 '" + std::string(1, c) + "'");
  }

  Tok end;
  end.pos = src.size();
  out.push_back(std::move(end));
  return out;
}

// ================= AST =================

struct Ast {
  enum class K { Or, And, Not, Cmp, In, Contains, Lit, Ref };
  K k = K::Lit;
  std::shared_ptr<Ast> l;
  std::shared_ptr<Ast> r;
  std::string op;   // Cmp
  Json lit = Json(nullptr);
  Ref ref;
};

struct Parser {
  explicit Parser(std::vector<Tok> toks) : toks_(std::move(toks)) {}
  std::vector<Tok> toks_;
  std::size_t i = 0;

  // 栈深护栏。这里有两条**独立**的爆栈路径：
  //   ① 解析：嵌套括号 / 连串取反是递归下降，深度无任何限制；
  //   ② 求值：`a || b || c || …` 解析端是 while 循环（自身不递归），但会构造出
  //      深度 = 项数的左倾树，求值端照样按树深递归。
  // ① 由 kMaxDepth 直接限制；② 由 kMaxTokens 限制 —— AST 节点数 ≤ token 数，
  // 而树的深度 ≤ 节点数，故求值递归深度同样有界。
  // validator 会 parse 模型产出的表达式，崩在这里 = fail-closed 承诺失效，必须挡住。
  static constexpr int kMaxDepth = 256;
  static constexpr std::size_t kMaxTokens = 1024;
  int depth = 0;

  struct DepthGuard {
    int& d;
    bool ok;
    explicit DepthGuard(int& depth) : d(depth), ok(++depth <= kMaxDepth) {}
    ~DepthGuard() { --d; }
  };

  [[nodiscard]] std::string too_deep() const {
    return "表达式嵌套过深（超过 " + std::to_string(kMaxDepth) + " 层，位置 " +
           std::to_string(peek().pos) + "）";
  }

  Tok const& peek() const { return toks_[i]; }
  bool at(Tok::K k) const { return peek().k == k; }

  std::expected<std::shared_ptr<Ast>, std::string> run() {
    auto root = parse_or();
    if (!root) return root;
    if (!at(Tok::K::End)) return std::unexpected("表达式有无法解析的尾部内容（位置 " +
                                                std::to_string(peek().pos) + "）");
    return root;
  }

  std::expected<std::shared_ptr<Ast>, std::string> parse_or() {
    auto left = parse_and();
    if (!left) return left;
    while (at(Tok::K::Or)) {
      ++i;
      auto right = parse_and();
      if (!right) return right;
      auto n = std::make_shared<Ast>();
      n->k = Ast::K::Or;
      n->l = *left;
      n->r = *right;
      left = std::move(n);
    }
    return left;
  }

  std::expected<std::shared_ptr<Ast>, std::string> parse_and() {
    auto left = parse_not();
    if (!left) return left;
    while (at(Tok::K::And)) {
      ++i;
      auto right = parse_not();
      if (!right) return right;
      auto n = std::make_shared<Ast>();
      n->k = Ast::K::And;
      n->l = *left;
      n->r = *right;
      left = std::move(n);
    }
    return left;
  }

  std::expected<std::shared_ptr<Ast>, std::string> parse_not() {
    DepthGuard g(depth);
    if (!g.ok) return std::unexpected(too_deep());
    if (at(Tok::K::Not)) {
      ++i;
      auto c = parse_not();
      if (!c) return c;
      auto n = std::make_shared<Ast>();
      n->k = Ast::K::Not;
      n->l = *c;
      return n;
    }
    return parse_comparison();
  }

  std::expected<std::shared_ptr<Ast>, std::string> parse_comparison() {
    auto left = parse_primary();
    if (!left) return left;
    Tok::K const k = peek().k;
    if (k == Tok::K::Cmp || k == Tok::K::In || k == Tok::K::Contains) {
      std::string op;
      if (k == Tok::K::Cmp) op = peek().op;
      else if (k == Tok::K::In) op = "in";
      else op = "contains";
      ++i;
      auto right = parse_primary();
      if (!right) return right;
      auto n = std::make_shared<Ast>();
      n->k = (k == Tok::K::Cmp) ? Ast::K::Cmp : (k == Tok::K::In ? Ast::K::In : Ast::K::Contains);
      n->op = std::move(op);
      n->l = *left;
      n->r = *right;
      return std::shared_ptr<Ast>(std::move(n));
    }
    return left;
  }

  std::expected<std::shared_ptr<Ast>, std::string> parse_primary() {
    DepthGuard g(depth);
    if (!g.ok) return std::unexpected(too_deep());
    Tok const& t = peek();
    switch (t.k) {
      case Tok::K::LParen: {
        ++i;
        auto e = parse_or();
        if (!e) return e;
        if (!at(Tok::K::RParen)) return std::unexpected("缺少 ')'（位置 " + std::to_string(t.pos) + "）");
        ++i;
        return e;
      }
      case Tok::K::Str:
      case Tok::K::Num:
      case Tok::K::True:
      case Tok::K::False:
      case Tok::K::Null: {
        auto n = std::make_shared<Ast>();
        n->k = Ast::K::Lit;
        n->lit = t.lit;
        ++i;
        return n;
      }
      case Tok::K::Ref: {
        auto n = std::make_shared<Ast>();
        n->k = Ast::K::Ref;
        n->ref = t.ref;
        ++i;
        return n;
      }
      default:
        return std::unexpected("此处需要一个操作数（位置 " + std::to_string(t.pos) + "）");
    }
  }
};

// ================= 宽松求值 =================

std::optional<double> numberish(Json const& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    std::string const& s = v.get_ref<std::string const&>();
    double d = 0.0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), d);
    if (res.ec == std::errc{} && res.ptr == s.data() + s.size()) return d;
    // 数值字符串（如 "800"）可能带尾随空白——再试一次去空白后整体解析
    std::size_t b = s.find_first_not_of(" \t");
    std::size_t e2 = s.find_last_not_of(" \t");
    if (b != std::string::npos && e2 != std::string::npos && e2 >= b) {
      std::string_view sub(s.data() + b, e2 - b + 1);
      auto res2 = std::from_chars(sub.data(), sub.data() + sub.size(), d);
      if (res2.ec == std::errc{} && res2.ptr == sub.data() + sub.size()) return d;
    }
  }
  return std::nullopt;
}

bool truthy(Json const& v) {
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number()) return v.get<double>() != 0.0;
  if (v.is_string()) return !v.get_ref<std::string const&>().empty();
  if (v.is_null()) return false;
  return true;  // object / array
}

bool eq(Json const& a, Json const& b) {
  auto na = numberish(a);
  auto nb = numberish(b);
  if (na && nb) return *na == *nb;
  if (a.is_null() && b.is_null()) return true;
  if (a.is_string() && b.is_string()) return a == b;
  if (a.is_boolean() && b.is_boolean()) return a == b;
  return a == b;
}

bool cmp(std::string const& op, Json const& a, Json const& b) {
  if (op == "==") return eq(a, b);
  if (op == "!=") return !eq(a, b);
  auto na = numberish(a);
  auto nb = numberish(b);
  if (na && nb) {
    double const x = *na, y = *nb;
    if (op == ">") return x > y;
    if (op == ">=") return x >= y;
    if (op == "<") return x < y;
    if (op == "<=") return x <= y;
  } else if (a.is_string() && b.is_string()) {
    std::string const& x = a.get_ref<std::string const&>();
    std::string const& y = b.get_ref<std::string const&>();
    if (op == ">") return x > y;
    if (op == ">=") return x >= y;
    if (op == "<") return x < y;
    if (op == "<=") return x <= y;
  }
  return false;  // 不可比 → false（宽松，不抛）
}

// contains(x, y)：x 含 y（数组成员的 eq 或字符串子串）
bool contains_op(Json const& x, Json const& y) {
  if (x.is_array()) {
    for (Json const& m : x)
      if (eq(m, y)) return true;
    return false;
  }
  if (x.is_string() && y.is_string())
    return x.get_ref<std::string const&>().find(y.get_ref<std::string const&>()) !=
           std::string::npos;
  return false;
}

// in(x, y)：x 属于 y（y 为数组，或 y 字符串含 x）
bool in_op(Json const& x, Json const& y) {
  if (y.is_array()) {
    for (Json const& m : y)
      if (eq(x, m)) return true;
    return false;
  }
  if (x.is_string() && y.is_string())
    return y.get_ref<std::string const&>().find(x.get_ref<std::string const&>()) !=
           std::string::npos;
  return false;
}

Json eval_value(Ast const& n, Json const& state) {
  switch (n.k) {
    case Ast::K::Lit:
      return n.lit;
    case Ast::K::Ref:
      return resolve_ref(n.ref, state);
    case Ast::K::Not:
      return Json(truthy(eval_value(*n.l, state)) ? false : true);
    case Ast::K::And:
      return Json(truthy(eval_value(*n.l, state)) && truthy(eval_value(*n.r, state)));
    case Ast::K::Or:
      return Json(truthy(eval_value(*n.l, state)) || truthy(eval_value(*n.r, state)));
    case Ast::K::Cmp:
      return Json(cmp(n.op, eval_value(*n.l, state), eval_value(*n.r, state)));
    case Ast::K::In:
      return Json(in_op(eval_value(*n.l, state), eval_value(*n.r, state)));
    case Ast::K::Contains:
      return Json(contains_op(eval_value(*n.l, state), eval_value(*n.r, state)));
  }
  return Json(false);
}

}  // namespace

struct Expr::Impl {
  std::string source;
  std::shared_ptr<Ast> root;
  std::vector<Ref> refs;
};

std::expected<Expr, std::string> Expr::parse(std::string const& text) {
  auto toks = tokenize(text);
  if (!toks) return std::unexpected("表达式语法错误：" + toks.error());
  // 长度上限：见 Parser 处注释 —— 它兜住"左倾长链"那条求值爆栈路径。
  // 正常表达式只有几个 token，这个上限极宽松。
  if (toks->size() > Parser::kMaxTokens)
    return std::unexpected("表达式过长（token 数超过 " + std::to_string(Parser::kMaxTokens) + "）");

  Parser p(std::move(*toks));
  auto root = p.run();
  if (!root) return std::unexpected("表达式语法错误：" + root.error());

  auto impl = std::make_shared<Impl>();
  impl->source = text;
  impl->root = *root;
  // 静态收集 expr 里的全部引用（validator 回读）
  std::vector<std::shared_ptr<Ast const>> stack{impl->root};
  while (!stack.empty()) {
    auto n = stack.back();
    stack.pop_back();
    if (n->k == Ast::K::Ref) impl->refs.push_back(n->ref);
    if (n->l) stack.push_back(n->l);
    if (n->r) stack.push_back(n->r);
  }
  return Expr(std::move(impl));
}

bool Expr::eval(Json const& state) const {
  return truthy(eval_value(*impl_->root, state));
}

std::string_view Expr::source() const { return impl_->source; }
std::vector<Ref> const& Expr::refs() const { return impl_->refs; }

}  // namespace cppchain::workflow