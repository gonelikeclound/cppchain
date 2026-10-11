// cppchain —— tool/command_classify.hpp
// 命令工具的"词面静态分类"（wfr-㊸ @①，L2 静态门禁）。
// 铁律：静态分类**不预测命令的行为**，只做保守裁决——词面可证"纯只读"才放行，
// 无法证明的（有写/危险签名/解析失败/命令替换）一律交给上层 fail-closed 或升级审批。
// 与 ToolSpec::command_classify 装配，bash 工具由 make_bash_tool 绑定本实现。
#pragma once

#include <string>
#include <string_view>

namespace cppchain::tool {

// 分类结果（闭合四格）。ReadOnly 之外一律视为"需要批准"。
enum class CommandClass : unsigned char {
  ReadOnly,  // 词面可证纯只读（无写重定向、无命令替换、每个子命令都在只读表内）→ 免审放行
  Write,     // 词面有普通写效果（重定向 / mv cp mkdir touch / git 写子命令 …）→ 需审批
  Risky,     // 危险签名（rm -rf / sudo / eval / 提权 / 网络 / 解释器任意执行 …）→ 强制审批
  Unknown,   // 解析不了 / 命令不在任何表内 / 无法完整分析 → fail-closed 审批
};

// 带依据的分类（审计/报错时给人看"为什么"）
struct CommandClassify {
  CommandClass cls = CommandClass::Unknown;
  std::string why;  // 人读依据，形如 "命令 cat 在只读白名单内" / "危险命令: sudo"
};

// 便捷入口：只看类别。
[[nodiscard]] CommandClass classify_bash_command(std::string_view command);
// 详情入口：类别 + 依据。
[[nodiscard]] CommandClassify classify_bash_command_detail(std::string_view command);

}  // namespace cppchain::tool