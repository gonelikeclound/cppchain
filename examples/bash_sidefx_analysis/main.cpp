// demo: bash 工具副作用分析（wfr-㊸ @① L2 词面静态门禁的可视化）
//
// 给一组代表性 shell 命令逐个跑 `classify_bash_command_detail`，打印：
//   类别（ReadOnly / Write / Risky / Unknown）+ 依据（为什么这么判）+ 处置（免审 / 需审批）。
// 并自带断言（每条都带预期类别，跑偏即 FAIL 退出非零 —— 数据由分类器打印，文案不硬编码）。
//
// 语义（与 01-discuss/06 §5 L2 一致）：
//   ReadOnly —— 词面可证纯只读（无写重定向/无命令替换/每个子命令都在只读白名单）→ 免审放行
//   Write    —— 普通写效果（重定向 / mv cp mkdir touch / git 写子命令 …）→ 需审批
//   Risky    —— 危险签名（rm -rf / sudo / 网络 / 解释器任意执行 …）→ 强制审批
//   Unknown  —— 解析失败 / 命令不在任何表内 / 无法完整分析 → fail-closed 审批（宁可多拦）
//
// 把这套分析接到 wfr 里的实机效果：`wfr approve_bash_demo`（只读词面可证的命令不打断；
// 其余悬挂等 `approve_policy`/人工裁决）。本 demo 只看分析本身，不执行任何命令。
#include <cppchain/tool/command_classify.hpp>

#include <cstdlib>
#include <print>
#include <string>

using namespace cppchain::tool;

namespace {

char const* cls_name(CommandClass cl) {
  switch (cl) {
    case CommandClass::ReadOnly: return "ReadOnly";
    case CommandClass::Write: return "Write";
    case CommandClass::Risky: return "Risky";
    case CommandClass::Unknown: return "Unknown";
  }
  return "?";
}
char const* cls_mark(CommandClass cl) {
  switch (cl) {
    case CommandClass::ReadOnly: return "🟢";
    case CommandClass::Write: return "🟡";
    case CommandClass::Risky: return "🔴";
    case CommandClass::Unknown: return "⚫";
  }
  return "?";
}
char const* cls_policy(CommandClass cl) {
  switch (cl) {
    case CommandClass::ReadOnly: return "免审放行（guard command_readonly）";
    case CommandClass::Write: return "需审批（静态策略或悬挂人审）";
    case CommandClass::Risky: return "强制审批（危险签名）";
    case CommandClass::Unknown: return "fail-closed 审批（宁可多拦）";
  }
  return "?";
}

}  // namespace

int main() {
  // （命令, 预期类别, 一句为什么这样归类）—— 预期类别来自 tests/tool/command_classify_test
  struct Case {
    std::string cmd;
    CommandClass expect;
  };
  Case const cases[] = {
      {"ls -la", CommandClass::ReadOnly},
      {"cat a.txt | grep x | wc -l", CommandClass::ReadOnly},
      {"cd /tmp && git diff --stat", CommandClass::ReadOnly},  // 管道/复合全只读
      {"find . -name '*.cpp'", CommandClass::ReadOnly},       // find 无 -exec/-delete
      {"echo x > out.txt", CommandClass::Write},               // 写重定向
      {"cp a b && touch f", CommandClass::Write},              // 写子命令
      {"git commit -m x", CommandClass::Write},                // git 写子命令
      {"ls | tee tee.log", CommandClass::Write},               // tee 是写
      {"rm -rf /tmp/x", CommandClass::Risky},                  // 危险签名
      {"sudo ls", CommandClass::Risky},                         // 提权
      {"curl http://example.com", CommandClass::Risky},         // 网络
      {"python3 script.py", CommandClass::Risky},               // 解释器任意执行
      {"bash $CMD", CommandClass::Unknown},                     // bash 不在白名单（可带任意脚本）
      {"some_fancy_thing --flag", CommandClass::Unknown},       // 命令不在任何表内
      {"sh -c 'echo $HOME'", CommandClass::ReadOnly},           // 嵌套 shell 递归：echo 只读 + $VAR 非命令替换
  };

  std::size_t fail = 0;
  std::println("── bash 工具副作用分析（词面静态分类） ──────────────");
  for (auto const& c : cases) {
    CommandClassify const r = classify_bash_command_detail(c.cmd);
    bool const ok = r.cls == c.expect;
    fail += ok ? 0 : 1;
    std::println("{} {} {}", cls_mark(r.cls), cls_name(r.cls),
                 ok ? "✓" : "✗ 与预期不符");
    std::println("   命令: {}", c.cmd);
    std::println("   依据: {}", r.why);
    std::println("   处置: {}\n", cls_policy(r.cls));
  }
  std::println("── 小结：{} 条，{} 条跑偏（跑偏 = 分类器行为与既有测试矩阵不一致） ──",
               std::size(cases), fail);
  return fail == 0 ? 0 : 1;
}