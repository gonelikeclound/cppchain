// demo: m9_approval —— 审批记忆化与只读免审（离线确定性）
//
//  看点：同一参数指纹只审一次；参数键序不同 ≠ 不同指纹；不同参数才重新决策；
//  给了 registry 的只读工具直接免审。结论全部由计数打印（数据由观测生成）。
#include <cppchain/agent/agent.hpp>
#include <cppchain/tool/tool.hpp>

#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::ToolCall;

namespace {

ToolCall call(std::string name, std::string args) {
  return ToolCall{"call_1", std::move(name), nlohmann::json::parse(std::move(args))};
}

std::string_view verdict(bool ok) { return ok ? "放行" : "拒绝"; }

}  // namespace

int main() {
  tool::ToolRegistry reg;
  reg.add(tool::make_tool("get_weather", "查天气", nlohmann::json::object(),
                          [](nlohmann::json const&, tool::ToolContext&) {
                            return std::expected<nlohmann::json, tool::ToolError>{
                                nlohmann::json("晴")};
                          },
                          /*read_only=*/true));
  reg.add(tool::make_tool("send_email", "发送邮件（有副作用）",
                          tool::SchemaBuilder().prop("to", "string", "收件人").prop("body", "string", "正文").build(),
                          [](nlohmann::json const&, tool::ToolContext&) {
                            return std::expected<nlohmann::json, tool::ToolError>{
                                nlohmann::json("sent")};
                          },
                          /*read_only=*/false));

  ToolApproval::Options opts;
  opts.registry = &reg;  // → get_weather 只读免审
  ToolApproval app(std::vector<std::string>{"send_email"}, opts);

  std::println("=== 审批记忆化（默认 RememberDeny：只记拒绝）===\n");
  std::println("{:<42} {:6} {:6} {:6}", "调用", "结论", "累计拒绝", "记忆命中");

  auto run = [&](std::string name, std::string args) {
    std::string why;
    bool const ok = app.approve_tool_call(call(name, args), why);
    std::println("{:<42} {:6} {:6} {:6}", name + " " + args, verdict(ok), app.denials(),
                 app.memo_hits());
  };

  run("send_email", R"({"to":"boss@example.com","body":"报告"})");   // 决策：拒
  run("send_email", R"({"to":"boss@example.com","body":"报告"})");   // 记忆：拒（不再问）
  run("send_email", R"({"body":"报告","to":"boss@example.com"})");   // 键序不同 → 同一指纹
  run("send_email", R"({"to":"vip@example.com","body":"报告"})");    // 参数真不同 → 重新决策
  run("get_weather", R"({"city":"广州"})");                          // 只读免审

  std::println("\n累计：拒绝 {} 次；记忆命中 {} 次；记忆条数 {}", app.denials(), app.memo_hits(),
               app.memo_size());
  std::println("\n解读：\n  · 同参数二次调用不再走决策（denials 只加 1 次、memo_hits 见涨）；\n"
               "  · 参数键序不影响指纹（canonical_json 按键排序）；\n"
               "  · 只读工具给 registry 后直接放行，不消耗审批预算。");
}