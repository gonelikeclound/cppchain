// tests/message —— 防幻觉信号灯（problems/50 设计稿 · 最小切片 1）：
//   识别 <anti-phantasma-off/>（是否处于内容开头白名单位）与剥离（任一位置剥净、防标签泄漏）。
// v0 口径（problems/50 §5.2/§6.1）：
//   格式良好 = 精确 ASCII、可带一个 ` reason="…"`、必须自闭合；变体一律不认（= 普通文本）。
//   识别只认“首个标记位于内容开头”；别处标记“剥但不算”（宁查）。
//   剥离 = 剥净所有格式良好标记（连带紧随的单个换行）。
#include <gtest/gtest.h>

#include <string>

#include "cppchain/message/phantasma.hpp"

using namespace cppchain::message;

std::string const kTag = "<anti-phantasma-off/>";

// —— 无信号 / 全通过 -------------------------------------------
TEST(PhantasmaSignal, no_marker_returns_unchanged) {
  std::string const t = "小米 15 的发布时间是 2025 年。";
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
  EXPECT_FALSE(s.at_start);
}

// —— 开头白名单位：识别 + 剥除 ---------------------------------
TEST(PhantasmaSignal, prefix_marker_stripped) {
  EXPECT_EQ(strip_phantasma_signal(kTag + "\n确定。"), "确定。");
}

TEST(PhantasmaSignal, prefix_marker_recognized_at_start) {
  PhantasmaSignal s;
  (void)strip_phantasma_signal(kTag + "\n答案是确定的。", &s);
  EXPECT_TRUE(s.seen);
  EXPECT_TRUE(s.at_start);
  EXPECT_EQ(s.reason, "");
}

TEST(PhantasmaSignal, leading_blank_lines_ignored_on_start_check) {
  PhantasmaSignal s;
  std::string const t = "\n\r\n  " + kTag + "\n答案。";
  std::string const clean = strip_phantasma_signal(t, &s);
  EXPECT_TRUE(s.at_start);                          // 前导空白不算内容
  EXPECT_EQ(clean, "\n\r\n  答案。");                // 保留前导空白，只剥标记+换行
}

TEST(PhantasmaSignal, crlf_after_marker_consumed) {
  EXPECT_EQ(strip_phantasma_signal(kTag + "\r\n正文"), "正文");
}

// —— 非白名单位（答案中部）：「剥但不算」= 宁查方向 ------------------
TEST(PhantasmaSignal, marker_midtext_not_at_start_but_stripped) {
  PhantasmaSignal s;
  std::string const t = "结论是确定的。" + kTag + "另外";
  std::string const clean = strip_phantasma_signal(t, &s);
  ASSERT_TRUE(s.seen);
  EXPECT_FALSE(s.at_start);                          // 不在开头 → 不算豁免信号（宁查）
  EXPECT_EQ(clean, "结论是确定的。另外");            // 仍剥净，防标签泄漏
}

// —— reason 属性 ----------------------------------------------
TEST(PhantasmaSignal, reason_attribute_parsed) {
  PhantasmaSignal s;
  (void)strip_phantasma_signal("<anti-phantasma-off reason=\"well-known\"/>\nAnswer", &s);
  EXPECT_EQ(s.reason, "well-known");
}

TEST(PhantasmaSignal, multiple_markers_all_stripped_reason_taken_from_first) {
  PhantasmaSignal s;
  std::string const t = "<anti-phantasma-off reason=\"小米8 参数烂熟\"/>" +
                        std::string("<anti-phantasma-off/>\n正文");
  EXPECT_EQ(strip_phantasma_signal(t, &s), "正文");
  EXPECT_TRUE(s.seen);
  EXPECT_TRUE(s.at_start);
  EXPECT_EQ(s.reason, "小米8 参数烂熟");             // reason 取首个
}

TEST(PhantasmaSignal, empty_reason_kept_empty_for_upper_gate) {
  PhantasmaSignal s;
  (void)strip_phantasma_signal("<anti-phantasma-off reason=\"\"/>\n答复", &s);
  ASSERT_TRUE(s.seen);
  EXPECT_TRUE(s.at_start);
  EXPECT_EQ(s.reason, "");                           // reason 空 → 上层「双条件」不满足，照查
}

TEST(PhantasmaSignal, reason_value_kept_raw) {
  PhantasmaSignal s;
  (void)strip_phantasma_signal("<anti-phantasma-off reason=\"&amp;\"/>\nX", &s);
  EXPECT_EQ(s.reason, "&amp;");                       // 原样不解码（对抗表在上层按原串判）
}

// —— 变体一律不认（= 普通文本，原样保留）-----------------------
TEST(PhantasmaSignal, uppercase_variant_ignored) {
  std::string const t = "<ANTI-PHANTASMA-OFF/>\n正文";
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}

TEST(PhantasmaSignal, inner_space_variant_ignored) {
  std::string const t = "<anti-phantasma-off />\n正文";  // `/>` 前被塞空格 → 格式不良
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}

TEST(PhantasmaSignal, fullwidth_variant_ignored) {
  std::string const t = "<anti-phantasma-off／>\n正文";  // 全角斜杠取代 `/>`
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}

TEST(PhantasmaSignal, html_entity_variant_ignored) {
  std::string const t = "&lt;anti-phantasma-off/&gt;\n正文";
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}

TEST(PhantasmaSignal, zero_width_variant_ignored) {
  std::string const t = std::string("<anti-phantasma-off​/>\n正文");  // 零宽空格塞进口
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}

TEST(PhantasmaSignal, missing_selfclose_ignored) {
  std::string const t = "<anti-phantasma-off>\n正文";  // 没有 `/>`，不是信标
  PhantasmaSignal s;
  EXPECT_EQ(strip_phantasma_signal(t, &s), t);
  EXPECT_FALSE(s.seen);
}