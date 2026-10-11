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
  tool/probe.cpp
  tool/exec_guard.cpp
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
  state/memory_backends.cpp
  state/file_checkpointer.cpp)

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

  # 只依赖 CURL：本库仅实现 http::Transport（+ core/cancel.hpp），与模型层无关。
  # 早先 PUBLIC 链了 cppchain_model，导致"想单独用传输层"也被迫拖入整个模型层。
  target_link_libraries(cppchain_http_curl PUBLIC
    CURL::libcurl)

  target_compile_options(cppchain_http_curl PRIVATE -Wall -Wextra -Wpedantic)
endif()

# （M0 的工具链冒烟 src/smoke 已在 M1 落地后移除 —— 它的职责由 300+ 项测试取代，
#   保留只会多一个无人依赖的 target。）
# ---- M6: cppchain_graph（极简 Graph 引擎）----
# 只含引擎本身：仅依赖 state。图引擎是最可独立复用的一层，不应被上层拖累。
add_library(cppchain_graph STATIC
  graph/graph.cpp)

target_include_directories(cppchain_graph PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_graph SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_graph PUBLIC
  cppchain_state
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_graph PRIVATE -Wall -Wextra -Wpedantic)

# ---- M8: cppchain_graph_agent（Agent → 图节点适配器）----
# 单独成库的理由：as_agent_node 只是把 agent::Agent 包成 NodeFn 的适配器，
# 引擎本身不需要 agent。合并的话，任何只用 StateGraph 的人（如图 demo、workflow 编译器）
# 都会被 PUBLIC 链拖入 agent→tool→model 整栈。
add_library(cppchain_graph_agent STATIC
  graph/agent_node.cpp)

target_include_directories(cppchain_graph_agent PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_graph_agent SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_graph_agent PUBLIC
  cppchain_graph
  cppchain_agent   # 单向无环：agent 不依赖 graph
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_graph_agent PRIVATE -Wall -Wextra -Wpedantic)

# ---- M7/M9: cppchain_agent（Agent 循环 + middleware + subagent + tokens/artifact）----
add_library(cppchain_agent STATIC
  agent/agent.cpp
  agent/journal.cpp
  agent/stale.cpp
  agent/subagent.cpp
  agent/tokens.cpp
  agent/artifact.cpp
  agent/history.cpp)

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
  workflow/session_log.cpp
  workflow/node_types.cpp
  workflow/plugin.cpp
  workflow/import.cpp
  workflow/debug_view.cpp
  workflow/wfr_lookup.cpp
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
  cppchain_agent
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_workflow PRIVATE -Wall -Wextra -Wpedantic)

# ---- wfr：正式 CLI（按名执行 workflow + 内置 workflow + 调试台）----
# 输出到 <build>/bin（正式产物，区别于 examples/ 的演示二进制）
add_executable(wfr wfr/main.cpp)
target_link_libraries(wfr PRIVATE
  cppchain_workflow
  cppchain_tool
  cppchain_http_curl)
target_include_directories(wfr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(wfr PRIVATE -Wall -Wextra -Wpedantic)
# 发布构建：静态链接 libstdc++/libgcc —— 产物不依赖本机 GCC runtime（无 GLIBCXX 版本问题）
if(CPPCHAIN_WFR_STATIC_RUNTIME)
  target_link_options(wfr PRIVATE -static-libstdc++ -static-libgcc)
  message(STATUS "wfr: 静态链接 libstdc++/libgcc（发布构建）")
endif()
# 发布自带的固定 workflow 目录（编译期注入；开发构建 = 源码 share/wfr/workflows，安装后 = <prefix>/share/wfr/workflows）
target_compile_definitions(wfr PRIVATE
  WFR_SHARE_WORKFLOWS_DIR="${PROJECT_SOURCE_DIR}/share/wfr/workflows")
set_target_properties(wfr PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 发布：二进制 → <prefix>/bin；固定 workflow → <prefix>/share/wfr/workflows
install(TARGETS wfr RUNTIME DESTINATION bin)
# share/wfr/workflows/*.json（DESTINATION share → <prefix>/share/wfr/workflows）
install(DIRECTORY ${PROJECT_SOURCE_DIR}/share/wfr DESTINATION share
  FILES_MATCHING PATTERN "*.json")
