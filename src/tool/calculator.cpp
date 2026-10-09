// cppchain —— src/tool/calculator.cpp
#include "cppchain/tool/calculator.hpp"

#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

namespace cppchain::tool {
namespace {

// 解析失败（内部用异常承载，calculate 统一转 expected）
struct ParseFail {
  std::string msg;
};

bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string_view trim_ascii(std::string_view s) {
  while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
  return s;
}

// 递归下降解析器。文法（优先级从低到高）：
//   expression := term (('+'|'-') term)*
//   term       := unary (('*'|'/'|'%') unary)*
//   unary      := ('+'|'-') unary | power
//   power      := atom (('^'|'**') unary)?     // 右结合；-2^2 == -(2^2)
//   atom       := NUMBER | pi | e | func '(' args ')' | '(' expression ')'
class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  double parse() {
    double const v = expression(0);
    skip_ws();
    if (!at_end())
      throw ParseFail{"表达式末尾有多余内容「" + std::string(s_.substr(i_)) + "」"};
    return v;
  }

 private:
  [[noreturn]] void fail(std::string msg) const { throw ParseFail{std::move(msg)}; }

  bool at_end() const { return i_ >= s_.size(); }
  char cur() const { return at_end() ? '\0' : s_[i_]; }
  char peek(std::size_t off = 1) const { return i_ + off < s_.size() ? s_[i_ + off] : '\0'; }
  bool consume(char c) {
    if (cur() == c) {
      ++i_;
      return true;
    }
    return false;
  }
  void skip_ws() {
    while (!at_end() && is_ws(s_[i_])) ++i_;
  }

  double expression(int depth) {
    if (depth > 120) fail("表达式嵌套过深");
    skip_ws();
    double a = term(depth + 1);
    for (;;) {
      skip_ws();
      if (consume('+')) {
        a += term(depth + 1);
      } else if (consume('-')) {
        a -= term(depth + 1);
      } else {
        return a;
      }
    }
  }

  double term(int depth) {
    skip_ws();
    double a = unary(depth + 1);
    for (;;) {
      skip_ws();
      if (consume('*')) {
        a *= unary(depth + 1);
      } else if (consume('/')) {
        double const b = unary(depth + 1);
        if (b == 0.0) fail("除以零");
        a /= b;
      } else if (consume('%')) {
        double const b = unary(depth + 1);
        if (b == 0.0) fail("对 0 取模");
        a = std::fmod(a, b);
      } else {
        return a;
      }
    }
  }

  double unary(int depth) {
    skip_ws();
    if (consume('+')) return unary(depth + 1);
    if (consume('-')) return -unary(depth + 1);
    return power(depth + 1);
  }

  double power(int depth) {
    skip_ws();
    double const base = atom(depth + 1);
    skip_ws();
    if (cur() == '^') {
      ++i_;
      return std::pow(base, unary(depth + 1));
    }
    if (cur() == '*' && peek() == '*') {
      i_ += 2;
      return std::pow(base, unary(depth + 1));
    }
    return base;
  }

  double atom(int depth) {
    skip_ws();
    if (consume('(')) {
      double const v = expression(depth + 1);
      skip_ws();
      if (!consume(')')) fail("缺少右括号 ')'");
      return v;
    }
    char const c = cur();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') return function_or_const();
    return number();
  }

  double number() {
    std::size_t const start = i_;
    while (cur() >= '0' && cur() <= '9') ++i_;
    if (cur() == '.' && peek() >= '0' && peek() <= '9') {  // 小数（须有后继数字）
      ++i_;
      while (cur() >= '0' && cur() <= '9') ++i_;
    }
    if (cur() == 'e' || cur() == 'E') {  // 科学计数法：e/E 后须跟（可选符号+）数字
      std::size_t j = i_ + 1;
      if (peek(1) == '+' || peek(1) == '-') ++j;
      if (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') {
        i_ = j + 1;
        while (cur() >= '0' && cur() <= '9') ++i_;
      }
    }
    if (i_ == start) fail("此处应为数字或函数（位置 " + std::to_string(i_) + "）");
    double v = 0.0;
    auto const [ptr, ec] = std::from_chars(s_.data() + start, s_.data() + i_, v);
    if (ec != std::errc{} || ptr != s_.data() + i_) fail("无效的数字字面量「" +
                                                        std::string(s_.substr(start, i_ - start)) + "」");
    return v;
  }

  double function_or_const() {
    std::size_t const start = i_;
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(cur())) || cur() == '_')) ++i_;
    std::string const name(s_.substr(start, i_ - start));
    skip_ws();
    if (cur() == '(') return call(name);
    if (name == "pi") return std::numbers::pi_v<double>;
    if (name == "e") return std::numbers::e_v<double>;
    fail("未知标识符「" + name +
         "」（常量 pi/e，函数 sqrt abs round floor ceil sin cos tan exp log log10 pow min max）");
  }

  double call(std::string const& name) {
    ++i_;  // '('
    double const a = expression(1);
    skip_ws();
    double b = 0.0;
    bool const has_b = consume(',');
    if (has_b) {
      skip_ws();
      b = expression(1);
      skip_ws();
    }
    if (!consume(')')) fail("函数「" + name + "」缺少右括号 ')'");

    if (name == "pow" || name == "min" || name == "max") {
      if (!has_b) fail("函数「" + name + "」需要两个参数");
      if (name == "pow") return std::pow(a, b);
      if (name == "min") return std::min(a, b);
      return std::max(a, b);
    }
    if (has_b) fail("函数「" + name + "」只需要一个参数");

    if (name == "sqrt") {
      if (a < 0.0) fail("sqrt 参数须为非负数");
      return std::sqrt(a);
    }
    if (name == "abs") return std::fabs(a);
    if (name == "round") return std::round(a);
    if (name == "floor") return std::floor(a);
    if (name == "ceil") return std::ceil(a);
    if (name == "sin") return std::sin(a);
    if (name == "cos") return std::cos(a);
    if (name == "tan") return std::tan(a);
    if (name == "exp") return std::exp(a);
    if (name == "log") {
      if (a <= 0.0) fail("log 参数须为正数");
      return std::log(a);
    }
    if (name == "log10") {
      if (a <= 0.0) fail("log10 参数须为正数");
      return std::log10(a);
    }
    fail("未知函数「" + name +
         "」（支持：sqrt abs round floor ceil sin cos tan exp log log10 pow min max）");
  }

  std::string_view s_;
  std::size_t i_ = 0;
};

}  // namespace

std::expected<double, std::string> calculate(std::string_view expr, std::size_t max_expr_len) {
  std::string_view const s = trim_ascii(expr);
  if (s.empty()) return std::unexpected(std::string{"表达式为空"});
  if (s.size() > max_expr_len)
    return std::unexpected("表达式过长（" + std::to_string(s.size()) + " 字符，上限 " +
                           std::to_string(max_expr_len) + "）");
  Parser p(s);
  try {
    double const v = p.parse();
    if (!std::isfinite(v)) return std::unexpected(std::string{"结果不是有限数（可能溢出）"});
    return v;
  } catch (ParseFail const& e) {
    return std::unexpected(e.msg);
  }
}

std::string format_number(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  double const r = std::round(value);
  if (std::fabs(value - r) < 1e-9 && r >= -9.2e18 && r <= 9.2e18)  // 整数值 → 整数文本
    return std::to_string(static_cast<long long>(r));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.12g", value);  // 12 位有效数字
  return buf;
}

ToolSpec make_calculator_tool(CalculatorConfig cfg) {
  ToolSpec spec = make_tool(
      "calculator",
      "计算数学表达式，返回精确数值结果。模型自己口算容易出错，涉及四则运算、"
      "百分比、单位换算、指数对数等需要精确数值的场景，都应交给本工具。"
      "支持：+ - * / % ^（幂，也可 **）、括号、常量 pi/e、"
      "函数 sqrt abs round floor ceil sin cos tan exp log(自然对数) log10 pow(a,b) min(a,b) max(a,b)。"
      "返回结构化 JSON：{expression, result}。",
      SchemaBuilder()
          .prop("expression", "string", "要计算的数学表达式，如 (100+50)*0.2、2^10、sqrt(144)")
          .build(),
      [cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        auto expr = arg<std::string>(args, "expression");
        if (!expr) return std::unexpected(expr.error());
        auto const r = calculate(*expr, cfg.max_expr_len);
        if (!r) return std::unexpected(ToolError{r.error()});
        return nlohmann::json{{"expression", *expr}, {"result", format_number(*r)}};
      },
      /*read_only=*/true);
  spec.category = "compute";  // 供历史精简按类别过滤（wfr-㊵）
  return spec;
}

}  // namespace cppchain::tool