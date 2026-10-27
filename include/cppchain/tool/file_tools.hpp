// cppchain —— tool/file_tools.hpp
// 文件系统工具六件套（参照 langchain-community FileManagementToolkit）：
//   file_read   只读：读文本文件（offset+max_chars 分页、二进制检测）
//   list_dir    只读：列目录（名称/类型/大小/修改时间）
//   file_grep   只读：沙箱内按行正则搜索（确定性定位，不把大文件整段喂给模型）
//   file_extract 只读：按结构提取（json 指针 / yaml 键路径 / markdown 标题 / xml 元素路径）
//   file_sample 只读：采样跳读（head/middle/tail 三段 + 明确的覆盖率声明）
//   file_write  副作用：写/追加文本文件，自动建父目录 —— read_only=false，
//               会被 M7 的 ToolApproval 审批中间件拦截 & SideEffectGuard 防重放记账。
//
// 安全边界：所有路径都**相对沙箱根目录 root_dir** 解析。
// 越界失败三连挡：`..` 词法逃逸 / 绝对路径 / 符号链接逃出 root → 一律拒绝（返回 ToolError 回喂）。
// 不在 root 内的任何操作都不执行（写操作尤其如此）。
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct FileToolsConfig {
  std::filesystem::path root_dir;      // 沙箱根目录：三个工具唯一可访问的范围（必填）
  long default_read_chars = 4000;      // file_read 默认返回正文上限（UTF-8 安全截断）
  long max_read_chars = 8000;          // file_read 可请求的上限
  std::size_t max_write_chars = 100000;  // file_write 单次写入上限（防 LLM 超长输出）
  long default_list_entries = 100;     // list_dir 默认返回条数
  long max_list_entries = 1000;        // list_dir 可请求的上限
  long default_grep_matches = 50;      // file_grep 默认最多返回的命中行数
  long max_grep_matches = 200;         // file_grep 可请求的上限
  long default_grep_line_chars = 200;  // file_grep 单行展示上限（UTF-8 安全截断）
  long max_grep_line_chars = 1000;     // 单行展示可请求的上限
  std::uintmax_t max_grep_file_bytes = 2 * 1024 * 1024;  // 单文件扫描上限（超过跳过并计数）
  std::uintmax_t max_grep_scan_bytes = 64 * 1024 * 1024;  // 单次 grep 总扫描上限（防病态大搜索）
  long default_extract_chars = 4000;   // file_extract 返回文本默认上限
  long max_extract_chars = 8000;       // file_extract 可请求的上限
  std::uintmax_t max_extract_file_bytes = 8 * 1024 * 1024;  // 解析类提取的单文件上限
  long default_sample_chars = 400;     // file_sample 每段默认字节数
  long max_sample_chars = 2000;        // file_sample 每段可请求的上限
  // 文件/目录快照时效性：30 秒后视为可能已过期（由 agent::StaleStateGuard 消费）。
  std::chrono::seconds ttl = std::chrono::seconds(30);
};

// 纯函数：把用户路径规范化到 root 内；越界 / 绝对路径 / 符号链接逃逸 → nullopt。
// 返回**未解析**的规范化绝对路径（写操作创建的仍是它）。供离线单测与安全自检。
[[nodiscard]] std::optional<std::filesystem::path> resolve_path_in_root(
    std::filesystem::path const& root, std::string_view user_path);

// file_read（只读）。参数：path（必填，相对 root）、offset（可选，字节起点，缺省 0）、
//   max_chars（可选，窗口上限）。返回：{path, size, offset, next_offset, content, truncated}。
// 分页语义：`next_offset` 非空 = 后面还有内容，把它当下一轮 offset 即可顺序读全；
// content 末尾的「…」是截断标记（不是文件内容）。文件不存在/是目录/二进制 → unexpected。
[[nodiscard]] ToolSpec make_file_read_tool(FileToolsConfig cfg);

// file_grep（只读）。参数：pattern（必填，ECMAScript 正则）、path（可选，缺省 "."，
//   可为子目录或单个文件）、ignore_case（可选）、max_matches（可选）、max_line_chars（可选）。
// 返回：{pattern, path, matches:[{path, line, text}], match_count, files_searched,
//        files_skipped, bytes_scanned, truncated}。二进制 / 超过 max_grep_file_bytes 的文件
// 跳过并计数；总扫描超过 max_grep_scan_bytes → 停止并 truncated=true。
// **没命中 ≠ 不存在**——词面不匹配时应改用向量/语义检索，别据此断言内容缺失。
[[nodiscard]] ToolSpec make_file_grep_tool(FileToolsConfig cfg);

// file_extract（只读）：按文档结构取子内容。
//   参数：path（必填）、format（可选 auto/json/yaml/markdown/xml，缺省按扩展名）、
//         selector（结构路径；各格式语义见下）、max_chars（可选）。
//   语义：
//     json     —— JSON Pointer（"/a/b/0"）或点路径（"a.b[0]"）；空 = 整份文档
//     yaml     —— 点键路径（"server.ports.1"）；空 = 整份文档
//     markdown —— 标题子串（大小写不敏感）；空 = 目录（各级标题 + 行号）
//     xml      —— 元素路径（"root/item[2]/name"，[n] 1 起）；空 = 元素目录
//   返回：{path, format, selector, found, size, text, truncated}；
//   找不到 selector 时 found=false（不报错），并提示目录/近似用法。
[[nodiscard]] ToolSpec make_file_extract_tool(FileToolsConfig cfg);

// file_sample（只读）：采样跳读——head / middle / tail 三段（UTF-8 安全），
//   参数：path（必填）、chars_per_window（可选，默认 400/上限 2000）。
//   返回：{path, size, windows:[{where, offset, text}], note}。
//   **不保证覆盖全文**：只适合"先看轮廓/风格/全局印象"，完整性/有没有类问题请勿据此断言。
[[nodiscard]] ToolSpec make_file_sample_tool(FileToolsConfig cfg);

// file_write（**read_only=false**，副作用）。参数：path（必填）、content（必填）、
//   append（可选，默认 false=覆盖写）。返回：{path, bytes_written, created, mode}。
// 越界/目标为目录/内容超限 → unexpected(ToolError)。
[[nodiscard]] ToolSpec make_file_write_tool(FileToolsConfig cfg);

// list_dir（只读）。参数：path（可选，默认 "."）、max_entries（可选）。返回：
//   {path, truncated, entries:[{name, type, size, modified}]}（按名称排序）。
[[nodiscard]] ToolSpec make_list_dir_tool(FileToolsConfig cfg);

// 六件套批量生成（便捷：直接 reg.add() 循环；顺序 = read / write / list / grep / extract / sample）
[[nodiscard]] std::vector<ToolSpec> make_file_tools(FileToolsConfig cfg);

}  // namespace cppchain::tool