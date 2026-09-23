// cppchain —— src/tool/tool.cpp
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

nlohmann::json ToolSpec::to_openai() const {
  return nlohmann::json{{"type", "function"},
                        {"function",
                         {{"name", name},
                          {"description", description},
                          {"parameters", parameters}}}};
}

}  // namespace cppchain::tool