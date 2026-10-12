// cppchain —— tool/probe.hpp
// 副作用产物探测（wfr-㊷）：崩溃缺环时，用工具自查"外部世界现状"来兜底判定
// "执行到一半崩溃"与"根本没执行"的区别（日志只有 Intent 残留、无结果）。
// 判据必须区分**完成度**：产物存在 ≠ 完整 —— 写一半的残缺文件算 Partial，fail-closed。
// 机制见 02-arch-design/18 §6 D3；probe 铁律：只读、失败当 Unknown。
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace cppchain::tool {

// 探测结果（闭合四格）
enum class ProbeResult {
  Complete,  // 产物存在且完整（如：文件存在、内容与本次预期一致）
  Partial,   // 产物存在但不完整（如：写一半的残缺文件）—— 调用方 fail-closed
  Absent,    // 产物不存在（副作用没发生过）—— 视为未开始，可安全执行
  Unknown,   // 无法探测 / probe 自身失败或被中断 —— 调用方按 Intent 原义 fail-closed
};

// 便捷 helper：按"文件存在 + 内容/字节数签名命中"判定（file_write 的磁盘 probe 用）。
//   expected_content 非空 → 读文件比对内容；expected_bytes 非空 → 比对文件大小。
// 文件不存在 → Absent；存在且全部签名命中 → Complete；存在但任一签名不中 → Partial；
// 读取失败/权限异常 → Unknown（绝不当 Absent）。
[[nodiscard]] ProbeResult probe_file(std::filesystem::path const& p,
                                     std::optional<std::string> const& expected_content,
                                     std::optional<std::size_t> expected_bytes);

// 子串探针（wfr-㊹ 补）：文件存在且**包含** fragment 子串 → Complete；存在但不含 → Partial；
// 不存在 → Absent；目录/读失败 → Partial/Unknown（fail-closed 方向）。
[[nodiscard]] ProbeResult probe_file_contains(std::filesystem::path const& p,
                                              std::string const& fragment);

}  // namespace cppchain::tool