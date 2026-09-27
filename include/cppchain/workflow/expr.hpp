// cppchain —— workflow/expr.hpp
// 受限表达式（规范 11-ir-syntax.md §6）：只判定，不做计算。
//
// 文法（递归下降）：
//   expression := or
//   or         := and { ("||"|"or") and }
//   and        := not { ("&&"|"and") not }
//   not        := "!" not | comparison
//   comparison := primary [ (">"|">="|"<"|"<="|"=="|"!="|"in"|"contains") primary ]
//   primary    := "(" expression ")" | STRING | NUMBER | true | false | null | $引用
//
// 边界（§6 明确禁止）：
//   · 无函数（`len(x)` 文法错）、无数组（`[` `]` 文法错）、无 `x.length`
//   · 一层 comparison 一个运算符（不支持 `a == b == c`）、无裸标识符
//
// 语义（D3 宽松）：见 expr.cpp 顶部注释；所有比较对 `null` 安全（不抛异常）。
// parse 即完整语法校验（含 ref 语法）；validator 调用 parse 得到"语法是否合法"，
// eval 在运行期对状态求值。
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/workflow/refs.hpp"

namespace cppchain::workflow {

using Json = nlohmann::json;

class Expr {
 public:
  // 解析即校验；失败返回人类可读错误（可直接回喂给生成器）。
  static std::expected<Expr, std::string> parse(std::string const& text);

  // 在状态上求值 → bool（顶层按 truthy 归约）。
  [[nodiscard]] bool eval(Json const& state) const;

  [[nodiscard]] std::string_view source() const;
  // 表达式里引用的全部 $/@config 引用（validator 回读用）。
  [[nodiscard]] std::vector<Ref> const& refs() const;

 private:
  struct Impl;
  std::shared_ptr<Impl const> impl_;
  explicit Expr(std::shared_ptr<Impl const> impl) : impl_(std::move(impl)) {}
};

}  // namespace cppchain::workflow