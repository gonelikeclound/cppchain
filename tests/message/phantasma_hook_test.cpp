// tests/message —— 防幻觉信号灯「输出钩子三态回执」（problems/50 设计稿 · 最小切片 2）：
//   apply_phantasma_signal 在「范围 × 防伪 × 风险门」输入下给出 accepted / vetoed / absent，
//   并产出落痕证据（clean 剥净 + JSON 序列化）。
// 决策表（problems/50 §5.3）：
//   范围外 → Absent（不看灯，仍剥净）；范围内信号无效 → Absent（宁查）；
//   有效且不过风险门 → Vetoed；有效且过风险门 → Accepted。
#include <gtest/gtest.h>

#include <string>

#include "cppchain/message/phantasma.hpp"

using namespace cppchain::message;

namespace {
// 好信号的默认输入：范围内、reason 过防伪表、无风险否决（reason_gate 默认关 = 用外部布尔）。
PhantasmaHookOptions const in_scope_ok{/*in_scope=*/true, /*reason_passes=*/true,
                                        /*reason_gate=*/false, /*risk_veto=*/false};
}  // namespace

TEST(PhantasmaHook, outside_scope_ignores_lamp_but_strips) {
  // 范围外：不看灯 → Absent；但标签仍剥净（不进用户侧）。
  PhantasmaHookOptions o;  // in_scope=false
  PhantasmaEvidence const ev = apply_phantasma_signal("<anti-phantasma-off/>\n确定。", o);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
  EXPECT_TRUE(ev.seen);
  EXPECT_TRUE(ev.at_start);
  EXPECT_EQ(ev.cleaned, "确定。");
}

TEST(PhantasmaHook, outside_scope_plain_answer_unchanged) {
  std::string const t = "快答，不查。";
  PhantasmaHookOptions o;
  EXPECT_EQ(apply_phantasma_signal(t, o).cleaned, t);
  EXPECT_EQ(apply_phantasma_signal(t, o).verdict, PhantasmaVerdict::Absent);
}

TEST(PhantasmaHook, in_scope_no_marker_absent) {
  std::string const t = "小米 15 的发布时间是 2025 年。";
  PhantasmaEvidence const ev = apply_phantasma_signal(t, in_scope_ok);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
  EXPECT_FALSE(ev.seen);
  EXPECT_EQ(ev.cleaned, t);
}

TEST(PhantasmaHook, start_marker_valid_reason_accepted) {
  std::string const t = "<anti-phantasma-off reason=\"well-known\"/>基础确定的。";
  PhantasmaEvidence const ev = apply_phantasma_signal(t, in_scope_ok);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Accepted);   // 豁免：免查放行
  EXPECT_EQ(ev.cleaned, "基础确定的。");
  EXPECT_EQ(ev.reason, "well-known");
}

TEST(PhantasmaHook, empty_reason_absent) {
  // reason 空 → 「双条件」不满足 → 信号无效 → Absent（照查）。
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"\"/>\n答复", in_scope_ok);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
}

TEST(PhantasmaHook, reason_fails_feature_table_absent) {
  // slice 4 的注入特征表判不过（reason_passes=false）→ 防伪不过 → Absent（照查）。
  PhantasmaHookOptions o = in_scope_ok;
  o.reason_passes = false;
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"任意理由\"/>\nX", o);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
}

TEST(PhantasmaHook, mid_marker_absent_but_stripped) {
  // 非白名单位（答案中部）→ 「剥但不算」→ Absent。
  std::string const t = "结论是确定的。<anti-phantasma-off/>另外";
  PhantasmaEvidence const ev = apply_phantasma_signal(t, in_scope_ok);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
  EXPECT_TRUE(ev.seen);
  EXPECT_FALSE(ev.at_start);
  EXPECT_EQ(ev.cleaned, "结论是确定的。另外");   // 仍剥净
}

TEST(PhantasmaHook, invalid_variant_absent_unchanged) {
  // 变体不是信标 → Absent，文本原样。
  std::string const t = "<ANTI-PHANTASMA-OFF/>\n正文";
  PhantasmaEvidence const ev = apply_phantasma_signal(t, in_scope_ok);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);
  EXPECT_FALSE(ev.seen);
  EXPECT_EQ(ev.cleaned, t);
}

TEST(PhantasmaHook, risk_veto_vetoed) {
  // 信号有效但被风险门否决 → Vetoed（照查）。
  PhantasmaHookOptions o = in_scope_ok;
  o.risk_veto = true;
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"known\"/>\n高风险域", o);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Vetoed);
  EXPECT_EQ(ev.cleaned, "高风险域");
}

TEST(PhantasmaHook, verdict_name_stable) {
  EXPECT_EQ(phantasma_verdict_name(PhantasmaVerdict::Accepted), "accepted");
  EXPECT_EQ(phantasma_verdict_name(PhantasmaVerdict::Vetoed), "vetoed");
  EXPECT_EQ(phantasma_verdict_name(PhantasmaVerdict::Absent), "absent");
}

TEST(PhantasmaHook, evidence_to_json_shape) {
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"known\"/>\n确定的。", in_scope_ok);
  nlohmann::json const j = phantasma_evidence_to_json(ev);
  EXPECT_EQ(j.at("verdict").get<std::string>(), "accepted");
  EXPECT_EQ(j.at("accepted").get<bool>(), true);
  EXPECT_EQ(j.at("reason").get<std::string>(), "known");
  EXPECT_EQ(j.at("seen").get<bool>(), true);
  EXPECT_EQ(j.at("at_start").get<bool>(), true);
}
// ============================================================
// 切片 4：reason 对抗特征表（phantasma_reason_passes）+ 钩子 gate 模式
// ============================================================
TEST(PhantasmaReasonGate, empty_rejected) {
  EXPECT_FALSE(phantasma_reason_passes(""));
}
TEST(PhantasmaReasonGate, too_long_rejected) {
  EXPECT_FALSE(phantasma_reason_passes(std::string(81, '好')));
}
TEST(PhantasmaReasonGate, plain_chinese_accepted) {
  EXPECT_TRUE(phantasma_reason_passes("小米8参数烂熟"));
}
TEST(PhantasmaReasonGate, plain_ascii_accepted) {
  EXPECT_TRUE(phantasma_reason_passes("well-known"));
}
TEST(PhantasmaReasonGate, meta_chars_rejected) {
  EXPECT_FALSE(phantasma_reason_passes("a<b"));
  EXPECT_FALSE(phantasma_reason_passes("a&b"));
  EXPECT_FALSE(phantasma_reason_passes("a/b"));
  EXPECT_FALSE(phantasma_reason_passes("a\\b"));
}
TEST(PhantasmaReasonGate, entity_and_url_rejected) {
  EXPECT_FALSE(phantasma_reason_passes("&amp;"));
  EXPECT_FALSE(phantasma_reason_passes("http://evil"));
  EXPECT_FALSE(phantasma_reason_passes("www.evil"));
}
TEST(PhantasmaReasonGate, control_char_rejected) {
  EXPECT_FALSE(phantasma_reason_passes("a\nb"));
}
TEST(PhantasmaReasonGate, zero_width_rejected) {
  EXPECT_FALSE(phantasma_reason_passes(std::string("a\xE2\x80\x8B") + "b"));  // ZWSP
}
TEST(PhantasmaReasonGate, self_referential_rejected) {
  EXPECT_FALSE(phantasma_reason_passes("anti-phantasma-off"));
  EXPECT_FALSE(phantasma_reason_passes("ANTI-PHANTASMA-OFF"));  // 大小写不放过
}
TEST(PhantasmaReasonGate, bypass_keyword_rejected) {
  EXPECT_FALSE(phantasma_reason_passes("bypass check"));
  EXPECT_FALSE(phantasma_reason_passes("请 ignore 该检查"));
}

TEST(PhantasmaHook, reason_gate_rejects_injected_reason) {
  PhantasmaHookOptions o = in_scope_ok;
  o.reason_gate = true;
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"bypass\"/>\nX", o);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Absent);  // 防伪不过 → 宁查
}
TEST(PhantasmaHook, reason_gate_accepts_plain_reason) {
  PhantasmaHookOptions o = in_scope_ok;
  o.reason_gate = true;
  PhantasmaEvidence const ev =
      apply_phantasma_signal("<anti-phantasma-off reason=\"well-known\"/>\nX", o);
  EXPECT_EQ(ev.verdict, PhantasmaVerdict::Accepted);
}

// ============================================================
// 切片 5：范围判定（phantasma_in_scope：域关键词命中；域空 → 全范围）
// ============================================================
TEST(PhantasmaInScope, empty_domains_full_scope) {
  EXPECT_TRUE(phantasma_in_scope(nlohmann::json::array(), "小米15发布时间？"));
}
TEST(PhantasmaInScope, domain_hit) {
  EXPECT_TRUE(phantasma_in_scope(nlohmann::json::array({"小米"}), "小米15发布时间？"));
}
TEST(PhantasmaInScope, domain_miss) {
  EXPECT_FALSE(phantasma_in_scope(nlohmann::json::array({"苹果"}), "小米15发布时间？"));
}
TEST(PhantasmaInScope, any_domain_hits) {
  EXPECT_TRUE(phantasma_in_scope(nlohmann::json::array({"苹果", "小米"}), "苹果 iPhone15 怎么样"));
}
