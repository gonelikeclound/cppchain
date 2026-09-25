// cppchain —— tool/calculator.hpp
// 安全数学计算器（calculator）：递归下降解析 + 直接求值（非 eval，无任意代码执行风险）。
// 定位同 langchain 内置 Calculator：给模型一个"必须精确"的计算兜底，
// 覆盖百分比/换算/四则这类模型口算容易错的场景。
//
// 可解析的表达式语言：
//   数字字面量：整数 / 小数 / 科学计数法（1e6、2.5e-3）
//   二元运算符：+ - * / % ^（幂，也可写 **，右结合）
//   一元运算符：+ -
//   括号：( )
//   常量：pi、e
//   函数：sqrt abs round floor ceil sin cos tan exp log(自然对数) log10
//         pow(a,b) min(a,b) max(a,b)
//   语义对齐 Python：-2^2 = -(2^2) = -4。
// 纯本地、零依赖，可离线单测；对 LLM 生成的输入做了长度/嵌套深度防御。
#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct CalculatorConfig {
  std::size_t max_expr_len = 200;  // 表达式最大字符数（防御 LLM 生成超长输入）
};

// 纯函数：求值表达式 → double；失败返回含位置提示的可读错误（字符串）。
[[nodiscard]] std::expected<double, std::string> calculate(std::string_view expr,
                                                           std::size_t max_expr_len = 200);

// 纯函数：结果格式化——整数值显示为整数（"42"），否则 %.12g 去尾零（"0.5"）。
[[nodiscard]] std::string format_number(double value);

// 生成 calculator 工具。参数：expression（必填）。返回：{expression, result}。
// 语法 / 除零 / 溢出 → unexpected(ToolError)。
[[nodiscard]] ToolSpec make_calculator_tool(CalculatorConfig cfg = {});

}  // namespace cppchain::tool