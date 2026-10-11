// cppchain —— src/tool/probe.cpp
#include "cppchain/tool/probe.hpp"

#include <fstream>
#include <string>
#include <system_error>

namespace cppchain::tool {

ProbeResult probe_file(std::filesystem::path const& p,
                       std::optional<std::string> const& expected_content,
                       std::optional<std::size_t> expected_bytes) {
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) {
    // 存在性检查失败（权限等）：也无法可靠说"不存在" → Unknown，fail-closed
    if (ec) return ProbeResult::Unknown;
    return ProbeResult::Absent;
  }

  // 目标是个目录：根本不是"写完的文件" → Partial（不稳定证据），fail-closed
  if (std::filesystem::is_directory(p, ec)) return ProbeResult::Partial;
  if (ec) return ProbeResult::Unknown;

  // 字节数签名
  if (expected_bytes) {
    auto const size = std::filesystem::file_size(p, ec);
    if (ec) return ProbeResult::Unknown;
    if (size != *expected_bytes) return ProbeResult::Partial;
  }

  // 内容签名（逐字节比对；不一致 → 写了一半/被改过 → Partial）
  if (expected_content) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return ProbeResult::Unknown;  // 打不开：不把"打不开"当"不完整"或"不存在"
    std::string data(static_cast<std::size_t>(expected_content->size()), '\0');
    if (!in.read(data.data(), static_cast<std::streamsize>(data.size()))) {
      // 不足预期长度 → 明显不完整；但 read 失败也可能含 EOF 正常读完（ifstream 非异常）
      // 再确认实际读了多少
    }
    std::streamsize const got = in.gcount();
    if (got != static_cast<std::streamsize>(data.size())) return ProbeResult::Partial;
    if (data != *expected_content) return ProbeResult::Partial;
  }

  return ProbeResult::Complete;
}

}  // namespace cppchain::tool