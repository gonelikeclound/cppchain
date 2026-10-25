// demo: config_decl —— 声明式配置（P1.5，arch 25，纯离线）
//
// 演示 workflow 自描述配置（declarations.configs）在 cppchain 的落地：
//   ① 命名空间引用 —— 声明 `{name:"llm", from:"configs/llm.json"}` 后，节点参数写
//      `@config.llm.base_url` 即**点分导航**进该文件（多级路径）。
//   ② target 平铺 —— 声明 `{name:"tools", from:"configs/tools.json", target:"tool"}`：
//      该文件顶层键平铺为 config 默认值，所有节点 `@config.hint_root` 直接命中
//      （等价 miclaw 形态①"该类型节点免声明即用"）。
//   ③ 运行变量 —— 声明文件里可写 `${workdir}`/`${sessionId}`：fire 时对合并后的
//      config 树**全树替换**（host 给 `workspace` 即生效）。
//   ④ 覆盖优先级 —— 宿主 config 顶层键 > 声明缺省（命名空间整块覆盖、平铺键逐键覆盖）。
//   ⑤ 启动即炸 —— 缺 config_dir / from 文件缺失 / JSON 坏 / target 未命中 → 编译期拒。
//
// 全离线：不调模型；用自定义 echo 工具回显参数，证明 @config 引用按声明解析到位。
#include <cppchain/tool/tool.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/refs.hpp>
#include <cppchain/workflow/validator.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <system_error>

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;

namespace {

// 声明区：llm（命名空间）/ tools（命名空间 + target 平铺）/ store（命名空间）
Json demo_ir() {
  return Json::parse(R"json({
    "id": "config_decl_demo",
    "declarations": {"configs": [
      {"name": "llm",   "from": "configs/llm.json"},
      {"name": "tools", "from": "configs/tools.json", "target": "tool"},
      {"name": "store", "from": "configs/store.json"}
    ]},
    "nodes": [
      {"id": "read", "tool": "echo",
       "parameters": {"url": "@config.llm.base_url", "hint": "@config.hint_root"},
       "output_schema": {"got": "string"}, "next": "use_path"},
      {"id": "use_path", "tool": "echo",
       "parameters": {"path": "@config.store.root"},
       "output_schema": {"got": "string"}, "next": "_turn_end"}
    ]
  })json");
}

// 把声明文件写进 workdir（std::filesystem 相对 demo 运行目录）。
void write_config_files(std::filesystem::path const& root) {
  std::filesystem::create_directories(root / "configs");
  auto w = [&](std::string const& rel, std::string const& text) {
    std::ofstream out(root / rel);
    out << text;
  };
  w("configs/llm.json", R"({"base_url":"https://dashscope.aliyuncs.com/compatible-mode/v1","max_tokens":1024})");
  w("configs/tools.json", R"({"hint_root":"${workdir}/data","write_enabled":true})");
  w("configs/store.json", R"({"root":"${workdir}/store"})");
}

struct LastArgs {
  Json url_hint = Json::object();  // 第一节点参数
  Json path = Json::object();      // 第二节点参数
};

}  // namespace

int main() {
  // 工作目录放仓库根 tmp/（项目纪律：不走 /tmp）。
  std::filesystem::path const root = std::filesystem::path("tmp") / "config_decl";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  write_config_files(root);

  // 回显工具（schema 带 properties 层，VN7 可校验；handler 记录参数供展示）。
  LastArgs args;
  auto echo = make_tool(
      "echo", "echo 回显",
      Json{{"type", "object"},
           {"properties",
            Json{{"url", Json{{"type", "string"}}},
                 {"hint", Json{{"type", "string"}}},
                 {"path", Json{{"type", "string"}}}}}},
      [&args](Json const& a, ToolContext&) -> std::expected<Json, ToolError> {
        if (a.contains("path")) args.path = a;
        else args.url_hint = a;
        return Json{{"got", a.dump()}};
      });
  tool::ToolRegistry registry;
  registry.add(std::move(echo));

  auto ir = WorkflowIR::parse(demo_ir());
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 1;
  }
  auto v = validate(*ir, ValidatorOptions{&registry, std::nullopt, nullptr});
  if (!v.ok()) {
    std::println("校验失败: {}", v.describe());
    return 1;
  }

  CompileOptions copts;
  copts.config_dir = root.string();  // P1.5：声明式配置基准目录（from 相对它解析）
  auto wf = compile_workflow(*ir, registry, /*cp=*/nullptr, copts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 1;
  }
  std::println("编译产物: 声明 {} 个 config 文件装入（命名空间 {}）",
               wf->declared_config.size(), wf->declared_names.size());

  // 宿主 config：只给 workspace/session —— 其余全部来自声明文件。
  Json const config{{"workspace", root.string()}, {"session", "config_decl_demo"}};
  auto fire = fire_once(*wf, config);
  if (!fire) {
    std::println("运行失败: {}", fire.error().message);
    return 1;
  }
  Json const& st = *fire;

  std::println("=== config_decl demo（P1.5 · arch 25）===");
  std::println("① 命名空间点分: @config.llm.base_url = {}", args.url_hint.value("url", ""));
  std::println("② target 平铺:   @config.hint_root   = {}（tools.json 顶层键，含 ${{workdir}}）",
               args.url_hint.value("hint", ""));
  std::println("③ 运行变量注入:  store.root (含 ${{workdir}}) = {}", args.path.value("path", ""));
  std::println("④ 宿主覆盖:      workspace = {}（宿主提供，声明未写）",
               config.at("workspace").get<std::string>());
  std::println("最终 config 键: {} 个（声明+宿主；含内部 _config_declared 名单）",
               st.at("config").size());
  return 0;
}