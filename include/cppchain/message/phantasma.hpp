// cppchain —— message/phantasma.hpp
// 防幻觉信号灯（problems/50 设计稿 · 最小切片 1 + 2）：
//   模型在输出流打「防幻觉豁免」文本标记 <anti-phantasma-off/>，harness 剥除并据之免查。
//   切片 1（本文件前半）：**识别**（信号在哪、是否处于内容开头白名单位）与**剥离**。
//   切片 2（本文件后半）：**输出钩子三态回执**——在"范围 × 防伪 × 风险门"输入下给出
//   accepted / vetoed / absent，并产出**落痕证据**（plog/审计序列化用）。
// v0 规则（problems/50 §5.2/§5.3/§6.1 的落地口径）：
//   · 格式良好 = 精确 ASCII `<anti-phantasma-off/>`，或带一个 ` reason="…"` 属性；
//     区分大小写、不含内部空白、必须自闭合 `/>`；任何变体（全角/HTML 实体/零宽字符/
//     大小写/内嵌空白）一律是普通文本，不识别也不碰。
//   · 识别（切片 1）：只有**首个**格式良好标记位于“内容开头”（忽略前导空白）才算有效
//     信号（at_start=true）；出现在别处的标记 → **剥但不算**（seen=true, at_start=false）——
//     宁可查证，不轻信。
//   · 剥离：所有格式良好标记（任一位置）连同紧随其后的单个换行（LF 或 CRLF）一并剥除；
//     文本其余内容原样保留。
//   · 回执（切片 2 决策表，见 apply_phantasma_signal）：
//       范围外  → Absent（不看灯）；标记仍剥净（防标签泄漏给用户）
//       范围内  → 信号无效（非开头 / reason 空 / 不过防伪表）→ Absent（照查）
//                → 信号有效且风险门不否决 → Accepted（免查放行）
//                → 信号有效但被风险/范围否决    → Vetoed（照查）
//   · 防伪（切片 4）：`phantasma_reason_passes` = reason 的保守对抗特征表（空 / 超长 /
//     元字符·HTML 实体·URL / 控制字符 / 零宽字符 / 自指·绕过关键词 → 判不过）；
//     `PhantasmaHookOptions::reason_gate=true` 时钩子内部自动套用该表（节点侧开启）；
//     注入文本中的信号，reason 几乎必然踩表 → 判无效（宁查）。特征表偏保守：
//     宁把正经理由误判为可疑（→ 多查一次），不可把注入放行。
#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace cppchain::message {

// 标记名常量（problems/50 §5.2 白名单字面量）。
inline constexpr std::string_view kAntiPhantasmaOffTag = "anti-phantasma-off";

// 信号识别结果。只报告事实；accepted / vetoed / absent 三态由上层钩子结合注入特征表
// 与范围·风险门给出（problems/50 §5.3/§5.4），本层不裁决。
struct PhantasmaSignal {
  bool seen      = false;  // 文本中存在格式良好的标记（任意位置）
  bool at_start  = false;  // 首个格式良好标记位于内容开头（前导空白除外）——白名单位
  std::string reason;      // 首个标记的 reason 属性原值（无属性 / 属性为空 → 空串）
};

namespace detail {

// 从 s[pos]（调用方保证 s[pos]=='<'）匹配一个格式良好标记：
//   命中 → 返回“消耗的字符数”，并把 reason 属性原值写入 `reason`（无属性 → 空串）；
//   不命中 → 返回 0。
inline std::size_t match_marker(std::string_view s, std::size_t pos, std::string* reason) {
  constexpr std::string_view open = "<anti-phantasma-off";
  if (s.substr(pos, open.size()) != open) return 0;
  std::size_t i = pos + open.size();
  std::string rea;
  if (i < s.size() && s[i] == ' ') {  // 白名单里只有一个可选属性：` reason="…"`（前导 1 空格）
    constexpr std::string_view ka = " reason=\"";  // 前置 1 空格 + 属性名 + `="`
    if (s.substr(i, ka.size()) != ka) return 0;    // 有空格却不是 ` reason="` → 格式不良，不认
    std::size_t j = i + ka.size();
    std::size_t k = s.find('"', j);
    if (k == std::string_view::npos) return 0;  // 引号不闭合 → 不认
    rea = std::string(s.substr(j, k - j));
    i = k + 1;
  }
  if (s.substr(i, 2) != "/>") return 0;  // 必须自闭合 `/>`
  if (reason) *reason = std::move(rea);
  return i + 2 - pos;
}

}  // namespace detail

// 识别：扫描全部格式良好标记，返回信号事实（v0 语义见文件头）。
[[nodiscard]] inline PhantasmaSignal find_phantasma_signal(std::string_view text) {
  PhantasmaSignal out;
  std::size_t const begin = text.find_first_not_of(" \t\r\n\f\v");  // 内容真正开头
  std::size_t first_at = std::string_view::npos;
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '<') {
      std::string rea;
      std::size_t const n = detail::match_marker(text, i, &rea);
      if (n > 0) {
        if (!out.seen) {  // 只记录首个 occurrence（reason 取它）
          out.seen = true;
          first_at = i;
          out.reason = std::move(rea);
        }
        i += n;
        continue;
      }
    }
    ++i;
  }
  if (out.seen && first_at == begin) out.at_start = true;
  return out;
}

// 剥离：剥掉文本里**所有**格式良好标记（任一位置，连带紧随其后的单个换行），其余原样；
// `out`（可选）带回识别事实。
[[nodiscard]] inline std::string strip_phantasma_signal(std::string_view text,
                                                         PhantasmaSignal* out = nullptr) {
  PhantasmaSignal sig = find_phantasma_signal(text);
  if (out) *out = sig;
  if (!sig.seen) return std::string(text);
  std::string res;
  res.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '<') {
      std::string rea;
      std::size_t const n = detail::match_marker(text, i, &rea);
      if (n > 0) {
        std::size_t j = i + n;
        if (j + 1 < text.size() && text[j] == '\r' && text[j + 1] == '\n')
          j += 2;  // CRLF 一并吞
        else if (j < text.size() && text[j] == '\n')
          j += 1;  // LF
        i = j;     // 跳过标记（+ 单个换行）
        continue;
      }
    }
    res.push_back(text[i]);
    ++i;
  }
  return res;
}

// ============================================================
// 切片 2：输出钩子三态回执 + 落痕证据
// ============================================================

// 范围 × 防伪 × 风险门 的输入（slice 4/5 从配置灌入；本层不读配置、只消费参数）。
struct PhantasmaHookOptions {
  bool in_scope      = false;  // 命中防幻觉范围（@config.anti_phantasma，slice 5 灌）
  bool reason_passes = true;   // reason 通过注入特征表（外部显式注入；slice 4 起节点侧走 reason_gate）
  bool reason_gate    = false;  // true → 钩子内部对解析出的 reason 跑内置对抗特征表
                               //         phantasma_reason_passes()（优先于 reason_passes）
  bool risk_veto     = false;  // 范围/风险门否决（slice 4/5 灌；true = 信号再有效也 vetoed）
};

// —— 防伪（切片 4）：reason 的保守对抗特征表（注入文本中的信号，reason 几乎必然踩表）——
// 通过判据：非空、≤80 字、无元字符/HTML 实体/URL/转义/控制字符/零宽字符/自指·绕过关键词。
// 设计取保守：**宁把正经理由误判为可疑（→ 多查一次），不可把注入放行**。
[[nodiscard]] inline bool phantasma_reason_passes(std::string_view reason) {
  if (reason.empty() || reason.size() > 80) return false;
  for (char const c : reason)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;  // 控制字符（含换行/制表）
  for (char const c : reason) {
    char const u = static_cast<char>(static_cast<unsigned char>(c) | 0);
    if (u == '&' || u == '<' || u == '>' || u == '/' || u == '\\' || u == '%') return false;
  }
  static constexpr std::string_view kBadSub[] = {
      "http", "www", "://",
      "\xE2\x80\x8B",  // U+200B ZWSP
      "\xE2\x80\x8C",  // U+200C
      "\xE2\x80\x8D",  // U+200D
      "\xE2\x80\x8E",  // U+200E
      "\xE2\x80\x8F",  // U+200F
      "\xEF\xBB\xBF",  // U+FEFF BOM
  };
  for (std::string_view const b : kBadSub)
    if (reason.find(b) != std::string_view::npos) return false;
  // 自指 / 绕过关键词（ASCII，小写化后子串匹配；中文理由不含这些词 → 天然放行）
  static constexpr std::string_view kBadKw[] = {
      "anti-phantasma", "ignore", "bypass", "override", "disable", "hack", "skip", "do not", "trust"};
  std::string low;
  low.reserve(reason.size());
  for (char const c : reason)
    low.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  for (std::string_view const k : kBadKw)
    if (low.find(k) != std::string_view::npos) return false;
  return true;
}

// —— 范围判定（切片 5 v0）：域关键词命中。
// `domains` 来自 @config.anti_phantasma.domains；**空/缺省 → 视为全范围**（与切片 3 的
// enabled-only 行为向后兼容）；非空 → 任一关键词命中用户提问（last_human）才算范围内。
[[nodiscard]] inline bool phantasma_in_scope(nlohmann::json const& domains,
                                             std::string_view last_human) {
  if (!domains.is_array() || domains.empty()) return true;
  for (nlohmann::json const& d : domains) {
    if (!d.is_string()) continue;
    std::string const& kw = d.get_ref<std::string const&>();
    if (!kw.empty() && last_human.find(kw) != std::string_view::npos) return true;
  }
  return false;
}

// —— P0 范围机械化：时间窗（"近一年"/日期提问）——
// 命中判据（months_back>0 时）：问题里出现相对时间词（今年/去年/最近/最新/本月/上月），
// 或出现 19xx/20xx 年份（可带 -MM / /MM / 年MM），且该年月 ≥ 当前年月 - months_back。
// 未来日期视为在窗内（时效性问题）。解析失败/无日期 → false（不回退成全范围）。
[[nodiscard]] inline bool phantasma_time_in_window(std::string_view text, std::time_t now,
                                                   int months_back) {
  if (months_back <= 0) return false;
  std::tm tmv{};
  ::localtime_r(&now, &tmv);
  int const now_ym = (tmv.tm_year + 1900) * 12 + (tmv.tm_mon + 1);
  int const floor_ym = now_ym - months_back;
  for (std::string_view const w : {"今年", "去年", "最近", "近期", "最新", "本月", "上个月",
                                   "本月", "近一年"})
    if (text.find(w) != std::string_view::npos) return true;

  auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
  for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
    if (!is_digit(text[i]) || (text[i] != '1' && text[i] != '2')) continue;
    int year = 0;
    bool ok = true;
    for (std::size_t k = 0; k < 4; ++k) {
      if (!is_digit(text[i + k])) { ok = false; break; }
      year = year * 10 + (text[i + k] - '0');
    }
    if (!ok || year < 1900 || year > 2100) continue;
    int month = 1;
    bool has_month = false;
    std::size_t j = i + 4;
    std::size_t k = j;
    if (k < text.size() && (text[k] == '-' || text[k] == '/')) {
      ++k;
    } else if (text.substr(k, 3) == "\xE5\xB9\xB4") {  // 年
      k += 3;
    } else {
      k = 0;
    }
    if (k != 0) {
      int m = 0;
      std::size_t digits = 0;
      while (k < text.size() && digits < 2 && is_digit(text[k])) {
        m = m * 10 + (text[k] - '0');
        ++k;
        ++digits;
      }
      if (digits > 0 && m >= 1 && m <= 12) {
        month = m;
        has_month = true;
        j = k;
      }
    }
    // 只给年份（"2025 年"）→ 按整年判：年份 ≥ 窗口起始年份即在窗内；
    // 给了月份 → 按年月精确比较。
    if (!has_month) {
      if (year >= floor_ym / 12) return true;
    } else if (year * 12 + month >= floor_ym) {
      return true;
    }
    i = (j > i ? j - 1 : i);
  }
  return false;
}

// —— P0 范围机械化：模型自评"不确定" ——
// 命中后即使信号有效也**不豁免**（照查）：过度自信与幻觉同源，自称不确定时宁查。
[[nodiscard]] inline bool phantasma_uncertainty_detected(std::string_view text) {
  for (std::string_view const w :
       {"不确定", "不太确定", "无法确定", "没把握", "记不清", "也许", "或许", "大概是",
        "应该是", "印象中"})
    if (text.find(w) != std::string_view::npos) return true;
  return false;
}

// 三态回执：决定“查还是不查”。
enum class PhantasmaVerdict : unsigned char { Accepted, Vetoed, Absent };

// 稳定名（落 plan_log / 审计用；与 problems/50 §5.3 三态拼写一致）。
[[nodiscard]] inline std::string_view phantasma_verdict_name(PhantasmaVerdict v) {
  switch (v) {
    case PhantasmaVerdict::Accepted: return "accepted";
    case PhantasmaVerdict::Vetoed:   return "vetoed";
    case PhantasmaVerdict::Absent:   return "absent";
  }
  return "unknown";
}

// 落痕证据：钩子的全部输出。node / step 由「接线处」补入后再写 plan_log / 审计。
struct PhantasmaEvidence {
  PhantasmaVerdict verdict = PhantasmaVerdict::Absent;
  bool seen      = false;  // 事实：文本里存在格式良好标记
  bool at_start  = false;  // 事实：首个标记位于内容开头
  std::string reason;      // 事实：首个标记的 reason 原值
  std::string cleaned;     // 剥净后的文本（任何 verdict 都剥净——标签不进下游 / 用户侧）
};

// 钩子主入口（决策表见文件头）：
//   范围外 → Absent（不看灯，仍剥净）；
//   范围内 → 信号无效（!at_start / reason 空 / !reason_passes）→ Absent（照查）
//          → 信号有效 且  !risk_veto → Accepted（免查放行）
//          → 信号有效 且   risk_veto → Vetoed（照查）
[[nodiscard]] inline PhantasmaEvidence apply_phantasma_signal(
    std::string_view answer, PhantasmaHookOptions const& opt) {
  PhantasmaSignal sig;
  std::string cleaned = strip_phantasma_signal(answer, &sig);
  // 先全部基于 sig 计算，再 move 字段——避免移后读取。
  bool const reason_ok = [&] {
    if (sig.reason.empty()) return false;
    if (opt.reason_gate) return phantasma_reason_passes(sig.reason);  // 内置对抗特征表
    return opt.reason_passes;                                          // 外部显式注入
  }();
  bool const signal_ok = sig.at_start && reason_ok;
  PhantasmaVerdict v;
  if (!opt.in_scope)
    v = PhantasmaVerdict::Absent;  // 范围外：不看灯
  else if (!signal_ok)
    v = PhantasmaVerdict::Absent;  // 范围内但信号无效 → 宁查
  else if (opt.risk_veto)
    v = PhantasmaVerdict::Vetoed;
  else
    v = PhantasmaVerdict::Accepted;
  PhantasmaEvidence ev;
  ev.verdict = v;
  ev.seen = sig.seen;
  ev.at_start = sig.at_start;
  ev.reason = std::move(sig.reason);
  ev.cleaned = std::move(cleaned);
  return ev;
}

// 落痕序列化：{verdict, accepted, reason, seen, at_start}；node / step 由接线处补入。
[[nodiscard]] inline nlohmann::json phantasma_evidence_to_json(
    PhantasmaEvidence const& ev) {
  return nlohmann::json{{"verdict", std::string(phantasma_verdict_name(ev.verdict))},
                        {"accepted", ev.verdict == PhantasmaVerdict::Accepted},
                        {"reason", ev.reason},
                        {"seen", ev.seen},
                        {"at_start", ev.at_start}};
}

// 落账行（plan_log kind=phantasma）；step 由调用方填。
[[nodiscard]] inline nlohmann::json phantasma_plan_log_entry(
    PhantasmaEvidence const& ev, std::string const& node, int step, bool in_scope, bool forced) {
  return nlohmann::json{{"kind", "phantasma"},
                        {"node", node},
                        {"step", step},
                        {"verdict", phantasma_verdict_name(ev.verdict)},
                        {"accepted", ev.verdict == PhantasmaVerdict::Accepted},
                        {"reason", ev.reason},
                        {"seen", ev.seen},
                        {"at_start", ev.at_start},
                        {"in_scope", in_scope},
                        {"forced", forced}};
}

}  // namespace cppchain::message