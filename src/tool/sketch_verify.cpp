// cppchain —— tool/sketch_verify.cpp
// verify_sketch：校验一段 JSXGraph/Konva 画板脚本。
//   · 静态脚枪：board.elements/board.objects/.elements[ 等本版本必崩的写法（纯 C++ 扫描，零依赖）；
//   · 语法：node --check（ESM，.mjs）；node 缺失则记 skipped、不算失败。
// 返回 {ok, errors:[], note?}；ok=false 时 errors 给模型自修参考。
#include "cppchain/tool/sketch_verify.hpp"

#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/tool/spawn_runner.hpp"

namespace cppchain::tool {
namespace {

// 已知"一眼崩"的写法（与 geo-starter 线上踩坑清单对齐，见 docs/lessons/questions/）。
constexpr std::pair<std::string_view, std::string_view> kFootguns[] = {
    {"board.elements", "引用了不存在的内部表 board.elements"},
    {"board.objects", "引用了不存在的内部表 board.objects"},
    {".elements[", "通过 .elements[下标] 引用对象，本版本 JSXGraph 无此 API"},
};

std::vector<std::string> static_lint(std::string_view js) {
  std::vector<std::string> issues;
  for (auto const& [pat, msg] : kFootguns)
    if (js.find(pat) != std::string_view::npos) issues.emplace_back(msg);
  return issues;
}

// node --check 语法（ESM 强制 .mjs）。返回 "" = 通过；"skipped: …" = node 不可用；其它 = 错误。
std::string syntax_check(std::string const& js) {
  namespace fs = std::filesystem;
  fs::path const tmp = fs::temp_directory_path() / ("qverify-" + std::to_string(::getpid()) + ".mjs");
  {
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return "skipped: 无法写临时文件";
    std::fwrite(js.data(), 1, js.size(), f);
    std::fclose(f);
  }
  auto const out = detail::run_argv(
      {"/usr/bin/env", "node", "--check", tmp.string()},
      /*cwd=*/std::nullopt, detail::SpawnLimits{},
      /*timeout=*/std::chrono::milliseconds(15000), /*max_output=*/2048);
  std::error_code ec;
  fs::remove(tmp, ec);

  if (out.exit_code == -1) return "skipped: node 不可用（" + out.note + "）";
  if (out.exit_code == 0) return "";
  std::string const first = out.output.substr(0, out.output.find('\n'));
  return first.empty() ? "语法错误（未知）" : first;
}

}  // namespace

ToolSpec make_sketch_verify_tool() {
  return make_tool(
      "verify_sketch",
      "校验一段交互画板脚本（JSXGraph/Konva）能否挂载：先扫一眼必崩的写法，再跑 node 语法检查。"
      "模型刚生成或修订 sketch_js 后**必须调用它自检**；ok=false 时按 errors 修正后重新输出整份题目。"
      "参数 sketch 传完整的 sketch_js 源码。返回 {ok: bool, errors: [原因], note?}。",
      SchemaBuilder()
          .prop("sketch", "string", "待校验的完整画板脚本源码", /*required=*/true)
          .build(),
      [](nlohmann::json const& args, ToolContext& ctx) -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        auto s = arg<std::string>(args, "sketch", /*required=*/true);
        if (!s) return std::unexpected(s.error());
        std::string const& js = *s;

        std::vector<std::string> errors = static_lint(js);
        std::string const syn = syntax_check(js);
        if (!syn.empty() && !syn.starts_with("skipped: ")) errors.push_back("语法错误：" + syn);

        nlohmann::json out = {{"ok", errors.empty()}, {"errors", errors}};
        if (syn.starts_with("skipped: ")) out["note"] = syn;
        return out;
      },
      /*read_only=*/true);
}

}  // namespace cppchain::tool
