// cppchain —— src/tool/tool.cpp
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

nlohmann::json ToolSpec::to_openai() const {
  // 归一化 schema：真实端点要求 parameters 必须是 type:"object" 的 JSON Schema。
  // 无参工具的默认值是 {}（空对象）→ 会被端点拒绝：
  //   Invalid schema for function 'x': schema must be a JSON Schema of 'type: "object"',
  //   got 'type: null'
  // 该问题**离线测试发现不了**（FakeTransport 不校验 schema），只有真机才暴露。
  // 这里补齐缺失字段，但不丢弃用户已写的内容。
  nlohmann::json params = parameters.is_object() ? parameters : nlohmann::json::object();
  if (!params.contains("type")) params["type"] = "object";
  if (!params.contains("properties")) params["properties"] = nlohmann::json::object();

  return nlohmann::json{{"type", "function"},
                        {"function",
                         {{"name", name},
                          {"description", description},
                          {"parameters", std::move(params)}}}};
}

}  // namespace cppchain::tool