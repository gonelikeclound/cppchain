// cppchain —— src/agent/artifact.cpp
#include "cppchain/agent/artifact.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "cppchain/agent/tokens.hpp"

namespace cppchain::agent {

namespace {

// 文件名只留 [A-Za-z0-9_-]，其它一律剔除 —— 防止 tool_call_id 污染路径（沿用文件工具沙箱纪律）。
std::string sanitize_id(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-')
      out.push_back(c);
  }
  return out.empty() ? "unknown" : out;
}

// UTF-8 安全的字符边界回退（复制自 agent.cpp 的口径；断不能切断多字节字符）
std::size_t char_boundary(std::string_view s, std::size_t cut) {
  while (cut > 0 && cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return cut;
}

}  // namespace

std::optional<std::string> write_tool_artifact(Message& tool_message, std::string const& dir,
                                               std::size_t max_inline_tokens, std::size_t idx) {
  std::string const text = tool_message.text();
  if (text.empty()) return std::nullopt;

  namespace tok = cppchain::agent::tokens;
  if (tok::estimate(text) <= max_inline_tokens) return std::nullopt;  // 短结果不进文件

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    tool_message.content.clear();
    tool_message.content.push_back(message::TextBlock{"[artifact 落盘失败] " + ec.message(), std::nullopt});
    return std::nullopt;
  }

  std::string const path = dir + "/artifact_" + std::to_string(idx) + "_" +
                           sanitize_id(tool_message.tool_call_id.value_or("")) + ".txt";

  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    tool_message.content.clear();
    tool_message.content.push_back(
        message::TextBlock{"[artifact 落盘失败] cannot open " + path, std::nullopt});
    return std::nullopt;
  }
  ofs << text;
  ofs.close();

  tool_message.artifact = path;

  // content 换引用行：头部（内联预算内） + 完整路径 + 规模标注
  std::size_t const keep = char_boundary(text, max_inline_tokens * 3);
  std::string const head = text.substr(0, keep);
  tool_message.content.clear();
  tool_message.content.push_back(
      message::TextBlock{head + "\n…[完整结果已落盘: " + path + "（约 " +
                    std::to_string(tok::estimate(text)) + " tokens / " +
                    std::to_string(text.size()) + " 字节）。如需细节请用文件工具读取该文件。]",
                std::nullopt});
  return path;
}

void ArtifactToolResults::after_tool(Message& tool_message) {
  if (write_tool_artifact(tool_message, opts_.dir, opts_.max_inline_tokens, written_ + 1))
    ++written_;  // 只有真的写了文件才占号（与抽函数前一致）
}

}  // namespace cppchain::agent