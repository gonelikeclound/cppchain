// cppchain —— agent/artifact.hpp
// M9：超长工具结果的 artifact 落盘（问题 2 收官）。全文→文件，prompt 只放引用行 + token/字节标注。
// 与 TruncateToolResults 可叠加：截断压 inline 预算，落盘把超预算全文交给文件。
#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "cppchain/agent/agent.hpp"

namespace cppchain::agent {

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