// cppchain —— agent/artifact.hpp
// M9：超长工具结果的 artifact 落盘（问题 2 收官）。全文→文件，prompt 只放引用行 + token/字节标注。
// 与 TruncateToolResults 可叠加：截断压 inline 预算，落盘把超预算全文交给文件。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "cppchain/agent/agent.hpp"

namespace cppchain::agent {

// 把超长工具结果落盘：全文进文件，content 换成"头部 + 完整路径 + 规模标注"，并设
// `tool_message.artifact`。返回落盘文件的**展示路径**；**未落盘**（内容不超预算）→ nullopt。
// 落盘失败时 nullopt，且 content 被写成错误说明（不抛异常）。
// `idx` 由调用方保证唯一（文件名编号）—— Middleware 版本内部有计数器，节点版本用序号。
// `display_root`（wfr-㊶ 补）：非空时，`artifact` 字段与引用行里的路径都改成**相对该根**的形式，
// 让 agent 能直接把路径喂给沙箱 file_read（沙箱只认根内相对路径，绝对路径会被拒）。空 = 旧行为
// （写什么路径就展示什么，ex. 离线单测传临时绝对目录）。
// wfr-㊵：从 `ArtifactToolResults::after_tool` 抽出来，供历史的规则引擎直接调用
//（workflow 层没有 middleware 机制）。
[[nodiscard]] std::optional<std::string> write_tool_artifact(Message& tool_message,
                                                            std::string const& dir,
                                                            std::size_t max_inline_tokens,
                                                            std::size_t idx,
                                                            std::string const& display_root = {});

class ArtifactToolResults final : public Middleware {
 public:
  struct Options {
    std::string dir = "./artifacts";
    std::size_t max_inline_tokens = 40;  // 估算超限才落盘（估算口径见 tokens.hpp）
  };

  explicit ArtifactToolResults(Options opts) : opts_(std::move(opts)) {}

  void after_tool(Message& tool_message) override;

  // 落盘次数（观测"路径真的被走到"）
  [[nodiscard]] std::size_t artifacts_written() const { return written_; }

 private:
  Options opts_;
  std::size_t written_ = 0;
};

}  // namespace cppchain::agent