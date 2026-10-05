// cppchain —— workflow/validator.hpp
// IR 校验器（规范 11-ir-syntax.md：§1–§7 的规则，参考原实现 V1–V9 + V10 分组）。
// 与生成器解耦：validator 是纯确定性逻辑（无 LLM），错误可直接回喂给生成器重写。
//
// 规则分组（实现见 validator.cpp 顶部注释的编号表）：
//   V*  —— 顶层；VT* —— trigger；VN* —— 节点/命名；VR* —— 引用；VH* —— on_error；
//   VG* —— 图结构/label 消解（D2：整体计算关系，不要求 label 先于 goto）。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"

namespace cppchain::workflow {

struct ValidationError {
  std::string rule;     // 规则编号，如 "VT3"
  std::string message;  // 人类可读说明（可回喂）
};

struct ValidationResult {
  std::vector<ValidationError> errors;
  [[nodiscard]] bool ok() const { return errors.empty(); }
  // 全部错误拼成一段（生成器把它整体回喂给模型，要求整篇重写）。
  [[nodiscard]] std::string describe() const;
};

struct ValidatorOptions {
  tool::ToolRegistry const* registry = nullptr;  // 非拥有；null → 跳过工具存在性检查

  // 宿主**将要提供**的 config 键（B2）。nullopt = 不做该项检查（向后兼容）。
  //
  // 为什么要它：`@config.<键>` 原先没有任何约束 —— 提示词不限定键名，IR 里也没有
  // 声明。于是模型写一个宿主不会提供的键能**通过校验**，运行期静默解析成 null，
  // 直到工具读到 null 才炸（校验期 fail-closed，运行期 fail-open）。
  // 声明了契约后，VR4 会把这种 IR 挡在编译之前。
  //
  // 语义：nullopt → 跳过；空 vector → "不允许任何 @config 引用"。
  std::optional<std::vector<std::string>> config_keys = std::nullopt;
};

// 对已解析的 IR 做全量校验。
[[nodiscard]] ValidationResult validate(WorkflowIR const& ir, ValidatorOptions opts = {});

// 解析 + 校验一步到位（生成器回读刚拿到的原始 JSON 时用）。
[[nodiscard]] ValidationResult validate_json(Json const& raw, ValidatorOptions opts = {});

}  // namespace cppchain::workflow