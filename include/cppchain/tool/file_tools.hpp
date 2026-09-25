// cppchain —— tool/file_tools.hpp
// 文件系统工具三件套（参照 langchain-community FileManagementToolkit）：
//   file_read  只读：读文本文件（max_chars 截断、二进制检测）
//   list_dir   只读：列目录（名称/类型/大小/修改时间）
//   file_write 副作用：写/追加文本文件，自动建父目录 —— read_only=false，
//              会被 M7 的 ToolApproval 审批中间件拦截 & SideEffectGuard 防重放记账。
//
// 安全边界：所有路径都**相对沙箱根目录 root_dir** 解析。
// 越界失败三连挡：`..` 词法逃逸 / 绝对路径 / 符号链接逃出 root → 一律拒绝（返回 ToolError 回喂）。
// 不在 root 内的任何操作都不执行（写操作尤其如此）。
#pragma once

#include <chrono>
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
  // 文件/目录快照时效性：30 秒后视为可能已过期（由 agent::StaleStateGuard 消费）。
  std::chrono::seconds ttl = std::chrono::seconds(30);
};

// 纯函数：把用户路径规范化到 root 内；越界 / 绝对路径 / 符号链接逃逸 → nullopt。
// 返回**未解析**的规范化绝对路径（写操作创建的仍是它）。供离线单测与安全自检。
[[nodiscard]] std::optional<std::filesystem::path> resolve_path_in_root(
    std::filesystem::path const& root, std::string_view user_path);

// file_read（只读）。参数：path（必填，相对 root）、max_chars（可选）。返回：
//   {path, size, content, truncated}。文件不存在/是目录/二进制 → unexpected(ToolError)。
[[nodiscard]] ToolSpec make_file_read_tool(FileToolsConfig cfg);

// file_write（**read_only=false**，副作用）。参数：path（必填）、content（必填）、
//   append（可选，默认 false=覆盖写）。返回：{path, bytes_written, created, mode}。
// 越界/目标为目录/内容超限 → unexpected(ToolError)。
[[nodiscard]] ToolSpec make_file_write_tool(FileToolsConfig cfg);

// list_dir（只读）。参数：path（可选，默认 "."）、max_entries（可选）。返回：
//   {path, truncated, entries:[{name, type, size, modified}]}（按名称排序）。
[[nodiscard]] ToolSpec make_list_dir_tool(FileToolsConfig cfg);

// 三件套批量生成（便捷：直接 reg.add() 循环）
[[nodiscard]] std::vector<ToolSpec> make_file_tools(FileToolsConfig cfg);

}  // namespace cppchain::tool