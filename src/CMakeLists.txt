# 源码入口。自底向上逐步加入：message → model → tool → state → graph → agent ...
# 每个模块一个 static lib，demo 与问题验证在顶层 examples/。

# ---- M1: cppchain_message ----
add_library(cppchain_message STATIC
  message/message.cpp)

target_include_directories(cppchain_message PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

# nlohmann 头文件在 C++26 下会告警(std::is_trivial deprecated)——来自第三方,
# 用 SYSTEM include 压制,避免污染我们自己的零告警目标。
target_include_directories(cppchain_message SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_message PUBLIC
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_message PRIVATE -Wall -Wextra -Wpedantic)

# ---- M2: cppchain_model（ChatModel 抽象 + OpenAI 兼容适配）----
# 不链接 libcurl：传输通过 http::Transport 注入，保持分层干净。
add_library(cppchain_model STATIC
  model/openai_chat_model.cpp
  model/dashscope_embeddings.cpp
  model/structured.cpp)

target_include_directories(cppchain_model PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_model SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_model PUBLIC
  cppchain_message
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_model PRIVATE -Wall -Wextra -Wpedantic)

# ---- M3: cppchain_tool（工具规格 / 注册表 / 检索 / ToolNode / 真网络工具）----
add_library(cppchain_tool STATIC
  tool/tool.cpp
  tool/tool_node.cpp
  tool/embedding_selector.cpp
  tool/http_json.cpp
  tool/weather.cpp
  tool/web_search.cpp
  tool/datetime.cpp
  tool/fetch_url.cpp
  tool/calculator.cpp
  tool/file_tools.cpp
  tool/arxiv.cpp
  tool/hybrid_selector.cpp)

target_include_directories(cppchain_tool PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_tool SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_tool PUBLIC
  cppchain_model
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_tool PRIVATE -Wall -Wextra -Wpedantic)

# ---- M5: cppchain_state（状态 schema/reducer + Checkpointer + Store）----
add_library(cppchain_state STATIC
  state/state.cpp
  state/memory_backends.cpp)

target_include_directories(cppchain_state PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_state SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_state PUBLIC
  cppchain_message
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_state PRIVATE -Wall -Wextra -Wpedantic)

# ---- M2: cppchain_http_curl（真实传输，libcurl multi）----
# 仅在 CPPCHAIN_BUILD_HTTP=ON 时构建；模型层不依赖它（传输是可注入的）。
if(CPPCHAIN_BUILD_HTTP)
  add_library(cppchain_http_curl STATIC
    http/curl_multi_transport.cpp)

  target_include_directories(cppchain_http_curl PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

  target_link_libraries(cppchain_http_curl PUBLIC
    cppchain_model
    CURL::libcurl)

  target_compile_options(cppchain_http_curl PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- 工具链冒烟（随首模块落地后会替换/保留） ----
add_executable(smoke smoke/main.cpp)
target_link_libraries(smoke PRIVATE cppchain_message)
target_compile_options(smoke PRIVATE -Wall -Wextra -Wpedantic)
# ---- M6: cppchain_graph（极简 Graph 引擎 + M8 的 Agent 节点适配）----
add_library(cppchain_graph STATIC
  graph/graph.cpp
  graph/agent_node.cpp)

target_include_directories(cppchain_graph PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_graph SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_graph PUBLIC
  cppchain_state
  cppchain_agent   # M8: as_agent_node 需要 Agent（agent 不依赖 graph，单向无环）
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_graph PRIVATE -Wall -Wextra -Wpedantic)

# ---- M7/M9: cppchain_agent（Agent 循环 + middleware + subagent + tokens/artifact）----
add_library(cppchain_agent STATIC
  agent/agent.cpp
  agent/journal.cpp
  agent/stale.cpp
  agent/subagent.cpp
  agent/tokens.cpp
  agent/artifact.cpp)

target_include_directories(cppchain_agent PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_agent SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_agent PUBLIC
  cppchain_model
  cppchain_tool
  cppchain_state
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_agent PRIVATE -Wall -Wextra -Wpedantic)

# ---- 主动智能 workflow：IR → 图编译/执行/生成（leaf over graph/tool/model/state）----
# D1 触发仅为元数据（不调度）；生成端收敛到 ChatModel&（可 stub 离线测）。
add_library(cppchain_workflow STATIC
  workflow/ir.cpp
  workflow/refs.cpp
  workflow/expr.cpp
  workflow/validator.cpp
  workflow/compiler.cpp
  workflow/generator.cpp)

target_include_directories(cppchain_workflow PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_workflow SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_workflow PUBLIC
  cppchain_graph
  cppchain_tool
  cppchain_model
  cppchain_state
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_workflow PRIVATE -Wall -Wextra -Wpedantic)
