// wfr-㊹ 治理（problems/33）· effect_class 副作用分类 + 启动自检。
// 覆盖：read_only=false 未声明 → violation；声明/只读 → 干净；词表封闭。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"

using namespace cppchain::tool;

namespace {

ToolSpec make_tool_with(std::string name, bool read_only, EffectClass ec) {
  ToolSpec s;
  s.name = std::move(name);
  s.read_only = read_only;
  s.effect_class = ec;
  return s;
}

}  // namespace

// 1) read_only=false 且未声明（=Read）→ violation
TEST(EffectClass, non_read_without_declaration_is_violation) {
  ToolRegistry reg;
  reg.add(make_tool_with("leaky", /*read_only=*/false, EffectClass::Read));
  auto v = effect_class_violations(reg);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0], "leaky");
}

// 2) 只读（隐含 Read）+ 显式声明 Idempotent/Irreversible → 干净
TEST(EffectClass, declared_and_read_tools_are_clean) {
  ToolRegistry reg;
  reg.add(make_tool_with("read_tool", /*read_only=*/true, EffectClass::Read));  // 只读隐含 Read
  reg.add(make_tool_with("writer", /*read_only=*/false, EffectClass::Idempotent));
  reg.add(make_tool_with("bash_like", /*read_only=*/false, EffectClass::Irreversible));
  EXPECT_TRUE(effect_class_violations(reg).empty());
}

// 3) 词表封闭：只认 read/idempotent/irreversible
TEST(EffectClass, vocab_closed) {
  EXPECT_TRUE(is_known_effect_class("read"));
  EXPECT_TRUE(is_known_effect_class("idempotent"));
  EXPECT_TRUE(is_known_effect_class("irreversible"));
  EXPECT_FALSE(is_known_effect_class("writable"));
  EXPECT_FALSE(is_known_effect_class("reversible"));
  EXPECT_FALSE(is_known_effect_class(""));
}
