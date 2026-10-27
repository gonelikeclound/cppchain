// demo: tools_files —— 文件系统工具四件套（file_read / file_write / list_dir / file_grep）
//
// langchain 参照：FileManagementToolkit（ReadFileTool / WriteFileTool / ListDirectoryTool）；
// file_grep 是本仓库为"超长工具返回"补的确定性定位件（04-problems/02 追加 S1/S2）。
//
// 三个看点：
// ① 安全边界：所有路径相对沙箱根目录（demo-out/tools_files_sandbox，已 gitignore）。
//    .. 越界 / 绝对路径 / 符号链接逃逸 一律拒绝 → 错误回喂（不炸进程、不写出去）。
// ② 副作用标记：file_write 是 read_only=false —— 真实 Agent 循环里会被
//    ToolApproval 审批 + SideEffectGuard 防重放记账（见 04-problems/03、side_effect_guard）。
// ③ 大文件按需读：file_read 的 offset/next_offset 翻页 + file_grep 只回命中行，
//    不必把整份超长内容塞进上下文。
#include <cppchain/tool/file_tools.hpp>

#include <filesystem>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

void run_tool(std::string const& label, ToolSpec const& spec, json args) {
  ToolContext ctx;
  ctx.tool_call_id = "demo";
  auto r = spec.handler(args, ctx);
  std::println("— {} —", label);
  std::println("  参数: {}", args.dump());
  if (r) {
    std::println("  结果: {}", r->dump(2));
  } else {
    std::println("  错误: {}", r.error().message);
  }
  std::println("");
}

}  // namespace

int main() {
  std::println("== 文件系统工具四件套：file_read / file_write / list_dir / file_grep ==");
  std::println("");

  std::filesystem::path const sandbox = "demo-out/tools_files_sandbox";
  std::filesystem::create_directories(sandbox);
  std::println("  沙箱根目录: {}（相对仓库根）\n", sandbox.generic_string());

  FileToolsConfig cfg;
  cfg.root_dir = sandbox;
  auto const tools = make_file_tools(cfg);
  if (tools.size() != 6) return 1;
  auto const& read_tool = tools[0];
  auto const& write_tool = tools[1];
  auto const& list_tool = tools[2];
  auto const& grep_tool = tools[3];
  auto const& extract_tool = tools[4];
  auto const& sample_tool = tools[5];

  // 副作用标记一览（写工具是 read_only=false → 走 M7 审批 + 防重放）
  std::println("  [标记] {} read_only={}；{} read_only={}；{} read_only={}；{} read_only={}",
               read_tool.name, read_tool.read_only, write_tool.name, write_tool.read_only,
               list_tool.name, list_tool.read_only, grep_tool.name, grep_tool.read_only);
  std::println("");

  run_tool("① 空沙箱 → list_dir()", list_tool, json::object());
  run_tool("② 写计划 → file_write(notes/plan.md, 内容)", write_tool,
           json{{"path", "notes/plan.md"},
                {"content", "# 实验计划\n1. 构建工具层\n2. 跑通 research 闭环"}});
  run_tool("③ 追加 → file_write(notes/plan.md, append)", write_tool,
           json{{"path", "notes/plan.md"}, {"content", "\n3. 数据驱动决策"}, {"append", true}});
  run_tool("④ 读回 → file_read(notes/plan.md)", read_tool, json{{"path", "notes/plan.md"}});
  run_tool("⑤ 列目录 → list_dir(notes)", list_tool, json{{"path", "notes"}});
  run_tool("⑥ 越界试探 → file_write(../escape.txt) —— 应被安全边界拒绝",
           write_tool, json{{"path", "../escape.txt"}, {"content", "不该写出去"}});
  run_tool("⑦ 绝对路径试探 → file_read(/etc/hostname) —— 应被拒绝",
           read_tool, json{{"path", "/etc/hostname"}});
  // ⑧ 分页：只读第 1 个字节起 12 字节，next_offset 告诉下一窗从哪开始
  run_tool("⑧ 分页读 → file_read(notes/plan.md, offset=0, max_chars=12)",
           read_tool, json{{"path", "notes/plan.md"}, {"offset", 0}, {"max_chars", 12}});
  // ⑨ 确定性定位：只回命中行，不把整份文件喂给模型
  run_tool("⑨ 定位 → file_grep(pattern=\"数据驱动\", path=\".\")", grep_tool,
           json{{"pattern", "数据驱动"}, {"path", "."}});

  // S4/S5：结构化提取 + 采样跳读（先落几份结构化样例）
  run_tool("⑩ 写样例 → config.json", write_tool,
           json{{"path", "config.json"},
                {"content", R"({"server":{"host":"localhost","ports":[80,443]}})"}});
  run_tool("⑪ 写样例 → app.yaml / doc.md / data.xml", write_tool,
           json{{"path", "app.yaml"},
                {"content", "server:\n  host: localhost\n  ports:\n    - 80\n    - 443\n"}});
  run_tool("⑪b 写样例 → doc.md", write_tool,
           json{{"path", "doc.md"},
                {"content", "# 标题\n引言\n## 第一节\nalpha\n### 小节\na1\n## 第二节\nbeta\n"}});
  run_tool("⑪c 写样例 → data.xml", write_tool,
           json{{"path", "data.xml"},
                {"content", "<root><item id=\"1\"><name>alpha</name></item>"
                            "<item id=\"2\"><name>beta</name></item></root>"}});
  run_tool("⑫ 结构提取 → file_extract(config.json, selector=/server/ports/1)", extract_tool,
           json{{"path", "config.json"}, {"selector", "/server/ports/1"}});
  run_tool("⑬ 结构提取 → file_extract(app.yaml, selector=server.ports.0)", extract_tool,
           json{{"path", "app.yaml"}, {"selector", "server.ports.0"}});
  run_tool("⑭ 结构提取 → file_extract(doc.md, selector=\"第一节\")", extract_tool,
           json{{"path", "doc.md"}, {"selector", "第一节"}});
  run_tool("⑮ 结构提取 → file_extract(data.xml, selector=root/item[2]/name)", extract_tool,
           json{{"path", "data.xml"}, {"selector", "root/item[2]/name"}});
  run_tool("⑯ 采样跳读 → file_sample(doc.md, chars_per_window=20)", sample_tool,
           json{{"path", "doc.md"}, {"chars_per_window", 20}});
}